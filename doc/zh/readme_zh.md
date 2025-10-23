# OTrader

<div align="center">

*支持自定义期权策略开发与实盘部署的实时交易平台*

---

<pre>
    ___  _____              _           
   / _ \|_   _| __ __ _  __| | ___ _ __ 
  | | | | | || '__/ _` |/ _` |/ _ \ '__|
  | |_| | | || | | (_| | (_| |  __/ |   
   \___/  |_||_|  \__,_|\__,_|\___|_|   
</pre>
---

[![Python 3.11+](https://img.shields.io/badge/python-3.11+-blue.svg)](https://www.python.org/downloads/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Broker: Interactive Brokers](https://img.shields.io/badge/trading-Interactive%20Brokers-FF6900.svg)](https://www.interactivebrokers.com/)
[![Market Data: Tradier](https://img.shields.io/badge/market%20data-Tradier-00D4AA.svg)](https://tradier.com/)
[![Code Style: ruff](https://img.shields.io/badge/code%20style-ruff-000000.svg)](https://github.com/astral-sh/ruff)
[![Type Checked: mypy](https://img.shields.io/badge/type%20checked-mypy-blue.svg)](https://github.com/python/mypy)

</div>

## 概述

OTrader 是一个面向策略的算法期权实盘交易平台。它提供全面的功能集来支持完整的交易生命周期，**策略模板**让交易者能够轻松构建和部署自己的期权策略。

## 功能特性

- **持仓跟踪：** 标的资产、单腿期权和组合期权的策略级跟踪
- **风险管理：** 综合希腊字母、已实现和未实现盈亏监控
- **市场数据：** 实时 Tradier 市场数据源：股票 + 期权快照
- **对冲引擎：** 策略级自动 Delta 对冲
- **策略运行时：** 多策略支持，独立生命周期管理
- **持久化层：** SQLite 存储期权列表和执行历史
- **策略状态存储：** YAML 持久化策略参数和持仓

## 架构

该平台采用模块化、事件驱动的架构构建。

重点关注代码质量和效率——这对实盘交易至关重要——形成了一个强大、可靠的系统。架构经过精心设计，提供简洁、流线型的设计，复杂度最小。

核心系统仅包含约 3000 行代码。

简单的服务器后端使用 FastAPI 构建，而 HTML 和原生 JS 前端提供轻量级控制面板来操作和监控系统。

更详细的引擎设计，请查看 **[引擎文档](doc_engines.md)**

### 核心系统架构

![OTrader Core Class Diagram](graph/design.svg)

## 安装

#### 1. 安装 Interactive Brokers API

从 [Interactive Brokers TWS API](https://interactivebrokers.github.io/) 下载并安装 `ibapi`。

详细的安装说明，请参考 [IBKR 官方教程](https://www.interactivebrokers.com/campus/trading-lessons/what-is-the-tws-api/)。

下载并安装 IBKR TWS/Gateway。

安装 ibapi 和 TWS/Gateway 后，进入设置并禁用"只读 API"并启用"绕过订单预防措施"。

**重要：** 不建议连接到实盘账户！！！

#### 2. 获取 Tradier API（生产 API，不是沙盒）

#### 3. 克隆仓库

```bash
git clone https://github.com/your-username/OTrader.git
cd OTrader
```

#### 4. 安装依赖

```bash
pip install -r requirements.txt
```

#### 5. 配置环境

```bash
cp .env.example .env
# 编辑 .env 文件，填入您的 Tradier 生产 API 凭据（不要使用沙盒密钥）
```

#### 6. 准备期权数据（下载期权列表）

要保存目标标的的期权链，设置您想要存储的投资组合并启用标的更新：

- 打开 `engine_db.py` 并调整这些行：

  ```python
  REQUIRED_SYMBOLS = ["SPX-USD-IND-CBOE", "AMZN-USD-STK-SMART"]  # 根据需要添加/删除
  AUTOMATIC_UPDATE_SYMBOLS = True              # 设置为 True 进行自动更新
  ```

- **注意：**
  - 每个标的可能需要 3-10 分钟查询，取决于 IBKR 服务器流量。
  - 建议每周查询一次以保持期权列表更新。
  - 系统会自动清除过期的期权合约。

#### 7. 运行示例策略的演示脚本

```bash
python run_script.py # 更可靠
```

#### 8. 启动带 GUI 的系统

```bash
python run_server.py
```

## 策略开发

OTrader 提供全面的策略开发框架，内置投资组合管理、订单接口和持仓跟踪。
系统处理复杂的基础设施，让您专注于核心交易逻辑。

详细的策略开发指南，请查看 **[策略模板指南](doc_strategy.md)**，涵盖：

- 策略实现模式和最佳实践
- 持仓管理和风险控制
- 使用对冲引擎进行 Delta 对冲
- 订单执行和组合订单处理
- 市场数据集成和链管理
- 错误处理和日志策略
- 完整的策略示例和模板

## 许可证和致谢

本项目采用 MIT 许可证。

本项目深受 [vn.py](https://github.com/vnpy/vnpy) 启发，原作者为 **Xiaoyou Chen**，并在设计架构概念方面进行了重大扩展。

## 开发

### 开发环境设置

对于想要贡献代码的开发者：

#### 1. 安装开发依赖和使用代码质量工具

```bash
pip install -r requirements-dev.txt
```

```bash
pre-commit install
```

#### 2. 配置

代码质量设置配置在 `pyproject.toml` 中：
- 行长度：140 字符
- Python 版本：3.12+
- 核心模块启用类型检查
- 后端/前端模块使用宽松类型检查

## 支持

如果您发现任何错误或对设计有建议，欢迎联系我 **hang.zhengyang1010@gmail.com**。

## 免责声明

本项目仅用于教育和研究目的。它可能包含未发现的错误，尚未在生产环境中进行测试。强烈不建议连接到实盘账户。交易存在重大损失风险。使用风险自负。
