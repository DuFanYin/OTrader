/**
 * The live paths inside the process: the engine as it runs live, minus the network.
 *
 * The backtest assembly with its event loop started (the event thread, the trading state's one
 * writer, as live), a stand-in broker on a thread of its own (as IB's message thread: it takes the
 * orders the event thread sends and reports them filled, the order's update then its fill), and a
 * strategy that sends an order on each bar of a real day. This thread plays the market data
 * thread: it prices each bar's snapshot (as MarketDataEngine does) and puts it, and the timer tick behind
 * it. Logging is on (INFO), written on the log thread to /dev/null.
 *
 * Paths, each hop timed with the TSC (PER_OP, ns):
 *   quote → decision    the snapshot priced and put (market data thread) → the strategy's
 *                       on_timer (event thread): pricing the whole chain, the hand-off, the
 *                       wake-up, the snapshot applied, the timer tick
 *   ... on_snapshot     a few quotes changed, priced again (only those) and put → the strategy's
 *                       on_snapshot: the path a strategy that answers each quote takes
 *   decision → broker   the strategy's order → the broker's thread has it: the pre-trade check, the
 *                       venue, its SUBMITTING report queued, the hand-off and the broker's wake-up
 *   broker → strategy   the broker reports the order filled and the fill → the strategy's on_fill:
 *                       two events through the queue, booked (orders, holdings, the storage)
 *   quote → fill        the whole loop
 *   ... the underlying  the same when only the underlying ticks (its options' quotes stay) and a
 *                       second passes every tenth snapshot, as live between option quotes
 *   fill → strategy     a fill put by another thread (the broker's) → the strategy's on_fill
 *   command             a task from another thread (as gRPC) → run on the event thread → its
 *                       result back on that thread
 *   ... under load      the same while another thread streams the portfolio's snapshots as fast
 *                       as it can (the event thread applies one after another)
 *   fills/s             four threads putting fills, until the event thread has booked them all
 *   write_log           what logging a line costs the thread that logs (the event thread)
 * idle: one at a time with a pause between, so the event thread and the broker sleep in between
 * (as between quotes live); back to back: the next one as soon as the last is done.
 *
 * Data: a synthetic day of SPY (data/synthetic/SPY/SPY-2025-03/20250303.parquet, or
 * OPTRA_TEST_DAY).
 */

#include "tests/bench/bench_util.hpp"

#include "adapters/market_data/historical_data.hpp"
#include "adapters/database/sqlite_database.hpp"
#include "core/gateway/gateway.hpp"
#include "core/main.hpp"
#include "core/log/log_engine.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "model/event.hpp"
#include "model/object.hpp"
#include "core/event_loop.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"

#include <atomic>
#include <barrier>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <print>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using bench::clock_ticks;
using bench::Histogram;
using bench::ticks_to_ns;
using namespace std::chrono_literals;

namespace {

/** Where each sample's hops were (TSC ticks): each thread writes its own column, read once the
 * sample is done. */
struct Probe {
    static constexpr std::size_t kMax = 1 << 17;
    std::vector<std::uint64_t> start = std::vector<std::uint64_t>(kMax);
    std::vector<std::uint64_t> decided = std::vector<std::uint64_t>(kMax);
    std::vector<std::uint64_t> at_broker = std::vector<std::uint64_t>(kMax);
    std::vector<std::uint64_t> filled = std::vector<std::uint64_t>(kMax);
    std::atomic<std::int64_t> sample{-1}; // the sample under way, set before its first hop
    std::atomic<std::int64_t> done{-1};   // the last sample whose fill reached the strategy
    std::atomic<bool> broker_fills{true}; // the broker reports each order filled
    std::atomic<bool> on_snapshot{false}; // the strategy decides on a snapshot, not on a bar
};

Probe g_probe;

/** On each bar, an order for one share, buying and selling in turn (the holding stays flat); a
 * fill ends the sample under way. */
class LatencyProbeStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {}
    void on_stop() override {}
    void on_timer() override {
        if (!g_probe.on_snapshot.load(std::memory_order_acquire)) {
            decide();
        }
    }
    void on_snapshot() override {
        if (g_probe.on_snapshot.load(std::memory_order_acquire)) {
            decide();
        }
    }
    void on_fill(const model::Fill& fill) override {
        Strategy::on_fill(fill); // its log line, as every strategy's
        const std::int64_t k = g_probe.sample.load(std::memory_order_acquire);
        if (k >= 0) {
            g_probe.filled[k] = clock_ticks();
            g_probe.done.store(k, std::memory_order_release);
        }
    }

