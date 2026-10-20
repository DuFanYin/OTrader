/** Backtests: the replay, the engine and its day, what a day measures, then trials over many days. */

#include "assembly/backtest.hpp"
#include "adapters/database/backtest_database.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "model/event.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include "utilities/text.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <tuple>
#include <utility>

namespace backtest {

namespace {

// What a day measures.

/** The trading days from the one `from` falls on to the one `to` does, both counted. */
auto trading_days(const model::SessionCalendar& calendar, model::DateTime from, model::DateTime to)
    -> int {
    const std::chrono::year_month_day last = calendar.date_of(to);
    int n = 1;
    for (auto d = calendar.date_of(from); d < last; d = calendar.next_trading_day(d)) {
        ++n;
    }
    return n;
}

/** A holding's P&L by source. */
using Summary = model::HoldingSummary;
constexpr std::array kBySource{&Summary::pnl_delta, &Summary::pnl_gamma, &Summary::pnl_theta,
                               &Summary::pnl_vega,  &Summary::pnl_other, &Summary::pnl_fills};

constexpr std::string_view kSettlement = "settle-"; // the order ids settlement books under

} // namespace

// ---- Backtest: the engine

Backtest::Backtest(adapters::Replay& replay)
    : replay_(replay),
      main_(
          core::Main::Ports{
              // No clock: each bar's time (run_bar).
              .database = [](utilities::Logger*) -> std::unique_ptr<core::Database> {
                  return std::make_unique<adapters::BacktestDatabase>();
              },
              .gateway = [this](utilities::Logger*, const model::Portfolios& portfolios)
                  -> std::unique_ptr<core::Gateway> {
                  auto gateway = std::make_unique<adapters::BacktestGateway>(
                      portfolios, adapters::BacktestGateway::Execution{});
                  backtest_gateway_ = gateway.get();
                  return gateway;
              },
              .portfolios = &replay_.portfolios()},
          // Quiet from the start: stdout carries the JSON result.
          utilities::DISABLED) {
    main_.log_engine()->set_clock([this] { return main_.now(); });
    main_.write_log("Main engine initialization successful", utilities::INFO);
}

Backtest::~Backtest() {
    main_.close();
}

void Backtest::configure_execution(const adapters::BacktestGateway::Execution& execution) {
    backtest_gateway_->configure_execution(execution);
}

auto Backtest::use_day(const adapters::LoadedDay& day, std::span<const std::string> dropped)
    -> adapters::HistoricalData* {
    day_ = day;
    if (!day->has_data()) {
        return day_.get(); // begin_day says why
    }
    if (used_a_day_) {
        main_.contracts_rolled(day->portfolio_name(),
                               std::vector<std::string>(dropped.begin(), dropped.end()));
    }
    used_a_day_ = true;
    portfolio_ = replay_.portfolios().find_portfolio(day->portfolio_name());
    return day_.get();
}

auto Backtest::strategy() const -> const model::StrategyRecord* {
    const auto all = main_.strategy_engine()->strategies();
    return std::ranges::distance(all) == 1 ? &*all.begin() : nullptr;
}

// ---- Backtest: a day, bar by bar

auto Backtest::begin_day() -> bool {
    result_ = DailyResult{.file_path = day_ != nullptr ? day_->path() : std::string{}};
    running_ = false;
    adapters::HistoricalData* day = day_.get();
    if (day == nullptr || !day->has_data()) {
        const std::string why = day != nullptr ? day->load_error() : std::string{};
        result_.errors.emplace_back(why.empty() ? "No data loaded. Call use_day() first."
                                                : "No data loaded: " + why);
        return false;
    }
    const model::StrategyRecord* strategy = this->strategy();
    if (strategy == nullptr) {
        result_.errors.emplace_back("No strategy added. Call add_strategy() first.");
        return false;
    }

    // The first day inits and starts the strategy; the days after run it on.
    if (strategy->state == model::StrategyState::Added) {
        main_.init_strategy(strategy->name);
    }
    if (strategy->state == model::StrategyState::Error) {
        result_.errors.push_back(main_.strategy_engine()->error(strategy->name));
        return false;
    }
    if (strategy->state != model::StrategyState::Running) {
        main_.start_strategy(strategy->name);
    }
    strategy_ = strategy;
    holding_ = main_.bookkeeping_engine()->find_holding(strategy->name);
    load_chains_wanted();
    // Nothing is forgotten during a run, and an id booked already is not booked again: the orders
    // added are the day's.
    earlier_orders_ = main_.bookkeeping_engine()->order_count();
    result_.start_time = result_.end_time = std::chrono::system_clock::now(); // kept with no bar
    running_ = true;
    return true;
}

void Backtest::load_chains_wanted() {
    const core::StrategyEngine& strategies = *main_.strategy_engine();
    wanted_changes_ = strategies.wanted_changes();
    replay_.load(*day_, strategies.wanted()->chains);
    if (std::string why = day_->load_error(); !why.empty() &&
                                                   !std::ranges::contains(result_.errors, why)) {
        result_.errors.push_back(std::move(why));
    }
}

void Backtest::run_bar(std::size_t i) {
    if (restart_minute_ > 0 &&
        portfolio_->calendar().minutes_since_open(portfolio_->frame().datetime) ==
            restart_minute_) {
        main_.strategy_engine()->make_again(strategy_->name);
    }
    const bool asked = main_.strategy_engine()->wanted_changes() != wanted_changes_;
    if (asked) { // a chain asked for since
        load_chains_wanted();
        // The bar applied again, so that the chain is read from this bar on, by this engine and
        // those after it.
        replay_.apply(day_, i);
    }
    const model::Frame& bar = portfolio_->frame();
    main_.set_time(bar.datetime);
    // The orders sent before fill at this bar's quotes, and their fills come first, as live they
    // come ahead of the quotes after them; whatever on_snapshot or on_timer sends now fills at the
    // next bar.
    backtest_gateway_->execute_pending_orders();
    main_.snapshot_applied(bar);
    main_.event_loop()->put_event(model::Event(model::EventType::Timer));
    measure(i, bar);
}

auto Backtest::end_day() -> DailyResult {
    if (!running_) {
        return std::move(result_);
    }
    running_ = false;
    cancel_active_orders();
    const adapters::HistoricalData& day = *day_;
    const std::string& name = strategy_->name;
    const model::StrategyHolding& holding = *holding_;
    const model::HoldingSummary& now = holding.summary;
    result_.processed_timesteps = static_cast<int>(day.minutes());
    result_.total_rows = day.rows();
    result_.total_orders =
        static_cast<int>(main_.bookkeeping_engine()->order_count() - earlier_orders_);
    // The engine sums from the start: the day's are since the close before.
    result_.final_pnl = now.pnl;
    result_.daily_pnl = now.pnl - before_.pnl;
    result_.daily_fees = backtest_gateway_->cumulative_fees() - fees_before_;
    result_.summary = now;
    for (const auto source : kBySource) {
        result_.summary.*source -= before_.*source;
    }
    result_.flat_at_close = holding.is_flat();
    result_.values = main_.strategy_engine()->take_values(name);
    before_ = now;
    fees_before_ += result_.daily_fees;

    const std::span<const model::Fill> fills = main_.bookkeeping_engine()->fills();
    result_.fills.assign(fills.begin() + static_cast<std::ptrdiff_t>(fills_reported_),
                         fills.end());
    fills_reported_ = fills.size();
    // By time; within a minute by order and contract.
    std::ranges::stable_sort(result_.fills, {}, [](const model::Fill& t) {
        return std::tie(t.datetime, t.order_id, t.symbol);
    });
    if (std::string error = main_.strategy_engine()->error(name); !error.empty()) {
        result_.errors.push_back(std::move(error));
    }
    return std::move(result_);
}

void Backtest::cancel_active_orders() {
    std::vector<model::CancelRequest> active;
    main_.bookkeeping_engine()->for_each_active_order(
        {}, [&](const model::Order& o) { active.push_back(o.create_cancel_request()); });
    for (const model::CancelRequest& cancel : active) {
        main_.cancel_order(cancel); // the venue reports each cancelled
    }
}

void Backtest::forget_finished_orders() {
    main_.bookkeeping_engine()->forget_finished();
    // After the day's result: the fills kept were all taken.
    fills_reported_ = fills_costed_ = main_.bookkeeping_engine()->fills().size();
}

// ---- Backtest: what a day measures

void Backtest::measure(std::size_t i, const model::Frame& bar) {
    if (i == 0) {
        result_.start_time = bar.datetime;
    }
    result_.end_time = bar.datetime;
    if (bar.underlying_mark > 0.0) {
        if (result_.underlying_open == 0.0) {
            result_.underlying_open = bar.underlying_mark;
        }
        result_.underlying_close = bar.underlying_mark;
    }
    const model::HoldingSummary& now = holding_->summary;
    result_.max_delta = std::max(std::abs(now.delta), result_.max_delta);
    result_.max_gamma = std::max(std::abs(now.gamma), result_.max_gamma);
    result_.max_theta = std::max(std::abs(now.theta), result_.max_theta);
    result_.max_margin = std::max(main_.risk_engine()->margin(strategy_->name), result_.max_margin);
    if (keep_metrics_) {
        result_.metrics.push_back(
            {.pnl = now.pnl, .delta = now.delta, .theta = now.theta, .gamma = now.gamma});
    }
    track_round_trip(bar);
}

void Backtest::track_round_trip(const model::Frame& bar) {
    const core::BookkeepingEngine& books = *main_.bookkeeping_engine();
    const model::StrategyHolding& holding = *holding_;
    const model::Portfolio* portfolio = portfolio_;
    const double fees = backtest_gateway_->cumulative_fees();
    last_bar_time_ = bar.datetime;

    const std::span<const model::Fill> fresh = books.fills().subspan(fills_costed_);
    fills_costed_ = books.fills().size();
    if (!open_trip_ && !fresh.empty()) {
        open_trip_ = RoundTrip{.opened = bar.datetime, .max_loss = holding.max_loss()};
        opening_orders_.clear();
        for (const model::Fill& fill : fresh) {
            opening_orders_.insert(fill.order_id);
        }
    }
    if (!open_trip_) {
        last_bar_ = holding.summary;
        fees_at_last_bar_ = fees;
        return;
    }
    RoundTrip& trip = *open_trip_;
    for (const model::Fill& fill : fresh) {
        const std::optional<model::OptionId> id = model::parse_option_symbol(fill.symbol);
        if (!id) {
            ++trip.share_fills;
            continue;
        }
        const double multiplier = main_.portfolios()->multiplier(fill.symbol);
        const double sign = fill.side == model::Side::BUY ? 1.0 : -1.0;
        if (fill.order_id.starts_with(kSettlement)) {
            trip.settled -= sign * fill.price * fill.quantity * multiplier;
            continue;
        }
        const std::optional<model::Option> option =
            portfolio != nullptr ? portfolio->option(fill.symbol) : std::nullopt;
        const double mid = option ? option->mid() : 0.0;
        if (mid <= 0.0) {
            continue; // no mark to cost it against
        }
        const double cost = sign * (fill.price - mid) * fill.quantity * multiplier;
        (opening_orders_.contains(fill.order_id) ? trip.entry_cost : trip.exit_cost) += cost;
    }
    trip.net += holding.summary.pnl - last_bar_.pnl - (fees - fees_at_last_bar_);
    trip.fees += fees - fees_at_last_bar_;
    for (const auto source : kBySource) {
        trip.attribution.*source += holding.summary.*source - last_bar_.*source;
    }
    last_bar_ = holding.summary;
    fees_at_last_bar_ = fees;
    if (holding.is_flat()) {
        trip.closed = bar.datetime;
        trip.days = trading_days(portfolio_->calendar(), trip.opened, trip.closed);
        round_trips_.push_back(std::move(trip));
        open_trip_.reset();
    }
}

auto Backtest::round_trips() const -> std::vector<RoundTrip> {
    std::vector<RoundTrip> out = round_trips_;
    if (open_trip_) {
        RoundTrip& still = out.emplace_back(*open_trip_);
        still.days = trading_days(portfolio_->calendar(), still.opened, last_bar_time_);
    }
    return out;
}

// ---- Many trials over many days

namespace {

/** The options held that the day does not quote (its file has no row of them), sorted. */
auto not_quoted(const model::StrategyHolding& holding, const adapters::LoadedDay& day)
    -> std::vector<std::string> {
    const model::Listing& listing = *day->listing();
    std::vector<std::string> missing;
    holding.for_each_open_option([&](const model::Position& position) {
        if (!listing.column(position.symbol)) {
            missing.push_back(position.symbol);
        }
    });
    std::ranges::sort(missing);
    return missing;
}

/** In date order when every file's name is a date; else as given. */
auto in_date_order(std::vector<std::string> files) -> std::vector<std::string> {
    const auto dated = [](const std::string& f) { return adapters::day_of_file(f).has_value(); };
    if (std::ranges::all_of(files, dated)) {
        std::ranges::stable_sort(files, {},
                                 [](const std::string& f) { return *adapters::day_of_file(f); });
    }
    return files;
}

/** The days in order, each loaded ahead of its turn on a thread of its own, with the chains the
 * days run so far loaded (wanted: their days to expiry), as a day's engines will most likely ask
 * for them again: up to `ahead` at a time. A day is freed on a thread of the loader's when the
 * last engine lets go of it (its contracts and chains are thousands of blocks and mappings), not
 * on that engine's: the loader outlives the engines. */
class DayLoader {
  public:
    DayLoader(std::span<const std::string> files, double risk_free_rate, std::string iv_price_mode,
              std::string cache_dir, std::size_t ahead)
        : files_(files), risk_free_rate_(risk_free_rate), iv_price_mode_(std::move(iv_price_mode)),
          cache_dir_(std::move(cache_dir)), ahead_(std::max<std::size_t>(ahead, 1)),
          freeing_([this](const std::stop_token& stop) { free_days(stop); }) {}
    DayLoader(const DayLoader&) = delete;
    DayLoader& operator=(const DayLoader&) = delete;
    ~DayLoader() {
        loading_.clear(); // a day loaded and never taken is freed too
        {
            const std::scoped_lock lock(to_free_mutex_);
            freeing_.request_stop();
        }
        to_free_ready_.notify_one();
    }

