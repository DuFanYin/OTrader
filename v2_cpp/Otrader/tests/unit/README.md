## Otrader Unit Tests (GTest)

This folder contains GoogleTest-based unit/regression tests for **correctness**, **dataflow contracts**,
and **concurrency safety**. Tests are built via `otrader_gtest_all` (see `CMakeLists.txt`) and run by
`./test_gtest.sh run`.

Below is a brief “what does this test validate” index for the **59** test cases currently in this
directory.

---

## `test_system_data_flow.cpp`

- **SystemDataFlow.StrategyUpdateRingRoundTrip**: Validates `ObjectPool<StrategyUpdateData>` + `MpscRing` pointer round-trip (produce → push → pop → consume).
- **SystemDataFlow.EventOrderPointerFlow**: Validates `Event` carrying `OrderData*` through a ring without type/payload corruption.

## `test_concurrency_design.cpp`

- **ConcurrencyDesign.MpscQueueNoLossWithObjectPool**: Multi-producer queue stress to validate no-loss behavior under concurrent push/pop with pooled objects.
- **ConcurrencyDesign.ObjectPoolCloseStopsNewAcquireAcrossThreads**: Closing an object pool stops new acquires across threads (shutdown contract).

## `test_concurrency_shutdown.cpp`

- **ConcurrencyShutdown.ProducerStopAndDrainWithoutLeak**: Producer stops + consumer drains; validates no leak and clean shutdown.
- **ConcurrencyShutdown.PoolCloseRejectsNewAcquireAfterStop**: After stop/close, pool rejects new acquires deterministically.

## `test_spsc_ring.cpp`

- **SpscRing.OrderNoLoss**: SPSC ring preserves FIFO order without loss.
- **SpscRing.FullReturnsFalse**: Full-ring push fails (backpressure behavior).
- **SpscRing.EmptyReturnsFalse**: Empty-ring pop fails.
- **SpscRing.CapacityAndSizeApprox**: Basic capacity/size invariants under typical usage.

## `test_mpsc_ring.cpp`

- **MpscRing.MultiProducerSingleConsumer**: MPSC ring correctness with multiple producers and one consumer.
- **MpscRing.FullAndEmpty**: Full/empty boundary behavior for MPSC ring.

## `test_object_pool.cpp`

- **ObjectPool.AcquireReleaseReuse**: Acquire/release reuse semantics (no allocation explosion).
- **ObjectPool.Emplace**: `emplace(...)` constructs objects with args correctly.
- **ObjectPool.ReleaseNullIsNoOp**: Releasing `nullptr` is safe.
- **ObjectPool.AfterCloseAcquireReturnsNull**: After `close()`, acquire returns `nullptr`.
- **ObjectPool.AfterCloseExistingAcquireCanStillRelease**: Objects acquired before close can still be released.
- **ObjectPool.DoubleReleaseSecondIsNoOp**: Double-release must not crash the process (second release treated as no-op).
- **ObjectPool.ConcurrentAcquireRelease**: Concurrent acquire/release under contention remains safe.

## `test_execution_engine_tracking.cpp`

- **ExecutionEngineTracking.RegisterAndRemoveOnTerminalStatuses**: Active order tracking removed on terminal statuses (`ALLTRADED/CANCELLED/REJECTED`) but retained while active.
- **ExecutionEngineTracking.CancelOrderRemovesTrackingEvenWithoutImpl**: `cancel_order()` removes tracking even without injected cancel impl.
- **ExecutionEngineTracking.RemoveStrategyTrackingClearsReverseMap**: Removing a strategy clears both active set and orderid→strategy reverse mapping.

## `test_position_engine_invariants.cpp`

- **PositionEngineInvariants.DuplicateTradeIdIsIgnored**: `tradeid` idempotency (duplicate trade event must not double-count).
- **PositionEngineInvariants.UnderlyingAvgCostAndRealizedPnlOnCloseAndReverse**: Avg-cost, realized PnL, and reversal handling for underlying positions.
- **PositionEngineInvariants.SingleLegOptionUsesMultiplierAndRealizedPnl**: Option multiplier (100) and realized PnL correctness for single-leg options.
- **PositionEngineInvariants.ComboOrderRoutesHeadAndLegTrades**: Combo meta routing: head vs leg fills update the expected components.

## `test_position_serialize_roundtrip.cpp`

- **PositionEngineDataflow.SerializeLoadRoundTripPreservesKeyFields**: `serialize_holding()` / `load_serialized_holding()` preserves key fields (qty/avg_cost/realized/multiplier) for recovery.

---

## Backtest dataflow / contracts (requires `backtest_engines`)

## `test_backtest_event_dispatch_alignment.cpp`

- **BacktestEventDispatch.OrderAndTradeUpdateExecutionAndPosition**: Backtest `EventEngine` dispatch updates `ExecutionEngine` cache + `PositionEngine` holding consistently.

## `test_backtest_event_out_of_order.cpp`