  private:
    void decide() {
        const std::int64_t k = g_probe.sample.load(std::memory_order_acquire);
        if (k < 0) {
            return;
        }
        g_probe.decided[k] = clock_ticks();
        buy_ = !buy_;
        const model::Side side = buy_ ? model::Side::BUY : model::Side::SELL;
        send_order(std::array{leg(underlying()->symbol(), side)},
                   side == model::Side::BUY ? 100.0 : -100.0, 1.0, model::OrderType::LIMIT);
    }

    bool buy_ = false;
};

REGISTER_STRATEGY(LatencyProbeStrategy);

/** The broker, on a thread of its own as IB's message thread: woken through a mutex and a
 * condition variable (as the TWS API's signal is), it takes each order the event thread sent and
 * reports it filled, as IbGateway reports IB's messages: the order's update, then its fill. */
class StandInBroker final : public core::Gateway {
  public:
    explicit StandInBroker(utilities::Logger* logger)
        : Gateway(logger, "StandInBroker"),
          thread_([this](const std::stop_token& st) { run(st); }) {}

    [[nodiscard]] bool is_connected() const override { return true; }

    /** As IbGateway: an id, SUBMITTING reported at once (from the event thread), the order handed
     * to the broker's thread. */
    std::string send_order(const model::OrderRequest& req) override {
        model::Order order = req.create_order(std::to_string(++next_id_));
        if (on_order_) {
            on_order_(order);
        }
        std::string order_id = order.order_id;
        {
            std::scoped_lock lock(mutex_);
            orders_.push_back(std::move(order));
        }
        wake_.notify_one();
        return order_id;
    }

  private:
    void run(const std::stop_token& st) {
        std::vector<model::Order> taken;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, st, [this] { return !orders_.empty(); })) {
                    return;
                }
                taken.swap(orders_);
            }
            for (model::Order& order : taken) {
                const std::int64_t k = g_probe.sample.load(std::memory_order_acquire);
                if (k >= 0) {
                    g_probe.at_broker[k] = clock_ticks();
                }
                if (!g_probe.broker_fills.load(std::memory_order_acquire)) {
                    continue;
                }
                order.status = model::Status::FILLED;
                order.filled = order.quantity;
                on_order_(order);
                model::Fill fill;
                fill.symbol = order.symbol;
                fill.order_id = order.order_id;
                fill.fill_id = "F" + order.order_id;
                fill.side = order.legs.front().side;
                fill.price = std::abs(order.price);
                fill.quantity = order.quantity;
                on_trade_(fill);
            }
            taken.clear();
        }
    }

    int next_id_ = 0; // the event thread's
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<model::Order> orders_;
    std::jthread thread_; // last: it starts once the rest is there, and stops first
};

/** core::Main as it runs live, over a backtest's day: the stand-in broker its venue, the storage
 * in memory, the data's clock, logging on (to log_out), the day loaded, the probe strategy
 * started, its event loop started (no timer of its own: the ticks are put). */
class LiveAssembly {
  public:
    LiveAssembly(const std::string& day, std::FILE* log_out)
        : main_(core::Main::Ports{
                    .database = [](utilities::Logger* logger) -> std::unique_ptr<core::Database> {
                        return std::make_unique<adapters::SqliteDatabase>(logger, ":memory:");
                    },
                    .gateway = [](utilities::Logger* logger, const model::Portfolios&)
                        -> std::unique_ptr<core::Gateway> {
                        return std::make_unique<StandInBroker>(logger);
                    }},
                utilities::DISABLED),
          data_(main_.log_engine()) {
        main_.log_engine()->set_sink([log_out](const utilities::LogRecord& r) {
            std::println(log_out, "{} | {} | {} | {}", r.time, utilities::level_to_string(r.level),
                         r.source, r.msg);
        });
        main_.log_engine()->set_level(utilities::INFO);
        data_.load(day);
        data_.load_all_chains();
        data_.build_portfolio(*main_.portfolios());
        core::EventLoop& events = *main_.event_loop();
        events.set_interval(0ms);
        events.configure_from_environment(); // as live: OPTRA_EVENT_SPIN_US, OPTRA_EVENT_CPU
        events.start();
        strategy_name_ = main_.add_strategy("LatencyProbeStrategy", data_.portfolio_name(), {});
        main_.init_strategy(strategy_name_);
        main_.start_strategy(strategy_name_);
    }
    ~LiveAssembly() { main_.close(); } // the event thread done before the day goes
    LiveAssembly(const LiveAssembly&) = delete;
    LiveAssembly& operator=(const LiveAssembly&) = delete;

