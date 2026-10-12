/**
 * The live paths inside the process: the engine as it runs live, minus the network.
 *
 * The backtest assembly with its event loop started (the event thread, the trading state's one
 * writer, as live), a stand-in broker on a thread of its own (as IB's message thread: it takes the
 * orders the event thread sends and reports them filled, the order's update then its fill), and a
 * strategy that sends an order on each bar of a real day. This thread plays the market data
 * thread: it prices each bar's snapshot (as QuoteBook does) and puts it, and the timer tick behind
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
 *   broker → strategy   the broker reports the order filled and the fill → the strategy's on_trade:
 *                       two events through the queue, booked (orders, holdings, the storage)
 *   quote → fill        the whole loop
 *   fill → strategy     a fill put by another thread (the broker's) → the strategy's on_trade
 *   command             a task from another thread (as gRPC) → run on the event thread → its
 *                       result back on that thread
 *   ... under load      the same while another thread streams the portfolio's snapshots as fast
 *                       as it can (the event thread applies one after another)
 *   fills/s             four threads putting fills, until the event thread has booked them all
 *   write_log           what logging a line costs the thread that logs (the event thread)
 * idle: one at a time with a pause between, so the event thread and the broker sleep in between
 * (as between quotes live); back to back: the next one as soon as the last is done.
 *
 * Data: a day of SPY (data/SPY/SPY-2025-03/20250303.parquet, or OPTRA_TEST_DAY).
 */

#include "bench_util.hpp"

#include "core/engine_backtest.hpp" // core::backtest::MainEngine
#include "adapters/backtest/engine_data_historical.hpp"
#include "adapters/engine_gateway.hpp"
#include "core/engines/log.hpp"
#include "core/engines/option_strategy.hpp"
#include "model/event.hpp"
#include "model/object.hpp"
#include "core/engine_event.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"

#include <atomic>
#include <barrier>
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
class LatencyProbeStrategy final : public strategy::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override {}
    void on_stop_logic() override {}
    void on_timer_logic() override {
        if (!g_probe.on_snapshot.load(std::memory_order_acquire)) {
            decide();
        }
    }
    void on_snapshot() override {
        if (g_probe.on_snapshot.load(std::memory_order_acquire)) {
            decide();
        }
    }
    void on_trade(const model::TradeData& trade) override {
        OptionStrategyTemplate::on_trade(trade); // its log line, as every strategy's
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
        underlying_order(buy_ ? model::Direction::LONG : model::Direction::SHORT, 100.0, 1.0,
                         model::OrderType::LIMIT);
    }

    bool buy_ = false;
};

REGISTER_STRATEGY(LatencyProbeStrategy);

/** The broker, on a thread of its own as IB's message thread: woken through a mutex and a
 * condition variable (as the TWS API's signal is), it takes each order the event thread sent and
 * reports it filled, as IbGateway reports IB's messages: the order's update, then its fill. */
class StandInBroker final : public adapters::Gateway {
  public:
    explicit StandInBroker(utilities::Logger* logger)
        : Gateway(logger, "StandInBroker"),
          thread_([this](const std::stop_token& st) { run(st); }) {}

    [[nodiscard]] bool is_connected() const override { return true; }

    /** As IbGateway: an id, SUBMITTING reported at once (from the event thread), the order handed
     * to the broker's thread. */
    std::string send_order(const model::OrderRequest& req) override {
        model::OrderData order = req.create_order_data(std::to_string(++next_id_), engine_name);
        if (on_order_) {
            on_order_(order);
        }
        std::string orderid = order.orderid;
        {
            std::scoped_lock lock(mutex_);
            orders_.push_back(std::move(order));
        }
        wake_.notify_one();
        return orderid;
    }

  private:
    void run(const std::stop_token& st) {
        std::vector<model::OrderData> taken;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, st, [this] { return !orders_.empty(); })) {
                    return;
                }
                taken.swap(orders_);
            }
            for (model::OrderData& order : taken) {
                const std::int64_t k = g_probe.sample.load(std::memory_order_acquire);
                if (k >= 0) {
                    g_probe.at_broker[k] = clock_ticks();
                }
                if (!g_probe.broker_fills.load(std::memory_order_acquire)) {
                    continue;
                }
                order.status = model::Status::ALLTRADED;
                order.traded = order.volume;
                on_order_(order);
                model::TradeData fill;
                fill.symbol = order.symbol;
                fill.orderid = order.orderid;
                fill.tradeid = "F" + order.orderid;
                fill.direction = order.direction;
                fill.price = order.price;
                fill.volume = order.volume;
                on_trade_(fill);
            }
            taken.clear();
        }
    }

    int next_id_ = 0; // the event thread's
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::vector<model::OrderData> orders_;
    std::jthread thread_; // last: it starts once the rest is there, and stops first
};

