# 使用 Google Test 搭建测试套件

本文说明如何在 Otrader 中引入 Google Test（Googletest），并在此基础上组织单元测试与集成测试。

---

## 1. 引入 Googletest

### 1.1 方式一：CMake FetchContent（推荐，无需系统安装）

在顶层 `CMakeLists.txt` 中，在 `project(Otrader)` 之后、`add_subdirectory(tests)` 之前加入：

```cmake
# Google Test（仅测试需要时拉取）
if(BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  # 可选：仅构建 gtest 与 gtest_main，不构建 gmock
  # set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
endif()
```

之后在 `tests/CMakeLists.txt` 或各子目录中可 `target_link_libraries(your_test PRIVATE GTest::gtest_main)`。

### 1.2 方式二：系统或 Homebrew 安装

若已通过 Homebrew 安装：`brew install googletest`，则可用：

```cmake
find_package(GTest REQUIRED)
target_link_libraries(your_test PRIVATE GTest::gtest_main)
```

注意：Homebrew 的 googletest 可能只提供 `GTest::gtest` / `GTest::gtest_main`，且需与当前编译器一致（本项目要求 Clang）。

---

## 2. 启用 CTest 与测试注册

在顶层 `CMakeLists.txt` 中，在 `add_subdirectory(tests)` 之前：

```cmake
enable_testing()
```

在定义每个测试可执行文件之后，将其注册为 CTest 用例，便于 `ctest` 统一运行：

```cmake
add_test(NAME YourTestName COMMAND your_test_executable)
# 若测试需要工作目录或环境变量：
# set_tests_properties(YourTestName PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
```

若使用 **Googletest 的“按用例发现”**（每个可执行文件内有多组 `TEST`），可在一个 test 目标上使用：

```cmake
include(GoogleTest)
gtest_discover_tests(your_test_executable
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DISCOVERY_MODE PRE_TEST
)
```

这样 `ctest` 会为每个 `TEST(Suite, Case)` 生成一条独立测试项。

---

## 3. 测试目录与目标划分

建议保持与现有结构一致，并按“是否需要完整 runtime”区分：

| 目录 | 用途 | 依赖 | 说明 |
|------|------|------|------|
| `tests/unit/` | 纯逻辑、工具类、无引擎 | utilities、core 等 | 不依赖 backtest/live 运行时，适合大量 EXPECT 断言 |
| `tests/backtest/` | 回测引擎与数据流 | backtest_engines | 现有 test_backtest_* 可逐步改为 GTest 用例 |
| `tests/live/` | 实盘组件与 MainEngine | engines_cpp | 现有 test_live_* 可保留为“集成脚本”或拆成 GTest |

新建 `tests/unit/` 时，在 `tests/CMakeLists.txt` 中增加：

```cmake
add_subdirectory(unit)   # 仅当存在 unit/CMakeLists.txt 且依赖已满足时
```

---

## 4. 编写 GTest 用例

### 4.1 基本用法：TEST(SuiteName, TestName)

单文件内用 `TEST` 宏定义用例，无需自定义 main（链接 `GTest::gtest_main` 即可）：

```cpp
#include <gtest/gtest.h>

TEST(Utilities, SomeHelper) {
    // 使用 EXPECT_* 表示“失败则标记失败但继续”
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
}

TEST(Utilities, AnotherHelper) {
    // 使用 ASSERT_* 表示“失败则终止当前 TEST”
    ASSERT_NE(nullptr, some_ptr);
    EXPECT_STREQ("ok", str);
}
```

常用断言：`EXPECT_EQ` / `ASSERT_EQ`、`EXPECT_TRUE` / `ASSERT_TRUE`、`EXPECT_NE`、`EXPECT_STREQ`、`EXPECT_DOUBLE_EQ`、`EXPECT_THROW` 等。

### 4.2 带 Fixture 的用例：TEST_F(FixtureClass, TestName)

需要每个用例前做相同准备（如构造引擎、读文件）时，可定义继承 `::testing::Test` 的 Fixture，再用 `TEST_F`：

```cpp
#include <gtest/gtest.h>
#include "engine_main.hpp"   // 示例：回测 MainEngine

class BacktestMainEngineFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // 每个 TEST_F 运行前执行
        engine_ = std::make_unique<backtest::BacktestEngine>();
    }
    void TearDown() override {
        engine_.reset();
    }
    std::unique_ptr<backtest::BacktestEngine> engine_;
};

TEST_F(BacktestMainEngineFixture, LoadDataSucceeds) {
    ASSERT_NE(engine_->main_engine(), nullptr);
    engine_->load_backtest_data("path/to/sample.parquet");
    auto* de = engine_->data_engine();
    ASSERT_TRUE(de != nullptr && de->has_data());
}
```

