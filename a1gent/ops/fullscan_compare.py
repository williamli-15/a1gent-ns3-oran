#!/usr/bin/env python3
"""
Database comparison for two ns-3 O-RAN SQLite databases.

Scope:
  - Scan tables, columns, and rows in the two databases.
  - Compute distribution stats for numeric columns and compare baseline vs app.
  - For tables with a time column (simulationtime), also compute per-second deltas
    to highlight time-localized shifts.
  - Not a complete explanation of agent behavior.
  - For local decisions, sequencing, and emerging multi-agent behavior, use summarize_run.py, orch_ops_scan.py, and the orchestrator log.

Usage:


python -m a1gent.ops.fullscan_compare \
  oran-repository-baseline-60-150-300.db \
  oran-repository-agent-run-60-150-300.db \
  --phases 0 60 150 300 \
  --emerg_t0_s 60 --emerg_t1_s 150 \
  --ho_win_s 10 \
  --cio_win_s 10 \
  --out fullscan_report_agent_run_60-150-300.md
"""

from __future__ import annotations

import argparse
import math
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import pandas as pd

import re


TIME_COL_CANDIDATES = ("simulationtime", "sim_ns", "time_ns", "t_ns")


@dataclass
class ColStats:
    n: int
    mean: float
    std: float
    p10: float
    p50: float
    p90: float
    mn: float
    mx: float


def _is_numeric_series(s: pd.Series) -> bool:
    return pd.api.types.is_numeric_dtype(s)


def _safe_float(x: object, default: float = float("nan")) -> float:
    try:
        return float(x)
    except Exception:
        return default


def _percentile(vals: Sequence[float], q: float) -> float:
    # q in [0,100]
    if not vals:
        return float("nan")
    a = pd.Series(vals).dropna().to_numpy()
    if a.size == 0:
        return float("nan")
    return float(pd.Series(a).quantile(q / 100.0, interpolation="linear"))


def _stats_numeric(series: pd.Series) -> Optional[ColStats]:
    s = pd.to_numeric(series, errors="coerce").dropna()
    if s.empty:
        return None
    vals = s.to_numpy()
    n = int(vals.size)
    mean = float(s.mean())
    std = float(s.std(ddof=0)) if n > 1 else 0.0
    p10 = float(pd.Series(vals).quantile(0.10))
    p50 = float(pd.Series(vals).quantile(0.50))
    p90 = float(pd.Series(vals).quantile(0.90))
    mn = float(s.min())
    mx = float(s.max())
    return ColStats(n=n, mean=mean, std=std, p10=p10, p50=p50, p90=p90, mn=mn, mx=mx)


def _effect_size(a: ColStats, b: ColStats) -> float:
    # Cohen-ish d using pooled std (population std)
    s1, s2 = a.std, b.std
    if (s1 <= 1e-12) and (s2 <= 1e-12):
        return 0.0
    pooled = math.sqrt((s1 * s1 + s2 * s2) / 2.0)
    if pooled <= 1e-12:
        return 0.0
    return (b.mean - a.mean) / pooled


def _list_tables(con: sqlite3.Connection) -> List[str]:
    rows = con.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
    ).fetchall()
    return [r[0] for r in rows]


def _pragma_cols(con: sqlite3.Connection, table: str) -> List[Tuple[str, str]]:
    rows = con.execute(f"PRAGMA table_info({table})").fetchall()
    # (cid, name, type, notnull, dflt_value, pk)
    return [(r[1], str(r[2] or "")) for r in rows]


def _detect_time_col(df: pd.DataFrame) -> Optional[str]:
    for c in TIME_COL_CANDIDATES:
        if c in df.columns:
            return c
    return None


def _time_to_seconds(series: pd.Series) -> pd.Series:
    # Heuristic: if values look like ns timestamps (>1e6), convert to seconds
    s = pd.to_numeric(series, errors="coerce")
    vmax = _safe_float(s.max(), 0.0)
    if vmax > 1e6:
        return s / 1e9
    return s


def _phase_masks(t_s: pd.Series, phase_edges_s: List[float]) -> List[Tuple[str, pd.Series]]:
    # edges like [0,100,200,300] => phases: [0,100), [100,200), [200,300]
    out = []
    for i in range(len(phase_edges_s) - 1):
        a = phase_edges_s[i]
        b = phase_edges_s[i + 1]
        name = f"{int(a)}-{int(b)}s"
        out.append((name, (t_s >= a) & (t_s < b)))
    return out


def _md_table(headers: List[str], rows: List[List[str]]) -> str:
    if not rows:
        return "_(no rows)_\n"
    out = []
    out.append("| " + " | ".join(headers) + " |")
    out.append("| " + " | ".join(["---"] * len(headers)) + " |")
    for r in rows:
        out.append("| " + " | ".join(r) + " |")
    return "\n".join(out) + "\n"


def _fmt(x: float, digits: int = 3) -> str:
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "-"
    return f"{x:.{digits}f}"


def _fmt_pct(x: float, digits: int = 1) -> str:
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "-"
    return f"{x*100:.{digits}f}%"


def _ns(s: float) -> int:
    return int(s * 1e9)

# ===== Capability scan (topology/phase aware) =====
def _q(con, sql, params=()):
    return pd.read_sql_query(sql, con, params=params)

def _pctl_from_series(s: pd.Series, q: float) -> float:
    s = pd.to_numeric(s, errors="coerce").dropna()
    if s.empty:
        return float("nan")
    return float(s.quantile(q))

def _fmtf(x, d=3):
    return "-" if (x is None or (isinstance(x, float) and math.isnan(x))) else f"{x:.{d}f}"

def cap_emergency_ue_metrics(con, t0_s, t1_s, modulo, offset, dl_out_th):
    # UE IDs in DB are 1..N (nodeid). emergency selection matches UE index i=ueid-1.
    # (ueid-1 + offset) % modulo == 0  <=> (ueid + (offset-1)) % modulo == 0
    # easiest: use python to build set, then IN (...)
    df_ue = _q(con, "SELECT nodeid AS ueid FROM lteue ORDER BY nodeid;")
    ueids = [int(x) for x in df_ue["ueid"].tolist()]
    ueids_sorted = sorted(ueids)
    # map: position in sorted list -> "i"
    emer = []
    for i, u in enumerate(ueids_sorted):
        if ((i + offset) % modulo) == 0:
            emer.append(u)
    if not emer:
        return {}

    # query PDCP samples for these UEs in [t0,t1]
    # avoid giant IN clause if needed; for your 36 UEs it's fine
    in_list = "(" + ",".join(str(u) for u in emer) + ")"
    df = _q(con, f"""
        SELECT ueid, dlmbps
        FROM lteue_pdcp_tp
        WHERE simulationtime BETWEEN ? AND ?
          AND ueid IN {in_list}
    """, (int(t0_s*1e9), int(t1_s*1e9)))

    if df.empty:
        return {}

    dl = df["dlmbps"]
    return {
        "n_samples": int(len(dl)),
        "dl_p10": _pctl_from_series(dl, 0.10),
        "dl_p05": _pctl_from_series(dl, 0.05),
        "dl_mean": float(pd.to_numeric(dl, errors="coerce").mean()),
        "outage_ratio": float((pd.to_numeric(dl, errors="coerce") < dl_out_th).mean()),
    }

