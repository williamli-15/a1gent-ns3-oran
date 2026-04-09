#!/usr/bin/env python3
"""
Parse orchestrator logs and summarize actions and APT events.

Reports:
  - overall action counts (ho / set_cio / cell_sleep / cell_wake)
  - per-phase breakdown (normal / emergency / recovery)
  - per-tick timeline from commands.json writes
  - APT trial events and touched knobs
  - parsed HO / CIO detail lines

Use this for local decisions, sequencing, and emerging multi-agent behavior.

Usage:
  python -m a1gent.ops.orch_ops_scan orch.log --out orch_ops_report.md --csv orch_ops_timeline.csv
"""

from __future__ import annotations

import argparse
import ast
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple
import math
import pandas as pd


# ---------- regex patterns ----------
RE_SNAPSHOT = re.compile(r"\bsnapshot: .*?\(sim_t=([0-9.]+)s\)")
RE_PHASE = re.compile(r"\bintent phase -> (\w+)\s*\(sim_t=([0-9.]+)s\)")
RE_AGENT_COUNTS = re.compile(r"\bactions:\s+energy=(\d+)\s+qoe=(\d+)\s+load=(\d+)\s+\(order=([^)]+)\)")
RE_WRITE = re.compile(r"\bwrite -> .*?\|\s*actions=(\d+)\s*\|\s*kinds=(\[[^\]]*\])\s*\|\s*ts=([0-9.]+)")
RE_APT_UPDATES = re.compile(r"\bAPT applied updates:\s*(\[.*\])")

# Agent-level detail lines (optional but useful)
RE_CIO_STEP = re.compile(r"\bCIO step:\s*src=(\d+)\s*->\s*nbr=(\d+)\b")
RE_CIO_REBIND = re.compile(r"\bCIO rebinding\(clear\):\s*src=(\d+)\s*->\s*nbr=(\d+)\b")
RE_LOAD_HO = re.compile(r"\bLoad HO:\s*UE\s+(\d+)\s+from\s+(\d+)\s*->\s*(\d+)\b")
RE_QOE_HO = re.compile(r"\bQoE HO -> UE\s+(\d+)\s+to cell\s+(\d+)\b")
RE_ENERGY_SLEEP = re.compile(r"\bEnergy:\s+sleeping cell\s+(\d+)\b")
RE_ENERGY_WAKE = re.compile(r"\bEnergy:\s+waking cell\s+(\d+)\b")


@dataclass
class Tick:
    ts: float
    phase: str
    actions: int
    kinds: Counter


def _md_table(headers: List[str], rows: List[List[str]]) -> str:
    if not rows:
        return "_(no rows)_\n"
    out = []
    out.append("| " + " | ".join(headers) + " |")
    out.append("| " + " | ".join(["---"] * len(headers)) + " |")
    for r in rows:
        out.append("| " + " | ".join(r) + " |")
    return "\n".join(out) + "\n"


def _safe_list_literal(s: str) -> List[str]:
    # expects something like "['ho','set_cio']"
    try:
        v = ast.literal_eval(s)
        if isinstance(v, list):
            return [str(x).strip() for x in v]
    except Exception:
        pass
    # fallback: split inside brackets
    inner = s.strip().lstrip("[").rstrip("]")
    parts = [p.strip().strip("'\"") for p in inner.split(",") if p.strip()]
    return parts


def _safe_updates_literal(s: str) -> List[str]:
    # expects something like "['trial_start ...', 'trial_accept ...']"
    try:
        v = ast.literal_eval(s)
        if isinstance(v, list):
            return [str(x) for x in v]
    except Exception:
        pass
    return [s.strip()]


def _infer_phase(ts: float, phase_spans: List[Tuple[float, str]]) -> str:
    # phase_spans sorted by time: [(t_start, phase), ...]
    if not phase_spans:
        return "unknown"
    cur = phase_spans[0][1]
    for t0, ph in phase_spans:
        if ts >= t0:
            cur = ph
        else:
            break
    return cur