    /** The next day, and why it did not load (empty when it did). */
    std::pair<adapters::LoadedDay, std::string> next() {
        for (; queued_ < files_.size() && loading_.size() < ahead_; ++queued_) {
            loading_.push_back(std::async(std::launch::async, &DayLoader::load, this,
                                          files_[queued_], days_to_expiry_));
        }
        std::pair<adapters::LoadedDay, std::string> out{loading_.front().get(), {}};
        loading_.pop_front();
        if (!out.first->has_data()) {
            const std::string why = out.first->load_error();
            out.second = "Cannot load " + out.first->path() + (why.empty() ? "" : ": " + why);
        }
        return out;
    }

    /** The day run: the days to expiry of its chains loaded are what the days loaded next load. */
    void ran(const adapters::HistoricalData& day) {
        days_to_expiry_.clear();
        const auto today = std::chrono::floor<std::chrono::days>(day.day());
        for (const std::string& symbol : day.loaded_chains()) {
            const auto c = day.listing()->chain(symbol);
            const auto expiry =
                std::chrono::floor<std::chrono::days>(day.listing()->chains()[*c].expiry);
            days_to_expiry_.push_back(static_cast<int>((expiry - today).count()));
        }
    }

  private:
    [[nodiscard]] adapters::LoadedDay load(const std::string& file,
                                           std::vector<int> days_to_expiry) {
        adapters::LoadedDay loaded =
            adapters::load_day(file, risk_free_rate_, iv_price_mode_, cache_dir_);
        adapters::HistoricalData* at = loaded.get();
        // Its last holder hands it to free_days.
        adapters::LoadedDay day(
            at, [this, loaded = std::move(loaded)](adapters::HistoricalData* /*at*/) mutable {
                const std::scoped_lock lock(to_free_mutex_);
                to_free_.push_back(std::move(loaded));
                to_free_ready_.notify_one();
            });
        if (day->has_data() && !days_to_expiry.empty()) {
            const auto today = std::chrono::floor<std::chrono::days>(day->day());
            std::vector<std::string> chains;
            for (const model::Listing::Chain& chain : day->listing()->chains()) {
                const auto expiry = std::chrono::floor<std::chrono::days>(chain.expiry);
                const int days = static_cast<int>((expiry - today).count());
                if (std::ranges::contains(days_to_expiry, days)) {
                    chains.push_back(chain.symbol);
                }
            }
            day->load_chains(chains);
        }
        return day;
    }