def _select_ueids_by_modulo(con, modulo: int, offset: int) -> List[int]:
    df_ue = _q(con, "SELECT nodeid AS ueid FROM lteue ORDER BY nodeid;")
    ueids = sorted(int(x) for x in df_ue["ueid"].tolist())
    sel = []
    for i, u in enumerate(ueids):
        if modulo > 0 and ((i + offset) % modulo) == 0:
            sel.append(u)
    return sel

def cap_group_ue_qoe(con, t0_s: float, t1_s: float, ueids: List[int], dl_out_th: float):
    if not ueids:
        return {}
    in_list = "(" + ",".join(str(u) for u in ueids) + ")"
    df = _q(con, f"""
        SELECT dlmbps
        FROM lteue_pdcp_tp
        WHERE simulationtime BETWEEN ? AND ?
          AND ueid IN {in_list}
    """, (_ns(t0_s), _ns(t1_s)))

    if df.empty:
        return {}
    dl = pd.to_numeric(df["dlmbps"], errors="coerce").dropna()
    if dl.empty:
        return {}
    return {
        "n_samples": int(len(dl)),
        "dl_mean": float(dl.mean()),
        "dl_p10": float(dl.quantile(0.10)),
        "dl_p05": float(dl.quantile(0.05)),
        "outage_ratio": float((dl < dl_out_th).mean()),
    }


def cap_all_ue_tail_qoe(con, t0_s: float, t1_s: float, dl_out_th: float):
    df = _q(con, """
        SELECT dlmbps
        FROM lteue_pdcp_tp
        WHERE simulationtime BETWEEN ? AND ?
    """, (_ns(t0_s), _ns(t1_s)))

    if df.empty:
        return {}

    dl = pd.to_numeric(df["dlmbps"], errors="coerce").dropna()
    if dl.empty:
        return {}

    return {
        "n_samples": int(len(dl)),
        "dl_mean": float(dl.mean()),
        "dl_p10": float(dl.quantile(0.10)),
        "dl_p05": float(dl.quantile(0.05)),
        "outage_ratio": float((dl < dl_out_th).mean()),
    }


def cap_hotcell_overload(con, t0_s, t1_s, hot_th=0.9):
    df = _q(con, """
        SELECT cellid, dlutil
        FROM lteenbprbutilization
        WHERE simulationtime BETWEEN ? AND ?
    """, (int(t0_s*1e9), int(t1_s*1e9)))
    if df.empty:
        return {}

    # each row corresponds to 1s window => count rows as seconds
    df["is_over"] = df["dlutil"] >= hot_th
    agg = df.groupby("cellid")["is_over"].sum().sort_values(ascending=False)
    if agg.empty:
        return {}
    top_cell = int(agg.index[0])
    return {
        "top_hot_cell": top_cell,
        "overload_seconds_top1": int(agg.iloc[0]),
        "overload_seconds_top2_sum": int(agg.iloc[:2].sum()) if len(agg) >= 2 else int(agg.iloc[0]),
    }

def cap_cell_metrics(con, cellid: int, t0_s: float, t1_s: float, hot_th: float = 0.9):
    t0 = int(t0_s * 1e9)
    t1 = int(t1_s * 1e9)

    # PRB util
    prb = _q(con, """
        SELECT dlutil, ulutil
        FROM lteenbprbutilization
        WHERE cellid = ? AND simulationtime BETWEEN ? AND ?
    """, (cellid, t0, t1))

    # UE count
    uec = _q(con, """
        SELECT ue_count
        FROM lteenb_uecount
        WHERE cellid = ? AND simulationtime BETWEEN ? AND ?
    """, (cellid, t0, t1))

    # Scheduled throughput
    tp = _q(con, """
        SELECT dl_mbps, ul_mbps
        FROM lteenb_sched_tp
        WHERE cellid = ? AND simulationtime BETWEEN ? AND ?
    """, (cellid, t0, t1))

    out = {}

    if not prb.empty:
        dl = pd.to_numeric(prb["dlutil"], errors="coerce")
        out["prb_dl_mean"] = float(dl.mean())
        out["prb_dl_p90"]  = float(dl.quantile(0.90))
        out["overload_seconds"] = int((dl >= hot_th).sum())

    if not uec.empty:
        u = pd.to_numeric(uec["ue_count"], errors="coerce")
        out["ue_count_mean"] = float(u.mean())
        out["ue_count_p90"]  = float(u.quantile(0.90))

    if not tp.empty:
        d = pd.to_numeric(tp["dl_mbps"], errors="coerce")
        out["sched_dl_mean"] = float(d.mean())
        out["sched_dl_p10"]  = float(d.quantile(0.10))

    return out


def cap_action_counts(con):
    out = {}
    for table in ("lmaction", "lmcommand", "terminatorcommand"):
        try:
            df = _q(con, f"SELECT COUNT(*) AS n FROM {table};")
            out[table] = int(df["n"].iloc[0])
        except Exception:
            out[table] = None
    return out



def _mean_series(df: pd.DataFrame, col: str) -> float:
    s = pd.to_numeric(df[col], errors="coerce").dropna()
    return float(s.mean()) if not s.empty else float("nan")

def _pctl_series(df: pd.DataFrame, col: str, q: float) -> float:
    s = pd.to_numeric(df[col], errors="coerce").dropna()
    return float(s.quantile(q)) if not s.empty else float("nan")

def find_cio_src_cells(con, t0_s: float, t1_s: float) -> List[int]:
    """
    Find CIO actions in [t0,t1] by parsing lmaction.description:
      'SET_CIO cell=<src> neighbor=<nbr> offset=<x> dB'
    Return unique src cell ids.
    """
    try:
        df = _q(con, """
            SELECT simulationtime, description
            FROM lmaction
            WHERE simulationtime BETWEEN ? AND ?
              AND description LIKE 'SET_CIO %'
            ORDER BY simulationtime
        """, (_ns(t0_s), _ns(t1_s)))
    except Exception:
        return []

    srcs = set()
    for desc in df.get("description", []):
        m = re.search(r"\bcell=(\d+)\b", str(desc))
        if m:
            srcs.add(int(m.group(1)))
    return sorted(srcs)

