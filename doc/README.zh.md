<div align="center">

# OTrader

**事件驱动的 C++20 期权交易与研究引擎 —— 一套策略代码，回测与实盘通用。**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](#)
[![Build: CMake](https://img.shields.io/badge/build-CMake%20presets-064F8C?logo=cmake&logoColor=white)](#构建与运行)
[![Broker: IBKR](https://img.shields.io/badge/live-Interactive%20Brokers-FF6600)](#)
[![UI: Next.js](https://img.shields.io/badge/UI-Next.js%2016-black?logo=nextdotjs)](#)
[![License: non-commercial](https://img.shields.io/badge/license-learning%20%2F%20portfolio-lightgrey)](#许可)

[English](../README.md) · **中文**

</div>

---

大多数回测器和实盘系统是两套会悄悄跑偏的代码。**OTrader 是同一个引擎**：策略消费 **Event**（定时、快照、订单、成交），产出 **Intent**（下单、撤单、日志）——**同一份**策略代码既跑确定性回测，也接实盘券商，底层只换运行时接线。

核心是 **约 11K 行 C++**（引擎 + 策略框架），不含测试、生成的 protobuf 与示例策略。

## 亮点

- 🔁 **一套策略，两种模式** —— 写一次，回测与实盘通用；不存在会跑偏的"实盘专用重写版"。
- 🧩 **纯逻辑核心，I/O 注入** —— 策略/持仓/对冲从不直接碰网络或数据库；一切经 `RuntimeAPI` 注入，因此核心可以轻松单测。
- ⚡ **为低延迟而生** —— 无锁 SPSC/MPSC 环形缓冲、对象池、指针载荷事件与 zero-copy 热路径；重计算跑在持久线程池上，而非每事件建线程。
- 🛰️ **进程隔离的实盘栈** —— 行情与 IB 网关作为**独立进程**经 ZeroMQ IPC 通信；gRPC 服务对外暴露引擎控制。
- 📈 **期权原生** —— 内置多腿组合、Black-Scholes 隐波/Greeks、以及策略级 delta 对冲引擎。
- 🖥️ **完整控制界面** —— FastAPI 后端 + Next.js 前端，启动回测、管理策略、查看订单/成交。

## 架构

```
          Events (Timer · Snapshot · Order · Trade)
                          │
                          ▼
   ┌───────────────────────────────────────────────┐
   │  Domain Core  (纯逻辑, 无 I/O)                  │
   │  strategy · position · hedge · execution · log │
   └───────────────────────────────────────────────┘
                          │  Intents (order · cancel · log)
                          ▼
   ┌───────────────────────────────────────────────┐
   │  Runtime   分发循环 + 意图处理                  │
   │   回测: 同步            │   实盘: 队列          │
   └───────────────────────────────────────────────┘
        │                                   │
        ▼ 回测                              ▼ 实盘 (独立进程, ZMQ IPC)
   Parquet 历史数据                    行情  ·  IB 网关  ·  gRPC
```

核心所需的一切都经 `RuntimeAPI` **注入**（数据视图、发单、日志）。换基础设施——不同数据源、模拟网关、另一种数据库——策略与核心逻辑一行都不用改。

## 构建与运行

```bash
# 1. 构建 C++ 引擎 (Release) → Otrader/build/entry_backtest, entry_system
./build.sh r

# 2. 跑回测：<parquet 某一天> <已注册策略>
./Otrader/build/entry_backtest data/SPXW/SPXW-2025-08/20250804.parquet StraddleTestStrategy

# 3. 完整开发栈 (引擎 + FastAPI 后端 + Next.js 前端)
(cd app/backend && uv sync)
./scripts/system_up.sh dev
```

**前置依赖 (macOS)：** Clang、CMake ≥ 3.21，以及 Homebrew 的 `protobuf grpc zeromq apache-arrow libpqxx`；前端需 Node.js；Python 后端用 [`uv`](https://github.com/astral-sh/uv)。工具链由 [`Otrader/CMakePresets.json`](../Otrader/CMakePresets.json) 固定。

> 回测不需要任何实盘基础设施——无网络、无数据库。`./build.sh r` 即可开跑。

## 需要你自备

这是一个引擎，不是开箱即用的成品。有三样东西刻意不包含在仓库里：

- **回测数据。** 仓库不含任何数据。回测读取固定 schema 的按天 Parquet 文件（样本从 [Databento](https://databento.com/) DBN 清洗而来，但任何数据源都可以）。清洗这一步需要你自己写 —— schema 与目录约定见 **[data/README.md](../data/README.md)**。
- **实盘行情源。** 内置的 provider 轮询 **Tradier** API，需要*你自己的付费 Tradier production token*（`TRADIER_TOKEN`；sandbox 不行）。想用别的数据商？实现一个能向引擎产出 `PortfolioSnapshot` 的 provider 即可——接口很小、可替换。
- **实盘执行网关。** 内置的那个封装了 Interactive Brokers TWS API（**IBJts**）——一个私有 SDK，不在本仓库再分发，需你自行获取并编译；或者按 ZeroMQ 消息契约**自己实现一个网关**（它作为独立进程运行，引擎只通过 ZMQ 与它通信）。实盘还需 PostgreSQL。

## 内置策略

注册于 [`strategy_registry.cpp`](../Otrader/strategy/strategy_registry.cpp) —— 按名运行，或作为你自己策略的模板：

| 策略 | 思路 |
|------|------|
| `StraddleTestStrategy` | 跨式多空参考实现 |
| `IvMeanRevertStrategy` | 隐含波动率均值回归 |
| `IronCondorTestStrategy` | 限定风险铁鹰 |
| `StraddleInventoryScalperStrategy` | 跨式库存刷单 |

写自己的策略 = 继承策略模板 + 一行 `REGISTER_STRATEGY(...)`——见[策略指南](./)。

## 文档

- **架构** —— [English](en/architecture.md) · [中文](cn/architecture.md)
- **引擎深挖**（线程模型、zero-copy 数据路径、低延迟设计）—— [doc/cn/engine/](cn/engine/)
- **组件概览** —— [引擎](../Otrader/README.md) · [后端](../app/backend/README.md) · [前端](../app/frontend/README.md)

## 两条血脉

本仓库有意保留两条实现线：

- **`main`** —— C++20 引擎（本 README）。一次 **agent 辅助**的重写，聚焦延迟、结构与双模式设计。
- **[`python-mvp`](../../../tree/python-mvp)** —— C++ 引擎脱胎而来的、最初**纯手写**的 Python 实现。完整保留，作为参考设计。

## 目录结构

```
Otrader/   C++ 引擎 —— core · runtime · infra · strategy · utilities · entry
app/       后端 (FastAPI, gRPC 桥接) + 前端 (Next.js UI)
doc/       设计文档 (cn/ + en/)
scripts/   开发启动、测试与 lint 辅助脚本
```

## 许可

仅供**学习与作品展示**。未经书面许可，不得商用、再分发或用于生产。交易有重大风险，接入实盘账户风险自负。