    core::Main& main() { return main_; }
    adapters::HistoricalData* historical_data() { return &data_; }
    /** A fill, as the broker's thread reports one. */
    void report_fill(const model::Fill& fill) {
        main_.event_loop()->put_fill(fill);
    }
    [[nodiscard]] const std::string& strategy_name() const { return strategy_name_; }

  private:
    core::Main main_;
    adapters::HistoricalData data_; // into main_'s portfolios
    std::string strategy_name_;
};

auto test_day() -> std::string {
    if (const char* day = std::getenv("OPTRA_TEST_DAY"); day != nullptr && *day != '\0') {
        return day;
    }
    return std::string(OPTRA_DATA_DIR) + "/synthetic/SPY/SPY-2025-03/20250303.parquet";
}

/** Spin until the sample's fill reached the strategy; false after a second without it. */
bool wait_done(std::int64_t k) {
    const auto give_up = std::chrono::steady_clock::now() + 1s;
    for (int spins = 0; g_probe.done.load(std::memory_order_acquire) != k; ++spins) {
        bench::cpu_relax();
        if ((spins & 0xfff) == 0 && std::chrono::steady_clock::now() > give_up) {
            return false;
        }
    }
    return true;
}

double ns_between(std::uint64_t from, std::uint64_t to) {
    return to > from ? ticks_to_ns(to - from) : 0.0;
}

struct LoopRows {
    Histogram decide, to_broker, back, total;
    int skipped = 0;
};

constexpr std::size_t kWarmBars = 10; // each pass's first bars: caches and pools warming

/** Each bar of the day: its snapshot priced and put, and a timer tick, from this thread (the market
 * data's), then wait for the strategy's fill; pause_ns between bars. A fresh engine each pass. */
void run_day(const std::string& day, std::FILE* log_out, double pause_ns, int passes,
             LoopRows& rows, std::size_t& options) {
    for (int pass = 0; pass < passes; ++pass) {
        LiveAssembly assembly(day, log_out);
        core::Main& engine = assembly.main();
        adapters::HistoricalData* data = assembly.historical_data();
        const model::Portfolio* portfolio =
            engine.find_portfolio(assembly.historical_data()->portfolio_name());
        const std::size_t bars = data->minutes();
        options = data->listing()->size();
        g_probe.broker_fills = true;
        g_probe.on_snapshot = false;
        g_probe.done = -1;
        model::PortfolioSnapshot snapshot(data->listing(), data->portfolio_name());
        for (std::size_t bar = 0; bar < bars && bar < Probe::kMax; ++bar) {
            const auto k = static_cast<std::int64_t>(bar);
            data->fill(bar, snapshot); // as the quotes stand: not timed
            g_probe.sample.store(k, std::memory_order_release);
            g_probe.start[k] = clock_ticks();
            portfolio->price_snapshot(snapshot); // a new bar: every quote and the time moved
            engine.event_loop()->put_snapshot(snapshot);
            engine.event_loop()->put_event(model::Event(model::EventType::Timer));
            if (!wait_done(k)) {
                ++rows.skipped; // a bar with no decision (a minute repeated in the data)
                engine.event_loop()->on_event_thread([] {});
                continue;
            }
            if (bar >= kWarmBars) {
                rows.decide.record_ns(ns_between(g_probe.start[k], g_probe.decided[k]));
                rows.to_broker.record_ns(ns_between(g_probe.decided[k], g_probe.at_broker[k]));
                rows.back.record_ns(ns_between(g_probe.at_broker[k], g_probe.filled[k]));
                rows.total.record_ns(ns_between(g_probe.start[k], g_probe.filled[k]));
            }
            if (pause_ns > 0) {
                bench::spin_for_ns(pause_ns);
            }
        }
        g_probe.sample = -1;
    }
}

