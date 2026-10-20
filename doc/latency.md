# Latency

The engine's internal latency: from a quote inside the process to a strategy's decision, from the decision to the broker, and from a fill back to the strategy. How the threads are arranged, where the time goes, and the measurements on one machine.

The network and the venue are not measured here. Optra trades through Interactive Brokers, where quotes and order round trips take milliseconds. The market-data feed and the broker are ports, so a faster feed or a venue with direct access can replace them; the engine's own share, measured here, is the same with any venue.

## The numbers

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="bench/timeline-dark.svg">
  <img alt="Tick to order out and the backtest's throughput as key numbers; one quote through the engine to its fill at p50; every path from nanoseconds to seconds; the machine" src="bench/timeline-light.svg">
</picture>

The figure:

- **Key numbers**: tick to order out, 28 µs at p50 (p99 143 µs): from a quote, decoded, to its order at the broker's thread. Reading IB's socket and writing the order to it are not in it; the second is measured on its own below (the order reaches TWS about 93 µs after the strategy sends it). Then the backtest, from the day cache: 512 settings of one strategy over SPXW's whole chain, 2,386 trading days of minute bars (January 2016 to June 2025), in 71 s; one setting in 17 s.
- **The bar**: one tick of the underlying through the engine at p50, to the fill back at the strategy (33 µs). Each leg is split into the engine at work (tinted) and the wait for the next thread (dotted). Pricing and deciding take most of the first leg; the other two are mostly waiting.
- **The scale**: every path on one scale from nanoseconds to seconds. An order to its fill on IB's paper account takes median 3 s, p90 15 s (13 and 14 October).
- In these runs the event thread spins when idle ([Waking up](#waking-up)). Sleeping, the default, the loop to the fill is 45 µs at p50 (p99 172 µs), measured the same day.

Paths that cross threads, measured on 20 October, the event thread spinning:

| Path | p50 | p99 |
|---|---|---|
| The underlying ticks → the strategy has decided (492 options repriced) | 13.7 µs | 115 µs |
| The underlying ticks → its order at the broker's thread (tick to order out) | 27.7 µs | 143 µs |
| Two options' quotes change → the strategy has decided | 16.0 µs | 145 µs |
| The underlying ticks → the fill is back at the strategy | 32.5 µs | 162 µs |
| A fill from the broker's thread → the strategy | 12.0 µs | 24 µs |
| The same, while snapshots stream in | 3.4 µs | 25 µs |
| A command from the server → run → answered, while snapshots stream in | 16.4 µs | 44 µs |
| Logging a line, as the event thread pays for it | 0.10 µs | 0.92 µs |

Work on an order within one thread:

| Path | p50 | p99 |
|---|---|---|
| The risk check and the hand-off of an order, ten orders working | 1.9 µs | 12.5 µs |
| The same, 20,000 orders working | 1.8 µs | 2.0 µs |
| The engine's round trip: an order sent, two updates and its fill booked | 3.5 µs | 6.9 µs |
| Sending an order to IB, what it costs the event thread (18 October) | 3.6 µs | 11 µs |
| The same, until TWS has read the order off its socket (18 October) | 93 µs | 176 µs |

- Each number is the median of three runs at a fixed clock on the machine below.
- Spinning: the event thread polls for work on a core of its own instead of sleeping ([Waking up](#waking-up)); off by default.
- The broker in the first table is a stand-in on its own thread that fills every order at once: "the fill is back" is the engine's share of the loop, without the network.
- The last two rows are the real IB adapter talking to a stand-in for TWS on a local socket.

## Where the time goes

Of the 45 µs loop with the event thread sleeping (the default), each piece measured on its own:

| Piece | Time | What it is |
|---|---|---|
| Pricing the tick | 1.1 µs, on the market-data thread | only the underlying moved: every option's delta and gamma rescaled, nothing solved |
| The engine's work on the order | about 3.7 µs | risk check, hand-off to the broker, booking its updates and fill |
| Waking threads | most of the rest | the loop crosses three threads, each asleep; one wake costs about 11 µs. 78% of the event thread's CPU time is in the kernel, sleeping and being woken |

The remaining microseconds are in the hand-offs, not in the engine's code.

## The machine

| | |
|---|---|
| Computer | a ThinkPad laptop that is also the project's home server |
| CPU | AMD Ryzen 7 PRO 5850U (Zen 3), 8 cores and 16 threads; per core 32 KB of L1 data cache and 512 KB of L2, 16 MB of L3 shared |
| Clock | fixed at 1.9 GHz with boost off while measuring; otherwise 0.4 to 4.5 GHz |
| Memory | 16 GB of DDR4-3200 in one channel, 14 GB of it left to the system |
| System | Ubuntu 26.04, Linux 7.0, the stock kernel and scheduler: no isolated cores, no real-time priorities, no huge pages |
| Compiler | GCC 15.2, `-O3`, no `-march=native` |
| Timer | the CPU's time-stamp counter (`rdtsc`, constant rate, 0.527 ns a tick) |

- The machine is not dedicated: while measuring it also served a website, the paper-trading engine, the public demo and IB Gateway.
- Without the fixed clock, the same code took up to three times as long from run to run, and three runs differed by up to 40%. At 1.9 GHz they agree within about 3% at p50.
- At 4.5 GHz every number would be roughly half.

## One thread owns the trading state

| Thread | Job |
|---|---|
| the event thread | runs every strategy and books every order and fill; the only thread that changes trading state |
| the IB message thread | receives order updates and fills from IB, writes orders to the network |
| the IB market-data thread | receives quotes and turns each batch into priced snapshots |
| the log thread | writes log lines |
| the database thread | saves orders and fills in batches |
| the gRPC threads | answer the server's requests |

- Strategies, booking, positions, hedging and the risk check all run on the event thread, one event after another. No locks around orders or positions; events happen in the same order as in a backtest, where the same code runs on one thread.
- Because everything waits on the event thread, it does no network, log or database writes and no pricing; other threads do them.

## Handing work over

Three ways to reach the event thread, none with a lock:

| What | How | Why |
|---|---|---|
| Order updates, fills, the server's commands | one ordered queue: a linked list whose nodes are the events; adding one is a single atomic exchange, no allocation, no waiting; no size limit | a lost fill is a bookkeeping error; a full queue would have to drop it or stall the broker's thread |
| Snapshots | one slot per portfolio, an atomic pointer; a new snapshot replaces one not yet applied | strategies see the latest prices; a burst of quotes cannot delay a fill |
| Log lines | a fixed-size ring | if full, lines are dropped and counted |

### Waking up

- An idle event thread sleeps on a futex. A producer makes a system call only when the thread is asleep; otherwise a hand-off costs a memory fence and a load.
- A wake through the kernel takes about 11 µs (p99 36 µs) at the fixed clock. A quote or a fill crosses one thread boundary to reach the strategy; a quote to its fill crosses three.
- A condition variable, which IB's message thread waits on, takes 12 µs, so changing how that thread waits gains nothing.
- `OPTRA_EVENT_SPIN_US` makes the event thread poll for that long before sleeping; `OPTRA_EVENT_CPU` pins it to a core (the runs above: 200 µs, core 3). A hand-off then costs well under a microsecond: while snapshots stream in, a fill reaches its strategy in 3.6 µs instead of 18, and the p99 falls from 0.76 ms to 21 µs.
- Cost: one core busy all day. Off by default on this shared machine; on a dedicated server, on, with the core isolated from everything else, the numerics library's threads included.

## Nothing is copied on the way

- Events carry a pointer to their data. A snapshot, an order update or a fill is copied once by the producing thread into a pooled object and read in place after that; the object returns to its pool when handled.
- The pools are lock-free; objects are built once and reused. Strings and buffers inside keep their memory, so once warmed up, a quote or a fill allocates nothing. The one copy of a snapshot is about 3% of the market-data thread's pricing time.
- Other copies are at the edges: decoding from the network, writing to the database, formatting a log line.
- Applying a snapshot swaps buffers: the portfolio takes the snapshot's prices and Greeks and gives it its old ones, which go back to the market-data thread to be filled again. The strategy then reads a state no other thread can change.

## Pricing once, and not on the event thread

Every option needs a forward, an IV and Greeks. For 492 options this is the largest piece of work in the engine. It runs on the market-data thread before the hand-off (`pricing::price_snapshot`, in `engine/pricing/`), and only for what changed:

| Shortcut | What it does |
|---|---|
| Only moved inputs are repriced | two quotes change, two options repriced; identical to repricing the chain, checked by a test |
| Underlying-only moves solve nothing | an option's forward comes from its own quotes, so its IV, theta and vega are unchanged; delta and gamma are rescaled, two multiplications per option, equal up to rounding. Pricing this takes 1.1 µs |
| A chain's forward is kept | while its quotes and time to expiry are unchanged and the forward is still near the spot |
| Time moves in ten-second steps | the whole chain's IVs are solved again every 10 s, and every second in the last hour before an expiry |
| A smile is fitted once a minute | each chain's SVI smile (`pricing::fit_smiles`) is fitted again once a minute, chains in parallel on the pool: a same-day SPY chain takes 45 µs at p50 and 105 µs at p99 |

- The ten-second step is the 116 µs at p99 in the first row: in the benchmark one snapshot in a hundred crosses a step and reprices the whole chain on a thread pool. Live, the underlying ticks many times a second, so the share is smaller.
- A backtest takes none of these shortcuts: it prices every minute of the day exactly, in parallel, so its results do not depend on them.

## Other work, kept off the event thread or small

| Work | Cost to the event thread | How |
|---|---|---|
| Logging | 0.2 µs (was 1.7 µs) | the log thread wakes every 2 ms on its own, early only when 2,048 lines wait; the old cost was the wake per line, not formatting |
| Database | none | written by its own thread every 20 ms, or once 256 records wait. Holdings are not saved; a restart replays orders and fills |
| Sending an order to IB | 3.6 µs | the event thread checks the order, numbers it, reports it submitted and hands it over; IB's contract and order objects and the log line are built on IB's message thread, which writes the socket. It reaches TWS about 93 µs after the strategy sends it. Writing from the event thread was tried: 22 µs sooner at TWS, but 33 µs of the event thread per order, so it was not kept |
| The risk check | 2.0 µs with 10 or 20,000 orders working (was 16 µs at 20,000) | each strategy's working quantity is kept by contract as orders change, instead of walking every working order |
| A slow strategy | bounded | every callback is timed; live, over 5 ms is reported, and ten in a row stop the strategy, its positions left for the operator |

## Before and after

Measured on 13 October before and after the changes above, on 18 October after the engine was rebuilt around its ports and strategies, and on 20 October. p50, each the median of three runs at the fixed clock:

| Path | Before | 13 October | 18 October | 18 October, spinning | 20 October, spinning |
|---|---|---|---|---|---|
| The underlying ticks → decision | 69.6 µs | 26.1 µs | 15.4 µs | 14.2 µs | 13.7 µs |
| The underlying ticks → fill back at the strategy | 109.6 µs | 62.0 µs | 49.7 µs | 32.5 µs | 32.5 µs |
| A fill → the strategy, while snapshots stream in | 25.6 µs | 17.9 µs | 18.4 µs | 3.6 µs | 3.4 µs |
| Logging a line | 1.7 µs | 0.2 µs | 0.22 µs | 0.12 µs | 0.10 µs |
| An order, ten orders working | 3.1 µs | 2.7 µs | 2.0 µs | | 1.9 µs |

- The first row's p99: about the same before and on 13 October (the full repricing moved from every second, above p50, to every ten seconds, at p99); 141 µs → 116 µs from 13 to 18 October, when the portfolio's data were rewritten and applying a snapshot became the buffer swap.
- One path got worse: with the event thread sleeping and snapshots streaming in, a fill's p99 to the strategy is 0.5 to 0.8 ms on 17 and 18 October, against 75 µs on 13 October. The code of 15 October, alternated with that of 17 October (three rounds each), has the same tail (0.49 against 0.54 ms) and every p50 within 7%, so no change after 15 October caused it. Whether the code of 13 to 15 October or the machine did is not measured. Spinning, it is 25 µs in both.

## How it is measured

| Benchmark | What it runs |
|---|---|
| `bench_live_paths` | the engine as live minus the network: the real event and log threads, a stand-in broker on its own thread, a strategy that sends an order; the benchmark's main thread prices snapshots from a real SPY day as the market-data thread. Every hop is stamped with the time-stamp counter, so a sample across three threads is timed by one clock. 2,000 samples a path, 300 µs apart so threads sleep between them, as live |
| `bench_order_lifecycle` | the engine's own segments of an order, on one thread |
| `bench_ib_order` | the real IB adapter against a stand-in for TWS on a local socket, which stamps each order as it reads it |

- Old against new: both built with the same flags and run in turn, three rounds, so the machine's other load falls on both.
- Profiles: `perf record` on a build with frame pointers, at the fixed clock.
- Live, the event thread keeps histograms of four stages (a quote's arrival to its snapshot applied, a quote to an order sent, an update or fill waiting to be booked, a command waiting to run) and logs them every 15 minutes with the strategies' callback times.

```bash
sudo scripts/bench.sh clock on 1900   # fix the clock at 1.9 GHz, boost off (clock off puts it back)
scripts/bench.sh --check              # three runs, each hot path's median against the baseline
sudo scripts/bench.sh clock off
```

- The baseline (`engine/tests/bench/baseline.tsv`) holds the 13 October numbers. A path fails when its p50 is more than 15% slower or its p99 more than 50% slower.
- The code of 20 October passes on every path, the event thread sleeping: the underlying's tick to the fill back 44.5 µs (p99 172 µs) against 62.0 µs (193 µs); logging's p99 is 0.8 µs against 0.6, inside the margin. The code from before 13 October fails on five paths.

## What is left

Ranked by what each would gain; they matter most on a faster venue, where the engine's microseconds are a larger share:

| Step | What it would gain | What it needs |
|---|---|---|
| A core of its own | each hand-off from about 11 µs to a few hundred ns, most of the tail gone | isolated cores for the event and market-data threads, always polling |
| A faster full repricing | the 60–80 µs of solving 492 IVs on a pool, paid by the ten-second step, the p99 and every backtest minute | batching, vectorising, or starting each solve from a cheap estimate |
| Find where the tail comes from | the cause, before any fix | `perf sched` and `perf record` on the slowest samples; only then pre-faulted pages, huge pages or locked memory |
| Fixed-size data on the hot path | a few µs together | contracts looked up by index instead of name, orders and fills without strings, dispatch without `std::function` |
| One thread from quote to order | the remaining hand-offs | a direct market-data feed that thread reads itself; gives up the latest-snapshot slot that keeps fills ahead of quote bursts |