    void free_days(const std::stop_token& stop) {
        std::unique_lock lock(to_free_mutex_);
        while (true) {
            to_free_ready_.wait(lock, [&] { return !to_free_.empty() || stop.stop_requested(); });
            if (to_free_.empty()) {
                return; // stopped: the engines have let go of every day
            }
            std::vector<adapters::LoadedDay> days = std::exchange(to_free_, {});
            lock.unlock();
            days.clear();
            lock.lock();
        }
    }

    std::span<const std::string> files_;
    double risk_free_rate_;
    std::string iv_price_mode_;
    std::string cache_dir_;
    std::size_t ahead_;
    std::size_t queued_ = 0; // the next day to start loading
    std::deque<std::future<adapters::LoadedDay>> loading_;
    std::vector<int> days_to_expiry_; // of the chains the day run last loaded
    std::mutex to_free_mutex_;
    std::condition_variable to_free_ready_;
    std::vector<adapters::LoadedDay> to_free_; // let go of by the engines, to free
    std::jthread freeing_;                     // last: it reads the members above
};

/** One trial over the days: which days its engine may take, the strategy added on the first, a
 * throw stopping this trial alone, its log kept, its run summed. */
class TrialRun {
  public:
    TrialRun(const BacktestTrial& trial, utilities::LogLevel log_level, adapters::Replay& replay)
        : trial_(trial), backtest_(replay) {
        backtest_.configure_execution(trial.execution);
        backtest_.keep_metrics(trial.minute_metrics);
        backtest_.restart_at(trial.restart_minute);
        backtest_.main().use_daily(trial.daily);
        if (trial.log) {
            backtest_.main().log_engine()->set_sink(
                [this](const utilities::LogRecord& record) { keep_log(record); });
            log_level = std::min(log_level, utilities::INFO);
        }
        backtest_.main().log_engine()->set_level(log_level);
    }

