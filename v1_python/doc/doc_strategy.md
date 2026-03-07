# Strategy Development Guide

## Overview

This guide explains how to build option trading strategies using the platform. The system provides a template that handles the complex parts (portfolio access, order management, position tracking) so you can focus on your trading logic.

## Getting Started

### Basic Strategy Structure

Every strategy inherits from `OptionStrategyTemplate` and must implement three core methods:

```python
class MyStrategy(OptionStrategyTemplate):
    author = "Your Name"
    parameters = ["risk_limit", "entry_delta", "width"] # parameters needed to be set
    variables = ["inited", "started", "error"]          # to record strategy runtime condition
    
    def on_init_logic(self) -> None:
        # select and subscribe needed chains
        pass
    
    def on_timer_logic(self) -> None:
        # Main trading logic
        pass
    
    def on_stop_logic(self) -> None:
        # Cleanup
        pass
```

### Strategy Lifecycle

1. **Initialization** (`on_init_logic`): Choose option chains, subscribe to market data, validate setup
2. **Running** (`on_timer_logic`): Execute your trading logic periodically
3. **Stopping** (`on_stop_logic`): Close positions and clean up

Create any other helper methods as you like

## Working with Market Data

### Portfolio and Chains

- **Portfolio**: Contains all option chains for a specific underlying (e.g., SPY, AAPL)
- **Chains**: Groups of options with the same expiration date
- **Underlying**: The stock or ETF the options are based on

```python
# select chains from portfolio with a range
chains = self.portfolio.get_chain_by_expiry(0, 7)  # 0-7 days to expiry
chains = self.portfolio.get_chain_by_expiry(0, 0)  # 0 DTE chain

# Subscribe to option chains
self.subscribe_chains(["SPY_20241220"])

# Get a specific chain
chain = self.get_chain("SPY_20241220")

```

### Option Data

Each option has:
- Strike price and expiration
- Current market price and Greeks (delta, gamma, theta, vega)
- Implied volatility

```python
# Get ATM call and put
atm_strike = chain.atm_index
call_option = chain.calls.get(atm_strike)
put_option = chain.puts.get(atm_strike)
```

## Placing Orders

### Underlying Stock Orders

```python
# Buy or sell the underlying stock
self.underlying_order(direction=Direction.LONG, price=0, volume=100, order_type=OrderType.MARKET)  # Market order
self.underlying_order(direction=Direction.SHORT, price=450.00, volume=100, order_type=OrderType.LIMIT)  # Limit order
```

### Single Option Orders

```python
# Buy or sell individual options
self.option_order(option_data=call_option, direction=Direction.LONG, price=0, volume=1, order_type=OrderType.MARKET)  # Market order
self.option_order(option_data=put_option, direction=Direction.SHORT, price=1.80, volume=1, order_type=OrderType.LIMIT)  # Limit order
```

### Combo Orders

```python
# Create option combinations
straddle_data = {
    "call": call_option,
    "put": put_option
}

# Straddle: Buy call + put at same strike
self.combo_order(combo_type=ComboType.STRADDLE, option_data=straddle_data, direction=Direction.LONG, price=0, volume=1, order_type=OrderType.MARKET)  # Market order

iron_condor_data = {
    "put_lower": put_lower,
    "put_upper": put_upper,
    "call_lower": call_lower,
    "call_upper": call_upper
}
# Iron Condor: Sell call spread + sell put spread
self.combo_order(combo_type=ComboType.IRON_CONDOR, option_data=iron_condor_data, direction=Direction.SHORT, price=1.20, volume=1, order_type=OrderType.LIMIT)  # Limit order
```

## Position Management

### Checking Your Positions

```python
# Get current holdings
holding = self.get_holding_data()

# Check P&L
total_pnl = holding["summary"]["pnl"]
unrealized_pnl = holding["summary"]["unrealized_pnl"]

# Check Greeks
total_delta = holding["summary"]["delta"]
total_theta = holding["summary"]["theta"]
```

### Closing Positions

```python
# Close all positions
self.close_all_strategy_positions()

# Acquire specific combo position
combo_position = self.holding.comboPositions

# close specific position
self.close_combo_position(combo_position)

# close one type of position
self.close_all_combo_positions()

# close one type of position
self.close_underlying_position()

# Cancel specific orders
self.cancel_order("12345")
self.cancel_all()
```

## Risk Management

### Hedging

Register for automatic delta hedging:

```python
# Auto-hedge to keep delta near zero
self.register_hedging(
    timer_trigger=5,
    delta_target=0,
    delta_range=50
)

# Unregister when no longer needed
self.unregister_hedging()
```

### Error Handling

```python
def on_timer_logic(self) -> None:
    if self.error:
        return  # Exit if there's an error
    
    try:
        # Your trading logic here
        pass
    except Exception as e:
        self.set_error(f"Trading error: {e}")
```

## Best Practices

1. **Always check for errors** at the start of `on_timer_logic`
2. **Validate data** before trading (check if chains exist, strikes are available)
3. **Use meaningful parameter names** and provide sensible defaults
4. **Log important events** for debugging and monitoring
5. **Handle exceptions gracefully** with `set_error()`
6. **Monitor your Greeks** and use hedging when appropriate
7. **Type safe** Run mypy to ensure your strategy code is type safe

## What the Platform Handles

- Market data updates and portfolio management
- Order routing and execution
- Position tracking and P&L calculation
- Risk management and hedging
- Error handling and logging
- UI updates and monitoring

## What You Focus On

- Choosing the right option chains
- Developing entry and exit signals
- Managing position sizing and risk
- Implementing your trading strategy logic

This template system lets you focus on what matters most: your trading strategy, while the platform handles all the technical complexity.