### 4.3 参数化测试（可选）：TEST_P

同一套逻辑、多组输入时可用 `INSTANTIATE_TEST_SUITE_P` + `TEST_P`，参见 Googletest 文档的 “Value-Parameterized Tests”。

### 4.4 与现有“main + 命令行”测试共存

- **保留现有可执行文件**：不链接 gtest_main，仍用 `int main(int argc, char** argv)`，适合需要传路径、开关的集成脚本（如 `test_backtest_data <parquet_path>`）。
- **新增 GTest 可执行文件**：同一目录下新增 `test_xxx_gtest.cpp`，内仅 `#include <gtest/gtest.h>` 和若干 `TEST`，CMake 里再加一个 `add_executable(test_xxx_gtest ...)` 并 `target_link_libraries(test_xxx_gtest PRIVATE GTest::gtest_main backtest_engines)`。

---

## 5. CMake 示例：单个 GTest 目标

以 `tests/unit` 下单元测试为例（仅依赖 utilities）：

```cmake
# tests/unit/CMakeLists.txt
add_executable(test_utilities_gtest test_utilities_gtest.cpp)
target_include_directories(test_utilities_gtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(test_utilities_gtest PRIVATE GTest::gtest_main utilities_cpp)

include(GoogleTest)
gtest_discover_tests(test_utilities_gtest WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
```

若未使用 `gtest_discover_tests`，至少保留：

```cmake
add_test(NAME test_utilities_gtest COMMAND test_utilities_gtest)
```

---

## 6. 运行测试

- **运行全部 CTest 注册测试**（build 目录下）：
  ```bash
  ctest --output-on-failure
  ```
- **运行单个可执行文件**：
  ```bash
  ./test_utilities_gtest
  ```
- **按用例过滤**：
  ```bash
  ./test_utilities_gtest --gtest_filter="Utilities.*"
  ./test_utilities_gtest --gtest_filter="*Helper*"
  ```
- **重复运行、列出用例**：
  ```bash
  ./test_utilities_gtest --gtest_repeat=2
  ./test_utilities_gtest --gtest_list_tests
  ```

---

## 7. 与现有 Otrader 构建的衔接

- **BUILD_TESTS**：建议仅在 `BUILD_TESTS=ON` 时 `FetchContent_MakeAvailable(googletest)` 并构建 GTest 目标，避免未启用测试时拉取 GTest。
- **依赖**：单元测试只 link utilities/core 等；backtest 测试 link `backtest_engines`；live 测试 link `engines_cpp`，与现有 `tests/backtest`、`tests/live` 的依赖一致。
- **include 路径**：保持以 Otrader 根为 include 根（如 `#include "engine_main.hpp"`），在 CMake 里用 `target_include_directories(.. PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)` 或项目统一的 `$<BUILD_INTERFACE:...>`。

按上述方式即可在现有 CMake 与目录结构下，用 Google Test 搭起一套可扩展的测试套件，并与现有“脚本式”可执行测试并存。

---

## 8. 专项测试：Live 数据流、Ring Buffer、Object Pool

以下三类测试均可落地，建议单独建用例或独立 test binary，便于 CI 与本地排查。

### 8.1 Live 数据流（EventEngine + 主队列 + 策略队列）

**目标**：验证事件从入队到 dispatch、再到策略回调的整条路径，不依赖真实 Gateway/MarketData 进程。

**思路**：

- 进程内构造 `MainEngine`（含 EventEngine），`connect()` 启动 EventEngine 的 main / timer / strategy 三条线程（可不连 ZMQ：不调 `start_market_data_update()` 或仅用 mock）。
- **注入事件**：在测试线程直接调用 `main_engine.put_event(...)`：
  - Snapshot：`acquire_snapshot()` → 填 portfolio_name 等 → `put_event(Event(Snapshot, p))`，断言某 portfolio 在 dispatch 后 `apply_frame` 被调用（可通过状态或 test double 观测）。
  - Timer：由 timer 线程自然推，或测试里直接 `put_event(Event(Timer))`，然后等 strategy 线程处理；断言策略侧 `on_timer` 被调用（例如在测试策略里设计数器或 flag）。
  - Order/Trade：`acquire_order()`/`acquire_trade()` 填 orderid/strategy_name 等，`put_event(Event(Order, p))`，断言对应策略的 `process_order`/`process_trade` 被调用、或 ExecutionEngine 状态一致。