/** The backtest assembly as it runs live: a day loaded, logging on (to /dev/null), the stand-in
 * broker its venue, the probe strategy started, its event loop started (no timer of its own: the
 * ticks are put). */
class LiveAssembly final : public core::backtest::MainEngine {
  public:
    LiveAssembly(const std::string& day, std::FILE* log_out) {
        log_engine()->set_sink([log_out](const utilities::LogData& r) {
            std::println(log_out, "{} | {} | {} | {}", r.time, utilities::level_to_string(r.level),
                         r.source, r.msg);
        });
        set_log_level(utilities::INFO);
        load_backtest_data(day);
        auto broker = std::make_unique<StandInBroker>(log_engine());
        broker->set_callbacks(
            [this](const model::OrderData& o) {
                post(model::EventType::Order, acquire_order(), o);
            },
            [this](const model::TradeData& t) {
                post(model::EventType::Trade, acquire_trade(), t);
            });
        gateway_ = std::move(broker); // the backtest's venue goes: run() is never called
        event_engine_ = std::make_unique<core::EventEngine>(this, 0ms);
        event_engine_->start();
        strategy_name_ =
            add_strategy("LatencyProbeStrategy", get_data_engine()->portfolio_name(), {});
        on_event_thread([this] {
            strategy::OptionStrategyTemplate* s = option_strategy_engine()->get_strategy();
            s->on_init();
            s->on_start();
        });
    }
    ~LiveAssembly() { event_engine_->stop(); }
    LiveAssembly(const LiveAssembly&) = delete;
    LiveAssembly& operator=(const LiveAssembly&) = delete;

    /** A fill, as the broker's thread reports one. */
    void report_fill(const model::TradeData& fill) {
        post(model::EventType::Trade, acquire_trade(), fill);
    }
    [[nodiscard]] const std::string& strategy_name() const { return strategy_name_; }

  private:
    std::string strategy_name_;
};