def find_cio_pairs(con, t0_s: float, t1_s: float):
    """
    Parse lmaction.description in [t0,t1]:
      'SET_CIO cell=<src> neighbor=<nbr> offset=<x> dB'
    Return list of dicts: {t_s, src, nbr, offset_db}
    """
    try:
        df = _q(con, """
            SELECT simulationtime, description
            FROM lmaction
            WHERE simulationtime BETWEEN ? AND ?
              AND description LIKE 'SET_CIO %'
            ORDER BY simulationtime
        """, (_ns(t0_s), _ns(t1_s)))
    except Exception:
        return []

    out = []
    for row in df.itertuples(index=False):
        desc = str(getattr(row, "description", "") or "")
        # src cell
        m_src = re.search(r"\bcell=(\d+)\b", desc)
        # neighbor cell
        m_nbr = re.search(r"\bneighbor=(\d+)\b", desc)
        # offset
        m_off = re.search(r"\boffset=([+-]?\d+(?:\.\d+)?)\b", desc)

        if not (m_src and m_nbr):
            continue

        src = int(m_src.group(1))
        nbr = int(m_nbr.group(1))
        off = float(m_off.group(1)) if m_off else float("nan")

        t_ns = int(getattr(row, "simulationtime", 0) or 0)
        out.append({
            "t_s": t_ns / 1e9,
            "src": src,
            "nbr": nbr,
            "offset_db": off,
        })
    return out


def _clip_window(t0_s: float, t1_s: float, sim_end_s: float) -> Tuple[float, float]:
    t0_s = max(0.0, t0_s)
    t1_s = min(sim_end_s, t1_s)
    if t1_s <= t0_s:
        return (t0_s, t0_s)
    return (t0_s, t1_s)

def _cell_kpis(con, cellid: int, t0_s: float, t1_s: float, hot_th: float = 0.9) -> Dict[str, float]:
    # PRB util
    df_prb = _q(con, """
        SELECT dlutil
        FROM lteenbprbutilization
        WHERE cellid = ? AND simulationtime BETWEEN ? AND ?
    """, (int(cellid), _ns(t0_s), _ns(t1_s)))

    # UE count
    try:
        df_uc = _q(con, """
            SELECT ue_count
            FROM lteenb_uecount
            WHERE cellid = ? AND simulationtime BETWEEN ? AND ?
        """, (int(cellid), _ns(t0_s), _ns(t1_s)))
    except Exception:
        df_uc = pd.DataFrame()

    # Scheduled DL throughput
    try:
        df_tp = _q(con, """
            SELECT dl_mbps
            FROM lteenb_sched_tp
            WHERE cellid = ? AND simulationtime BETWEEN ? AND ?
        """, (int(cellid), _ns(t0_s), _ns(t1_s)))
    except Exception:
        df_tp = pd.DataFrame()

    out = {
        "prb_dl_mean": _mean_series(df_prb, "dlutil"),
        "prb_dl_p90":  _pctl_series(df_prb, "dlutil", 0.90),
        "overload_seconds": float((pd.to_numeric(df_prb.get("dlutil"), errors="coerce") >= hot_th).sum()) if not df_prb.empty else float("nan"),
        "ue_count_mean": _mean_series(df_uc, "ue_count") if not df_uc.empty else float("nan"),
        "ue_count_p90":  _pctl_series(df_uc, "ue_count", 0.90) if not df_uc.empty else float("nan"),
        "sched_dl_mean": _mean_series(df_tp, "dl_mbps") if not df_tp.empty else float("nan"),
        "sched_dl_p10":  _pctl_series(df_tp, "dl_mbps", 0.10) if not df_tp.empty else float("nan"),
    }
    return out

def cap_cio_before_after(con, cio_events: List[Dict], sim_end_s: float, win_s: float = 10.0, hot_th: float = 0.9):
    """
    For each CIO event at time t_s:
      BEFORE: [t-win, t)
      AFTER : [t, t+win]
    Returns per-event rows + aggregate mean(after-before).
    """
    if not cio_events:
        return {"events": [], "agg": {}}

    keys = ["prb_dl_mean","prb_dl_p90","overload_seconds","ue_count_mean","ue_count_p90","sched_dl_mean","sched_dl_p10"]

    per_evt = []
    agg_sum = {k: 0.0 for k in keys}
    agg_cnt = {k: 0 for k in keys}

    for ev in cio_events:
        t = float(ev["t_s"])
        src = int(ev["src"])
        nbr = int(ev["nbr"])
        off = ev.get("offset_db", float("nan"))

        b0, b1 = _clip_window(t - win_s, t, sim_end_s)
        a0, a1 = _clip_window(t, t + win_s, sim_end_s)
        if b1 <= b0 or a1 <= a0:
            continue

        src_b = _cell_kpis(con, src, b0, b1, hot_th=hot_th)
        src_a = _cell_kpis(con, src, a0, a1, hot_th=hot_th)
        nbr_b = _cell_kpis(con, nbr, b0, b1, hot_th=hot_th)
        nbr_a = _cell_kpis(con, nbr, a0, a1, hot_th=hot_th)

        # store deltas (after-before)
        row = {
            "t_s": t,
            "src": src,
            "nbr": nbr,
            "offset_db": off,
            "window_s": win_s,
            "src_delta": {k: (src_a[k] - src_b[k]) for k in keys},
            "nbr_delta": {k: (nbr_a[k] - nbr_b[k]) for k in keys},
        }
        per_evt.append(row)

        # aggregate only src deltas (you can also aggregate nbr if you want)
        for k in keys:
            v = row["src_delta"][k]
            if not math.isnan(v):
                agg_sum[k] += float(v)
                agg_cnt[k] += 1

    agg = {}
    for k in keys:
        agg[k] = (agg_sum[k] / agg_cnt[k]) if agg_cnt[k] > 0 else float("nan")

    return {"events": per_evt, "agg": agg}



