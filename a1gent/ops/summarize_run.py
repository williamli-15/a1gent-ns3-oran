#!/usr/bin/env python3
"""
Run summary for O-RAN ns-3 experiment databases.

Summarize KPI tables for an oran-repository SQLite database, optionally
with orchestrator action counts parsed from the orchestrator log.

Use this for a compact per-run KPI summary.

python -m a1gent.ops.summarize_run oran-repository-baseline.db > summary_baseline.txt
python -m a1gent.ops.summarize_run oran-repository-agent-run.db > summary_agent_run.txt


"""

import argparse
import collections
import math
import os
import re
import sqlite3
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


def ns_to_s(value: Optional[int]) -> float:
    return (value or 0) / 1e9


def percentile(values: Sequence[float], frac: float) -> float:
    if not values:
        return math.nan
    if len(values) == 1:
        return float(values[0])
    k = (len(values) - 1) * frac
    f = math.floor(k)
    c = math.ceil(k)
    if f == c:
        return float(values[int(k)])
    lower = values[f]
    upper = values[c]
    return float(lower + (upper - lower) * (k - f))


def describe(values: Iterable[float]) -> Optional[Dict[str, float]]:
    data = [float(v) for v in values if v is not None]
    if not data:
        return None
    data.sort()
    n = len(data)
    mean_val = sum(data) / n
    if n > 1:
        variance = sum((x - mean_val) ** 2 for x in data) / n
        std_val = math.sqrt(variance)
    else:
        std_val = 0.0
    return {
        "samples": float(n),
        "mean": mean_val,
        "std": std_val,
        "p05": percentile(data, 0.05),
        "p25": percentile(data, 0.25),
        "p50": percentile(data, 0.50),
        "p75": percentile(data, 0.75),
        "p95": percentile(data, 0.95),
        "min": float(data[0]),
        "max": float(data[-1]),
    }


def fmt_float(value: Optional[float], digits: int = 3) -> str:
    if value is None or math.isnan(value):
        return "-"
    return f"{value:.{digits}f}"


def fmt_int(value: Optional[float]) -> str:
    if value is None or math.isnan(value):
        return "-"
    return f"{int(round(value))}"


def format_stats_table(label_to_stats: Dict[str, Optional[Dict[str, float]]]) -> str:
    headers = ["Metric", "Samples", "Mean", "Std", "P05", "P25", "P50", "P75", "P95", "Min", "Max"]
    rows: List[List[str]] = []
    for label, stats in label_to_stats.items():
        if not stats:
            rows.append([label, "-", "-", "-", "-", "-", "-", "-", "-", "-", "-"])
            continue
        rows.append(
            [
                label,
                fmt_int(stats.get("samples")),
                fmt_float(stats.get("mean")),
                fmt_float(stats.get("std")),
                fmt_float(stats.get("p05")),
                fmt_float(stats.get("p25")),
                fmt_float(stats.get("p50")),
                fmt_float(stats.get("p75")),
                fmt_float(stats.get("p95")),
                fmt_float(stats.get("min")),
                fmt_float(stats.get("max")),
            ]
        )
    return format_table(headers, rows)


def format_table(headers: Sequence[str], rows: Sequence[Sequence[str]]) -> str:
    if not rows:
        return "  (no samples)\n"
    widths = [len(h) for h in headers]
    for row in rows:
        for idx, cell in enumerate(row):
            widths[idx] = max(widths[idx], len(cell))
    lines = []
    header_line = "  " + "  ".join(headers[i].ljust(widths[i]) for i in range(len(headers)))
    lines.append(header_line)
    lines.append("  " + "  ".join("-" * widths[i] for i in range(len(headers))))
    for row in rows:
        line = "  " + "  ".join(row[i].ljust(widths[i]) for i in range(len(headers)))
        lines.append(line)
    return "\n".join(lines) + "\n"


def load_rows(con: sqlite3.Connection, query: str) -> List[Tuple]:
    cur = con.execute(query)
    rows = cur.fetchall()
    cur.close()
    return rows


def pct_fraction(total: int, count: int) -> float:
    if total <= 0:
        return math.nan
    return (count / total) * 100.0


