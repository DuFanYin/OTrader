# 策略开发指南

## 概述

本指南解释如何本平台构建期权交易策略。系统提供了一个模板，处理复杂部分（投资组合访问、订单管理、持仓跟踪），让您专注于交易逻辑。

## 开始使用

### 基本策略结构

每个策略都继承自`OptionStrategyTemplate`，必须实现三个核心方法：

```python
class MyStrategy(OptionStrategyTemplate):
    author = "您的姓名"
    parameters = ["risk_limit", "entry_delta", "width"] # 需要设置的参数
    variables = ["inited", "started", "error"]          # 记录策略运行时状态
    
    def on_init_logic(self) -> None:
        # 选择并订阅需要的链
        pass
    
    def on_timer_logic(self) -> None:
        # 主要交易逻辑
        pass
    
    def on_stop_logic(self) -> None:
        # 清理
        pass
```

### 策略生命周期

1. **初始化**（`on_init_logic`）：选择期权链，订阅市场数据，验证设置
2. **运行**（`on_timer_logic`）：定期执行您的交易逻辑
3. **停止**（`on_stop_logic`）：平仓和清理

根据需要创建任何其他辅助方法

## 使用市场数据

### 投资组合和链

- **投资组合**：包含特定标的（如SPY、AAPL）的所有期权链
- **链**：具有相同到期日的期权组
- **标的**：期权所基于的股票或ETF

```python
# 从投资组合中选择一定范围内的链
chains = self.portfolio.get_chain_by_expiry(0, 7)  # 0-7天到期
chains = self.portfolio.get_chain_by_expiry(0, 0)  # 0 DTE链

# 订阅期权链
self.subscribe_chains(["SPY_20241220"])

# 获取特定链
chain = self.get_chain("SPY_20241220")

```

### 期权数据

每个期权都有：
- 行权价和到期日
- 当前市场价格和希腊字母（delta、gamma、theta、vega）
- 隐含波动率

```python
# 获取ATM看涨和看跌期权
atm_strike = chain.atm_index
call_option = chain.calls.get(atm_strike)
put_option = chain.puts.get(atm_strike)
```

## 下单

### 标的股票订单

```python
# 买入或卖出标的股票
self.underlying_order(direction=Direction.LONG, price=0, volume=100, order_type=OrderType.MARKET)  # 市价单
self.underlying_order(direction=Direction.SHORT, price=450.00, volume=100, order_type=OrderType.LIMIT)  # 限价单
```

### 单期权订单

```python
# 买入或卖出单个期权
self.option_order(option_data=call_option, direction=Direction.LONG, price=0, volume=1, order_type=OrderType.MARKET)  # 市价单
self.option_order(option_data=put_option, direction=Direction.SHORT, price=1.80, volume=1, order_type=OrderType.LIMIT)  # 限价单
```

### 组合订单

```python
# 创建期权组合
straddle_data = {
    "call": call_option,
    "put": put_option
}

# 跨式：买入相同行权价的看涨+看跌
self.combo_order(combo_type=ComboType.STRADDLE, option_data=straddle_data, direction=Direction.LONG, price=0, volume=1, order_type=OrderType.MARKET)  # 市价单

iron_condor_data = {
    "put_lower": put_lower,
    "put_upper": put_upper,
    "call_lower": call_lower,
    "call_upper": call_upper
}
# 铁鹰式：卖出看涨价差+卖出看跌价差
self.combo_order(combo_type=ComboType.IRON_CONDOR, option_data=iron_condor_data, direction=Direction.SHORT, price=1.20, volume=1, order_type=OrderType.LIMIT)  # 限价单
```

## 持仓管理

### 检查您的持仓

```python
# 获取当前持仓
holding = self.get_holding_data()

# 检查盈亏
total_pnl = holding["summary"]["pnl"]
unrealized_pnl = holding["summary"]["unrealized_pnl"]

# 检查希腊字母
total_delta = holding["summary"]["delta"]
total_theta = holding["summary"]["theta"]
```

### 关闭持仓

```python
# 关闭所有持仓
self.close_all_strategy_positions()

# 获取特定组合持仓
combo_position = self.holding.comboPositions

# 关闭特定持仓
self.close_combo_position(combo_position)

# 关闭一种类型的持仓
self.close_all_combo_positions()

# 关闭一种类型的持仓
self.close_underlying_position()

# 取消特定订单
self.cancel_order("12345")
self.cancel_all()
```


## 风险管理

### 对冲

注册自动Delta对冲：

```python
# 自动对冲保持Delta接近零
self.register_hedging(
    timer_trigger=5,
    delta_target=0,
    delta_range=50
)

# 不再需要时取消注册
self.unregister_hedging()
```

### 错误处理

```python
def on_timer_logic(self) -> None:
    if self.error:
        return  # 如果有错误则退出
    
    try:
        # 您的交易逻辑在这里
        pass
    except Exception as e:
        self.set_error(f"交易错误: {e}")
```

## 最佳实践

1. **始终在`on_timer_logic`开始时检查错误**
2. **交易前验证数据**（检查链是否存在，行权价是否可用）
3. **使用有意义的参数名称**并提供合理的默认值
4. **记录重要事件**用于调试和监控
5. **优雅处理异常**使用`set_error()`
6. **监控您的希腊字母**并在适当时使用对冲
7. **类型安全** 运行mypy确保您的策略代码类型安全

## 平台处理的内容

- 市场数据更新和投资组合管理
- 订单路由和执行
- 持仓跟踪和盈亏计算
- 风险管理和对冲
- 错误处理和日志
- UI更新和监控

## 您专注的内容

- 选择合适的期权链
- 开发入场和出场信号
- 管理持仓规模和风险
- 实现您的交易策略逻辑

这个模板系统让您专注于最重要的内容：您的交易策略，而平台处理所有技术复杂性。
