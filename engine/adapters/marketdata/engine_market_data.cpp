/** MarketDataEngine: the feed's lifecycle and the strategies' subscriptions. */

#include "engine_market_data.hpp"
#include "../../model/symbol.hpp"
#include "../../model/utility.hpp"
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <system_error>
#include <stdexcept>
#include <utility>
#include <vector>

namespace adapters {

namespace {

auto join(std::span<const std::string> names) -> std::string {
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

} // namespace

MarketDataEngine::MarketDataEngine(utilities::Logger* logger, const model::PortfolioStructure& structure,
                                   std::unique_ptr<MarketDataFeed> feed,
                                   QuoteBook::SnapshotSink on_snapshot,
                                   std::chrono::milliseconds interval)
    : BaseEngine(logger, "MarketData"), structure_(structure),
      book_(
          structure,
          [this, sink = std::move(on_snapshot)](const model::PortfolioSnapshot& s) {
              record(s);
              if (sink) {
                  sink(s);
              }
          },
          interval),
      feed_(std::move(feed)) {}

MarketDataEngine::~MarketDataEngine() {
    stop();
    if (recorder_.joinable()) {
        recorder_.request_stop();
        recorder_.join(); // it writes what was handed over first
    }
}

void MarketDataEngine::record_to(const std::string& dir) {
    if (dir.empty() || recorder_.joinable()) {
        return;
    }
    record_dir_ = dir;
    for (const std::string& name : structure_.get_all_portfolio_names()) {
        std::vector<std::string>& symbols = record_symbols_[name];
        for (const model::OptionData* option : structure_.get_portfolio(name)->option_apply_order()) {
            const std::optional<model::OptionId> id =
                option != nullptr ? model::parse_option_symbol(option->symbol) : std::nullopt;
            symbols.push_back(id ? model::occ_code(*id) : std::string{}); // the backtest data's
        }
    }
    recorder_ = std::jthread([this](const std::stop_token& st) { run_recorder(st); });
    write_log("Recording the quotes, a minute at a time, under " + dir, utilities::INFO);
}

void MarketDataEngine::record(const model::PortfolioSnapshot& snapshot) {
    if (record_dir_.empty()) {
        return;
    }
    const auto minute_end = std::chrono::ceil<std::chrono::minutes>(snapshot.datetime);
    auto& [kept_end, kept] = minute_[snapshot.portfolio_name];
    if (kept_end != model::DateTime{} && kept_end != minute_end) {
        {
            std::scoped_lock lock(recorded_mutex_);
            recorded_.emplace_back(kept_end, kept);
        }
        recorded_cv_.notify_one();
    }
    kept_end = minute_end;
    kept = snapshot; // its buffers reused
}

void MarketDataEngine::hand_over_all() {
    {
        std::scoped_lock lock(recorded_mutex_);
        for (auto& [_, kept] : minute_) {
            if (kept.first != model::DateTime{}) {
                recorded_.push_back(kept);
            }
        }
    }
    minute_.clear();
    recorded_cv_.notify_one();
}

void MarketDataEngine::run_recorder(const std::stop_token& st) {
    for (;;) {
        std::vector<std::pair<model::DateTime, model::PortfolioSnapshot>> minutes;
        {
            std::unique_lock lock(recorded_mutex_);
            recorded_cv_.wait(lock, st, [this] { return !recorded_.empty(); });
            minutes.swap(recorded_);
        }
        for (const auto& [minute_end, snapshot] : minutes) {
            write_minute(minute_end, snapshot);
        }
        if (st.stop_requested()) {
            std::scoped_lock lock(recorded_mutex_);
            if (recorded_.empty()) {
                return;
            }
        }
    }
}

void MarketDataEngine::write_minute(const model::DateTime& minute_end,
                                   const model::PortfolioSnapshot& s) {
    const auto symbols = record_symbols_.find(s.portfolio_name);
    if (symbols == record_symbols_.end() || symbols->second.size() != s.bid.size()) {
        return;
    }
    const std::chrono::year_month_day day = model::new_york_date(minute_end);
    const std::filesystem::path dir = std::filesystem::path(record_dir_) / s.portfolio_name /
                                      std::format("{}-{:%Y-%m}", s.portfolio_name,
                                                  std::chrono::sys_days{day});
    const std::filesystem::path file =
        dir / std::format("{:%Y%m%d}.csv", std::chrono::sys_days{day});
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const bool fresh = !std::filesystem::exists(file, ec);
    std::ofstream out(file, std::ios::app);
    if (!out) {
        write_log("Cannot record the quotes to " + file.string(), utilities::ERROR);
        return;
    }
    if (fresh) {
        out << "ts_ns,symbol,bid,ask,underlying_bid,underlying_ask\n";
    }
    // An index quotes a last price only: it stands for both sides.
    const double under_bid = s.underlying_bid > 0.0 ? s.underlying_bid : s.underlying_mark;
    const double under_ask = s.underlying_ask > 0.0 ? s.underlying_ask : s.underlying_mark;
    const long long ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(minute_end.time_since_epoch()).count();
    for (std::size_t i = 0; i < s.bid.size(); ++i) {
        if ((s.bid[i] > 0.0 || s.ask[i] > 0.0) && !symbols->second[i].empty()) {
            out << ns << ',' << symbols->second[i] << ',' << s.bid[i] << ',' << s.ask[i] << ','
                << under_bid << ',' << under_ask << '\n';
        }
    }
}

void MarketDataEngine::start() {
    std::scoped_lock lock(control_mutex_);
    if (running_) {
        return;
    }
    if (!feed_) {
        throw std::runtime_error("No market data source in this build: build the live engine with"
                                 " the IB gateway (./build.sh r 0 g)");
    }
    feed_->start(book_, *this);
    running_ = true;
    write_log("Market data started", utilities::INFO);
}

void MarketDataEngine::stop() {
    std::scoped_lock lock(control_mutex_);
    if (!running_) {
        return;
    }
    feed_->stop();
    running_ = false;
    hand_over_all(); // the feed's thread is gone: its minutes are this thread's now
    book_.reset();   // and so is the book
    write_log("Market data stopped", utilities::INFO);
}

void MarketDataEngine::subscribe_chains(const std::string& strategy_name,
                                        std::span<const std::string> chains) {
    write_log(std::format("{} subscribes {}", strategy_name, join(chains)), utilities::INFO);
    if (const std::vector<std::string> unknown = book_.subscribe(strategy_name, chains);
        !unknown.empty()) {
        write_log(std::format("{}: no chain {} to stream", strategy_name, join(unknown)), utilities::WARNING);
    }
}

void MarketDataEngine::unsubscribe_chains(const std::string& strategy_name) {
    book_.unsubscribe(strategy_name);
}

void MarketDataEngine::hold(std::span<const std::string> options) {
    // An option the book lacks has expired (its contract is gone) or was never synced.
    for (const std::string& symbol : book_.hold(options)) {
        write_log("Held option not streamed (no contract): " + symbol, utilities::DEBUG);
    }
}

} // namespace adapters