def cap_cio_targeted_impact(con, t0_s: float, t1_s: float, src_cell: int, hot_th: float = 0.9) -> Dict[str, float]:
    """
    Metrics for a specific source cell (CIO src):
      - PRB dlutil: mean, p90, overload_seconds(dlutil>=hot_th)
      - UE count: mean, p90 (if table exists)
      - Scheduled DL throughput: mean, p10 (if table exists)
    """
    out: Dict[str, float] = {}

    # PRB util
    df_prb = _q(con, """
        SELECT dlutil
        FROM lteenbprbutilization
        WHERE simulationtime BETWEEN ? AND ?
          AND cellid = ?
    """, (_ns(t0_s), _ns(t1_s), int(src_cell)))
    out["prb_dl_mean"] = _mean_series(df_prb, "dlutil")
    out["prb_dl_p90"]  = _pctl_series(df_prb, "dlutil", 0.90)
    if not df_prb.empty:
        out["overload_seconds"] = float((pd.to_numeric(df_prb["dlutil"], errors="coerce") >= hot_th).sum())
    else:
        out["overload_seconds"] = float("nan")

    # UE count (optional table)
    try:
        df_uc = _q(con, """
            SELECT ue_count
            FROM lteenb_uecount
            WHERE simulationtime BETWEEN ? AND ?
              AND cellid = ?
        """, (_ns(t0_s), _ns(t1_s), int(src_cell)))
        out["ue_count_mean"] = _mean_series(df_uc, "ue_count")
        out["ue_count_p90"]  = _pctl_series(df_uc, "ue_count", 0.90)
    except Exception:
        out["ue_count_mean"] = float("nan")
        out["ue_count_p90"]  = float("nan")

    # Scheduled throughput (optional table)
    try:
        df_tp = _q(con, """
            SELECT dl_mbps
            FROM lteenb_sched_tp
            WHERE simulationtime BETWEEN ? AND ?
              AND cellid = ?
        """, (_ns(t0_s), _ns(t1_s), int(src_cell)))
        out["sched_dl_mean"] = _mean_series(df_tp, "dl_mbps")
        out["sched_dl_p10"]  = _pctl_series(df_tp, "dl_mbps", 0.10)
    except Exception:
        out["sched_dl_mean"] = float("nan")
        out["sched_dl_p10"]  = float("nan")

    return out

def _ue_window_mean(con, table: str, ueid: int, col: str, t0_ns: int, t1_ns: int) -> float:
    try:
        df = _q(con, f"""
            SELECT {col}
            FROM {table}
            WHERE simulationtime BETWEEN ? AND ?
              AND ({'ueid' if table == 'lteue_pdcp_tp' else 'nodeid'}) = ?
        """, (t0_ns, t1_ns, int(ueid)))
    except Exception:
        return float("nan")
    return _mean_series(df, col)

def cap_ho_winrate(con, t0_s: float, t1_s: float, win_s: float = 10.0, eps: float = 1e-6) -> Dict[str, float]:
    """
    For each HO EndOk in [t0,t1], compute UE dlmbps & sinr_db mean before vs after:
      before: [t-win, t)
      after : [t, t+win]
    Return win-rates + avg deltas.
    """
    df = _q(con, """
        SELECT ueid, srcCell, dstCell, simulationtime
        FROM lte_ho_events
        WHERE simulationtime BETWEEN ? AND ?
          AND event = 'EndOk'
        ORDER BY simulationtime
    """, (_ns(t0_s), _ns(t1_s)))

    if df.empty:
        return {"n_ho": 0, "dl_winrate": float("nan"), "sinr_winrate": float("nan"),
                "dl_avg_delta": float("nan"), "sinr_avg_delta": float("nan")}

    n = 0
    dl_wins = 0
    sinr_wins = 0
    dl_deltas = []
    sinr_deltas = []

    for row in df.itertuples(index=False):
        u = int(row.ueid)
        t = int(row.simulationtime)
        before0 = max(0, t - _ns(win_s))
        before1 = max(0, t - 1)              # exclude exact t
        after0  = t
        after1  = t + _ns(win_s)

        dl_before = _ue_window_mean(con, "lteue_pdcp_tp", u, "dlmbps", before0, before1)
        dl_after  = _ue_window_mean(con, "lteue_pdcp_tp", u, "dlmbps", after0, after1)
        sinr_before = _ue_window_mean(con, "lteuesinr", u, "sinr_db", before0, before1)
        sinr_after  = _ue_window_mean(con, "lteuesinr", u, "sinr_db", after0, after1)

        if not (math.isnan(dl_before) or math.isnan(dl_after)):
            dl_d = dl_after - dl_before
            dl_deltas.append(dl_d)
            if dl_d > eps:
                dl_wins += 1

        if not (math.isnan(sinr_before) or math.isnan(sinr_after)):
            s_d = sinr_after - sinr_before
            sinr_deltas.append(s_d)
            if s_d > eps:
                sinr_wins += 1

        n += 1

    dl_winrate = (dl_wins / n) if n > 0 else float("nan")
    sinr_winrate = (sinr_wins / n) if n > 0 else float("nan")

    def _qtl(xs, q):
        xs = [x for x in xs if not math.isnan(x)]
        if not xs:
            return float("nan")
        return float(pd.Series(xs).quantile(q))

    out = {
        "n_ho": n,
        "dl_winrate": float(dl_winrate),
        "sinr_winrate": float(sinr_winrate),
        "dl_avg_delta": float(sum(dl_deltas) / len(dl_deltas)) if dl_deltas else float("nan"),
        "sinr_avg_delta": float(sum(sinr_deltas) / len(sinr_deltas)) if sinr_deltas else float("nan"),
        "dl_p50_delta": _qtl(dl_deltas, 0.50),
        "dl_p10_delta": _qtl(dl_deltas, 0.10),
        "sinr_p50_delta": _qtl(sinr_deltas, 0.50),
        "sinr_p10_delta": _qtl(sinr_deltas, 0.10),
    }
    return out

def get_sim_end_s(con) -> float:
    # pick a table that always advances; lteuecell is good
    try:
        df = _q(con, "SELECT MAX(simulationtime) AS mx FROM lteuecell;")
        mx = int(df["mx"].iloc[0] or 0)
        return mx / 1e9
    except Exception:
        # fallback: prb util
        df = _q(con, "SELECT MAX(simulationtime) AS mx FROM lteenbprbutilization;")
        mx = int(df["mx"].iloc[0] or 0)
        return mx / 1e9

