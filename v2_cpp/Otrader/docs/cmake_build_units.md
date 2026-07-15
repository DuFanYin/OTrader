# Otrader CMake 编译单位与构建脚本说明

本文档说明：**顶层 CMake 选项与编译单位**，以及**项目根目录三个构建脚本**如何共用同一 build 目录、各自只编各自目标。

---

## 1. 三个脚本与共用 build 目录

| 脚本 | 默认 BUILD 目录 | 职责 |
|------|------------------|------|
| **build.sh** | `Otrader/build`（`BUILD_DIR`） | 创建目录、**唯一**执行 cmake 配置；**只编** 5 个主程序 target，不编任何 test。 |
| **test_otrader.sh** | 同上（共用） | **只编** 7 个 non-framwork test（bt: 4 个 backtest，live: 3 个 live），并运行；不配置、不 make 全量。 |
| **test_gtest.sh** | 同上（共用） | **只编** `otrader_unit_test`，并运行；不配置、不 make 全量。 |

- 三者共用 **`Otrader/build`**（可通过环境变量 `BUILD_DIR` 覆盖）。
- **必须先跑一次 `./build.sh`**，生成并配置该目录（含 `BUILD_TESTS=ON`，使 test target 存在于树中）；之后才可用 `./test_otrader.sh` / `./test_gtest.sh` 编并跑各自的 test。
- 若未先执行 build.sh 就运行 test 脚本，会报错并提示 `Run ./build.sh first (shared build dir).`

### 1.1 build.sh 实际编的 target

- `entry_backtest`
- `entry_live_grpc`
- `entry_gateway`
- `entry_market_data`  

（以及它们依赖的库；**不**包含 `test_*`、`otrader_unit_test`。）

### 1.2 test_otrader.sh 实际编的 target

- **bt 模式**：`test_backtest_data`、`test_backtest`、`test_backtest_pos`、`test_backtest_order`
- **live 模式**：`test_live_components`、`test_live_market`、`test_live_strategy_straddle`
- **build 模式**：以上 7 个一起

### 1.3 test_gtest.sh 实际编的 target

- `otrader_unit_test`

---

## 2. CMake 选项（顶层）

| 选项 | 默认值 | 含义 | 会拉入的依赖/子目录 |
|------|--------|------|---------------------|
| `BUILD_BACKTEST` | OFF | 回测入口与回测运行时 | `runtime/backtest` → `backtest_engines`（Arrow/Parquet） |
| `BUILD_GATEWAY` | OFF | Gateway 独立进程 | `infra/gateway` → `gateway_core`，需 cppzmq |
| `BUILD_MARKETDATA` | OFF | Market Data 独立进程 | 可能拉入 `runtime/live`；`entry_market_data` 链 engines_cpp + cppzmq |
| `BUILD_LIVE_GRPC` | OFF | 实盘 gRPC 入口 + 实盘运行时 | 拉入 `runtime/live`；`entry_live_grpc` 链 engines_cpp |
| `BUILD_TESTS` | OFF | 启用 CTest + tests 子目录 | `add_subdirectory(tests)`，**不**自动开 BACKTEST/LIVE_GRPC |

**注意**：`BUILD_TESTS` 只负责加 tests 目录。non-framwork/backtest 的 4 个 test 需已有 `backtest_engines`（即 `BUILD_BACKTEST=ON`）；non-framwork/live 的 3 个 test 需已有 `engines_cpp`（即 `BUILD_LIVE=ON`）；tests/unit 的 `otrader_unit_test` 只依赖 GTest。

当前 **build.sh** 的配置为：上述 6 个选项全 ON（BACKTEST、LIVE、TESTS、LIVE_GRPC、GATEWAY、MARKETDATA），因此该 build 树中**存在**所有主程序与 test target；由脚本决定**谁去编**谁。

---

## 3. 始终参与构建的编译单位（无选项控制）

以下在**任意配置**下都会存在（根 CMakeLists 直接或通过 tests 间接添加）：