    [[nodiscard]] bool stopped() const { return !stopped_.empty(); }

    /** True when its engine takes the day: run_bar for each bar, then end_day. `dropped`: the
     * chains the replay's roll dropped. */
    bool begin_day(std::size_t index, const adapters::LoadedDay& day,
                   const std::string& load_error, std::span<const std::string> dropped) {
        if (stopped()) {
            return false;
        }
        bool taken = false;
        guarded(day->path(), [&] { taken = take_day(index, day, load_error, dropped); });
        return taken;
    }

    void run_bar(std::size_t i) {
        if (bars_) {
            guarded(today_->file_path, [&] { backtest_.run_bar(i); });
        }
    }

    void end_day() {
        if (!stopped()) {
            guarded(today_->file_path, [&] { close_day(); });
        }
    }

    BacktestRunSummary finish(std::chrono::system_clock::duration took) {
        summary_.duration_seconds = std::chrono::duration<double>(took).count();
        summary_.round_trips = backtest_.round_trips();
        // Its names, and the errors of its days, each once, then why it stopped.
        summary_.strategy_name = trial_.strategy_name;
        std::vector<std::string>& errors = summary_.errors;
        for (const DailyResult& daily : summary_.daily_results) {
            if (!daily.load_failed) {
                summary_.portfolio_name = portfolio_;
            }
            for (const std::string& error : daily.errors) {
                if (std::ranges::find(errors, error) == errors.end()) {
                    errors.push_back(error);
                }
            }
        }
        if (!stopped_.empty()) {
            errors.push_back(std::move(stopped_));
        }
        return std::move(summary_);
    }

