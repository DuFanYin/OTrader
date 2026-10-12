<div align="center">

# Optra

**一套期权交易系统：研究策略、回测、然后实盘交易。**

C++23 引擎 · Python 服务端 · 用于交易和研究的 web 应用

[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](#快速上手)
[![Broker: IBKR](https://img.shields.io/badge/live-Interactive%20Brokers-D81222)](#实盘交易)
[![License: non-commercial](https://img.shields.io/badge/license-learning%20%2F%20portfolio-lightgrey)](#许可)

[在线 demo](https://dufanyin.dev/optra-demo/) · [架构图](https://dufanyin.dev/quant-option/architecture.html) · [文档](#文档) · [English](../README.md)

</div>

<p align="center"><img width="860" alt="Optra 的 web 应用：一个跨式策略在 SPY 期权上回测一个月，P&L 和持有的 Greeks" src="images/app-backtest.png"></p>

Optra 是为期权而建的。按到期日组织的期权链、隐含波动率和 Greeks、多腿组合、到期结算，都是引擎本身的一部分，不是后加上去的。

它覆盖一个策略的完整生命周期：用研究工具琢磨一个想法，把它写成策略，在录下来的期权数据上回测，然后在 Interactive Brokers 账户上交易。每一步用的都是同一份策略代码。

## 研究期权

研究工具用来在下单之前把一笔交易看清楚。

- **定价**：一个期权同时用 Black-Scholes、Monte Carlo、二叉树和三叉树定价，并列给出 Greeks 和方法之间的差异；美式期权另有有限差分和 Longstaff-Schwartz。
- **波动率**：隐含波动率，按期权链校准 Heston 模型，Greeks 曲面。
- **多腿组合**：任意腿的组合，自动识别（跨式、铁鹰……）并整体定价。
- **风险**：压力情景，每个情景的 P&L 按标的、波动率和利率拆开解释；在模拟路径上比较几种对冲方案。
- **筛选器**：在期权链里找值得一看的组合，按相对模型波动率的 edge 排序。
- **校验**：数值方法对照理论检验，可以在每次计算前自动跑。

BTC 和 ETH 的实时期权链来自 Deribit 的公开行情；筛选器也能读你自己的回测数据。每个研究工具都是一个带完整描述 schema 的 HTTP 接口，所以 AI agent 也能做同样的研究。

<p align="center"><img width="860" alt="筛选器按相对模型波动率的 edge 给期权策略排序" src="images/research/screener.png"></p>

## 回测

回测按分钟回放录下来的期权数据。策略每分钟看到完整的期权链，订单按卖价买入、按买价卖出，计入手续费和滑点；持有到到期的期权在收盘时结算。

每次回测按天给出 P&L，并拆成 delta、gamma、theta、vega 几部分，能看清钱是从哪里来的。每一天用一个独立的引擎跑，多天并行，同样的输入永远得到同样的结果。

仓库不带行情数据。可以转换 Databento 下载的数据（SPXW 和 SPY 期权），回放实盘引擎录下的行情，或者用一个答案已知的模型生成合成数据。

## 编写自己的策略

一个策略就是一个带几个回调的 C++ 类。它读取期权链和自己的持仓，通过辅助函数下单：单个期权、标准组合、自定义多腿都可以。

```cpp
class MyStraddle final : public OptionStrategyTemplate {
  public:
    MyStraddle(StrategyContext* context, const std::string& name, const std::string& portfolio,
               const std::unordered_map<std::string, double>& setting);
    static std::span<const StrategyParam> parameters();  // 在 web 应用里显示和设置

    void on_init_logic() override;   // 选择要交易的期权链
    void on_timer_logic() override;  // 每根 bar：读报价和 Greeks，下单
    void on_stop_logic() override;   // 平掉剩下的仓位
};
REGISTER_STRATEGY(MyStraddle);
```

注册之后，就能在 web 应用里回测和交易它，参数也在那里填。内置的 delta 对冲调用一个函数就能打开。

简单的组合不用写代码：`ComboStrategy` 把各条腿写成规则（看涨或看跌、比例、相对远期价格的行权价百分比、到期天数），再加上进出场时间、止盈、止损和可选的对冲。仓库里有九个策略，ComboStrategy 是其中之一；它们只是示例，不代表能赚钱。

## 实盘交易

同一个策略，一行不改，通过 TWS API 在 Interactive Brokers 账户上交易。

- 每笔订单都先过一遍下单前风控，一个按钮就能停掉全部交易。
- 重启后从停下的地方继续，任何策略开始之前先把持仓和券商核对一遍。
- 每五分钟做一次健康检查；收盘后出一份日报，把当天的成交和同一天的回测逐笔对比。

目前 Optra 在模拟盘上交易。引擎只交易指定的那一个账户，实盘账户除非机器管理员明确允许，否则拒绝连接。

## 试一试

[在线 demo](https://dufanyin.dev/optra-demo/) 跑的是真正的引擎，接在一个模拟账户上：合成的 SPY 期权数据当作行情回放，三个示例策略在交易，研究工具都能直接用。

### 快速上手

Linux，GCC 14 及以上（以 Ubuntu 26.04 为准）：

```bash
sudo apt install g++ cmake make libarrow-dev libparquet-dev libprotobuf-dev protobuf-compiler libgrpc++-dev \
  protobuf-compiler-grpc libabsl-dev nlohmann-json3-dev libsqlite3-dev libgtest-dev 7zip

./build.sh r                     # 引擎编译到 engine/build

# 一周的合成 SPY 期权数据
uv run --project app/server python scripts/data/synthetic_to_parquet.py \
  --start 2025-03-03 --end 2025-03-07 --out data --symbol SPY

# 每天一个跨式，P&L 按 Greeks 拆开
./engine/build/entry_backtest --files data/SPY/SPY-2025-03/*.parquet ComboStrategy \
  leg1_ratio=1 leg2_ratio=1 leg2_right=1

./scripts/dev.sh                 # 服务端和 web 应用，打开 http://localhost:5173
```

`entry_backtest --list-strategies` 列出所有策略和参数。回测不需要网络，也不需要数据库。

## 怎么搭起来的

<p align="center">
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/architecture-dark.png">
  <img width="720" alt="Optra 架构：控制面、gRPC 分界、引擎和它的端口、适配器、数值库" src="images/architecture-light.png">
</picture>
</p>

引擎是一个 C++ 程序。回测和实盘跑的是同一个引擎，只是接上的东西不同：回测接录下来的数据和模拟的成交场所，实盘接 Interactive Brokers。web 应用和它的服务端只通过 gRPC 向引擎提问，从不直接碰引擎的状态。

入账是确定的：所有交易状态只归一个线程管，持仓不会因为券商回报成交的先后顺序而不同。设计见 [架构](architecture.md)，[交互式架构图](https://dufanyin.dev/quant-option/architecture.html) 里点任何一块都会说明它做什么。

## 文档

文档是英文的：

- [How Optra fits together](architecture.md)：几个程序，为什么分开，一个报价在引擎里怎么走，从一个想法到模拟盘
- [The engine](engine.md)：事件和入账，实盘进程，重启和到期，回测，模拟市场，IB 回报什么，编译和测试
- [The server and the web app](server.md)：服务端做什么，各个视图，日常运行，公开 demo
- [Research](research.md)：研究视图，方法，行情数据，给 agent 的工具
- [Backtest data](data.md)：数据格式，转换 Databento 下载，回放模拟盘的一天，合成数据
- 给编程 agent：[AGENTS.md](../AGENTS.md)

## 项目状态

核心 C++ 约 13,000 行，154 个 GoogleTest 用例，包括并发、停机和 IB 模拟盘集成测试，并跑 ASan 和 TSan。目前只在模拟盘交易。下一步：更长的多日回测、股票、更低的延迟。[`python-mvp`](../../../tree/python-mvp) 分支保留了 C++ 系统最初的 Python 实现。

## 许可

仅供学习和作品展示（[LICENSE](../LICENSE)）。未经书面许可，不得商用、再分发或接入生产系统。交易有重大风险；连接实盘账户风险自负。
