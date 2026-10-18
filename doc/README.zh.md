<div align="center">

# Optra

**一套期权交易系统：研究策略、回测、然后实盘交易。**

C++23 引擎 · Python 服务端 · 用于交易和研究的 web 应用

[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](#快速上手)
[![Broker: IBKR](https://img.shields.io/badge/live-Interactive%20Brokers-D81222)](#在-interactive-brokers-上交易)
[![License: non-commercial](https://img.shields.io/badge/license-learning%20%2F%20portfolio-lightgrey)](#许可)

[在线 demo](https://dufanyin.dev/optra-demo/) · [架构图](https://dufanyin.dev/optra-architecture/) · [文档](#文档) · [English](../README.md)

</div>

<p align="center"><img width="860" alt="Optra 的 web 应用，交易页：当天概况、在模拟账户上运行的策略、选中策略的持仓、仓位和成交、风控限额和压力测试" src="images/app-home.png"></p>

Optra 的引擎直接以期权为对象：期权链按到期日组织，隐含波动率和 Greeks 随报价算出，多腿组合作为一笔订单下单，持有到期的期权在收盘时自动结算。它覆盖一个策略的完整生命周期：用研究工具琢磨一个想法，把它写成策略，回测，然后交易。每一步用的都是同一份策略代码。

克隆下来编译好，就能在模拟市场上把整个系统跑起来：不需要券商账户，不需要数据订阅，不用连接任何外部服务。不想先装，可以直接打开 **[在线 demo](https://dufanyin.dev/optra-demo/)**：同一套系统跑在模拟账户上，策略在交易，研究工具可以直接用。

## 快速上手

Linux，GCC 14 或更新（以 Ubuntu 26.04 为准），Python 用 [uv](https://docs.astral.sh/uv/)，页面用 Node.js：

```bash
sudo apt install g++ cmake make libarrow-dev libparquet-dev libprotobuf-dev protobuf-compiler libgrpc++-dev \
  protobuf-compiler-grpc libabsl-dev nlohmann-json3-dev libsqlite3-dev libgtest-dev 7zip nodejs npm
curl -LsSf https://astral.sh/uv/install.sh | sh

git clone https://github.com/DuFanYin/optra.git && cd optra
./build.sh r              # 引擎、回测程序和策略，编译到 engine/build
./scripts/dev.sh sim      # 在模拟市场上跑整个系统
```

打开 http://localhost:5173。第一次启动会往 `data/SPY` 写入合成的 SPY 期权数据：今天之前三个月的用来回测，今天的一天（周末则是下一个工作日）用作市场。然后启动四个进程：

- **引擎**，跑在模拟市场上（`entry_simulate`）：从开盘起按真实时间的节奏逐分钟回放今天这一天的合成数据，订单按它的报价成交。它就是实盘引擎，只是原来接 Interactive Brokers 的位置换成了一个模拟交易所；
- **服务端**：页面的所有操作都经过它，它再通过 gRPC 查询和指挥引擎；
- 一个**研究 worker**，跑服务端排进队列的回测和实验；
- **页面**。

Ctrl+C 一起停掉。模拟账户（策略、持仓、成交）保存在 `data/sim/`，重启后还在；市场则从当天开盘重新回放。

## 可以试什么

**交易。** 在 Home 的 *New strategy* 里选一个策略类和 SPY 组合，填好参数，按 *Add strategy*，再按 *Init* 和 *Start*。`ComboStrategy` 用默认参数是买一张平值看涨期权；再加一条看跌腿就是跨式，把两条腿都卖出、再买入 2% 外的两翼就是铁蝶。随着这一天回放，它的持仓、仓位和成交会更新；*Halt all* 停掉所有策略、撤掉所有挂单；*Limits* 显示每笔订单都要过的事前风控。

**回测。** 在 Backtest 里选策略、参数和日期范围，按 *Run backtest*。每一天用一个独立的引擎按分钟跑：策略每分钟看到完整的期权链，订单按卖价买入、按买价卖出，计入手续费和滑点；持有到到期的期权在收盘时结算。P&L 按天给出，并拆成 delta、gamma、theta、vega 几部分，能看清钱是从哪里来的。

<p align="center"><img width="860" alt="一次回测：卖出跨式在合成 SPY 数据上跑三个月，汇总、P&L 和持有的 Greeks" src="images/app-backtest.png"></p>

命令行也一样，不需要服务端、网络或数据库：

```bash
./engine/build/entry_backtest --list-strategies          # 所有策略和它们的参数
./engine/build/entry_backtest --files data/SPY/SPY-*/*.parquet ComboStrategy leg1_ratio=1 leg2_ratio=1 leg2_right=1
```

**做实验。** 在 Experiments 里让一个策略在一组参数空间上跑。每次尝试在跑之前就登记，并计入它的家族（围绕同一个问题做过的所有尝试），所以试得越多，最好的那组就要越好才算数：用 deflated Sharpe、回测过拟合概率和 walk-forward 对照全部尝试来判断。

<p align="center"><img width="860" alt="一个实验：卖出跨式的十二组参数、每组的结果、对照全部尝试的 deflated Sharpe 和过拟合概率、walk-forward、最佳一组的拆解" src="images/app-experiments.png"></p>

**琢磨一笔交易。** Lab 里是各种计算器：

- **Pricing**：一个期权同时用 Black-Scholes、Monte Carlo、二叉树和三叉树定价，并列给出五个 Greeks 和方法之间的差异，美式期权另有有限差分和 Longstaff-Schwartz；按市场校准 Black-Scholes 和 Heston，画出隐含波动率曲面。
- **Multi-Leg**：任意腿的组合（跨式、价差、铁鹰，或你自己的），逐腿定价，给出合计的 Greeks。
- **Risk**：把一个期权或组合放进一组市场冲击里，看亏损来自哪里；在同一批模拟路径上比较四种 delta 对冲，看对冲成本和尾部风险的取舍。
- **Screener**：从期权链里组出单腿、跨式、宽跨、铁鹰和日历价差，按相对模型波动率的 edge 排序；期权链可以是你数据里任意一分钟的，也可以是 Deribit 公开行情的 BTC、ETH 实时期权链。
- **Numerics**、**Validation**：数值方法互相对照（有限差分、树的收敛），并对照理论检验，每项有阈值。

**交给 agent 来驱动。** Tools 按研究流程的顺序列出 agent 能调用的每个工具：写策略、回测、做实验、晋级一个版本。每个都是带完整描述 schema 的 HTTP 接口。批准、安装、改动会交易的东西，始终由人来做。

<p align="center"><img width="860" alt="agent 使用的工具，按步骤：写策略、回测、晋级；下面是各个计算器" src="images/app-tools.png"></p>

## 编写自己的策略

一个策略就是一个带几个回调的 C++ 类，只写它的想法：什么时候交易、交易什么、交易多少。其余的由基类负责：交易日和当天第几分钟、选期权链、按离平值几档找期权、持仓，以及下单（单个期权、标准组合、自定义多腿）。

```cpp
class MyStraddle final : public Strategy {
  public:
    struct Setting {  // 在 web 应用里显示和设置，实验也在这些范围里搜
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

放进 `strategy/research/`，用 `./build.sh s` 编译：每个文件是一个独立的库，不用重新编译引擎、也不用重启就会被加载。也可以在 Home 左边栏的 *Your own strategy* 里直接粘贴整份代码，按 *Build* 编译，编译器报错原样显示；成功后再按 *Install for paper*，就能在 *New strategy* 里选它。回测和实验马上就能用它；会交易的引擎只有在它被安装到该账户之后（`strategy/paper`、`strategy/live`）才加载它，这一步由人来做，所以 agent 写的代码只能经过人才能到账户上。内置的 delta 对冲一个调用就能打开。

简单的组合不用写代码：`ComboStrategy` 把各条腿当作规则来设（看涨或看跌、比例、相对远期价格的行权价百分比、到期天数），加上进出场时间、止盈、止损和可选的对冲。`strategy/demo` 是仓库自带的策略：ComboStrategy、所有策略都要与之比较的对照组 RandomBuyStrategy、一个 delta 对冲的跨式、一个 IV 均值回归策略，以及两个测试用的策略；它们都不是已经证明有效的策略。

## 自己的数据

回测只认一种形状的按天文件：每个标的每个交易日一个 Parquet 文件，每行是一个期权某一时刻的最优买卖价，并带上标的同一时刻的报价。Optra 不绑定任何数据商，任何数据商的数据都离线转换成这个形状。[doc/data.md](data.md) 讲格式，`scripts/data/example_to_parquet.py` 是一个可以照着改的小转换示例，适用于以两个 CSV 文件提供的报价。

## 在 Interactive Brokers 上交易

同一个引擎、同样的策略，通过 TWS API 在 Interactive Brokers 账户上交易。编译时带上 IB 网关，运行 IB Gateway 或 TWS，在 `.env` 里写上 `IB_ACCOUNT=DU...`（一个模拟账户），然后不带 `sim` 启动：

```bash
./build.sh r g            # 带上 IB 网关（第一次会下载 TWS API）
./scripts/dev.sh          # entry_live：行情和订单都走 IB
```

- 每笔订单都要过事前风控，一个按钮停掉全部交易。
- 重启后从停下的地方接着来，任何策略启动前都会先把持仓和券商核对一遍。
- 按日常方式部署时（[doc/server.md](server.md)），每五分钟一次健康检查，收盘后有一份报告，把当天的成交和同一天的回测逐笔对比。

目前 Optra 只在模拟账户上交易。引擎只交易给它的那一个账户，除非机器的管理员允许，否则拒绝实盘账户。

## 怎么搭起来的

**[交互式架构图](https://dufanyin.dev/optra-architecture/)** 展示了 Optra 的每个部分，点开就能看到它做什么。

引擎是一个 C++ 程序。回测、模拟市场和实盘交易跑的是同一个引擎，只是接上的连接不同：录下的数据加模拟成交、回放的一天加模拟交易所，或者 Interactive Brokers。web 应用和它的服务端只通过 gRPC 问引擎，从不碰它的状态。

记账是确定性的：一个线程拥有全部交易状态，持仓永远不取决于券商回报成交的先后顺序。[架构文档](architecture.md)解释了设计。

## 文档

- [Optra 是怎么组合起来的](architecture.md)：有哪些程序、为什么分开、一条报价在引擎里走过的路、从一个想法到模拟账户
- [引擎](engine.md)：事件和记账、实盘进程、重启和到期、回测、模拟市场、IB 回报了什么、编译和测试
- [延迟](latency.md)：在一台具名机器上测的引擎内部延迟：线程、不加锁不复制的交接、只给变了的定价
- [服务端和 web 应用](server.md)：服务端做什么、各个页面、日常怎么运行、公开 demo
- [研究](research.md)：研究页面、方法、行情数据、给 agent 的工具
- [回测数据](data.md)：格式、把数据商的数据转换过来、回放模拟盘的日子、合成数据
- [高效的期权策略回测](backtesting.md)（英文短文）：三级存储、一天只读一次和定价一次、结果只汇总一次、实测带来的改动
- 给编程 agent：[AGENTS.md](../AGENTS.md)

## 项目状态

核心 C++ 约 18,500 行，338 个测试（GoogleTest，包括并发、关停和 IB 模拟账户的集成测试，跑过 ASan 和 TSan），另有服务端自己的测试。目前只在模拟账户上交易。下一步：更长的多日回测、股票、更低的延迟。[`python-mvp`](../../../tree/python-mvp) 分支保留了 C++ 系统最初由之发展而来的 Python 实现。

## 许可

用于学习和作品展示（[LICENSE](../LICENSE)）。未经书面许可，不得商用、再分发或接入生产环境。交易有重大风险，接入实盘账户风险自负。
