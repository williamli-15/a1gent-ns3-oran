from __future__ import annotations

# a1gent/orchestrator.py
import argparse
import os, time, sqlite3, re
from typing import List, TYPE_CHECKING
from collections import Counter
from .common.util import summarize_counter
from .common.paths import resolve_commands_path, resolve_db_path, resolve_ns3_dir

from .common.log import setup_logger
log = setup_logger("orchestrator")

if TYPE_CHECKING:
    from .common.models import Action


CYCLE_S = float(os.getenv("CYCLE_S", "2.0"))
SNAPSHOT_WINDOW_S = float(os.getenv("SNAPSHOT_WINDOW_S", "300.0"))

# auto-exit if no new sim data for N cycles (set to empty string to disable)
_idle_env = os.getenv("IDLE_CYCLES_BEFORE_EXIT", "5")
IDLE_CYCLES_BEFORE_EXIT = (
    None if _idle_env == "" else int(_idle_env)
)

def merge_actions(*action_batches: List[Action]) -> List[Action]:
    """
    Merge agent outputs with priority order determined by the order of batches.
    De-duplicate per-UE HO actions, per-cell CIO updates, and power-state commands.
    """
    seen_ue = set()
    seen_cio = set()
    seen_power = set()
    merged: List[Action] = []
    for actions in action_batches:
        for a in actions:
            if a.type == "ho":
                if a.ue_e2id in seen_ue:
                    continue
                seen_ue.add(a.ue_e2id)
                merged.append(a)
            elif a.type == "set_cio":
                if a.cellid in seen_cio:
                    continue
                seen_cio.add(a.cellid)
                merged.append(a)
            elif a.type in ("cell_sleep", "cell_wake"):
                if a.cellid in seen_power:
                    continue
                seen_power.add(a.cellid)
                merged.append(a)
            else:
                merged.append(a)
    return merged

def mitigate_actions(actions: List[Action], snap) -> List[Action]:
    """
    Post-filter to prevent cross-usecase conflicts.
    Rules:
      - Never HO/CIO into a cell that is (a) sleeping already (unless waking now) or (b) will sleep this cycle.
      - If a cell is both wake and sleep in same cycle, keep WAKE and drop SLEEP.
      - Optionally drop CIO updates for cells that will sleep now.
    """
    sleeping_now = set(getattr(snap, "sleeping_cells", set()) or set())

    wake_cells = {int(a.cellid) for a in actions if getattr(a, "type", None) == "cell_wake"}
    sleep_cells = {int(a.cellid) for a in actions if getattr(a, "type", None) == "cell_sleep"}

    # If both wake and sleep requested for same cell, prefer WAKE
    sleep_cells = {c for c in sleep_cells if c not in wake_cells}

    # Cells we should NOT steer/HO into
    banned_targets = set(sleep_cells) | (sleeping_now - wake_cells)

    drop = Counter()
    kept: List[Action] = []

    for a in actions:
        t = getattr(a, "type", None)

        if t == "cell_wake":
            kept.append(a)
            continue

        if t == "cell_sleep":
            cid = int(a.cellid)

            # If the cell is already sleeping and there's no wake request this cycle,
            # this sleep is redundant noise; drop it
            if cid in sleeping_now and cid not in wake_cells:
                drop["sleep_already_sleeping"] += 1
                continue

            # if wake+sleep requested in same cycle, keep WAKE and drop SLEEP
            if cid in wake_cells:
                drop["sleep_conflicts_with_wake"] += 1
                continue

            kept.append(a)
            continue

        if t == "ho":
            tgt = int(getattr(a, "target_cellid", 0) or 0)
            if tgt == 0:
                drop["ho_missing_target"] += 1
                continue
            if tgt in banned_targets:
                drop["ho_target_banned"] += 1
                continue
            kept.append(a)
            continue

        if t == "set_cio":
            src = int(getattr(a, "cellid", 0) or 0)
            nbr = getattr(a, "neighbor_cellid", None)
            nbr = int(nbr) if nbr is not None else 0
            if nbr == 0:
                drop["cio_missing_neighbor"] += 1
                continue
            if nbr in banned_targets:
                drop["cio_target_banned"] += 1
                continue
            if src in sleep_cells:
                drop["cio_source_sleeping"] += 1
                continue
            kept.append(a)
            continue

        # Unknown action types: keep (conservative)
        kept.append(a)

    # -------- Prevent premature sleep: ensure the cell is drained (no UEs in snapshot) before sleeping --------
    # Rationale: in the ns-3 bridge, cell_sleep takes effect immediately (inline),
    # while HO is applied asynchronously via commands. In the same cycle, "HO + sleep"
    # can result in sleep happening first.
    # Policy: if the snapshot still shows UEs served by the cell, do not sleep it this cycle
    # (retry on the next cycle).
    served_by_cell = {}
    for ue in getattr(snap, "ues", []) or []:
        served_by_cell.setdefault(int(ue.serving_cell), []).append(int(ue.ue_e2id))

    filtered: List[Action] = []
    for a in kept:
        if getattr(a, "type", None) != "cell_sleep":
            filtered.append(a)
            continue

        cid = int(getattr(a, "cellid", 0) or 0)
        # If the snapshot still shows UEs attached to this cell, do not sleep it yet
        # (wait until the next cycle when UE count reaches 0).
        if served_by_cell.get(cid):
            drop["sleep_ue_still_present"] += 1
            continue
        filtered.append(a)

    kept = filtered

    if drop:
        log.info("post-filter dropped: %s", summarize_counter(drop))

    return kept