def parse_actions_log(path: Optional[Path]) -> Optional[str]:
    if not path:
        return None
    if not path.exists():
        return f"  (log file {path} not found)\n"
    batches = 0
    counts = collections.Counter()
    for line in path.read_text().splitlines():
        if "commands | write" in line and "kinds=" in line:
            match = re.search(r"kinds=\[([^\]]*)\]", line)
            if not match:
                continue
            payload = match.group(1)
            items = [item.strip().strip("'\"") for item in payload.split(",") if item.strip()]
            if items:
                batches += 1
            for item in items:
                counts[item] += 1
    lines = ["Action log summary:"]
    lines.append(f"  batches with actions : {batches}")
    if counts:
        for key in ("ho", "set_cio", "cell_sleep", "cell_wake"):
            lines.append(f"  {key:12s}: {counts.get(key, 0)}")
        remainder = {k: v for k, v in counts.items() if k not in {"ho", "set_cio", "cell_sleep", "cell_wake"}}
        if remainder:
            lines.append(f"  other kinds  : {remainder}")
    else:
        lines.append("  (no action batches captured)")
    return "\n".join(lines) + "\n"


def summarise(db_path: Path, log_path: Optional[Path]) -> str:
    with sqlite3.connect(db_path) as con:
        con.row_factory = sqlite3.Row
        ue_count = con.execute("SELECT COUNT(*) FROM lteue").fetchone()[0]
        cell_count = con.execute("SELECT COUNT(*) FROM lteenb").fetchone()[0]
        sim_bounds = con.execute(
            "SELECT COALESCE(MIN(simulationtime),0), COALESCE(MAX(simulationtime),0) FROM lteuecell"
        ).fetchone()

        pdcp_rows = load_rows(con, "SELECT ueid, dlmbps, ulmbps FROM lteue_pdcp_tp")
        sinr_rows = load_rows(con, "SELECT nodeid, sinr_db, rsrp_dbm FROM lteuesinr")
        loss_rows = load_rows(con, "SELECT nodeid, loss FROM nodeapploss")
        ue_prb_rows = load_rows(con, "SELECT ueid, dl_prbs, ul_prbs FROM lteue_prb")
        prb_rows = load_rows(con, "SELECT cellid, dlutil, ulutil FROM lteenbprbutilization")
        sched_rows = load_rows(con, "SELECT cellid, dl_mbps, ul_mbps FROM lteenb_sched_tp")
        mcs_rows = load_rows(con, "SELECT cellid, dl_p50, dl_mean, ul_p50, ul_mean FROM lteenb_mcs")
        uecount_rows = load_rows(con, "SELECT cellid, ue_count FROM lteenb_uecount")
        interf_rows = load_rows(con, "SELECT cellid, p95dbm FROM lteenb_ul_interf")
        ho_rows = load_rows(con, "SELECT srcCell, dstCell, event FROM lte_ho_events")

    sim_start_s, sim_end_s = map(ns_to_s, sim_bounds)
    duration_s = sim_end_s - sim_start_s

    ue_metrics: Dict[int, Dict[str, List[float]]] = collections.defaultdict(
        lambda: {"dl": [], "ul": [], "sinr": [], "rsrp": [], "loss": [], "dl_prb": [], "ul_prb": []}
    )
    cell_metrics: Dict[int, Dict[str, List[float]]] = collections.defaultdict(
        lambda: {
            "dl_util": [],
            "ul_util": [],
            "sched_dl": [],
            "sched_ul": [],
            "ue_count": [],
            "ul_interf": [],
            "overload_count": 0,
            "idle_count": 0,
        }
    )

    global_dl = []
    global_ul = []
    for ueid, dl, ul in pdcp_rows:
        if dl is not None:
            value = float(dl)
            global_dl.append(value)
            ue_metrics[ueid]["dl"].append(value)
        if ul is not None:
            value = float(ul)
            global_ul.append(value)
            ue_metrics[ueid]["ul"].append(value)

    global_sinr = []
    global_rsrp = []
    for ueid, sinr_db, rsrp_dbm in sinr_rows:
        if sinr_db is not None:
            value = float(sinr_db)
            global_sinr.append(value)
            ue_metrics[ueid]["sinr"].append(value)
        if rsrp_dbm is not None:
            value = float(rsrp_dbm)
            global_rsrp.append(value)
            ue_metrics[ueid]["rsrp"].append(value)

    for ueid, loss in loss_rows:
        if loss is not None:
            ue_metrics[ueid]["loss"].append(float(loss))

    for ueid, dl_prbs, ul_prbs in ue_prb_rows:
        if dl_prbs is not None:
            ue_metrics[ueid]["dl_prb"].append(float(dl_prbs))
        if ul_prbs is not None:
            ue_metrics[ueid]["ul_prb"].append(float(ul_prbs))

    for cellid, dlutil, ulutil in prb_rows:
        entry = cell_metrics[cellid]
        if dlutil is not None:
            value = float(dlutil)
            entry["dl_util"].append(value)
            if value >= 0.90:
                entry["overload_count"] += 1
            if value <= 0.05:
                entry["idle_count"] += 1
        if ulutil is not None:
            entry["ul_util"].append(float(ulutil))

    for cellid, dl_mbps, ul_mbps in sched_rows:
        entry = cell_metrics[cellid]
        if dl_mbps is not None:
            entry["sched_dl"].append(float(dl_mbps))
        if ul_mbps is not None:
            entry["sched_ul"].append(float(ul_mbps))

    for cellid, dl_p50, dl_mean, ul_p50, ul_mean in mcs_rows:
        entry = cell_metrics[cellid]
        if dl_p50 is not None:
            entry.setdefault("mcs_dl_p50", []).append(float(dl_p50))
        if ul_p50 is not None:
            entry.setdefault("mcs_ul_p50", []).append(float(ul_p50))
        if dl_mean is not None:
            entry.setdefault("mcs_dl_mean", []).append(float(dl_mean))
        if ul_mean is not None:
            entry.setdefault("mcs_ul_mean", []).append(float(ul_mean))

    for cellid, count in uecount_rows:
        entry = cell_metrics[cellid]
        entry["ue_count"].append(float(count))

    for cellid, p95dbm in interf_rows:
        entry = cell_metrics[cellid]
        if p95dbm is not None:
            entry["ul_interf"].append(float(p95dbm))

    lines: List[str] = []
    lines.append(f"Database            : {db_path}")
    lines.append(f"Simulation interval : {sim_start_s:.1f}s → {sim_end_s:.1f}s ({duration_s:.1f}s span)")
    lines.append(f"Distinct UEs        : {ue_count}")
    lines.append(f"Cells               : {cell_count}")
    lines.append(f"PDCP samples        : {len(global_dl)} (DL) / {len(global_ul)} (UL)")
    lines.append(f"SINR samples        : {len(global_sinr)}")
    lines.append(f"Loss samples        : {len(loss_rows)}")
    lines.append(f"PRB util samples    : {len(prb_rows)}")
    lines.append(f"Scheduler samples   : {len(sched_rows)}")
    lines.append(f"MCS samples         : {len(mcs_rows)}")
    lines.append(f"UE-count samples    : {len(uecount_rows)}")
    lines.append(f"UL interference rows: {len(interf_rows)}")
    lines.append(f"Handover events     : {len(ho_rows)}")

    lines.append("\n== UE Throughput (PDCP Mbps) ==")
    ue_throughput_stats = {
        "DL Mbps": describe(global_dl),
        "UL Mbps": describe(global_ul),
    }
    lines.append(format_stats_table(ue_throughput_stats))

    lines.append("== UE SINR / RSRP ==")
    ue_sinr_stats = {
        "SINR dB": describe(global_sinr),
        "RSRP dBm": describe(global_rsrp),
    }
    lines.append(format_stats_table(ue_sinr_stats))

    # if loss_rows:
    #     lines.append("== UE Packet Loss ==")
    #     lines.append(format_stats_table({"Loss ratio": describe([row[1] for row in loss_rows])}))

    lines.append("== Per-UE Summary ==")
    ue_rows_output: List[List[str]] = []
    for ueid in sorted(ue_metrics.keys()):
        metrics = ue_metrics[ueid]
        dl_stats = describe(metrics["dl"])
        ul_stats = describe(metrics["ul"])
        sinr_stats = describe(metrics["sinr"])
        loss_stats = describe(metrics["loss"])
        dl_prb_stats = describe(metrics["dl_prb"])
        ul_prb_stats = describe(metrics["ul_prb"])
        ue_rows_output.append(
            [
                str(ueid),
                fmt_float(dl_stats["mean"]) if dl_stats else "-",
                fmt_float(dl_stats["p05"]) if dl_stats else "-",
                fmt_float(dl_stats["p95"]) if dl_stats else "-",
                fmt_float(ul_stats["mean"]) if ul_stats else "-",
                fmt_float(sinr_stats["p50"]) if sinr_stats else "-",
                fmt_float(sinr_stats["p05"]) if sinr_stats else "-",
                fmt_float(sinr_stats["p95"]) if sinr_stats else "-",
                fmt_float(loss_stats["mean"]) if loss_stats else "-",
                fmt_float(dl_prb_stats["mean"]) if dl_prb_stats else "-",
                fmt_float(ul_prb_stats["mean"]) if ul_prb_stats else "-",
                fmt_int(dl_stats["samples"]) if dl_stats else "-",
            ]
        )
    ue_headers = [
        "UE",
        "DL_mean",
        "DL_p05",
        "DL_p95",
        "UL_mean",
        "SINR_med",
        "SINR_p05",
        "SINR_p95",
        "Loss_mean",
        "DL_PRB",
        "UL_PRB",
        "Samples",
    ]
    lines.append(format_table(ue_headers, ue_rows_output))

    lines.append("== Cell Utilisation / Scheduling ==")
    cell_rows: List[List[str]] = []
    for cellid in sorted(cell_metrics.keys()):
        metrics = cell_metrics[cellid]
        dl_desc = describe(metrics["dl_util"])
        ul_desc = describe(metrics["ul_util"])
        sched_dl_desc = describe(metrics["sched_dl"])
        sched_ul_desc = describe(metrics["sched_ul"])
        mcs_dl_p50_desc = describe(metrics.get("mcs_dl_p50", []))
        mcs_ul_p50_desc = describe(metrics.get("mcs_ul_p50", []))
        ue_count_desc = describe(metrics["ue_count"])
        ul_interf_desc = describe(metrics["ul_interf"])
        sample_count = int(dl_desc["samples"]) if dl_desc else 0
        idle_pct = pct_fraction(sample_count, metrics["idle_count"])
        overload_pct = pct_fraction(sample_count, metrics["overload_count"])
        cell_rows.append(
            [
                str(cellid),
                fmt_float(dl_desc["mean"]) if dl_desc else "-",
                fmt_float(dl_desc["p95"]) if dl_desc else "-",
                fmt_float(overload_pct),
                fmt_float(idle_pct),
                fmt_float(ul_desc["mean"]) if ul_desc else "-",
                fmt_float(sched_dl_desc["mean"]) if sched_dl_desc else "-",
                fmt_float(sched_ul_desc["mean"]) if sched_ul_desc else "-",
                fmt_float(mcs_dl_p50_desc["mean"]) if mcs_dl_p50_desc else "-",
                fmt_float(mcs_ul_p50_desc["mean"]) if mcs_ul_p50_desc else "-",
                fmt_float(ue_count_desc["mean"]) if ue_count_desc else "-",
                fmt_float(ue_count_desc["p95"]) if ue_count_desc else "-",
                fmt_float(ul_interf_desc["p50"]) if ul_interf_desc else "-",
                fmt_float(ul_interf_desc["p95"]) if ul_interf_desc else "-",
            ]
        )
    cell_headers = [
        "Cell",
        "DL_util_mean",
        "DL_util_p95",
        "DL_over_%",   # % of samples ≥ 0.90
        "DL_idle_%",   # % of samples ≤ 0.05
        "UL_util_mean",
        "Sched_DL",
        "Sched_UL",
        "MCS_DL_p50",
        "MCS_UL_p50",
        "UE_mean",
        "UE_p95",
        "UL_Intf_p95dbm_p50",
        "UL_Intf_p95dbm_p95",
    ]
    lines.append(format_table(cell_headers, cell_rows))

    lines.append("== Handover Events ==")
    if not ho_rows:
        lines.append("  (no handover rows present)\n")
    else:
        success = [row for row in ho_rows if "EndOk" in (row[2] or "")]
        failure = [row for row in ho_rows if "EndOk" not in (row[2] or "")]
        lines.append(f"  total events   : {len(ho_rows)}")
        lines.append(f"  successful     : {len(success)}")
        lines.append(f"  other outcomes : {len(failure)}")
        pair_counts = collections.Counter((row[0], row[1]) for row in ho_rows)
        pair_rows = [
            [str(src), str(dst), str(count)]
            for (src, dst), count in sorted(pair_counts.items(), key=lambda kv: (-kv[1], kv[0]))
        ]
        if pair_rows:
            lines.append(format_table(["Src", "Dst", "Count"], pair_rows))

    action_section = parse_actions_log(log_path)
    if action_section:
        lines.append("== Orchestrator Actions ==")
        lines.append(action_section)

    return "\n".join(lines).strip() + "\n"


def main() -> None:
    default_db = os.getenv("A1GENT_DB_PATH", "workspace/ns-3.42/oran-repository.db")
    parser = argparse.ArgumentParser(description="Summarise oran-repository SQLite databases")
    parser.add_argument(
        "db",
        nargs="?",
        default=default_db,
        help="Path to oran-repository.db",
    )
    parser.add_argument(
        "--log",
        dest="log",
        type=Path,
        help="Optional orchestrator log to extract action counts",
    )
    args = parser.parse_args()

    db_path = Path(args.db)
    if not db_path.exists():
        sys.exit(f"Database {db_path} not found")

    summary = summarise(db_path, args.log)
    print(summary)


if __name__ == "__main__":
    main()