  private:
    static constexpr std::size_t kDayLogCap = 500;

    /** fn(), a throw stopping this trial. */
    template <typename Fn> void guarded(const std::string& path, Fn&& fn) {
        try {
            fn();
        } catch (const std::exception& e) {
            stop(std::format("{}: {}: the run stops here", path, e.what()));
        }
    }

    void stop(std::string why) {
        stopped_ = std::move(why);
        bars_ = false;
    }

    [[nodiscard]] const model::StrategyHolding* holding() {
        return strategy_.empty() ? nullptr
                                 : backtest_.main().bookkeeping_engine()->find_holding(strategy_);
    }

    bool take_day(std::size_t index, const adapters::LoadedDay& day,
                  const std::string& load_error, std::span<const std::string> dropped) {
        const std::string& path = day->path();
        const std::optional<std::chrono::year_month_day> date = adapters::day_of_file(path);
        const model::StrategyHolding* held_before = holding();
        const bool held = held_before != nullptr && !held_before->is_flat();

        if (std::string why = !load_error.empty() ? load_error : wrong_portfolio(day);
            !why.empty()) {
            if (held) {
                stop(why + ": the holding would skip the day, so the run stops here");
            } else {
                skip_day(index, path, std::move(why));
            }
            return false;
        }
        if (held && last_day_ && date &&
            model::calendar_of(day->portfolio_name()).next_trading_day(*last_day_) != *date) {
            stop(std::format("No day between {:%F} and {:%F}, which the holding would skip: the "
                             "run stops here",
                             *last_day_, *date));
            return false;
        }
        if (held) {
            if (const std::vector<std::string> missing = not_quoted(*held_before, day);
                !missing.empty()) {
                stop(std::format("{} holds what {} does not quote ({}): the run stops here",
                                 strategy_, path, utilities::join(missing)));
                return false;
            }
        }

        const adapters::HistoricalData& data = *backtest_.use_day(day, dropped);
        if (!lines_up(data)) {
            stop(std::format("{}: the day's quotes and the portfolio's contracts do not line up: "
                             "the run stops here",
                             path));
            return false;
        }
        if (strategy_.empty() && !add_strategy(data.portfolio_name())) {
            return false;
        }
        today_ = &summary_.daily_results.emplace_back(
            DailyResult{.file_path = path, .file_index = index});
        today_date_ = date;
        bars_ = backtest_.begin_day();
        return true;
    }