- **BacktestDataflow.TradeBeforeOrderWithStrategyMappingStillUpdatesHolding**: Out-of-order `Trade` before `Order` works when orderid→strategy mapping exists.
- **BacktestDataflow.OrderBeforeTradeStillUpdatesExecutionCache**: `Order` before `Trade` correctly updates execution cache and active tracking.

## `test_backtest_event_duplicate_trade.cpp`

- **BacktestDataflow.DuplicateTradeIdIsIdempotentAcrossDispatch**: Duplicate `tradeid` is idempotent across backtest dispatch (no double position, stable trade cache).

## `test_backtest_cancel_late_fill.cpp`

- **BacktestDataflow.CancelRemovesActiveButLateFillStillUpdatesHolding**: Cancel removes active tracking; late fills still update holdings deterministically.

## `test_backtest_terminal_status_regression.cpp`

- **BacktestDataflow.TerminalStatusDoesNotReAddActiveOnLaterNonTerminalUpdate**: Terminal status must not be “revived” back into active set by late non-terminal updates.

## `test_backtest_dispatch_order_contract.cpp`

- **BacktestDispatchContract.ExecutionAndPositionAreUpdatedBeforeStrategyCallbacks**: Dispatch contract: `ExecutionEngine`/`PositionEngine` are updated before strategy `on_order/on_trade` callbacks.

---

## Live runtime safety / concurrency (requires `engines_cpp`)

## `test_live_flow.cpp`

- **LiveFlow.PutTimerEventAndClose**: Minimal live main loop: `Timer` event can be enqueued and engine can close within timeout.
- **LiveFlow.PutMultipleTimerEvents**: Multiple timer events do not prevent teardown.

## `test_live_event_engine_safety.cpp`

- **LiveEventEngineSafety.PutBeforeStartDoesNotCrash**: `put_event` before `start()` is safe (payload rejected/released).
- **LiveEventEngineSafety.StopReturns**: `stop()` returns after queued payload bursts (drain/release safety).

## `test_live_shutdown_e2e.cpp`

- **LiveShutdownE2E.StopAfterQueuedPayloadsDrainsNoLeak**: Stop after large enqueue drains and releases payloads (no leak).
- **LiveShutdownE2E.StopWhileProducersRunningReturnsAndRejects**: Stop during concurrent producers returns and rejects safely.

## `test_live_close_fast.cpp`

- **LiveClose.EventEngine_Start_Returns**: Event engine start returns (no deadlock).
- **LiveClose.EventEngine_Stop_Returns**: Event engine stop returns (no deadlock).
- **LiveClose.MarketDataClient_Start_Returns_NoExternal**: MarketData client start is non-blocking without external processes.
- **LiveClose.MarketDataClient_Stop_Returns_NoExternal**: MarketData client stop is non-blocking without external processes.
- **LiveClose.MarketDataClient_Close_Returns_NoExternal**: MarketData client close returns without external processes.

## `test_live_event_engine_concurrency_mixed.cpp`

- **LiveEventEngineConcurrency.MultiProducerMixedPayloadsStopReturnsAndNoAcquireStarvation**: Concurrent producers enqueue mixed payloads; stop returns; pools remain usable (no starvation/leak).

## `test_live_event_engine_drop_backpressure.cpp`

- **LiveEventEngineBackpressure.QueueFullDropsPayloadsButEngineRemainsStoppable**: Under ring backpressure/drop, payloads are released and engine remains stoppable.

---

## ZMQ schema / message flow (requires `engines_cpp`)

## `test_zmq_schema_negative.cpp`

- **ZmqSchemaNegative.GatewayOrderDeserializeRejectsGarbage**: Rejects invalid gateway order payload.
- **ZmqSchemaNegative.GatewayTradeDeserializeRejectsGarbage**: Rejects invalid gateway trade payload.
- **ZmqSchemaNegative.GatewayRequestResponseRejectsGarbage**: Rejects invalid request/response payload.
- **ZmqSchemaNegative.GatewayConnectPayloadRejectsGarbage**: Rejects invalid connect payload.
- **ZmqSchemaNegative.MarketSnapshotRejectsGarbage**: Rejects invalid market snapshot payload.
- **ZmqSchemaNegative.MarketSubscribeUnsubscribeRejectsGarbage**: Rejects invalid subscribe/unsubscribe payload.

## `test_zmq_message_flow.cpp`

- **ZmqMessageFlow.GatewayOrderAndTradeRoundTrip**: Round-trip serialization for gateway order/trade messages.
- **ZmqMessageFlow.GatewayRequestResponseRoundTrip**: Round-trip serialization for gateway request/response messages.
- **ZmqMessageFlow.MarketSnapshotAndSubscribeRoundTrip**: Round-trip serialization for market snapshot + subscribe messages.

## `test_intent_routing.cpp`

- **IntentRouting.SendOrderReturnsOrderId**: Intent `SendOrder` returns a non-empty order id via routing.
- **IntentRouting.CancelOrderCallsHandler**: Intent `CancelOrder` invokes cancel handler.
- **IntentRouting.LogCallsHandler**: Intent `Log` routes to logging handler.

