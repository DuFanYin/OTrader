"""Backtest chart: matplotlib 4-panel PnL/Delta/Theta/Gamma."""
from __future__ import annotations

import base64
import io
from typing import Any

# Lazy matplotlib (no GUI)
def _plt():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    return plt


def _x_for_downsampled(n_total: int, n_samples: int) -> list[float]:
    """X coords: n_samples span 0..n_total-1 (align PnL timesteps)."""
    if n_total <= 0 or n_samples <= 0:
        return []
    if n_samples >= n_total:
        return [float(i) for i in range(n_total)]
    return [i * (n_total - 1) / (n_samples - 1) for i in range(n_samples)] if n_samples > 1 else [0.0]


# 1200px display; 2x for retina
DISPLAY_WIDTH_PX = 1200
RETINA = 2
TARGET_WIDTH_PX = DISPLAY_WIDTH_PX * RETINA  # 2400
WIDTH_INCHES = 10
HEIGHT_INCHES = 9.0
DEFAULT_DPI = TARGET_WIDTH_PX // WIDTH_INCHES  # 240 → 2400px wide

# Smaller font sizes for a denser chart
FONT_TICK = 7
FONT_LABEL = 8
FONT_TITLE = 8


def _draw_chart_from_chart_data(
    plt,
    chart_data: dict[str, Any],
    width_inches: float,
    height_inches: float,
):
    """4-panel figure from C++ chart_data."""
    pnl = chart_data.get("pnl") or []
    x_greek = chart_data.get("x_greek") or []
    delta = chart_data.get("delta") or []
    theta = chart_data.get("theta") or []
    gamma = chart_data.get("gamma") or []

    n_pnl = len(pnl)
    if n_pnl == 0:
        fig, _ = plt.subplots(figsize=(1, 1))
        fig.patch.set_visible(False)
        return fig

    # x_greek for aligned panels + cross-day markers
    if x_greek and len(x_greek) == len(pnl):
        x_pnl = [float(x) for x in x_greek]
    else:
        x_pnl = list(range(n_pnl))

    fig, axes = plt.subplots(
        4,
        1,
        figsize=(width_inches, height_inches),
        sharex=True,
        gridspec_kw={"height_ratios": [1, 1, 1, 1], "hspace": 0.1},
    )
    fig.patch.set_facecolor("#0a0a0a")
    for ax in axes:
        ax.set_facecolor("#0a0a0a")
        ax.tick_params(colors="#9ca3af", labelsize=FONT_TICK)
        for spine in ax.spines.values():
            spine.set_color("#374151")

    lw_pnl = 1.2
    lw_greek = 0.9

    axes[0].plot(x_pnl, pnl, color="#3b82f6", linewidth=lw_pnl)
    axes[0].set_ylabel("PnL", color="#9ca3af", fontsize=FONT_LABEL)
    axes[0].set_title("PnL / Delta / Theta / Gamma", color="#6b7280", fontsize=FONT_TITLE)

    # Fallback: even X over PnL range
    def _plot_greek(ax, values: list[float], color: str):
        if not values:
            return
        if x_greek and len(x_greek) == len(values):
            ax.plot(x_greek, values, color=color, linewidth=lw_greek)
        else:
            x_ds = _x_for_downsampled(n_pnl, len(values))
            ax.plot(x_ds, values, color=color, linewidth=lw_greek)

    _plot_greek(axes[1], delta, "#10b981")
    _plot_greek(axes[2], theta, "#f59e0b")
    _plot_greek(axes[3], gamma, "#ef4444")

    # Draw vertical markers at cross-day boundaries (same indices as original timesteps).
    day_boundaries = chart_data.get("day_boundaries") or []
    for x_day in day_boundaries:
        try:
            x_val = float(x_day)
        except Exception:
            continue
        for ax in axes:
            ax.axvline(x_val, color="#4b5563", linewidth=0.6, linestyle="--", alpha=0.4)

    axes[1].set_ylabel("Delta", color="#9ca3af", fontsize=FONT_LABEL)
    axes[2].set_ylabel("Theta", color="#9ca3af", fontsize=FONT_LABEL)
    axes[3].set_ylabel("Gamma", color="#9ca3af", fontsize=FONT_LABEL)
    axes[3].set_xlabel("Timestep", color="#6b7280", fontsize=FONT_LABEL)
    return fig


def render_backtest_chart(
    chart_data: dict[str, Any],
    width_inches: float = WIDTH_INCHES,
    height_inches: float = HEIGHT_INCHES,
    dpi: int = DEFAULT_DPI,
) -> bytes:
    """Draw chart from C++ chart_data and return PNG bytes."""
    plt = _plt()
    if not chart_data or chart_data.get("pnl") is None:
        fig, _ = plt.subplots(figsize=(1, 1))
        fig.patch.set_visible(False)
        buf = io.BytesIO()
        plt.savefig(buf, format="png", bbox_inches="tight", pad_inches=0)
        plt.close()
        return buf.getvalue()

    fig = _draw_chart_from_chart_data(plt, chart_data, width_inches, height_inches)

    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=dpi, bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close()
    return buf.getvalue()


def render_backtest_chart_svg(
    chart_data: dict[str, Any],
    width_inches: float = WIDTH_INCHES,
    height_inches: float = HEIGHT_INCHES,
    **kwargs: Any,
) -> str:
    """Draw chart; return SVG string (resolution-independent)."""
    if not chart_data or chart_data.get("pnl") is None:
        return '<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"/>'

    plt = _plt()
    fig = _draw_chart_from_chart_data(plt, chart_data, width_inches, height_inches)
    buf = io.BytesIO()
    fig.savefig(buf, format="svg", bbox_inches="tight", facecolor=fig.get_facecolor())
    plt.close()
    return buf.getvalue().decode("utf-8")


def render_backtest_chart_base64(
    chart_data: dict[str, Any],
    **kwargs: Any,
) -> str:
    """Render chart to PNG; return base64."""
    png_bytes = render_backtest_chart(chart_data=chart_data, **kwargs)
    return base64.b64encode(png_bytes).decode("ascii")