    [[nodiscard]] std::string wrong_portfolio(const adapters::LoadedDay& day) const {
        const model::StrategyRecord* s = backtest_.strategy();
        if (s == nullptr || day->portfolio_name() == s->portfolio) {
            return {};
        }
        return std::format("{} is a day of {}, not of the strategy's {}", day->path(),
                           day->portfolio_name(), s->portfolio);
    }

    [[nodiscard]] bool lines_up(const adapters::HistoricalData& data) const {
        const model::Portfolio* portfolio = backtest_.main().find_portfolio(data.portfolio_name());
        return portfolio != nullptr && portfolio->shared_listing() == data.listing();
    }

    /** False (the run stopped) when refused. */
    bool add_strategy(const std::string& portfolio) {
        try {
            strategy_ = backtest_.main().add_strategy(trial_.strategy_name, portfolio,
                                                      trial_.strategy_setting);
            portfolio_ = portfolio;
            backtest_.main().risk_engine()->set_limits(trial_.risk_setting);
            return true;
        } catch (const std::exception& e) { // e.g. a setting the class does not allow
            stop(std::string("Cannot add strategy: ") + e.what());
            return false;
        }
    }

    void skip_day(std::size_t index, const std::string& path, std::string reason) {
        summary_.daily_results.push_back(DailyResult{.file_path = path,
                                                     .file_index = index,
                                                     .load_failed = true,
                                                     .errors = {std::move(reason)}});
    }