| 目录 | Target | 类型 | 说明 |
|------|--------|------|------|
| `utilities/` | `utilities_cpp` | 库 | 工具、对象模型、ring、portfolio、combo、black_scholes、LetsBeRational |
| `proto/` | `otrader_proto` | 库 | protobuf 生成代码 |
| `core/` | `portfolio_structure_shared` | 共享库 | 组合/合约加载 |
| `core/` | `position_engine_shared` | 共享库 | 仓位引擎 |
| `core/` | `execution_engine_shared` | 共享库 | 执行/订单引擎 |
| `core/` | `hedge_engine_shared` | 共享库 | 对冲引擎 |
| `core/` | `option_strategy_engine_shared` | 共享库 | 策略引擎（RuntimeAPI + JSON） |
| `core/` | `log_engine_shared` | 共享库 | 日志引擎 |
| `strategy/` | `strategies` | 静态库 | 策略注册 + StraddleTest、IronCondor、IvMeanRevert、StraddleInventoryScalper |

可执行文件全部由选项控制，见下节。

---

## 4. 按选项出现的编译单位

### 4.1 BUILD_BACKTEST=ON

| 位置 | Target | 类型 | 说明 |
|------|--------|------|------|
| `runtime/backtest/` | `backtest_engines` | 库 | 回测引擎，**依赖 Arrow + Parquet** |
| 根目录 | `entry_backtest` | 可执行文件 | 回测主入口 |

### 4.2 BUILD_GATEWAY=ON

| 位置 | Target | 类型 | 说明 |
|------|--------|------|------|
| `infra/gateway/` | `gateway_core` | 库 | IB Gateway + ZMQ，依赖 TWS |
| 根目录 | `entry_gateway` | 可执行文件 | Gateway 进程入口 |

### 4.3 BUILD_MARKETDATA=ON

| 位置 | Target | 类型 | 说明 |
|------|--------|------|------|
| 根目录 | `entry_market_data` | 可执行文件 | Market Data 进程入口（可能拉入 engines_cpp） |

### 4.4 BUILD_LIVE_GRPC=ON

| 位置 | Target | 类型 | 说明 |
|------|--------|------|------|
| 根目录 | `entry_live_grpc` | 可执行文件 | 实盘 gRPC 服务入口（可能拉入 engines_cpp） |

### 4.6 BUILD_TESTS=ON 且 tests 子目录

| 条件 | 位置 | Target | 类型 | 说明 |
|------|------|--------|------|------|
| 已有 backtest_engines | `tests/non-framwork/backtest/` | `test_backtest_data` / `test_backtest` / `test_backtest_pos` / `test_backtest_order` | 可执行文件 | 回测脚本测试 |
| 已有 engines_cpp | `tests/non-framwork/live/` | `test_live_components` / `test_live_market` / `test_live_strategy_straddle` | 可执行文件 | 实盘脚本测试 |
| GTest 已找到 | `tests/unit/` | `otrader_unit_test` | 可执行文件 | GTest 单元测试 |

---

## 5. 依赖关系简图（库级）

```
utilities_cpp
    ↑
otrader_proto ← portfolio_structure_shared, position_engine_shared, ...
    ↑
strategies ← option_strategy_engine_shared
    ↑
backtest_engines (BUILD_BACKTEST)   engines_cpp (BUILD_LIVE_GRPC)
    ↑                                    ↑
entry_backtest, test_backtest_*      entry_live_grpc, entry_market_data, test_live_*
```

`otrader_unit_test` 只链 `GTest::gtest_main`，不依赖 backtest_engines / engines_cpp。

---

## 6. 可执行文件与所需选项（总结）

| 可执行文件 | 需要选项 | 由谁编（当前约定） |
|------------|----------|---------------------|
| `entry_backtest` | BUILD_BACKTEST=ON | build.sh |
| `entry_gateway` | BUILD_GATEWAY=ON | build.sh |
| `entry_market_data` | BUILD_MARKETDATA=ON | build.sh |
| `entry_live_grpc` | BUILD_LIVE_GRPC=ON | build.sh |
| `test_backtest_data` 等 4 个 | BUILD_BACKTEST=ON + BUILD_TESTS=ON | test_otrader.sh（bt/build） |
| `test_live_components` 等 3 个 | BUILD_LIVE_GRPC=ON + BUILD_TESTS=ON | test_otrader.sh（live/build） |
| `otrader_unit_test` | BUILD_TESTS=ON + GTest | test_gtest.sh |

以上即当前 CMake **编译单位**与**构建脚本分工**的完整说明。