/** A strategy that answers each quote: a few options' quotes change on one bar, the snapshot is
 * priced again (only those) and put, the strategy decides in on_snapshot; samples of them. */
/** What changes between one snapshot and the next in run_quotes. */
enum class QuoteChange : std::uint8_t {
    TwoOptions, // two options' quotes tick; the time and the underlying stay
    Underlying, // the underlying ticks and the options' quotes stay; every tenth, a second passes
};

void run_quotes(const std::string& day, std::FILE* log_out, double pause_ns, int samples,
                LoopRows& rows, QuoteChange change = QuoteChange::TwoOptions) {
    LiveAssembly assembly(day, log_out);
    core::Main& engine = assembly.main();
    adapters::HistoricalData* data = assembly.historical_data();
    const model::Portfolio* portfolio =
        engine.find_portfolio(assembly.historical_data()->portfolio_name());
    g_probe.broker_fills = true;
    g_probe.on_snapshot = true;
    g_probe.done = -1;
    model::PortfolioSnapshot book(data->listing(), data->portfolio_name());
    data->fill(data->minutes() / 2, book);
    portfolio->price_snapshot(book); // as MarketDataEngine's book stands after its first snapshot
    const std::size_t n = book.bid.size();
    std::vector<std::uint8_t> changed(n, 0);
    for (int i = 0; i < samples && i < static_cast<int>(Probe::kMax); ++i) {
        const std::size_t a = (static_cast<std::size_t>(i) * 7) % n;
        const std::size_t b = (static_cast<std::size_t>(i) * 13 + 5) % n;
        const double tick = (i % 2 == 0) ? 0.01 : -0.01;
        const bool underlying = change == QuoteChange::Underlying;
        if (underlying) {
            book.underlying_bid += tick;
            book.underlying_ask += tick;
            book.underlying_mark += tick;
            if (i % 10 == 9) {
                book.datetime += std::chrono::seconds(1); // live: a snapshot every 100 ms
            }
        } else {
            for (const std::size_t c : {a, b}) {
                if (book.bid[c] > 0.05) {
                    book.values(&model::Frame::bid)[c] += tick;
                    book.values(&model::Frame::ask)[c] += tick;
                }
                changed[c] = 1;
            }
        }
        const auto k = static_cast<std::int64_t>(i);
        g_probe.sample.store(k, std::memory_order_release);
        g_probe.start[k] = clock_ticks();
        book.quoted_tsc = g_probe.start[k]; // as MarketDataEngine stamps it: the engine measures from it
        portfolio->price_snapshot(book, changed, /*underlying_changed=*/underlying,
                                  model::Portfolio::kLiveTimeStep); // as MarketDataEngine prices
        engine.event_loop()->put_snapshot(book);
        changed[a] = changed[b] = 0;
        if (!wait_done(k)) {
            ++rows.skipped;
            engine.event_loop()->on_event_thread([] {});
            continue;
        }
        if (i >= static_cast<int>(kWarmBars)) {
            rows.decide.record_ns(ns_between(g_probe.start[k], g_probe.decided[k]));
            rows.to_broker.record_ns(ns_between(g_probe.decided[k], g_probe.at_broker[k]));
            rows.back.record_ns(ns_between(g_probe.at_broker[k], g_probe.filled[k]));
            rows.total.record_ns(ns_between(g_probe.start[k], g_probe.filled[k]));
        }
        if (pause_ns > 0) {
            bench::spin_for_ns(pause_ns);
        }
    }
    g_probe.sample = -1;
    g_probe.on_snapshot = false;
    // The engine's own measurement of the same samples (EventLoop's histograms), to check
    // against the probes above.
    std::printf("%s\n", engine.event_loop()->latency_report().c_str());
}

void print_loop(const char* title, const LoopRows& rows) {
    bench::print_header(title);
    bench::print_row("quote -> decision", rows.decide);
    bench::print_row("decision -> broker", rows.to_broker);
    bench::print_row("broker -> strategy (order + fill)", rows.back);
    bench::print_row("quote -> fill (the whole loop)", rows.total);
    if (rows.skipped > 0) {
        std::printf("(%d bars without a decision skipped)\n", rows.skipped);
    }
}

/** The strategy's order, left working (the broker does not fill it): the fills put below are its,
 * so each reaches the strategy. */