    void close_day() {
        bars_ = false;
        const std::size_t index = today_->file_index;
        *today_ = backtest_.end_day();
        today_->file_index = index;
        if (!trial_.fills) {
            today_->fills = {};
        }
        if (trial_.log) {
            take_log(*today_, trial_.log_days.empty() ||
                                  (today_date_ &&
                                   std::ranges::contains(trial_.log_days, *today_date_)));
        }
        for (const auto& [strategy, symbols] :
             backtest_.main().bookkeeping_engine()->unsettled_options()) {
            if (!symbols.empty()) {
                stop(std::format("{} holds expired options {} could not settle (its quotes end "
                                 "before the close): the run stops here",
                                 strategy, today_->file_path));
            }
        }
        last_day_ = today_date_;
        backtest_.forget_finished_orders();
        for (const auto& [name, why] : backtest_.main().strategy_engine()->errors()) {
            stop(std::format("{}: {}: the run stops here", today_->file_path, why)); // as a throw
        }
    }

    /** On the log thread. */
    void keep_log(const utilities::LogRecord& record) {
        std::scoped_lock lock(log_mutex_);
        if (day_log_.size() < kDayLogCap) {
            day_log_.push_back(record);
        } else {
            ++log_left_out_;
        }
    }

    /** The day's records into its result, or let go of when not `kept`. */
    void take_log(DailyResult& day, bool kept) {
        backtest_.main().log_engine()->flush();
        std::scoped_lock lock(log_mutex_);
        if (!kept) {
            day_log_.clear();
            log_left_out_ = 0;
            return;
        }
        day.log = std::move(day_log_);
        day_log_.clear();
        if (log_left_out_ > 0) {
            utilities::LogRecord& note = day.log.emplace_back();
            note.source = "Log";
            note.level = utilities::WARNING;
            note.at = backtest_.main().now();
            note.msg = std::format("{} more records left out: a day keeps {}", log_left_out_,
                                   kDayLogCap);
            log_left_out_ = 0;
        }
    }

    const BacktestTrial& trial_;
    // Before backtest_: its log thread writes here until it stops.
    std::mutex log_mutex_;
    std::vector<utilities::LogRecord> day_log_;
    std::size_t log_left_out_ = 0;
    Backtest backtest_;
    std::string strategy_;  // its name, once added on the first day that runs
    std::string portfolio_; // and its portfolio
    BacktestRunSummary summary_;
    DailyResult* today_ = nullptr;
    std::optional<std::chrono::year_month_day> last_day_;
    std::optional<std::chrono::year_month_day> today_date_;
    bool bars_ = false;   // begun the day: its bars run
    std::string stopped_; // why the run ended before its last day
};

/** The trials one thread runs, on one replay: each day rolled in once and each bar's quotes
 * applied once for all of them. */
class TrialGroup {
  public:
    void add(const BacktestTrial& trial, utilities::LogLevel log_level) {
        runs_.push_back(std::make_unique<TrialRun>(trial, log_level, replay_));
    }
    [[nodiscard]] bool stopped() const {
        return std::ranges::all_of(runs_, [](const auto& run) { return run->stopped(); });
    }
    TrialRun& run(std::size_t i) { return *runs_[i]; }

