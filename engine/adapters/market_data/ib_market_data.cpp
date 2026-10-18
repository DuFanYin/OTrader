/** IbMarketData: TWS quotes into the market data engine, on the session's message thread. */

#include "adapters/market_data/ib_market_data.hpp"
#include "utilities/log_source.hpp"
#include "model/constant.hpp"
#include "utilities/log_level.hpp"
#include "adapters/ib_api/ib_api.hpp"

#include "Contract.h"
#include "Decimal.h"
#include "TagValue.h"
#include "bar.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace adapters {

namespace {

/** The quote a price tick sets, live or delayed; none for the other ticks. */
auto quote_field(TickType tick) -> std::optional<core::QuoteField> {
    switch (tick) {
    case BID:
    case DELAYED_BID:
        return core::QuoteField::Bid;
    case ASK:
    case DELAYED_ASK:
        return core::QuoteField::Ask;
    case LAST:
    case DELAYED_LAST:
        return core::QuoteField::Last;
    default:
        return std::nullopt;
    }
}

/** The size a size tick sets, live or delayed; none for the other ticks. */
auto size_field(TickType tick) -> std::optional<core::QuoteField> {
    switch (tick) {
    case BID_SIZE:
    case DELAYED_BID_SIZE:
        return core::QuoteField::BidSize;
    case ASK_SIZE:
    case DELAYED_ASK_SIZE:
        return core::QuoteField::AskSize;
    default:
        return std::nullopt;
    }
}

/** How often the source logs what it streams and how many prices came in. */
constexpr auto kSummaryInterval = std::chrono::minutes(1);

/** Request ids of closes asked for, apart from the tickers' (1 up). */
constexpr int kCloseRequests = 1'000'000;

} // namespace

class IbMarketDataSession final : public IbSession {
  public:
    IbMarketDataSession(core::MarketDataEngine& engine, const IbSettings& settings)
        : IbSession(engine.log_source()), engine_(engine),
          lines_(static_cast<std::size_t>(std::max(0, settings.market_data_lines))),
          type_(settings.market_data_type) {}
    ~IbMarketDataSession() override { stop(); }

    void ask_close(const std::string& underlying, std::chrono::year_month_day day) {
        queue_on_message_thread([this, underlying, day] {
            const int id = kCloseRequests + static_cast<int>(closes_.size());
            closes_.push_back({.underlying = underlying, .day = day});
            const Contract c = ib_contract(underlying);
            const std::string end =
                std::format("{:%Y%m%d} 23:59:59 US/Eastern", std::chrono::sys_days{day});
            if (!send([&](EClientSocket& s) {
                    s.reqHistoricalData(id, c, end, "5 D", "1 day", "TRADES", 1, 1, false, {});
                })) {
                log(std::format("Not connected to IB: {}'s close of {} not asked", underlying,
                                day),
                    utilities::WARNING);
            }
        });
    }

  private:
    /** A close asked for; price 0 until its bar comes. */
    struct Close {
        std::string underlying;
        std::chrono::year_month_day day;
        double price = 0.0;
    };

    auto close_request(int id) -> Close* {
        const int i = id - kCloseRequests;
        return i >= 0 && static_cast<std::size_t>(i) < closes_.size() ? &closes_[i] : nullptr;
    }

    void historicalData(int reqId, const Bar& bar) override {
        if (Close* c = close_request(reqId);
            c != nullptr && bar.time == std::format("{:%Y%m%d}", std::chrono::sys_days{c->day})) {
            c->price = bar.close;
        }
    }

    void historicalDataEnd(int reqId, const std::string& /*start*/,
                           const std::string& /*end*/) override {
        Close* c = close_request(reqId);
        if (c == nullptr) {
            return;
        }
        if (c->price <= 0.0) {
            log(std::format("IB has no daily bar of {} on {}", c->underlying, c->day),
                utilities::WARNING);
            return;
        }
        log(std::format("IB's close of {} on {}: {}", c->underlying, c->day, c->price),
            utilities::INFO);
        engine_.closed(c->underlying, c->day, c->price);
    }

