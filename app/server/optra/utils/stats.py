"""Small statistics shared by the layers above."""


def quantile(sorted_vals: list[float], q: float) -> float:
    """The q-quantile of values sorted ascending, linear between neighbours; 0 for none."""
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    w = pos - lo
    return sorted_vals[lo] * (1.0 - w) + sorted_vals[hi] * w
