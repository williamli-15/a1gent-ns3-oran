from __future__ import annotations

from collections import Counter
from statistics import mean
from typing import Iterable, List, Optional, Sequence, TypeVar, Callable


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


T = TypeVar("T")


def recent(samples: Sequence[T], sim_time_ns: int, horizon_s: float, getter: Callable[[T], int] = lambda s: getattr(s, "sim_ns")) -> List[T]:
    """Return samples whose timestamp is within `horizon_s` seconds of `sim_time_ns`."""
    if sim_time_ns is None:
        return list(samples)
    cutoff = sim_time_ns - int(horizon_s * 1e9)
    return [s for s in samples if getter(s) >= cutoff]


def last(samples: Sequence[T], default: Optional[T] = None) -> Optional[T]:
    """Return the last sample by natural ordering (already sorted ascending)."""
    if not samples:
        return default
    return samples[-1]


def values(samples: Iterable[T], attr: str) -> List[float]:
    """Extract attribute values as floats."""
    vals: List[float] = []
    for s in samples:
        v = getattr(s, attr, None)
        if v is None:
            continue
        vals.append(float(v))
    return vals


def mean_or_none(vals: Sequence[float]) -> Optional[float]:
    return mean(vals) if vals else None


def percentile(vals: Sequence[float], pct: float) -> Optional[float]:
    """Compute percentile (0-100) using nearest-rank."""
    if not vals:
        return None
    if pct <= 0:
        return min(vals)
    if pct >= 100:
        return max(vals)
    sorted_vals = sorted(vals)
    k = (len(sorted_vals) - 1) * pct / 100.0
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return sorted_vals[f]
    d0 = sorted_vals[f] * (c - k)
    d1 = sorted_vals[c] * (k - f)
    return d0 + d1


def mean_recent(samples: Sequence[T], attr: str, sim_time_ns: int, horizon_s: float) -> Optional[float]:
    return mean_or_none(values(recent(samples, sim_time_ns, horizon_s), attr))


def percentile_recent(samples: Sequence[T], attr: str, sim_time_ns: int, horizon_s: float, pct: float) -> Optional[float]:
    return percentile(values(recent(samples, sim_time_ns, horizon_s), attr), pct)


def summarize_counter(counter: Counter, limit: int = 4) -> str:
    """
    Render the most common entries of a Counter as a compact string.
    """
    if not counter:
        return "none"
    items = counter.most_common(limit)
    parts = [f"{key}={value}" for key, value in items]
    remaining = sum(counter.values()) - sum(value for _, value in items)
    if remaining > 0:
        parts.append(f"+{remaining} other")
    return ", ".join(parts)