    /** A ticker (IB's id for a stream is its index + 1) and the slots its quotes go to; no
     * contract once cancelled. */
    struct Ticker {
        const model::Contract* contract = nullptr;
        std::vector<core::QuoteSlot> slots;
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
        engine_.publish(); // what this batch of messages changed, at once
        if (engine_.replan_due(now)) {
            stream(engine_.instruments(lines_));
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
    void stream(const std::vector<core::Instrument>& wanted) {
        std::vector<const model::Contract*> order; // as wanted, each once
        std::unordered_map<const model::Contract*, std::vector<core::QuoteSlot>> slots;
        for (const core::Instrument& i : wanted) {
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
                for (const core::QuoteSlot& slot : t.slots) {
                    if (!std::ranges::contains(kept->second, slot)) {
                        engine_.clear(slot);
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
        for (const model::Contract* contract : order) {
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

    void clear(const std::vector<core::QuoteSlot>& slots) {
        for (const core::QuoteSlot& slot : slots) {
            engine_.clear(slot);
        }
    }

    auto ticker(int id) const -> const Ticker* {
        if (id < 1 || static_cast<std::size_t>(id) > tickers_.size() ||
            tickers_[id - 1].contract == nullptr) {
            return nullptr;
        }
        return &tickers_[id - 1];
    }

    /** `value` into every slot of the ticker's contract; false for an unknown ticker or a tick
     * of no field. IB stamps no time on a tick: it is as of its arrival. */
    auto quote(int id, std::optional<core::QuoteField> field, double value) -> bool {
        const Ticker* t = ticker(id);
        if (t == nullptr || !field) {
            return false;
        }
        const auto now = std::chrono::system_clock::now();
        for (const core::QuoteSlot& slot : t->slots) {
            engine_.quote(slot, *field, value, now);
        }
        return true;
    }

    void tickPrice(int tickerId, TickType tick, double price,
                   const TickAttrib& /*attrib*/) override {
        if (quote(tickerId, quote_field(tick), std::max(price, 0.0))) { // -1: no quote
            ++prices_since_summary_;
        }
    }

    void tickSize(int tickerId, TickType tick, Decimal size) override {
        quote(tickerId, size_field(tick), std::max(DecimalFunctions::decimalToDouble(size), 0.0));
    }

    void on_error(int id, int code, const std::string& message) override {
        if (const Close* c = close_request(id); c != nullptr) {
            log(std::format("IB {} asking {}'s close of {}: {}", code, c->underlying, c->day,
                            message),
                utilities::WARNING);
            return;
        }
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

    core::MarketDataEngine& engine_;
    const std::size_t lines_;
    const int type_;
    // The message thread's, but for on_connected the first time.
    std::unordered_map<const model::Contract*, int> streams_; // → ticker id
    std::vector<Ticker> tickers_;
    std::unordered_set<int> reported_;
    std::unordered_set<const model::Contract*> unknown_;
    std::size_t prices_since_summary_ = 0;
    std::chrono::steady_clock::time_point next_summary_;
    std::vector<Close> closes_; // by request id, from kCloseRequests
};

IbMarketData::IbMarketData() = default;

IbMarketData::~IbMarketData() = default;

void IbMarketData::start(core::MarketDataEngine& engine) {
    const IbSettings s = IbSettings::from_env(engine.log_source());
    const int client_id = s.client_id + 1;
    engine.log_source().write_log(std::format("Connecting to {}:{} as client {} for quotes ({}"
                                              " lines, market data type {})",
                                              s.host, s.port, client_id, s.market_data_lines,
                                              s.market_data_type),
                                  utilities::INFO);
    session_ = std::make_unique<IbMarketDataSession>(engine, s);
    if (!session_->start(s.host, s.port, client_id)) {
        session_.reset();
        throw std::runtime_error(std::format("Cannot reach IB at {}:{} as client {}: is IB"
                                             " Gateway running, with its API on that port?",
                                             s.host, s.port, client_id));
    }
}

void IbMarketData::stop() { session_.reset(); } // its destructor stops it

void IbMarketData::ask_close(const std::string& underlying, std::chrono::year_month_day day) {
    if (session_) {
        session_->ask_close(underlying, day);
    }
}

} // namespace adapters