def order_actions(actions: List[Action]) -> List[Action]:
    """
    Ensure deterministic ordering in commands.json.
    Bridge applies in file order, so do:
      WAKE -> HO -> CIO -> SLEEP
    """
    rank = {"cell_wake": 0, "ho": 1, "set_cio": 2, "cell_sleep": 3}

    def key(a: Action):
        t = getattr(a, "type", None)
        r = rank.get(t, 99)

        if t in ("cell_wake", "cell_sleep"):
            return (r, int(getattr(a, "cellid", 0) or 0))
        if t == "ho":
            return (r, int(getattr(a, "ue_e2id", 0) or 0))
        if t == "set_cio":
            return (
                r,
                int(getattr(a, "cellid", 0) or 0),
                int(getattr(a, "neighbor_cellid", 0) or 0),
            )
        return (r, 0)

    return sorted(actions, key=key)

_AGENT_KEYS = ("energy", "qoe", "load")

def parse_priority(order: str) -> List[str]:
    tokens = [tok.strip().lower() for tok in re.split(r"[>,;]", order) if tok.strip()]
    deduped: List[str] = []
    for tok in tokens:
        if tok in _AGENT_KEYS and tok not in deduped:
            deduped.append(tok)
    for name in _AGENT_KEYS:
        if name not in deduped:
            deduped.append(name)
    return deduped

def _env_float(name: str):
    val = os.getenv(name)
    if val in (None, ""):
        return None
    try:
        return float(val)
    except ValueError:
        log.warning("Invalid float for %s=%r (ignored)", name, val)
        return None

def load_intent_config():
    orders = {
        "normal": parse_priority(os.getenv("INTENT_NORMAL_ORDER", "energy>qoe>load")),
        "emergency": parse_priority(os.getenv("INTENT_EMERGENCY_ORDER", "qoe>load>energy")),
        "recovery": parse_priority(os.getenv("INTENT_RECOVERY_ORDER", "load>qoe>energy")),
    }
    force_phase = (os.getenv("INTENT_FORCE_PHASE") or "").strip().lower()
    emerg_start = _env_float("INTENT_EMERGENCY_START_S")
    emerg_end = _env_float("INTENT_EMERGENCY_END_S")
    emerg_duration = _env_float("INTENT_EMERGENCY_DURATION_S")
    if emerg_start is not None and emerg_duration is not None:
        emerg_end = emerg_start + emerg_duration
    return {
        "orders": orders,
        "force_phase": force_phase if force_phase in ("normal", "emergency", "recovery") else "",
        "emerg_start": emerg_start,
        "emerg_end": emerg_end,
    }

