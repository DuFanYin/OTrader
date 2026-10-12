/** IbFeed: TWS market data into the quote book. */

#include "feed_ib.hpp"
#include "../../utilities/base_engine.hpp"
#include "../../model/constant.hpp"
#include "../../utilities/log_level.hpp"
#include "ib_session.hpp"
#include "ib_contracts.hpp"
#include "ib_settings.hpp"

#include "Contract.h"
#include "TagValue.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace adapters {

namespace {

/** The quote a price tick sets, live or delayed; none for the other ticks. */
auto quote_field(TickType tick) -> std::optional<QuoteField> {
    switch (tick) {
    case BID:
    case DELAYED_BID:
        return QuoteField::Bid;
    case ASK:
    case DELAYED_ASK:
        return QuoteField::Ask;
    case LAST:
    case DELAYED_LAST:
        return QuoteField::Last;
    default:
        return std::nullopt;
    }
}

/** How often the feed logs what it streams and how many prices came in. */
constexpr auto kSummaryInterval = std::chrono::minutes(1);

} // namespace

class IbFeedSession final : public IbSession {
  public:
    IbFeedSession(QuoteBook& book, const utilities::BaseEngine& owner, const IbSettings& settings)
        : IbSession(owner), book_(book),
          lines_(static_cast<std::size_t>(std::max(0, settings.market_data_lines))),
          type_(settings.market_data_type) {}
    ~IbFeedSession() override { stop(); }

  private:
    /** A ticker (IB's id for a stream is its index + 1) and the slots its quotes go to; no
     * contract once cancelled. */
    struct Ticker {
        const model::ContractData* contract = nullptr;
        std::vector<QuoteSlot> slots;
    };

    // Streams end with their connection: a new one starts from none.
    void on_connected() override {
        streams_.clear();
        tickers_.clear();
        reported_.clear();
        prices_since_summary_ = 0;
        send([this](EClientSocket& s) { s.reqMarketDataType(type_); });
    }

    void on_disconnected() override {
        for (const auto& [_, id] : streams_) {
            clear(tickers_[id - 1].slots);
        }
        streams_.clear();
        tickers_.clear();
    }

    void on_wake() override {
        const auto now = std::chrono::steady_clock::now();
        book_.publish(); // what this batch of messages changed, at once
        if (book_.replan_due(now)) {
            stream(book_.instruments(lines_));
        }
        if (now >= next_summary_) { // what is coming in, while anything streams
            if (!streams_.empty()) {
                log(std::format("Quotes: streaming {} contracts, {} prices in the last minute",
                                streams_.size(), prices_since_summary_),
                    utilities::INFO);
            }
            prices_since_summary_ = 0;
            next_summary_ = now + kSummaryInterval;
        }
    }

    /** Stream exactly `wanted`, each contract once into all its slots: cancel the others,
     * request the new ones. */
    void stream(const std::vector<Instrument>& wanted) {
        std::vector<const model::ContractData*> order; // as wanted, each once
        std::unordered_map<const model::ContractData*, std::vector<QuoteSlot>> slots;
        for (const Instrument& i : wanted) {
            auto [it, added] = slots.try_emplace(i.contract);
            if (added) {
                order.push_back(i.contract);
            }
            it->second.push_back(i.slot);
        }
        for (auto it = streams_.begin(); it != streams_.end();) {
            Ticker& t = tickers_[it->second - 1];
            if (const auto kept = slots.find(it->first); kept != slots.end()) {
                // Still streamed: forget the slots it no longer fills.
                for (const QuoteSlot& slot : t.slots) {
                    if (!std::ranges::contains(kept->second, slot)) {
                        book_.clear(slot);
                    }
                }
                t.slots = kept->second;
                ++it;
                continue;
            }
            const int id = it->second;
            send([id](EClientSocket& s) { s.cancelMktData(id); });
            clear(t.slots);
            t = {};
            it = streams_.erase(it);
        }
        for (const model::ContractData* contract : order) {
            if (streams_.contains(contract)) {
                continue;
            }
            const int con_id = contract->con_id.value_or(0);
            if (con_id == 0) {
                if (unknown_.insert(contract).second) {
                    log(std::format("No IB contract id for {}: sync the contracts",
                                    contract->symbol),
                        utilities::WARNING);
                }
                continue;
            }
            tickers_.push_back({contract, std::move(slots.at(contract))});
            const int id = static_cast<int>(tickers_.size());
            streams_.emplace(contract, id);
            Contract c;
            c.conId = con_id;
            // An index streams from the exchange it is listed on; the rest from SMART (NBBO).
            const IbListing* listing = contract->product == model::Product::INDEX
                                           ? find_listing(contract->symbol)
                                           : nullptr;
            c.exchange = listing != nullptr ? std::string(listing->exchange) : "SMART";
            send([&](EClientSocket& s) { s.reqMktData(id, c, "", false, false, {}); });
        }
    }