def main() -> None:
    ap = argparse.ArgumentParser(description="Parse orchestrator log and summarize ops")
    ap.add_argument("log", type=Path, help="orch.log path")
    ap.add_argument("--out", type=Path, default=Path("orch_ops_report.md"))
    ap.add_argument("--csv", type=Path, default=None, help="optional timeline csv output")
    ap.add_argument("--topk", type=int, default=10, help="top-K pairs to show")
    args = ap.parse_args()

    if not args.log.exists():
        raise SystemExit(f"log not found: {args.log}")

    # Phase timeline
    # default phase before first explicit switch
    phase_spans: List[Tuple[float, str]] = [(0.0, "normal")]

    # Most recent sim_t seen (for attaching APT lines that don't include ts)
    cur_sim_t: Optional[float] = None

    # Per-tick records from commands write lines
    ticks: List[Tick] = []

    # Agent proposal counts (from "actions: energy=.. qoe=.. load=..")
    proposals = []  # rows with (ts, phase, energy, qoe, load, order)
    last_order = None

    # APT events
    apt_events = []  # rows with (ts, phase, msg)
    apt_trial_ctr = Counter()  # start/accept/reject/abort
    apt_knobs = Counter()      # knob name counts

    # Detail action pairs
    cio_pairs = Counter()
    ho_pairs = Counter()
    energy_sleep_cells = Counter()
    energy_wake_cells = Counter()

    lines = args.log.read_text(errors="ignore").splitlines()
    for line in lines:
        # Update current sim time when we see it
        m = RE_SNAPSHOT.search(line)
        if m:
            cur_sim_t = float(m.group(1))

        # Phase changes
        m = RE_PHASE.search(line)
        if m:
            ph = m.group(1)
            t = float(m.group(2))
            phase_spans.append((t, ph))
            continue

        # Agent proposal counts line
        m = RE_AGENT_COUNTS.search(line)
        if m:
            e, q, l = int(m.group(1)), int(m.group(2)), int(m.group(3))
            order = m.group(4).strip()
            last_order = order
            ts = cur_sim_t if cur_sim_t is not None else float("nan")
            ph = _infer_phase(ts if not pd.isna(ts) else 0.0, sorted(phase_spans))
            proposals.append((ts, ph, e, q, l, order))
            continue

        # Commands write line (this is the authoritative "executed batch" per tick)
        m = RE_WRITE.search(line)
        if m:
            actions = int(m.group(1))
            kinds_list = _safe_list_literal(m.group(2))
            ts = float(m.group(3))
            ph = _infer_phase(ts, sorted(phase_spans))
            c = Counter(kinds_list)
            ticks.append(Tick(ts=ts, phase=ph, actions=actions, kinds=c))
            cur_sim_t = ts
            continue

        # APT updates line (list of strings)
        m = RE_APT_UPDATES.search(line)
        if m:
            updates = _safe_updates_literal(m.group(1))
            ts = cur_sim_t if cur_sim_t is not None else float("nan")
            ph = _infer_phase(ts if not pd.isna(ts) else 0.0, sorted(phase_spans))
            for u in updates:
                apt_events.append((ts, ph, u))
                if "trial_start" in u:
                    apt_trial_ctr["start"] += 1
                    # trial_start knob: "trial_start <knob> ..."
                    parts = u.split()
                    if len(parts) >= 2:
                        apt_knobs[parts[1]] += 1
                elif "trial_accept" in u:
                    apt_trial_ctr["accept"] += 1
                    parts = u.split()
                    if len(parts) >= 2:
                        apt_knobs[parts[1]] += 1
                elif "trial_reject" in u:
                    apt_trial_ctr["reject"] += 1
                    parts = u.split()
                    if len(parts) >= 2:
                        apt_knobs[parts[1]] += 1
                elif "trial_abort" in u:
                    apt_trial_ctr["abort"] += 1
                    parts = u.split()
                    if len(parts) >= 3:
                        # "trial_abort (phase change): <knob> ..."
                        apt_knobs[parts[3] if parts[2].endswith(":") and len(parts) >= 4 else parts[2]] += 1
            continue

        # Optional detail parsing
        m = RE_CIO_STEP.search(line) or RE_CIO_REBIND.search(line)
        if m:
            src, nbr = int(m.group(1)), int(m.group(2))
            cio_pairs[(src, nbr)] += 1
            continue

        m = RE_LOAD_HO.search(line)
        if m:
            ue, src, dst = int(m.group(1)), int(m.group(2)), int(m.group(3))
            ho_pairs[(src, dst)] += 1
            continue

        m = RE_QOE_HO.search(line)
        if m:
            ue, dst = int(m.group(1)), int(m.group(2))
            # source cell not in log line; skip pairing here
            continue

        m = RE_ENERGY_SLEEP.search(line)
        if m:
            energy_sleep_cells[int(m.group(1))] += 1
            continue

        m = RE_ENERGY_WAKE.search(line)
        if m:
            energy_wake_cells[int(m.group(1))] += 1
            continue

    phase_spans = sorted(phase_spans, key=lambda x: x[0])

    # Build timeline dataframe
    if ticks:
        df = pd.DataFrame([{
            "ts": t.ts,
            "phase": t.phase,
            "actions": t.actions,
            "ho": int(t.kinds.get("ho", 0)),
            "set_cio": int(t.kinds.get("set_cio", 0)),
            "cell_sleep": int(t.kinds.get("cell_sleep", 0)),
            "cell_wake": int(t.kinds.get("cell_wake", 0)),
        } for t in ticks]).sort_values("ts")
    else:
        df = pd.DataFrame(columns=["ts","phase","actions","ho","set_cio","cell_sleep","cell_wake"])

    # Per-phase summary
    phase_rows = []
    for ph, g in df.groupby("phase"):
        phase_rows.append([
            ph,
            str(len(g)),
            str(int(g["actions"].sum())),
            str(int(g["ho"].sum())),
            str(int(g["set_cio"].sum())),
            str(int(g["cell_sleep"].sum())),
            str(int(g["cell_wake"].sum())),
        ])
    phase_rows.sort(key=lambda r: r[0])

    # Overall counts
    overall = {
        "ticks": int(len(df)),
        "actions_total": int(df["actions"].sum()) if not df.empty else 0,
        "ho": int(df["ho"].sum()) if not df.empty else 0,
        "set_cio": int(df["set_cio"].sum()) if not df.empty else 0,
        "cell_sleep": int(df["cell_sleep"].sum()) if not df.empty else 0,
        "cell_wake": int(df["cell_wake"].sum()) if not df.empty else 0,
        "t_start": float(df["ts"].min()) if not df.empty else float("nan"),
        "t_end": float(df["ts"].max()) if not df.empty else float("nan"),
    }

    # Top pairs
    def top_pairs(counter: Counter, k: int) -> List[List[str]]:
        rows = []
        for (a, b), c in counter.most_common(k):
            rows.append([str(a), str(b), str(c)])
        return rows

    # APT summary
    apt_knob_rows = [[k, str(v)] for k, v in apt_knobs.most_common(args.topk)]

    # proposals dataframe (optional)
    dfp = pd.DataFrame(proposals, columns=["ts", "phase", "energy", "qoe", "load", "order"]) if proposals else pd.DataFrame()

    # Build markdown report
    out = []
    out.append("# Orchestrator Ops Report\n\n")
    out.append(f"- Log: `{args.log}`\n")
    if not math.isnan(overall["t_start"]) and not math.isnan(overall["t_end"]):
        out.append(f"- Sim time span (from writes): {overall['t_start']:.1f}s → {overall['t_end']:.1f}s\n")
    out.append("\n## Overall action counts (from commands write batches)\n\n")
    out.append(_md_table(
        ["ticks", "actions_total", "ho", "set_cio", "cell_sleep", "cell_wake"],
        [[
            str(overall["ticks"]),
            str(overall["actions_total"]),
            str(overall["ho"]),
            str(overall["set_cio"]),
            str(overall["cell_sleep"]),
            str(overall["cell_wake"]),
        ]]
    ))

    out.append("\n## Per-phase breakdown\n\n")
    out.append(_md_table(
        ["phase", "ticks", "actions_total", "ho", "set_cio", "cell_sleep", "cell_wake"],
        phase_rows
    ))

    out.append("\n## Phase timeline (from log)\n\n")
    out.append(_md_table(["t_start", "phase"], [[f"{t:.1f}s", ph] for t, ph in phase_spans]))

    if not dfp.empty:
        out.append("\n## Agent proposal counts (pre-merge)\n\n")
        # show last order and averages per phase
        rows = []
        for ph, g in dfp.groupby("phase"):
            rows.append([
                ph,
                str(len(g)),
                f"{g['energy'].mean():.2f}",
                f"{g['qoe'].mean():.2f}",
                f"{g['load'].mean():.2f}",
                str(g["order"].iloc[-1]) if len(g) else "-",
            ])
        out.append(_md_table(["phase", "samples", "avg_energy", "avg_qoe", "avg_load", "last_order"], rows))

    out.append("\n## APT summary\n\n")
    out.append(_md_table(
        ["trial_start", "trial_accept", "trial_reject", "trial_abort"],
        [[
            str(apt_trial_ctr.get("start", 0)),
            str(apt_trial_ctr.get("accept", 0)),
            str(apt_trial_ctr.get("reject", 0)),
            str(apt_trial_ctr.get("abort", 0)),
        ]]
    ))
    if apt_knob_rows:
        out.append("\n### Most touched knobs\n\n")
        out.append(_md_table(["knob", "count"], apt_knob_rows))

    if apt_events:
        out.append("\n### APT event log (last 25)\n\n")
        last = apt_events[-25:]
        out.append(_md_table(["ts", "phase", "event"], [[f"{t:.1f}", ph, msg] for t, ph, msg in last]))

    if cio_pairs:
        out.append("\n## Top CIO pairs (from agent logs)\n\n")
        out.append(_md_table(["src", "nbr", "count"], top_pairs(cio_pairs, args.topk)))

    if ho_pairs:
        out.append("\n## Top HO pairs (from agent logs)\n\n")
        out.append(_md_table(["src", "dst", "count"], top_pairs(ho_pairs, args.topk)))

    if energy_sleep_cells:
        out.append("\n## Energy sleep counts by cell (from agent logs)\n\n")
        rows = [[str(cid), str(cnt)] for cid, cnt in energy_sleep_cells.most_common(args.topk)]
        out.append(_md_table(["cell", "sleep_count"], rows))

    if energy_wake_cells:
        out.append("\n## Energy wake counts by cell (from agent logs)\n\n")
        rows = [[str(cid), str(cnt)] for cid, cnt in energy_wake_cells.most_common(args.topk)]
        out.append(_md_table(["cell", "wake_count"], rows))

    args.out.write_text("".join(out), encoding="utf-8")
    print(f"Wrote report -> {args.out}")

    if args.csv:
        df.to_csv(args.csv, index=False)
        print(f"Wrote timeline CSV -> {args.csv}")


if __name__ == "__main__":
    main()
