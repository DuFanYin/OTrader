<div align="center">

# Optra

**一套期权交易系统：研究策略、回测、然后实盘交易。**

[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](#快速上手)
[![Broker: IBKR](https://img.shields.io/badge/live-Interactive%20Brokers-D81222)](#在-interactive-brokers-上交易)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)](#许可)

[在线 demo](https://dufanyin.dev/optra-demo/) · [架构图](https://dufanyin.dev/optra-architecture/) · [文档](#架构和文档) · [English](../README.md)

</div>

Optra 以期权链为中心。报价一来就算出隐含波动率和 Greeks，多腿组合作为一笔订单下单，持有到期的期权在收盘时自动结算。

它覆盖一个策略的完整生命周期：用研究工具琢磨想法，写成策略，回测，然后交易。每一步用的都是同一份策略代码。

克隆下来就能在模拟市场上跑，不需要券商账户，也不需要数据订阅。想先看看，可以打开 **[在线 demo](https://dufanyin.dev/optra-demo/)**：同一套系统跑在模拟账户上。

<p align="center"><img width="760" alt="Optra 的 web 应用，交易页：当天概况、在模拟账户上运行的策略、选中策略的持仓、仓位和成交、风控限额和压力测试" src="images/app-home.png"></p>

## 快速上手

**Docker**

```bash
git clone https://github.com/DuFanYin/optra.git && cd optra
docker compose up --build
```

**Ubuntu 26.04**

```bash
sudo apt install g++ cmake make ccache lld libarrow-dev libparquet-dev libprotobuf-dev protobuf-compiler \
  libgrpc++-dev protobuf-compiler-grpc libabsl-dev nlohmann-json3-dev libsqlite3-dev libgtest-dev 7zip nodejs npm
curl -LsSf https://astral.sh/uv/install.sh | sh

git clone https://github.com/DuFanYin/optra.git && cd optra
./build.sh r              # 引擎、回测程序和策略，编译到 engine/build
./scripts/dev.sh sim      # 在模拟市场上跑整个系统
```

打开 http://localhost:5173。第一次启动会往 `data/backtest/SPY` 写入三个月的合成 SPY 期权数据用来回测，再写入今天的一天用作市场。然后启动四个进程：

- **引擎**（`entry_simulate`）：就是实盘引擎，只是把 Interactive Brokers 换成了模拟交易所，按真实时钟的节奏回放今天的合成数据；
- **服务端**：页面只和它打交道，它通过 gRPC 指挥引擎；
- 一个**研究 worker**：跑排队的回测和实验；
- **页面**。

Ctrl+C 一起停掉。模拟账户保存在 `data/sim/`，重启后还在。

## 可以试什么

**交易。** 在 Home 的 *New strategy* 里选一个策略类和 SPY 组合，填好参数。然后依次按 *Add strategy*、*Init*、*Start*。`ComboStrategy` 用默认参数是买一张平值看涨期权；加一条看跌腿就是跨式。随着这一天回放，持仓和成交会更新。*Halt all* 停掉一切，*Limits* 显示每笔订单都要过的风控。

**回测。** 在 Backtest 里选策略、参数和日期范围，按 *Run backtest*。策略按分钟一路跑过这些天，持仓带到下一天。订单按买卖价成交，也可以设在价差里的任意位置，计入手续费。持有到期的期权在收盘时结算。P&L 按天给出，并拆成 delta、gamma、theta、vega。

<p align="center"><img width="760" alt="一次回测：卖出跨式在合成 SPY 数据上跑三个月，汇总、P&L 和持有的 Greeks" src="images/app-backtest.png"></p>

命令行也一样，不需要服务端或数据库：

```bash
./engine/build/entry_backtest --list-strategies          # 所有策略、参数和取值范围
./engine/build/entry_backtest --files data/backtest/SPY/SPY-*/*.parquet ComboStrategy leg1_ratio=1 leg2_ratio=1 leg2_right=1
```

输出每天的 P&L、往返交易和钱从哪里来，按 IBKR 的真实收费。加 `--json` 给出全部数据列。

**做实验。** 在 Experiments 里让一个策略跑一组参数。每次尝试在跑之前就登记，并和同一个问题上的其他尝试一起计数。所以试得越多，最好的那组就要越好才算数：deflated Sharpe、回测过拟合概率和 walk-forward 对照全部尝试来判断。

<p align="center"><img width="760" alt="一个实验：卖出跨式的十二组参数、每组的结果、对照全部尝试的 deflated Sharpe 和过拟合概率、walk-forward、最佳一组的拆解" src="images/app-experiments.png"></p>

**研究一笔交易。** Lab 里是各种计算器：用多种方法给期权定价并给出全部 Greeks、多腿组合、压力测试、对冲方法对比，还有一个在任意一分钟的期权链上给结构排序的筛选器。完整列表见 [research.md](research.md)。

**交给 agent 来驱动。** Tools 按研究流程的顺序列出 agent 能调用的每个工具，每个都是带 schema 的 HTTP 接口。批准、安装、改动会交易的东西，始终由人来做。

## 编写自己的策略

策略是一个带几个回调的 C++ 类，只装它的想法：什么时候交易、交易什么、交易多少。其余的由基类负责：交易日、选期权链和行权价、持仓，以及单腿和组合的下单。

```cpp
class MyStraddle final : public Strategy {
  public:
    struct Setting {  // 在 web 应用里显示和设置，实验时会变化
        Param<int> min_dte{"min_dte", 0, 0, 30};
        Param<double> take_profit{"take_profit", 0.5, 0.0, 5.0};
    };
    using Strategy::Strategy;

    void on_day() override;    // 每个交易日：选期权链（select_chain）
    void on_timer() override;  // 每根 bar：读报价和 Greeks，下单
    void on_stop() override;   // 平掉剩下的

  private:
    const Setting setting_ = read_setting<Setting>();  // setting_.min_dte，带类型
};
REGISTER_STRATEGY(MyStraddle);
```

放进 `strategy/research/`，运行 `./build.sh s`。每个文件编成一个单独的库，不用重编引擎，也不用重启任何东西。也可以把整个文件贴进 Home 侧栏的 *Your own strategy*，按 *Build*。

回测马上就能用新策略。会交易的引擎只有在人把它装到对应账户之后才会加载（*Install for paper*），所以 agent 写的代码只能经过人才能进入账户。delta 对冲只要一个调用（`register_hedging`）。

简单的组合不用写代码：`ComboStrategy` 把各条腿当作规则来设（看涨或看跌、比例、相对远期的行权价、到期天数），加上进出场时间、止盈、止损和可选的对冲。`strategy/demo` 里还有所有策略都要与之比较的对照组 RandomBuyStrategy、SurfaceStrategy、SyntheticMomentumStrategy，以及一个可以当示例读的 0DTE 买方策略。它们都不是已经证明有效的策略。

## 自己的数据

回测程序读的是每个标的、每个交易日一个 Parquet 文件。每一行是某一时刻一个期权的最优买卖价，旁边带着标的的报价。任何数据商的数据都可以离线转换成这个样子：格式见 [data.md](data.md)，`scripts/data/example_to_parquet.py` 是一个可以照着改的小例子。

## 在 Interactive Brokers 上交易

同一个引擎和同样的策略通过 TWS API 交易 Interactive Brokers 账户。编译时带上 IB 网关，运行 IB Gateway 或 TWS，在 `.env` 里写上 `IB_ACCOUNT=DU...`（模拟账户），不带 `sim` 启动：

```bash
./build.sh r g            # 带上 IB 网关（第一次会下载 TWS API）
./scripts/dev.sh          # entry_live：行情和订单都走 IB
```

- 每笔订单都要过事前风控，一个按钮就能停掉所有交易。
- 重启后从停下的地方接着来，先和券商核对持仓。
- 按日常使用部署时（[server.md](server.md)），每五分钟一次健康检查；收盘后有一份报告，把当天的成交和同一天的回测逐笔对比。

目前 Optra 只在模拟账户上交易。除非这台机器的管理员允许，它拒绝实盘账户。

## 架构和文档

**[交互式架构图](https://dufanyin.dev/optra-architecture/)** 展示了 Optra 的每个部分，点开就能看到它做什么。

引擎是一个 C++ 程序。回测、模拟市场和实盘跑的是同一个引擎，只是接上的连接不同：录下的数据加模拟成交、回放的一天加模拟交易所，或者 Interactive Brokers。web 应用和服务端只通过 gRPC 和引擎打交道。

记账是确定性的。一个线程拥有全部交易状态，所以持仓永远不取决于券商回报成交的先后顺序。更多见下面的文档：

- [Optra 是怎么组合起来的](architecture.md)：有哪些程序、为什么分开、一条报价在引擎里走过的路、从一个想法到模拟账户
- [引擎](engine.md)：事件和记账、实盘进程、重启和到期、回测、模拟市场、IB 回报了什么、编译和测试
- [延迟](latency.md)：在一台具名机器上测的引擎内部延迟：线程、不加锁不复制的交接、只给变了的定价
- [服务端和 web 应用](server.md)：服务端做什么、各个页面、日常怎么运行、公开 demo
- [研究](research.md)：研究页面、Lab 的计算器、方法、行情数据、给 agent 的工具
- [回测数据](data.md)：格式、转换数据商的数据、回放模拟盘的日子、合成数据
- [高效的期权策略回测](backtesting.md)（英文短文）：一天只读一次、定价一次，所有试验共用
- 给编程 agent：[AGENTS.md](../AGENTS.md)

引擎的延迟和回测的吞吐量，实测（[latency.md](latency.md)）：

<p align="center"><img width="760" alt="延迟：报价到订单发出和回测吞吐量的关键数字；一个报价经过引擎到成交的 p50；从纳秒到秒的每条路径" src="bench/timeline-light.svg"></p>

## 项目状态

引擎 C++ 约 30,000 行，410 个测试（GoogleTest：并发、关停，其中 8 个连 IB 模拟账户；`scripts/test_sanitize.sh` 在 ASan 和 TSan 下跑它们），服务端另有 165 个。目前只在模拟账户上交易。下一步：让一个研究结果通过留出区、上模拟盘，期权之外加上股票，更低的延迟。

## 许可

Apache License 2.0（[LICENSE](../LICENSE)、[NOTICE](../NOTICE)）。构建时下载的第三方代码（TWS API、Intel 十进制浮点库、LetsBeRational）沿用各自的许可。交易有重大风险，接入实盘账户风险自负。