    void run_day(std::size_t index, const adapters::LoadedDay& day,
                 const std::string& load_error) {
        if (stopped()) {
            return;
        }
        std::vector<std::string> dropped;
        if (load_error.empty()) {
            dropped = replay_.roll(*day);
        }
        std::vector<TrialRun*> running;
        for (const auto& run : runs_) {
            if (run->begin_day(index, day, load_error, dropped)) {
                running.push_back(run.get());
            }
        }
        if (!running.empty()) {
            const std::size_t bars = day->minutes();
            for (std::size_t i = 0; i < bars; ++i) {
                replay_.apply(day, i);
                for (TrialRun* run : running) {
                    run->run_bar(i);
                }
            }
        }
        for (TrialRun* run : running) {
            run->end_day();
        }
    }

  private:
    adapters::Replay replay_; // before the runs: their engines are on it
    std::vector<std::unique_ptr<TrialRun>> runs_;
};

} // namespace

std::vector<BacktestRunSummary>
run_backtests(const std::vector<std::string>& parquet_files, std::span<const BacktestTrial> trials,
              double risk_free_rate, const std::string& iv_price_mode,
              utilities::LogLevel log_level, const std::string& cache_dir, std::size_t threads,
              std::size_t days_ahead) {
    if (parquet_files.empty()) {
        return std::vector<BacktestRunSummary>(trials.size());
    }
    // An unknown class or a bad setting would fail every day alike: reported once, up front.
    for (const BacktestTrial& trial : trials) {
        (void)strategy::StrategyRegistry::resolve_setting(trial.strategy_name,
                                                          trial.strategy_setting);
    }
    const std::vector<std::string> files = in_date_order(parquet_files);
    const auto start = std::chrono::system_clock::now();

    if (threads == 0) {
        threads = std::max(1U, std::thread::hardware_concurrency());
    }
    threads = std::min(threads, trials.size());
    DayLoader days(files, risk_free_rate, iv_price_mode, cache_dir, days_ahead); // outlives groups
    // Trial r in group r % threads.
    std::vector<std::unique_ptr<TrialGroup>> groups;
    for (std::size_t g = 0; g < threads; ++g) {
        groups.push_back(std::make_unique<TrialGroup>());
    }
    for (std::size_t r = 0; r < trials.size(); ++r) {
        groups[r % threads]->add(trials[r], log_level);
    }

    // A thread a group for the whole run; each day started on all at once and awaited (the
    // barrier orders the day's fields before and after).
    std::size_t index = 0;
    adapters::LoadedDay day;
    std::string load_error;
    bool finished = false;
    std::barrier sync(static_cast<std::ptrdiff_t>(groups.size() + 1));
    std::vector<std::jthread> workers;
    if (groups.size() > 1) {
        for (std::size_t g = 0; g < groups.size(); ++g) {
            workers.emplace_back([&, g] {
                for (sync.arrive_and_wait(); !finished; sync.arrive_and_wait()) {
                    groups[g]->run_day(index, day, load_error);
                    sync.arrive_and_wait();
                }
            });
        }
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!groups.empty() &&
            std::ranges::all_of(groups, [](const auto& group) { return group->stopped(); })) {
            break;
        }
        std::tie(day, load_error) = days.next();
        index = i;
        if (groups.size() == 1) {
            groups.front()->run_day(i, day, load_error);
        } else if (!workers.empty()) {
            sync.arrive_and_wait(); // started
            sync.arrive_and_wait(); // done
        }
        if (load_error.empty()) {
            days.ran(*day);
        }
        day.reset();
    }
    finished = true;
    if (!workers.empty()) {
        sync.arrive_and_wait(); // each sees finished and returns
    }
    workers.clear();

    const auto took = std::chrono::system_clock::now() - start;
    std::vector<BacktestRunSummary> summaries;
    summaries.reserve(trials.size());
    for (std::size_t r = 0; r < trials.size(); ++r) {
        summaries.push_back(groups[r % threads]->run(r / threads).finish(took));
    }
    return summaries;
}

} // namespace backtest