    void clear(const std::vector<QuoteSlot>& slots) {
        for (const QuoteSlot& slot : slots) {
            book_.clear(slot);
        }
    }

    auto ticker(int id) const -> const Ticker* {
        if (id < 1 || static_cast<std::size_t>(id) > tickers_.size() ||
            tickers_[id - 1].contract == nullptr) {
            return nullptr;
        }
        return &tickers_[id - 1];
    }

    void tickPrice(int tickerId, TickType tick, double price,
                   const TickAttrib& /*attrib*/) override {
        const Ticker* t = ticker(tickerId);
        const std::optional<QuoteField> field = quote_field(tick);
        if (t != nullptr && field) {
            // -1: no quote. IB stamps no time on a tick: it is as of its arrival.
            const auto now = std::chrono::system_clock::now();
            for (const QuoteSlot& slot : t->slots) {
                book_.quote(slot, *field, std::max(price, 0.0), now);
            }
            ++prices_since_summary_;
        }
    }

    void on_error(int id, int code, const std::string& message) override {
        const Ticker* t = ticker(id);
        if (t == nullptr) {
            IbSession::on_error(id, code, message);
            return;
        }
        // About one contract, but the same for most of the lines: say each code once.
        if (reported_.insert(code).second) {
            log(std::format("IB {} for {}: {} (the same for other contracts is not"
                            " logged)",
                            code, t->contract->symbol, message),
                utilities::WARNING);
        }
    }

    QuoteBook& book_;
    const std::size_t lines_;
    const int type_;
    // The message thread's, but for on_connected the first time.
    std::unordered_map<const model::ContractData*, int> streams_; // → ticker id
    std::vector<Ticker> tickers_;
    std::unordered_set<int> reported_;
    std::unordered_set<const model::ContractData*> unknown_;
    std::size_t prices_since_summary_ = 0;
    std::chrono::steady_clock::time_point next_summary_;
};

IbFeed::IbFeed() = default;

IbFeed::~IbFeed() = default;

void IbFeed::start(QuoteBook& book, const utilities::BaseEngine& log) {
    const IbSettings s = IbSettings::from_env(log);
    const int client_id = s.client_id + 1;
    log.write_log(std::format("Connecting to {}:{} as client {} for quotes ({} lines, market data"
                              " type {})",
                              s.host, s.port, client_id, s.market_data_lines, s.market_data_type),
                  utilities::INFO);
    session_ = std::make_unique<IbFeedSession>(book, log, s);
    if (!session_->start(s.host, s.port, client_id)) {
        session_.reset();
        throw std::runtime_error(std::format("Cannot reach IB at {}:{} as client {}: is IB"
                                             " Gateway running, with its API on that port?",
                                             s.host, s.port, client_id));
    }
}

void IbFeed::stop() { session_.reset(); } // its destructor stops it

} // namespace adapters