def determine_phase(sim_sec: float, cfg) -> str:
    forced = cfg["force_phase"]
    if forced:
        return forced
    start = cfg["emerg_start"]
    end = cfg["emerg_end"]
    if start is not None and sim_sec >= start and (end is None or sim_sec < end):
        return "emergency"
    if end is not None and sim_sec >= end:
        return "recovery"
    return "normal"

def priority_for_phase(phase: str, cfg) -> List[str]:
    orders = cfg["orders"]
    return orders.get(phase) or orders["normal"]

def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Run the A1gent orchestrator against an ns-3 O-RAN workspace."
    )
    parser.add_argument(
        "--ns3-dir",
        help="Path to the ns-3 tree that contains oran-repository.db and commands.json.",
    )
    parser.add_argument(
        "--db-path",
        help="Override the SQLite repository path. Defaults to <ns3-dir>/oran-repository.db.",
    )
    parser.add_argument(
        "--commands-path",
        help="Override the command bridge JSON path. Defaults to <ns3-dir>/commands.json.",
    )
    return parser.parse_args(argv)

def main(argv=None):
    args = parse_args(argv)

    from .common.apt import AptTuner
    from .common.commands import write_commands_json
    from .common.db import build_network_snapshot
    from .agents import energy as energy_agent
    from .agents import load as load_agent
    from .agents import qoe as qoe_agent

    ns3_dir = resolve_ns3_dir(args.ns3_dir)
    db_path = resolve_db_path(ns3_dir, args.db_path)
    commands_path = resolve_commands_path(ns3_dir, args.commands_path)

    log.info("Orchestrator rApp - starting")
    log.info("NS3_DIR=%s", ns3_dir)
    log.info("DB=%s", db_path)
    log.info("COMMANDS_PATH=%s", commands_path)

    intent_cfg = load_intent_config()
    log.info(
        "intent priority (normal=%s emergency=%s recovery=%s)",
        ">".join(intent_cfg["orders"]["normal"]),
        ">".join(intent_cfg["orders"]["emergency"]),
        ">".join(intent_cfg["orders"]["recovery"]),
    )
    if intent_cfg["force_phase"]:
        log.info("intent phase forced to %s", intent_cfg["force_phase"])
    elif intent_cfg["emerg_start"] is not None:
        if intent_cfg["emerg_end"] is not None:
            log.info(
                "intent emergency window: start=%.1fs end=%.1fs",
                intent_cfg["emerg_start"],
                intent_cfg["emerg_end"],
            )
        else:
            log.info(
                "intent emergency window: start=%.1fs (open-ended)",
                intent_cfg["emerg_start"],
            )

    try:
        commands_path.parent.mkdir(parents=True, exist_ok=True)
        commands_path.unlink()
    except FileNotFoundError:
        pass

    last_sim_ns = 0
    idle_cycles = 0
    waiting_for_db = False

    WARMUP_S = float(os.getenv("ORCH_WARMUP_S", "3"))
    EXPECTED_UES = int(os.getenv("EXPECTED_UES", "60"))
    EXPECTED_CELLS = int(os.getenv("EXPECTED_CELLS", "21"))
    UE_FRAC = float(os.getenv("ORCH_WARMUP_UE_FRAC", "0.8"))

    # Create APT once; it tunes module globals for later cycles.
    apt = AptTuner(
        energy_mod=energy_agent,
        qoe_mod=qoe_agent,
        load_mod=load_agent,
        log_fn=log.info,
    )

    while True:
        try:
            if not db_path.exists():
                if not waiting_for_db:
                    log.info("DB not available yet at %s; waiting for ns-3 to start", db_path)
                    waiting_for_db = True
                time.sleep(0.5)
                continue
            waiting_for_db = False

            try:
                snap = build_network_snapshot(str(db_path), window_s=SNAPSHOT_WINDOW_S)
            except sqlite3.OperationalError as e:
                log.info("Snapshot not ready yet (%s); retrying shortly", e)
                time.sleep(0.5)
                continue
            sim_ns = getattr(snap, "sim_time_ns", getattr(snap, "_sim_ts_ns", 0))
            sim_sec = sim_ns / 1e9
            log.info(f"snapshot: ues={len(snap.ues)} cells={len(snap.cells)} (sim_t={sim_sec:.0f}s)")

            ready = (
                sim_ns >= int(WARMUP_S * 1e9)
                and (EXPECTED_UES == 0 or len(snap.ues) >= max(1, int(UE_FRAC * EXPECTED_UES)))
                and (EXPECTED_CELLS == 0 or len(snap.cells) >= EXPECTED_CELLS)
            )

            if not hasattr(main, "_warm_streak"):
                main._warm_streak = 0
            main._warm_streak = main._warm_streak + 1 if ready else 0
            if main._warm_streak < 2:
                log.info("warm-up: sim_t>=%.0fs=%s, UEs=%d/%d, cells=%d/%d (streak=%d)",
                        WARMUP_S,
                        sim_ns >= int(WARMUP_S*1e9),
                        len(snap.ues), int(UE_FRAC*EXPECTED_UES),
                        len(snap.cells), EXPECTED_CELLS,
                        main._warm_streak)
                time.sleep(CYCLE_S)
                continue
            if main._warm_streak == 2:
                log.info(
                    "warm-up complete: sim_t=%.1fs UEs=%d cells=%d",
                    sim_sec,
                    len(snap.ues),
                    len(snap.cells),
                )

            # staleness tracking: skip issuing new actions if simulator time is not advancing
            stale_snapshot = sim_ns <= last_sim_ns
            if stale_snapshot:
                idle_cycles += 1
            else:
                idle_cycles = 0
            last_sim_ns = sim_ns

            if IDLE_CYCLES_BEFORE_EXIT is not None and idle_cycles >= IDLE_CYCLES_BEFORE_EXIT:
                log.info(f"No new simulation data for {idle_cycles} cycles; exiting orchestrator.")
                break
            if stale_snapshot and idle_cycles > 0:
                log.debug("stale snapshot (sim_t unchanged for %d cycles); skip cycle", idle_cycles)
                time.sleep(CYCLE_S)
                continue

            if not snap.ues and not snap.cells:
                time.sleep(CYCLE_S)
                continue

            phase = determine_phase(sim_sec, intent_cfg)
            if getattr(main, "_intent_phase", None) != phase:
                log.info("intent phase -> %s (sim_t=%.1fs)", phase, sim_sec)
                main._intent_phase = phase

            # delegate
            agent_actions = {
                "energy": energy_agent.propose_actions(snap),
                "qoe": qoe_agent.propose_actions(snap),
                "load": load_agent.propose_actions(snap),
            }
            priority = priority_for_phase(phase, intent_cfg)

            log.info(
                "actions: energy=%d qoe=%d load=%d (order=%s)",
                len(agent_actions["energy"]),
                len(agent_actions["qoe"]),
                len(agent_actions["load"]),
                ">".join(priority),
            )

            # merge & write
            ordered_batches = [agent_actions[name] for name in priority if name in agent_actions]
            actions = merge_actions(*ordered_batches)
            actions = mitigate_actions(actions, snap)
            actions = order_actions(actions)

            # IMPORTANT: use simulation time seconds, not wall clock
            write_commands_json(str(commands_path), actions, ts=sim_sec)
            if actions:
                log.info("wrote %d actions -> %s", len(actions), commands_path)
            else:
                log.info("no actions this cycle")

            # ---- APT: observe + maybe tune (tuning applies to *next* cycles) ----
            apt.observe(sim_sec, snap, actions, phase)
            updates = apt.maybe_tune(sim_sec, snap, phase)
            if updates:
                log.info("APT applied updates: %s", updates)

        except Exception:
            log.exception("orchestrator error")

        time.sleep(CYCLE_S)

if __name__ == "__main__":
    main()