- **同步**：事件是异步消费的，测试里需要“等一段时间”或轮询条件（如策略回调计数 ≥ 1），可用短 sleep + 重试或 `std::condition_variable` + 策略回调里 notify。
- **收尾**：`main_engine.close()` 会 stop EventEngine、join 线程、drain 队列；断言无崩溃、无泄漏（可配合 LeakSanitizer）。

**可测点**：主队列 MPSC 多生产者（Timer + 测试线程 put_event）单消费者；main worker 按类型 dispatch；策略队列 SPSC 单生产者（main worker）单消费者；策略线程按 event 类型调 on_timer/process_order/process_trade。

**依赖**：`engines_cpp`（MainEngine、EventEngine、OptionStrategyEngine 等），不需要真实 TWS/ZMQ 连接。

---

### 8.2 Ring Buffer 单独测试（SpscRing / MpscRing）

**目标**：在无 EventEngine 的前提下，验证无锁环的正确性与边界行为。

**SpscRing**（`utilities/spsc_ring.hpp`）：

- **顺序与无丢包**：起 1 个 producer 线程、1 个 consumer 线程；producer 循环 `try_push(i)`（i = 0..N-1），consumer 循环 `try_pop(v)` 并写入 vector；结束后断言 vector 长度等于 N 且顺序为 0,1,...,N-1。
- **满时行为**：`try_push` 在 ring 满时返回 false；先 push 满 capacity 个，再 push 一次应返回 false；consumer  pop 一个后再 push 应成功。
- **空时行为**：`try_pop` 在空时返回 false；初始 pop 应返回 false。
- **容量与 size_approx**：`capacity()`、`empty()`、push 若干后 `size_approx()` 与预期一致（允许近似语义）。

**MpscRing**（`utilities/mpsc_ring.hpp`）：

- **多生产者单消费者**：P 个 producer 线程各 push M 个值（如 thread_id * M + j），1 个 consumer 把全部 P*M 个 pop 到集合；断言集合大小等于 P*M 且内容正确（无重复、无丢失）。
- **满/空**：同 SPSC，满时 `try_push` 返回 false，空时 `try_pop` 返回 false。
- **压力**：大量迭代（如每线程 10^5 次 push）、多线程，配合 ThreadSanitizer 跑一遍，确保无 data race。

**实现**：单独 test 目标（如 `test_ring_gtest`），只 link `utilities_cpp`（或仅包含 ring 头文件的库）、`GTest::gtest_main`，不依赖 core/runtime。

---

### 8.3 Object Pool 安全性单独测试（ObjectPool）

**目标**：验证 acquire/release、重用、关闭、并发与 double-release 等行为。

**基础**：

- 单线程：`acquire()` N 次，记下指针，`release()` N 次；再 `acquire()` N 次，断言得到 N 个非空指针（可断言与第一次集合相同或仅数量一致，视是否要求复用同一批 slot）。
- `emplace(args...)`：带参构造，release 后再 acquire/emplace，对象状态正确。

**边界**：

- **close()**：`close()` 后 `acquire()`/`emplace()` 返回 nullptr；已 acquire 的指针仍可正常 `release()`，析构前应等待 active_ops_ 归零（可多线程里先 hold 若干 acquire，再 close，再 release，最后析构不崩溃）。
- **double-release**：同一指针 `release()` 两次；当前实现用 `in_use_` 保护，第二次 release 应 no-op（不重复 push 到 free list、不二次析构）。可写 EXPECT 或断言 NDEBUG 下行为。
- **release(nullptr)**：应 no-op。
- **外源指针**：对非本 pool acquire 得到的指针调用 `release()`，应安全（当前实现会查 in_use_，不在此 set 则不应破坏内部状态）。

**并发**：

- 多线程同时 `acquire()`/`release()` 大量次数（如 4 线程 × 10^4 次），最后全部 release，析构 pool；配合 ThreadSanitizer/AddressSanitizer 跑，无 data race、无 use-after-free。
- 可选：先 close()，再起多线程只做 release（之前主线程 acquire 的），验证 shutdown 协议（active_ops_、shutdown_cv_）无死锁、无 UAF。

**实现**：单独 test 目标（如 `test_object_pool_gtest`），link `utilities_cpp`（或仅含 object_pool 的库）、`GTest::gtest_main`。可先用简单类型（如 `struct Dummy {};`）测，再选一个实际类型（如 `Event`）做一次集成式用例。

---

上述三类测试均可通过 Googletest 的 `TEST`/`TEST_F` 组织，与 §1–7 的 CMake/CTest 流程一致；live 数据流需 `BUILD_LIVE`，ring 与 pool 仅需 utilities，便于分层启用与 CI 矩阵。