auto working_order(LiveAssembly& assembly) -> std::string {
    core::Main& engine = assembly.main();
    g_probe.broker_fills = false;
    // An order the fills can keep coming against: above the default risk limits (10,000 shares an
    // order, 100,000 held), which would refuse it.
    engine.set_risk_settings({{"max_share_order", 100'000'000}, {"max_share_position", 1'000'000'000}});
    model::OrderRequest req;
    req.symbol =
        engine.strategy_engine()->find_strategy(assembly.strategy_name())->underlying()->symbol();
    req.legs = {{.ratio = 1, .side = model::Side::BUY, .symbol = req.symbol}};
    req.type = model::OrderType::LIMIT;
    req.quantity = 1'000'000;
    req.price = 100.0;
    return engine.event_loop()->on_event_thread(
        [&] {
            return engine.send_order(
                *engine.strategy_engine()->find_record(assembly.strategy_name()), req);
        });
}

auto fill_of(const std::string& order_id, const std::string& symbol, std::size_t n)
    -> model::Fill {
    model::Fill fill;
    fill.symbol = symbol;
    fill.order_id = order_id;
    fill.fill_id = std::format("L{}-{}", order_id, n);
    fill.side = model::Side::BUY;
    fill.price = 100.0;
    fill.quantity = 1;
    return fill;
}

struct SideRows {
    Histogram fill, command;
};

/** Fills of the working order put from this thread (the broker's) and commands from it (as
 * gRPC's), one at a time; while `load` is set, another thread streams the day's snapshots. */
void run_side_paths(LiveAssembly& assembly, double pause_ns, int samples, SideRows& rows,
                    std::size_t& first_fill) {
    core::Main& engine = assembly.main();
    const std::string order_id = working_order(assembly);
    const std::string symbol =
        engine.strategy_engine()->find_strategy(assembly.strategy_name())->underlying()->symbol();
    for (int i = 0; i < samples; ++i) {
        const model::Fill fill = fill_of(order_id, symbol, first_fill++);
        const auto k = static_cast<std::int64_t>(i);
        g_probe.sample.store(k, std::memory_order_release);
        g_probe.start[k] = clock_ticks();
        assembly.report_fill(fill);
        if (!wait_done(k)) {
            std::printf("a fill did not reach the strategy\n");
            std::exit(1);
        }
        if (i >= static_cast<int>(kWarmBars)) {
            rows.fill.record_ns(ns_between(g_probe.start[k], g_probe.filled[k]));
        }
        if (pause_ns > 0) {
            bench::spin_for_ns(pause_ns);
        }
        const std::uint64_t t0 = clock_ticks();
        bench::keep(engine.event_loop()->on_event_thread([] { return 1; }));
        const std::uint64_t t1 = clock_ticks();
        if (i >= static_cast<int>(kWarmBars)) {
            rows.command.record_ns(ns_between(t0, t1));
        }
        if (pause_ns > 0) {
            bench::spin_for_ns(pause_ns);
        }
    }
    g_probe.sample = -1;
}

/** Another thread pricing and putting the day's snapshots (as the market data thread does), one
 * after another, until stopped. */
class SnapshotStream {
  public:
    explicit SnapshotStream(LiveAssembly& assembly)
        : thread_([&assembly](const std::stop_token& st) {
              core::Main& engine = assembly.main();
              adapters::HistoricalData* data = assembly.historical_data();
              const model::Portfolio* portfolio =
                  engine.find_portfolio(assembly.historical_data()->portfolio_name());
              const std::size_t bars = data->minutes();
              model::PortfolioSnapshot snapshot(data->listing(), data->portfolio_name());
              for (std::size_t i = 0; !st.stop_requested(); ++i) {
                  data->fill(i % bars, snapshot);
                  portfolio->price_snapshot(snapshot);
                  engine.event_loop()->put_snapshot(snapshot);
              }
          }) {}

  private:
    std::jthread thread_;
};

/** Four threads put n fills each of the working order; fills/s until all are booked. */
double fills_per_second(LiveAssembly& assembly, std::size_t n, std::size_t& first_fill) {
    constexpr int kThreads = 4;
    core::Main& engine = assembly.main();
    const std::string order_id = working_order(assembly);
    const std::string symbol =
        engine.strategy_engine()->find_strategy(assembly.strategy_name())->underlying()->symbol();
    std::vector<std::vector<model::Fill>> fills(kThreads);
    for (auto& mine : fills) {
        mine.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            mine.push_back(fill_of(order_id, symbol, first_fill++));
        }
    }
    std::barrier ready(kThreads + 1);
    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.arrive_and_wait();
            for (const model::Fill& fill : fills[t]) {
                assembly.report_fill(fill);
            }
        });
    }
    ready.arrive_and_wait();
    const std::uint64_t t0 = clock_ticks();
    threads.clear(); // joined: every fill is put
    engine.event_loop()->on_event_thread([] {}); // and booked
    const double ns = ticks_to_ns(clock_ticks() - t0);
    return static_cast<double>(kThreads * n) * 1e9 / ns;
}

