"""Research: backtests in bulk, kept count of.

- batch: the one way the server runs backtests (entry_backtest --batch, through the day cache), and a backtest's
  round trips;
- report: strategies over days by the promotion measures, the one way to the holdout;
- combo: a combo from the lab run as ComboStrategy, and the hedge study;
- space: a strategy's parameters and the settings an experiment tries;
- ledger: experiments, trials, families and their budgets, the workers' tasks, and the results day by day
  (data/experiments.db), the one place that writes them;
- analysis: what the results say once the tries are counted (the numerics library's selection statistics);
- worker: runs the queued trials (python -m optra.research.worker).

The API (optra.api.experiments, the workers' side) and the tools (optra.tools.experiments, optra.tools.engine) only
pass requests in.
"""