def cap_overload_area(con, t0_s: float, t1_s: float, hot_th: float = 0.9):
    df = _q(con, """
        SELECT simulationtime, cellid, dlutil
        FROM lteenbprbutilization
        WHERE simulationtime BETWEEN ? AND ?
    """, (_ns(t0_s), _ns(t1_s)))
    if df.empty:
        return {}
    d = pd.to_numeric(df["dlutil"], errors="coerce")
    over = (d >= hot_th)
    return {
        "cell_seconds_over_th": int(over.sum()),
        "overload_ratio_rows": float(over.mean()),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Compare two ns-3 O-RAN SQLite databases")
    ap.add_argument("baseline_db", type=Path)
    ap.add_argument("app_db", type=Path)
    ap.add_argument("--out", type=Path, default=Path("fullscan_report.md"))
    ap.add_argument("--phases", nargs="*", type=float, default=[],
                    help="Phase edges in seconds, e.g. --phases 0 100 200 300. If omitted, only overall stats.")
    ap.add_argument("--topk", type=int, default=60, help="Top-K changed metrics to show globally")
    ap.add_argument("--per_table_topk", type=int, default=12, help="Top-K changed metrics per table")
    ap.add_argument("--time_bin_s", type=float, default=1.0, help="Time bin for time-local delta scan (seconds)")

    ap.add_argument("--emerg_mod", type=int, default=3)
    ap.add_argument("--emerg_off", type=int, default=0)
    ap.add_argument("--emerg_t0_s", type=float, default=100.0,
                help="Emergency window start time (seconds) for capability scan.")
    ap.add_argument("--emerg_t1_s", type=float, default=200.0,
                    help="Emergency window end time (seconds) for capability scan.")
    ap.add_argument("--hot_mod", type=int, default=3)
    ap.add_argument("--hot_off", type=int, default=0)
    ap.add_argument("--dl_out_th", type=float, default=0.2)
    ap.add_argument("--cio_src", type=int, default=0,
                    help="If set (>0), run CIO targeted impact for this source cell (e.g., 1). If 0, auto-detect from lmaction in emergency window.")
    ap.add_argument("--ho_win_s", type=float, default=10.0,
                    help="Window size (seconds) for HO win-rate before/after comparison.")
    ap.add_argument("--cio_win_s", type=float, default=10.0,
                    help="Window size (seconds) for CIO before/after attribution.")

    args = ap.parse_args()

    if not args.baseline_db.exists():
        raise SystemExit(f"baseline db not found: {args.baseline_db}")
    if not args.app_db.exists():
        raise SystemExit(f"app db not found: {args.app_db}")

    con_a = sqlite3.connect(str(args.baseline_db))
    con_b = sqlite3.connect(str(args.app_db))
    try:
        sim_end = min(get_sim_end_s(con_a), get_sim_end_s(con_b))  # <-- ADD (global end time, seconds)

        tables_a = _list_tables(con_a)
        tables_b = _list_tables(con_b)
        tables = sorted(set(tables_a) | set(tables_b))

        phase_edges = args.phases
        if phase_edges and len(phase_edges) < 2:
            raise SystemExit("--phases needs at least two numbers, e.g. 0 100")

        report: List[str] = []
        report.append("# Full Scan Compare Report\n")
        report.append(f"- Baseline: `{args.baseline_db}`\n")
        report.append(f"- App     : `{args.app_db}`\n")
        report.append(f"- Tables  : baseline={len(tables_a)}, app={len(tables_b)}, union={len(tables)}\n")
        if phase_edges:
            report.append(f"- Phases  : {phase_edges}\n")
        report.append("\n---\n")

        global_rows = []  # (score_abs, table, col, baseline_stats, app_stats, effect, rel_mean)

        for table in tables:
            has_a = table in tables_a
            has_b = table in tables_b
            if not (has_a and has_b):
                report.append(f"## Table `{table}`\n")
                report.append(f"_Only in {'baseline' if has_a else 'app'} DB; skipped comparison._\n\n")
                continue

            # Load the full table
            try:
                dfA = pd.read_sql_query(f"SELECT * FROM {table}", con_a)
                dfB = pd.read_sql_query(f"SELECT * FROM {table}", con_b)
            except Exception as e:
                report.append(f"## Table `{table}`\n")
                report.append(f"_Failed to read table in one DB: {e}_\n\n")
                continue

            if dfA.empty and dfB.empty:
                continue

            # Detect time col (if any)
            tcolA = _detect_time_col(dfA)
            tcolB = _detect_time_col(dfB)
            tcol = tcolA if (tcolA == tcolB) else (tcolA or tcolB)

            # Detect numeric columns (exclude time col)
            numeric_cols = []
            for c in dfA.columns:
                if c == tcol:
                    continue
                if c not in dfB.columns:
                    continue
                if _is_numeric_series(dfA[c]) or _is_numeric_series(dfB[c]):
                    numeric_cols.append(c)

            # Also scan categorical columns lightly (top values)
            cat_cols = []
            for c in dfA.columns:
                if c == tcol:
                    continue
                if c not in dfB.columns:
                    continue
                if (not _is_numeric_series(dfA[c])) and (not _is_numeric_series(dfB[c])):
                    # ignore huge blobs
                    if str(dfA[c].dtype) == "object" or str(dfB[c].dtype) == "object":
                        cat_cols.append(c)

            # Per table summary header
            report.append(f"## Table `{table}`\n")
            report.append(f"- Rows: baseline={len(dfA)}, app={len(dfB)}\n")
            if tcol:
                report.append(f"- Time column: `{tcol}`\n")
            report.append("\n")

            # Numeric comparisons
            per_table_changes = []
            for col in numeric_cols:
                sa = _stats_numeric(dfA[col])
                sb = _stats_numeric(dfB[col])
                if not sa or not sb:
                    continue

                eff = _effect_size(sa, sb)
                rel = (sb.mean - sa.mean) / sa.mean if abs(sa.mean) > 1e-12 else float("nan")

                # rank score: abs(effect) primary, abs(rel) secondary
                score_abs = abs(eff) if not math.isnan(eff) else 0.0
                per_table_changes.append((score_abs, col, sa, sb, eff, rel))
                global_rows.append((score_abs, table, col, sa, sb, eff, rel))

            per_table_changes.sort(key=lambda x: x[0], reverse=True)
            top = per_table_changes[: args.per_table_topk]

            if top:
                rows = []
                for _, col, sa, sb, eff, rel in top:
                    rows.append([
                        col,
                        str(sa.n),
                        _fmt(sa.mean), _fmt(sb.mean),
                        _fmt(sb.mean - sa.mean),
                        _fmt_pct(rel),
                        _fmt(sa.p10), _fmt(sb.p10),
                        _fmt(sa.p90), _fmt(sb.p90),
                        _fmt(eff, 3),
                    ])
                report.append("### Top numeric changes\n")
                report.append(_md_table(
                    ["col", "n", "mean_base", "mean_app", "Δmean", "Δmean%", "p10_base", "p10_app", "p90_base", "p90_app", "effect"],
                    rows
                ))
            else:
                report.append("_No comparable numeric columns found._\n\n")

            # Phase slicing (optional)
            if phase_edges and tcol and (tcol in dfA.columns) and (tcol in dfB.columns):
                tA = _time_to_seconds(dfA[tcol])
                tB = _time_to_seconds(dfB[tcol])
                masksA = _phase_masks(tA, phase_edges)
                masksB = _phase_masks(tB, phase_edges)

                report.append("### Phase-sliced (means)\n")
                for (pname, mA), (_, mB) in zip(masksA, masksB):
                    rows = []
                    for col in numeric_cols[: min(len(numeric_cols), 8)]:  # keep readable
                        sa = _stats_numeric(dfA.loc[mA, col]) if col in dfA.columns else None
                        sb = _stats_numeric(dfB.loc[mB, col]) if col in dfB.columns else None
                        if not sa or not sb:
                            continue
                        rel = (sb.mean - sa.mean) / sa.mean if abs(sa.mean) > 1e-12 else float("nan")
                        rows.append([col, _fmt(sa.mean), _fmt(sb.mean), _fmt(sb.mean - sa.mean), _fmt_pct(rel)])
                    if rows:
                        report.append(f"**{pname}**\n\n")
                        report.append(_md_table(["col", "mean_base", "mean_app", "Δmean", "Δmean%"], rows))

            # Time-local delta scan (optional): per-second bin for ALL numeric cols, but only show top 5 columns
            if tcol and (tcol in dfA.columns) and (tcol in dfB.columns) and numeric_cols:
                tA = _time_to_seconds(dfA[tcol])
                tB = _time_to_seconds(dfB[tcol])
                bin_s = float(args.time_bin_s)

                # Prepare bins
                dfA_bin = dfA.copy()
                dfB_bin = dfB.copy()
                dfA_bin["_tbin"] = (tA / bin_s).apply(math.floor) * bin_s
                dfB_bin["_tbin"] = (tB / bin_s).apply(math.floor) * bin_s

                # Only scan top-changed columns (per-table) to limit report size
                scan_cols = [x[1] for x in top[: min(5, len(top))]] if top else numeric_cols[:5]
                rows = []
                for col in scan_cols:
                    try:
                        a_ts = dfA_bin.groupby("_tbin")[col].mean()
                        b_ts = dfB_bin.groupby("_tbin")[col].mean()
                        joined = pd.concat([a_ts, b_ts], axis=1, keys=["base", "app"]).dropna()
                        if joined.empty:
                            continue
                        joined["delta"] = joined["app"] - joined["base"]
                        # top 3 bins by abs delta
                        topbins = joined.reindex(joined["delta"].abs().sort_values(ascending=False).index).head(3)
                        for tbin, row in topbins.iterrows():
                            rows.append([col, f"{tbin:.0f}s", _fmt(row["base"]), _fmt(row["app"]), _fmt(row["delta"])])
                    except Exception:
                        continue
                if rows:
                    report.append("### Time-local patterns (largest per-bin deltas)\n")
                    report.append(_md_table(["col", "time_bin", "mean_base", "mean_app", "Δ"], rows))

            # Categorical scan (light)
            if cat_cols:
                report.append("### Categorical columns (top values)\n")
                shown = 0
                for c in cat_cols:
                    if shown >= 3:
                        break
                    try:
                        vcA = dfA[c].astype(str).value_counts().head(5)
                        vcB = dfB[c].astype(str).value_counts().head(5)
                        if vcA.empty and vcB.empty:
                            continue
                        report.append(f"**{c}**\n\n")
                        rows = []
                        for k in sorted(set(vcA.index) | set(vcB.index))[:8]:
                            rows.append([k, str(int(vcA.get(k, 0))), str(int(vcB.get(k, 0))), str(int(vcB.get(k, 0) - vcA.get(k, 0)))])
                        report.append(_md_table(["value", "count_base", "count_app", "Δcount"], rows))
                        shown += 1
                    except Exception:
                        continue

            report.append("\n---\n")

        # Global ranking
        global_rows.sort(key=lambda x: x[0], reverse=True)
        topg = global_rows[: args.topk]
        report.append("# Global Top Changes (all tables)\n\n")
        if topg:
            rows = []
            for _, table, col, sa, sb, eff, rel in topg:
                rows.append([
                    f"{table}.{col}",
                    str(sa.n),
                    _fmt(sa.mean), _fmt(sb.mean),
                    _fmt(sb.mean - sa.mean),
                    _fmt_pct(rel),
                    _fmt(sa.p10), _fmt(sb.p10),
                    _fmt(sa.p90), _fmt(sb.p90),
                    _fmt(eff, 3),
                ])
            report.append(_md_table(
                ["metric", "n", "mean_base", "mean_app", "Δmean", "Δmean%", "p10_base", "p10_app", "p90_base", "p90_app", "effect"],
                rows
            ))
        else:
            report.append("_No numeric diffs found._\n")

        report.append("\n---\n")
        report.append("## Capability Scan (topology- & phase-aware)\n\n")

        def _cap_block(title, rows):
            report.append(f"## {title}\n\n")
            report.append(_md_table(
                ["metric", "baseline", "app", "Δ(app-base)"],
                rows
            ))
            report.append("\n")

        # emergency window metrics (capability scan)
        tE0, tE1 = float(args.emerg_t0_s), float(args.emerg_t1_s)
        # Optional: clamp the window to sim_end
        tE0, tE1 = _clip_window(tE0, tE1, sim_end)

        mA = cap_emergency_ue_metrics(con_a, tE0, tE1, args.emerg_mod, args.emerg_off, args.dl_out_th)
        mB = cap_emergency_ue_metrics(con_b, tE0, tE1, args.emerg_mod, args.emerg_off, args.dl_out_th)

        rows = []
        for k in ("n_samples", "dl_mean", "dl_p10", "dl_p05", "outage_ratio"):
            a = mA.get(k, float("nan"))
            b = mB.get(k, float("nan"))
            if k == "outage_ratio":
                rows.append([k, _fmtf(a,3), _fmtf(b,3), _fmtf(b-a,3)])
            else:
                rows.append([k, _fmtf(a,3), _fmtf(b,3), _fmtf(b-a,3)])
        _cap_block(f"Emergency UE QoE (t={int(tE0)}-{int(tE1)}s, modulo={args.emerg_mod}, offset={args.emerg_off})", rows)

        # hot-cell overload seconds (100-200)
        hA = cap_hotcell_overload(con_a, tE0, tE1, hot_th=0.9)
        hB = cap_hotcell_overload(con_b, tE0, tE1, hot_th=0.9)

        rows = [
            ["top_hot_cell", str(hA.get("top_hot_cell","-")), str(hB.get("top_hot_cell","-")),
            str((hB.get("top_hot_cell",0) - hA.get("top_hot_cell",0)) if hA and hB else "-")],
            ["overload_seconds_top1", str(hA.get("overload_seconds_top1","-")), str(hB.get("overload_seconds_top1","-")),
            str((hB.get("overload_seconds_top1",0) - hA.get("overload_seconds_top1",0)) if hA and hB else "-")],
            ["overload_seconds_top2_sum", str(hA.get("overload_seconds_top2_sum","-")), str(hB.get("overload_seconds_top2_sum","-")),
            str((hB.get("overload_seconds_top2_sum",0) - hA.get("overload_seconds_top2_sum",0)) if hA and hB else "-")],
        ]
        _cap_block(f"Hot-cell overload duration (t={int(tE0)}-{int(tE1)}s, dlutil>=0.9)", rows)

        # action counts (whole run)
        aA = cap_action_counts(con_a)
        aB = cap_action_counts(con_b)
        rows = []
        for k in ("lmaction", "lmcommand", "terminatorcommand"):
            va, vb = aA.get(k), aB.get(k)
            rows.append([k, str(va), str(vb), str((vb-va) if (va is not None and vb is not None) else "-")])
        _cap_block("Control cost (whole run)", rows)

        # ---- All-UE tail QoE (whole run) ----
        t0, t1 = 0.0, sim_end
        qA = cap_all_ue_tail_qoe(con_a, t0, t1, args.dl_out_th)
        qB = cap_all_ue_tail_qoe(con_b, t0, t1, args.dl_out_th)

        rows = []
        for k in ("n_samples","dl_mean","dl_p10","dl_p05","outage_ratio"):
            a = qA.get(k, float("nan"))
            b = qB.get(k, float("nan"))
            rows.append([k, _fmtf(a,3), _fmtf(b,3), _fmtf(b-a,3)])

        _cap_block(f"All-UE QoE tail (t=0-{sim_end:.0f}s, outage< {args.dl_out_th})", rows)


        # ---------------- CIO targeted impact (emergency window) ----------------
        cio_src = int(args.cio_src)
        if cio_src <= 0:
            srcs = find_cio_src_cells(con_b, tE0, tE1)  # detect from APP DB in emergency window
            cio_src = srcs[0] if srcs else 0

        if cio_src > 0:
            cA = cap_cio_targeted_impact(con_a, tE0, tE1, cio_src, hot_th=0.9)
            cB = cap_cio_targeted_impact(con_b, tE0, tE1, cio_src, hot_th=0.9)

            rows = []
            for k in ("prb_dl_mean", "prb_dl_p90", "overload_seconds",
                      "ue_count_mean", "ue_count_p90",
                      "sched_dl_mean", "sched_dl_p10"):
                a = cA.get(k, float("nan"))
                b = cB.get(k, float("nan"))
                rows.append([k, _fmtf(a, 3), _fmtf(b, 3), _fmtf(b - a, 3)])

            _cap_block(f"CIO targeted impact on src_cell={cio_src} (t={int(tE0)}-{int(tE1)}s)", rows)
        else:
            _cap_block(f"CIO targeted impact (t={int(tE0)}-{int(tE1)}s)", [
                ["note", "no CIO in baseline", "no CIO detected in app", "-"]
            ])

        # ---------------- HO win-rate (recovery window 200-300) ----------------
        tH0, tH1 = 0.0, sim_end
        hoA = cap_ho_winrate(con_a, tH0, tH1, win_s=float(args.ho_win_s))
        hoB = cap_ho_winrate(con_b, tH0, tH1, win_s=float(args.ho_win_s))

        rows = [
            ["n_ho", str(hoA.get("n_ho", 0)), str(hoB.get("n_ho", 0)),
             str(hoB.get("n_ho", 0) - hoA.get("n_ho", 0))],

            ["dl_winrate",
             _fmtf(hoA.get("dl_winrate", float("nan")), 3),
             _fmtf(hoB.get("dl_winrate", float("nan")), 3),
             _fmtf(hoB.get("dl_winrate", float("nan")) - hoA.get("dl_winrate", float("nan")), 3)],

            ["sinr_winrate",
             _fmtf(hoA.get("sinr_winrate", float("nan")), 3),
             _fmtf(hoB.get("sinr_winrate", float("nan")), 3),
             _fmtf(hoB.get("sinr_winrate", float("nan")) - hoA.get("sinr_winrate", float("nan")), 3)],

            ["dl_avg_delta",
             _fmtf(hoA.get("dl_avg_delta", float("nan")), 3),
             _fmtf(hoB.get("dl_avg_delta", float("nan")), 3),
             _fmtf(hoB.get("dl_avg_delta", float("nan")) - hoA.get("dl_avg_delta", float("nan")), 3)],

            ["sinr_avg_delta",
             _fmtf(hoA.get("sinr_avg_delta", float("nan")), 3),
             _fmtf(hoB.get("sinr_avg_delta", float("nan")), 3),
             _fmtf(hoB.get("sinr_avg_delta", float("nan")) - hoA.get("sinr_avg_delta", float("nan")), 3)],


            # ---- ADD 4 lines here ----
            ["dl_p50_delta",
            _fmtf(hoA.get("dl_p50_delta", float("nan")), 3),
            _fmtf(hoB.get("dl_p50_delta", float("nan")), 3),
            _fmtf(hoB.get("dl_p50_delta", float("nan")) - hoA.get("dl_p50_delta", float("nan")), 3)],

            ["dl_p10_delta",
            _fmtf(hoA.get("dl_p10_delta", float("nan")), 3),
            _fmtf(hoB.get("dl_p10_delta", float("nan")), 3),
            _fmtf(hoB.get("dl_p10_delta", float("nan")) - hoA.get("dl_p10_delta", float("nan")), 3)],

            ["sinr_p50_delta",
            _fmtf(hoA.get("sinr_p50_delta", float("nan")), 3),
            _fmtf(hoB.get("sinr_p50_delta", float("nan")), 3),
            _fmtf(hoB.get("sinr_p50_delta", float("nan")) - hoA.get("sinr_p50_delta", float("nan")), 3)],

            ["sinr_p10_delta",
            _fmtf(hoA.get("sinr_p10_delta", float("nan")), 3),
            _fmtf(hoB.get("sinr_p10_delta", float("nan")), 3),
            _fmtf(hoB.get("sinr_p10_delta", float("nan")) - hoA.get("sinr_p10_delta", float("nan")), 3)],
        ]


        _cap_block(f"HO win-rate (t={int(tH0)}-{int(tH1)}s, win={args.ho_win_s:.0f}s before/after)", rows)


        # ---- CIO src + neighbor impact (whole run) ----
        tE0, tE1 = 0.0, sim_end

        # Prefer CLI-provided cio_src; otherwise auto-detect from lmaction
        src = int(args.cio_src)
        nbr = 0
        offset_db = float("nan")

        pairs = find_cio_pairs(con_b, tE0, tE1)  # detect from APP DB
        if src <= 0:
            # pick the most frequent (src,nbr); tie-break by first occurrence
            from collections import Counter
            cnt = Counter((p["src"], p["nbr"]) for p in pairs)
            if cnt:
                (src, nbr), _ = cnt.most_common(1)[0]
                # choose offset from earliest matching record
                for p in pairs:
                    if p["src"] == src and p["nbr"] == nbr:
                        offset_db = p["offset_db"]
                        break
        else:
            # if src forced, pick its most frequent neighbor
            from collections import Counter
            cnt = Counter(p["nbr"] for p in pairs if p["src"] == src)
            if cnt:
                nbr, _ = cnt.most_common(1)[0]
                for p in pairs:
                    if p["src"] == src and p["nbr"] == nbr:
                        offset_db = p["offset_db"]
                        break

        # fallback: if still not found, skip block gracefully
        if src <= 0 or nbr <= 0:
            _cap_block(f"CIO attribution (t=0-{sim_end:.0f}s)", [
                ["note", "-", "no CIO detected in app", "-"]
            ])
        else:
            # --- CIO attribution: per-event before/after around the action time ---
            cio_events = list(pairs)
            # if user forced a src, keep only those events
            if src > 0:
                cio_events = [e for e in cio_events if int(e["src"]) == int(src)]
            # if we also detected nbr, keep only that pair (optional but cleaner)
            if nbr > 0:
                cio_events = [e for e in cio_events if int(e["nbr"]) == int(nbr)]

            cio_attr_A = cap_cio_before_after(con_a, cio_events, sim_end_s=sim_end, win_s=float(args.cio_win_s), hot_th=0.9)
            cio_attr_B = cap_cio_before_after(con_b, cio_events, sim_end_s=sim_end, win_s=float(args.cio_win_s), hot_th=0.9)

            keys = ["prb_dl_mean","prb_dl_p90","overload_seconds",
                    "ue_count_mean","ue_count_p90",
                    "sched_dl_mean","sched_dl_p10"]

            rows = []

            # Add the two summary rows first
            n_evt = len(cio_attr_B["events"])
            t_first = min((e["t_s"] for e in cio_attr_B["events"]), default=float("nan"))
            t_last  = max((e["t_s"] for e in cio_attr_B["events"]), default=float("nan"))
            rows.append(["n_cio_events_used", str(n_evt), str(n_evt), "0"])
            rows.append(["cio_time_span_s", _fmtf(t_first,1), _fmtf(t_last,1), "-"])

            # Then add the KPI delta rows
            for k in keys:
                a = cio_attr_A["agg"].get(k, float("nan"))
                b = cio_attr_B["agg"].get(k, float("nan"))
                rows.append([f"src_delta_avg.{k}", _fmtf(a,3), _fmtf(b,3), _fmtf(b-a,3)])
                
            off_txt = f"{offset_db:.3g}dB" if not math.isnan(offset_db) else "unknown"
            _cap_block(
                f"CIO attribution (t=0-{sim_end:.0f}s, src={src} -> nbr={nbr}, offset={off_txt}, win={args.cio_win_s:.0f}s)",
                rows
            )


        # ---- Hotspot vs non-hotspot UE QoE (whole run) ----
        hotA = _select_ueids_by_modulo(con_a, args.hot_mod, args.hot_off)
        hotB = _select_ueids_by_modulo(con_b, args.hot_mod, args.hot_off)

        # Use the baseline UE list as the universe; both sides should match
        allU = sorted(set(_select_ueids_by_modulo(con_a, 1, 0)))  # trick: modulo=1 selects all
        hotU = sorted(set(hotA))
        coldU = [u for u in allU if u not in hotU]

        hot_qA  = cap_group_ue_qoe(con_a, 0.0, sim_end, hotU, args.dl_out_th)
        hot_qB  = cap_group_ue_qoe(con_b, 0.0, sim_end, hotU, args.dl_out_th)
        cold_qA = cap_group_ue_qoe(con_a, 0.0, sim_end, coldU, args.dl_out_th)
        cold_qB = cap_group_ue_qoe(con_b, 0.0, sim_end, coldU, args.dl_out_th)

        rows = []
        def add_group(prefix, A, B):
            for k in ("dl_mean","dl_p10","dl_p05","outage_ratio"):
                a = A.get(k, float("nan"))
                b = B.get(k, float("nan"))
                rows.append([f"{prefix}.{k}", _fmtf(a,3), _fmtf(b,3), _fmtf(b-a,3)])

        add_group("hot", hot_qA, hot_qB)
        add_group("cold", cold_qA, cold_qB)

        _cap_block(f"Hotspot vs cold UE QoE (t=0-{sim_end:.0f}s, hot_mod={args.hot_mod}, hot_off={args.hot_off})", rows)


        oa = cap_overload_area(con_a, 0.0, sim_end, hot_th=0.9)
        ob = cap_overload_area(con_b, 0.0, sim_end, hot_th=0.9)
        rows = []
        for k in ("cell_seconds_over_th","overload_ratio_rows"):
            a = oa.get(k, float("nan"))
            b = ob.get(k, float("nan"))
            rows.append([k, _fmtf(a,3) if isinstance(a,float) else str(a),
                            _fmtf(b,3) if isinstance(b,float) else str(b),
                            _fmtf(b-a,3) if (isinstance(a,float) and isinstance(b,float)) else str(b-a)])
        _cap_block(f"Overload area (t=0-{sim_end:.0f}s, dlutil>=0.9)", rows)


        # ---- Control cost normalized ----
        aA = cap_action_counts(con_a)
        aB = cap_action_counts(con_b)
        dur100 = max(1e-6, sim_end / 100.0)
        rows = []
        for k in ("lmaction","lmcommand","terminatorcommand"):
            va, vb = aA.get(k, 0) or 0, aB.get(k, 0) or 0
            rows.append([f"{k}_per_100s", _fmtf(va/dur100,3), _fmtf(vb/dur100,3), _fmtf((vb-va)/dur100,3)])
        _cap_block(f"Control cost normalized (t=0-{sim_end:.0f}s)", rows)

        args.out.write_text("".join(report), encoding="utf-8")
        print(f"Wrote report -> {args.out}")

    finally:
        con_a.close()
        con_b.close()


if __name__ == "__main__":
    main()