auto test_day() -> std::string {
    if (const char* day = std::getenv("OPTRA_TEST_DAY"); day != nullptr && *day != '\0') {
        return day;
    }
    return std::string(OPTRA_DATA_DIR) + "/SPY/SPY-2025-03/20250303.parquet";
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
        LiveAssembly engine(day, log_out);
        adapters::BacktestDataEngine* data = engine.get_data_engine();
        const model::PortfolioData* portfolio = engine.get_portfolio(engine.get_data_engine()->portfolio_name());
        const std::size_t bars = data->get_precomputed_snapshot_count();
        options = bars > 0 ? data->get_precomputed_snapshot(0).bid.size() : 0;
        g_probe.broker_fills = true;
        g_probe.on_snapshot = false;
        g_probe.done = -1;
        model::PortfolioSnapshot snapshot;
        for (std::size_t bar = 0; bar < bars && bar < Probe::kMax; ++bar) {
            const auto k = static_cast<std::int64_t>(bar);
            snapshot = data->get_precomputed_snapshot(bar); // as the quotes stand: not timed
            g_probe.sample.store(k, std::memory_order_release);
            g_probe.start[k] = clock_ticks();
            portfolio->price_snapshot(snapshot); // a new bar: every quote and the time moved
            engine.put_snapshot(snapshot);
            engine.put_event(model::Event(model::EventType::Timer));
            if (!wait_done(k)) {
                ++rows.skipped; // a bar with no decision (a minute repeated in the data)
                engine.on_event_thread([] {});
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
void run_quotes(const std::string& day, std::FILE* log_out, double pause_ns, int samples,
                LoopRows& rows) {
    LiveAssembly engine(day, log_out);
    adapters::BacktestDataEngine* data = engine.get_data_engine();
    const model::PortfolioData* portfolio = engine.get_portfolio(engine.get_data_engine()->portfolio_name());
    g_probe.broker_fills = true;
    g_probe.on_snapshot = true;
    g_probe.done = -1;
    model::PortfolioSnapshot book = data->get_precomputed_snapshot(data->get_precomputed_snapshot_count() / 2);
    portfolio->price_snapshot(book); // as QuoteBook's book stands after its first snapshot
    const std::size_t n = book.bid.size();
    std::vector<std::uint8_t> changed(n, 0);
    for (int i = 0; i < samples && i < static_cast<int>(Probe::kMax); ++i) {
        // Two options' quotes tick, the time and the underlying stay.
        const std::size_t a = (static_cast<std::size_t>(i) * 7) % n;
        const std::size_t b = (static_cast<std::size_t>(i) * 13 + 5) % n;
        const double tick = (i % 2 == 0) ? 0.01 : -0.01;
        for (const std::size_t c : {a, b}) {
            if (book.bid[c] > 0.05) {
                book.bid[c] += tick;
                book.ask[c] += tick;
            }
            changed[c] = 1;
        }
        const auto k = static_cast<std::int64_t>(i);
        g_probe.sample.store(k, std::memory_order_release);
        g_probe.start[k] = clock_ticks();
        portfolio->price_snapshot(book, changed, /*underlying_changed=*/false);
        engine.put_snapshot(book);
        changed[a] = changed[b] = 0;
        if (!wait_done(k)) {
            ++rows.skipped;
            engine.on_event_thread([] {});
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
auto working_order(LiveAssembly& engine) -> std::string {
    g_probe.broker_fills = false;
    model::OrderRequest req;
    req.symbol = engine.option_strategy_engine()->get_strategy()->underlying()->symbol;
    req.direction = model::Direction::LONG;
    req.type = model::OrderType::LIMIT;
    req.volume = 1'000'000;
    req.price = 100.0;
    return engine.on_event_thread([&] { return engine.send_order(engine.strategy_name(), req); });
}

auto fill_of(const std::string& orderid, const std::string& symbol, std::size_t n)
    -> model::TradeData {
    model::TradeData fill;
    fill.symbol = symbol;
    fill.orderid = orderid;
    fill.tradeid = std::format("L{}-{}", orderid, n);
    fill.direction = model::Direction::LONG;
    fill.price = 100.0;
    fill.volume = 1;
    return fill;
}

struct SideRows {
    Histogram fill, command;
};

/** Fills of the working order put from this thread (the broker's) and commands from it (as
 * gRPC's), one at a time; while `load` is set, another thread streams the day's snapshots. */
void run_side_paths(LiveAssembly& engine, double pause_ns, int samples, SideRows& rows,
                    std::size_t& first_fill) {
    const std::string orderid = working_order(engine);
    const std::string symbol = engine.option_strategy_engine()->get_strategy()->underlying()->symbol;
    for (int i = 0; i < samples; ++i) {
        const model::TradeData fill = fill_of(orderid, symbol, first_fill++);
        const auto k = static_cast<std::int64_t>(i);
        g_probe.sample.store(k, std::memory_order_release);
        g_probe.start[k] = clock_ticks();
        engine.report_fill(fill);
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
        bench::keep(engine.on_event_thread([] { return 1; }));
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
    explicit SnapshotStream(LiveAssembly& engine)
        : thread_([&engine](const std::stop_token& st) {
              adapters::BacktestDataEngine* data = engine.get_data_engine();
              const model::PortfolioData* portfolio = engine.get_portfolio(engine.get_data_engine()->portfolio_name());
              const std::size_t bars = data->get_precomputed_snapshot_count();
              model::PortfolioSnapshot snapshot;
              for (std::size_t i = 0; !st.stop_requested(); ++i) {
                  snapshot = data->get_precomputed_snapshot(i % bars);
                  portfolio->price_snapshot(snapshot);
                  engine.put_snapshot(snapshot);
              }
          }) {}

  private:
    std::jthread thread_;
};

/** Four threads put n fills each of the working order; fills/s until all are booked. */
double fills_per_second(LiveAssembly& engine, std::size_t n, std::size_t& first_fill) {
    constexpr int kThreads = 4;
    const std::string orderid = working_order(engine);
    const std::string symbol = engine.option_strategy_engine()->get_strategy()->underlying()->symbol;
    std::vector<std::vector<model::TradeData>> fills(kThreads);
    for (auto& mine : fills) {
        mine.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            mine.push_back(fill_of(orderid, symbol, first_fill++));
        }
    }
    std::barrier ready(kThreads + 1);
    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.arrive_and_wait();
            for (const model::TradeData& fill : fills[t]) {
                engine.report_fill(fill);
            }
        });
    }
    ready.arrive_and_wait();
    const std::uint64_t t0 = clock_ticks();
    threads.clear(); // joined: every fill is put
    engine.on_event_thread([] {}); // and booked
    const double ns = ticks_to_ns(clock_ticks() - t0);
    return static_cast<double>(kThreads * n) * 1e9 / ns;
}

/** What write_log costs the event thread: n lines, one at a time, a pause between. */
Histogram write_log_cost(LiveAssembly& engine, int n) {
    Histogram h;
    const std::string line(80, 'x'); // a typical line's length
    engine.on_event_thread([&] {
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