/** What write_log costs the event thread: n lines, one at a time, a pause between. */
Histogram write_log_cost(LiveAssembly& assembly, int n) {
    core::Main& engine = assembly.main();
    Histogram h;
    const std::string line(80, 'x'); // a typical line's length
    engine.event_loop()->on_event_thread([&] {
        for (int i = 0; i < n; ++i) {
            const std::uint64_t t0 = clock_ticks();
            engine.write_log(line, utilities::INFO);
            h.record_ns(ticks_to_ns(clock_ticks() - t0));
            bench::spin_for_ns(5'000); // the log thread keeps up
        }
    });
    return h;
}

} // namespace

int main() {
    const std::string day = test_day();
    if (!std::filesystem::exists(day)) {
        std::fprintf(stderr, "no day to replay: %s (set OPTRA_TEST_DAY)\n", day.c_str());
        return 1;
    }
    std::FILE* log_out = std::fopen("/dev/null", "w");
    bench::print_env();
    std::printf("day           : %s\n", day.c_str());

    std::size_t options = 0;
    LoopRows idle;
    run_day(day, log_out, /*pause_ns=*/300'000, /*passes=*/3, idle, options);
    std::printf("portfolio     : %zu options a snapshot\n", options);
    print_loop("quote -> decision -> broker -> fill back, idle (300 us between bars) (ns)", idle);

    LoopRows busy;
    run_day(day, log_out, /*pause_ns=*/0, /*passes=*/3, busy, options);
    print_loop("the same, back to back (ns)", busy);

    LoopRows quotes;
    run_quotes(day, log_out, /*pause_ns=*/300'000, /*samples=*/2000, quotes);
    print_loop("two quotes change -> on_snapshot -> broker -> fill back, idle (ns)", quotes);

    LoopRows ticks;
    run_quotes(day, log_out, /*pause_ns=*/300'000, /*samples=*/2000, ticks, QuoteChange::Underlying);
    print_loop("the underlying ticks -> on_snapshot -> broker -> fill back, idle (ns)", ticks);

    std::size_t first_fill = 0;
    {
        LiveAssembly engine(day, log_out);
        SideRows quiet;
        run_side_paths(engine, /*pause_ns=*/300'000, /*samples=*/3000, quiet, first_fill);
        bench::print_header("fills and commands from other threads, idle (300 us between) (ns)");
        bench::print_row("fill -> strategy", quiet.fill);
        bench::print_row("command round trip", quiet.command);

        SideRows quick;
        run_side_paths(engine, /*pause_ns=*/0, /*samples=*/20'000, quick, first_fill);
        bench::print_header("the same, back to back (ns)");
        bench::print_row("fill -> strategy", quick.fill);
        bench::print_row("command round trip", quick.command);

        SideRows loaded;
        {
            SnapshotStream stream(engine);
            run_side_paths(engine, /*pause_ns=*/50'000, /*samples=*/3000, loaded, first_fill);
        }
        bench::print_header("the same while another thread streams snapshots (ns)");
        bench::print_row("fill -> strategy", loaded.fill);
        bench::print_row("command round trip", loaded.command);

        bench::print_header("write_log on the event thread (ns)");
        bench::print_row("write_log (one 80-char line)", write_log_cost(engine, 5000));
    }
    {
        LiveAssembly engine(day, log_out);
        const double rate = fills_per_second(engine, 50'000, first_fill);
        std::printf("\n== throughput ==\n");
        std::printf("fills booked (4 threads putting, 200000 fills): %.0f fills/s (%.0f ns a fill)\n",
                    rate, 1e9 / rate);
    }
    std::fclose(log_out);
    return 0;
}
