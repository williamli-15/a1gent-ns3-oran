import json
import logging
import os
from collections import Counter
from dataclasses import dataclass
from typing import Dict, List, Optional

from pydantic import BaseModel, Field, ValidationError

from ..common.models import NetworkSnapshot, CellSleepAction, CellWakeAction, HoAction
from ..common.util import mean_recent, summarize_counter
from ..common.log import setup_logger
from ..common.llm import get_openrouter_client, structured_completion

log = setup_logger("agent.energy")

# Windows and thresholds
SLEEP_WINDOW_S = float(os.getenv("ENERGY_SLEEP_WINDOW_S", "30.0"))
WAKE_WINDOW_S = float(os.getenv("ENERGY_WAKE_WINDOW_S", "5.0"))
PRB_SLEEP_MAX = float(os.getenv("ENERGY_PRB_SLEEP_MAX", "0.4"))
SCHED_SLEEP_MAX = float(os.getenv("ENERGY_SCHED_SLEEP_MAX", "0.3"))
UE_SLEEP_MAX = int(os.getenv("ENERGY_UE_SLEEP_MAX", "2"))
HO_RATE_MAX = float(os.getenv("ENERGY_HO_RATE_MAX", "0.02"))
HEADROOM_MIN = float(os.getenv("ENERGY_HEADROOM_MIN", "0.2"))
WAKE_UE_COUNT_MIN = int(os.getenv("ENERGY_WAKE_UE_COUNT_MIN", "1"))
WAKE_SCHED_MIN = float(os.getenv("ENERGY_WAKE_SCHED_MIN", "0.4"))
SLEEP_MIN_DWELL = float(os.getenv("ENERGY_SLEEP_MIN_DWELL_S", "30.0"))
MIN_ACTIVE_CELLS = int(os.getenv("ENERGY_MIN_ACTIVE_CELLS", "3"))
ACTIVE_PRB_WAKE = float(os.getenv("ENERGY_ACTIVE_PRB_WAKE", str(max(0.7, 1.0 - HEADROOM_MIN))))
ENERGY_NEIGH_RSRP_MIN_DBM = float(os.getenv("ENERGY_NEIGH_RSRP_MIN_DBM", "-105.0"))

UE_NEIGHBOR_TOPK = int(os.getenv("UE_NEIGHBOR_TOPK", "3"))

USE_LLM = os.getenv("ENERGY_USE_LLM", "1").lower() not in ("0", "false", "no", "off")

if USE_LLM:
    if get_openrouter_client():
        log.info("Energy agent: LLM enabled via OpenRouter (ENERGY_USE_LLM=1)")
    else:
        log.warning("Energy agent: disabling LLM (OpenRouter client unavailable)")
        USE_LLM = False
else:
    log.info("Energy agent: LLM disabled; no automated actions (ENERGY_USE_LLM=0)")


def _log_payload_preview(label: str, payload: str, max_chars: int = 400) -> None:
    if not log.isEnabledFor(logging.DEBUG):
        return
    suffix = ""
    if len(payload) > max_chars:
        payload = payload[:max_chars]
        suffix = "... (truncated)"
    log.debug("%s%s%s", label, payload, suffix)


@dataclass
class CellEnergyStats:
    cell_id: int
    state: str
    ue_count_latest: int
    mean_prb_dl_5m: Optional[float]
    mean_sched_dl_5m: Optional[float]
    ho_arrival_rate: float
    mean_prb_dl_10s: Optional[float]
    mean_sched_dl_10s: Optional[float]
    dwell_s: float
    idle_ready: bool
    wake_ready: bool
    active_after_sleep: int = 0


class EnergyResponse(BaseModel):
    sleep: List[int] = Field(default_factory=list)
    wake: List[int] = Field(default_factory=list)


_cell_state: Dict[int, str] = {}
_last_state_change_ns: Dict[int, int] = {}

ENERGY_RESPONSE_SCHEMA = {
    "type": "object",
    "properties": {
        "sleep": {
            "type": "array",
            "items": {"type": "integer"},
            "default": [],
        },
        "wake": {
            "type": "array",
            "items": {"type": "integer"},
            "default": [],
        },
    },
    "required": ["sleep", "wake"],
    "additionalProperties": False,
}


def _get_state(cell_id: int) -> str:
    return _cell_state.get(cell_id, "awake")


def _dwell_seconds(cell_id: int, sim_ns: int) -> float:
    last_change = _last_state_change_ns.get(cell_id)
    if last_change is None:
        return 0.0
    return (sim_ns - last_change) / 1e9


def _count_ho_arrivals(snapshot: NetworkSnapshot, cell_id: int, window_s: float) -> float:
    sim_ns = snapshot.sim_time_ns
    cutoff = sim_ns - int(window_s * 1e9)
    arrivals = [
        ev for ev in snapshot.ho_events
        if ev.dst_cell == cell_id and ev.sim_ns >= cutoff and "EndOk" in ev.event
    ]
    return len(arrivals) / window_s if window_s > 0 else 0.0


def _mean_series(snapshot_dict, cell_id: int, attr: str, window_s: float, sim_ns: int) -> Optional[float]:
    series = snapshot_dict.get(cell_id, [])
    if not series:
        return None
    return mean_recent(series, attr, sim_ns, window_s)


def _build_stats(snapshot: NetworkSnapshot) -> List[CellEnergyStats]:
    sim_ns = snapshot.sim_time_ns
    sim_time_s = sim_ns / 1e9 if sim_ns else 0.0
    stats: List[CellEnergyStats] = []

    active_count = sum(1 for cell in snapshot.cells if _get_state(cell.cell_id) != "asleep")

    for cell in snapshot.cells:
        if cell.cell_id not in _cell_state:
            _cell_state[cell.cell_id] = "awake"
        if cell.cell_id not in _last_state_change_ns:
            _last_state_change_ns[cell.cell_id] = sim_ns

        ue_count_series = snapshot.cell_ue_count.get(cell.cell_id, [])
        latest_count = int(ue_count_series[-1].value) if ue_count_series else (cell.ue_count or 0)

        mean_prb_5m = _mean_series(snapshot.cell_prb_util, cell.cell_id, "dl", SLEEP_WINDOW_S, sim_ns)
        mean_sched_5m = _mean_series(snapshot.cell_sched_tp, cell.cell_id, "dl", SLEEP_WINDOW_S, sim_ns)
        mean_prb_10s = _mean_series(snapshot.cell_prb_util, cell.cell_id, "dl", WAKE_WINDOW_S, sim_ns)
        mean_sched_10s = _mean_series(snapshot.cell_sched_tp, cell.cell_id, "dl", WAKE_WINDOW_S, sim_ns)

        mean_prb_ul_5m  = _mean_series(snapshot.cell_prb_util, cell.cell_id, "ul", SLEEP_WINDOW_S, sim_ns)
        mean_prb_ul_10s = _mean_series(snapshot.cell_prb_util, cell.cell_id, "ul", WAKE_WINDOW_S, sim_ns)

        ho_rate = _count_ho_arrivals(snapshot, cell.cell_id, SLEEP_WINDOW_S)

        state = _get_state(cell.cell_id)
        dwell_s = _dwell_seconds(cell.cell_id, sim_ns)

        idle_ready = (
            state == "awake"
            and dwell_s >= SLEEP_MIN_DWELL
            and mean_prb_5m is not None
            and mean_prb_5m <= PRB_SLEEP_MAX
            and mean_sched_5m is not None
            and mean_sched_5m <= SCHED_SLEEP_MAX
            and latest_count <= UE_SLEEP_MAX
            and ho_rate <= HO_RATE_MAX
            and sim_time_s >= SLEEP_MIN_DWELL
            and mean_prb_ul_5m is not None
            and mean_prb_ul_5m <= PRB_SLEEP_MAX
        )

        wake_ready = False
        if state == "asleep" and dwell_s >= SLEEP_MIN_DWELL:
            if latest_count >= WAKE_UE_COUNT_MIN:
                wake_ready = True
            elif mean_sched_10s is not None and mean_sched_10s >= WAKE_SCHED_MIN:
                wake_ready = True
        stats.append(
            CellEnergyStats(
                cell_id=cell.cell_id,
                state=state,
                ue_count_latest=latest_count,
                mean_prb_dl_5m=mean_prb_5m,
                mean_sched_dl_5m=mean_sched_5m,
                ho_arrival_rate=ho_rate,
                mean_prb_dl_10s=mean_prb_10s,
                mean_sched_dl_10s=mean_sched_10s,
                dwell_s=dwell_s,
                idle_ready=idle_ready,
                wake_ready=wake_ready,
            )
        )

    active_cells = [s for s in stats if s.state == "awake"]
    sleeping_cells = [s for s in stats if s.state == "asleep"]
    active_count = len(active_cells)

    active_prb_10s_values = []
    for s in active_cells:
        dl = s.mean_prb_dl_10s
        ul = _mean_series(snapshot.cell_prb_util, s.cell_id, "ul", WAKE_WINDOW_S, sim_ns)
        if dl is None and ul is None:
            continue
        active_prb_10s_values.append(max(dl or 0.0, ul or 0.0))

    active_prb_10s_mean = (
        sum(active_prb_10s_values) / len(active_prb_10s_values)
        if active_prb_10s_values else None
    )

    for s in stats:
        if s.state == "awake":
            s.active_after_sleep = max(active_count - 1, 0)
            if s.idle_ready and s.active_after_sleep < MIN_ACTIVE_CELLS:
                s.idle_ready = False
        else:
            s.active_after_sleep = active_count
            if (
                not s.wake_ready
                and active_prb_10s_mean is not None
                and active_prb_10s_mean >= ACTIVE_PRB_WAKE
            ):
                s.wake_ready = True

    return stats


def _build_payload(stats: List[CellEnergyStats], snapshot: NetworkSnapshot) -> str:
    active_cells = [s for s in stats if s.state == "awake"]
    sleeping_cells = [s for s in stats if s.state == "asleep"]
    total_ue = sum(s.ue_count_latest for s in stats)

    def _avg(values: List[Optional[float]]) -> Optional[float]:
        vals = [v for v in values if v is not None]
        return (sum(vals) / len(vals)) if vals else None

    summary = {
        "active_cell_count": len(active_cells),
        "sleeping_cell_count": len(sleeping_cells),
        "min_active_cells": MIN_ACTIVE_CELLS,
        "total_ue": total_ue,
        "active_prb_dl_sleep_win_mean": _avg([s.mean_prb_dl_5m for s in active_cells]),
        "active_prb_dl_wake_win_mean": _avg([s.mean_prb_dl_10s for s in active_cells]),
        "active_sched_dl_mbps_sleep_win_mean": _avg([s.mean_sched_dl_5m for s in active_cells]),
        "active_sched_dl_mbps_wake_win_mean": _avg([s.mean_sched_dl_10s for s in active_cells]),
    }

    payload = {
        "sim_time_s": round(snapshot.sim_time_ns / 1e9, 3),

        # Explicit window definitions (avoid ambiguous labels like "5min" / "10s")
        "windows_s": {
            "sleep_window_s": SLEEP_WINDOW_S,   # used for *_sleep_win metrics
            "wake_window_s": WAKE_WINDOW_S,     # used for *_wake_win metrics
        },

        # Units reference (prevents the LLM from guessing semantics)
        "units": {
            "prb_util": "fraction in [0,1]",
            "sched_dl_mbps": "Mbps (MAC scheduled DL throughput)",
            "ho_arrival_rate_per_s": "events_per_second",
            "ue_count": "UEs",
            "dwell_s": "seconds",
        },

        # Definitions for the LLM payload
        "definitions": {
            "*_sleep_win": "mean over the last sleep_window_s seconds",
            "*_wake_win": "mean over the last wake_window_s seconds",
            "idle_ready": "cell is eligible for sleep (already checked by python guards)",
            "wake_ready": "cell is eligible for wake (already checked by python guards)",
            "active_after_sleep": "active-cell count if this single cell is slept (does not account for multiple sleeps)",
        },

        "thresholds": {
            "PRB_SLEEP_MAX": PRB_SLEEP_MAX,
            "SCHED_SLEEP_MAX": SCHED_SLEEP_MAX,
            "UE_SLEEP_MAX": UE_SLEEP_MAX,
            "HO_RATE_MAX": HO_RATE_MAX,
            "HEADROOM_MIN": HEADROOM_MIN,
            "NEIGH_RSRP_MIN_DBM": ENERGY_NEIGH_RSRP_MIN_DBM,
            "WAKE_UE_COUNT_MIN": WAKE_UE_COUNT_MIN,
            "WAKE_SCHED_MIN": WAKE_SCHED_MIN,
            "SLEEP_MIN_DWELL": SLEEP_MIN_DWELL,
            "MIN_ACTIVE_CELLS": MIN_ACTIVE_CELLS,
            "ACTIVE_PRB_WAKE": ACTIVE_PRB_WAKE,
        },

        "summary": {
            "active_cell_count": summary["active_cell_count"],
            "sleeping_cell_count": summary["sleeping_cell_count"],
            "min_active_cells": MIN_ACTIVE_CELLS,
            "total_ue": total_ue,
            "active_prb_dl_sleep_win_mean": round(summary["active_prb_dl_sleep_win_mean"], 4) if summary["active_prb_dl_sleep_win_mean"] is not None else None,
            "active_prb_dl_wake_win_mean": round(summary["active_prb_dl_wake_win_mean"], 4) if summary["active_prb_dl_wake_win_mean"] is not None else None,
            "active_sched_dl_mbps_sleep_win_mean": round(summary["active_sched_dl_mbps_sleep_win_mean"], 3) if summary["active_sched_dl_mbps_sleep_win_mean"] is not None else None,
            "active_sched_dl_mbps_wake_win_mean": round(summary["active_sched_dl_mbps_wake_win_mean"], 3) if summary["active_sched_dl_mbps_wake_win_mean"] is not None else None,
        },

        "active_cells": [
            {
                "cell_id": s.cell_id,
                "state": s.state,
                "dwell_s": round(s.dwell_s, 1),
                "ue_count": s.ue_count_latest,

                "mean_prb_dl_sleep_win": round(s.mean_prb_dl_5m, 4) if s.mean_prb_dl_5m is not None else None,
                "mean_prb_dl_wake_win": round(s.mean_prb_dl_10s, 4) if s.mean_prb_dl_10s is not None else None,

                "mean_sched_dl_mbps_sleep_win": round(s.mean_sched_dl_5m, 3) if s.mean_sched_dl_5m is not None else None,
                "mean_sched_dl_mbps_wake_win": round(s.mean_sched_dl_10s, 3) if s.mean_sched_dl_10s is not None else None,

                "ho_arrival_rate_per_s": round(s.ho_arrival_rate, 4),
                "idle_ready": s.idle_ready,
                "active_after_sleep": s.active_after_sleep,
            }
            for s in active_cells
        ],

        "sleeping_cells": [
            {
                "cell_id": s.cell_id,
                "state": s.state,
                "dwell_s": round(s.dwell_s, 1),
                "ue_count": s.ue_count_latest,

                "mean_prb_dl_wake_win": round(s.mean_prb_dl_10s, 4) if s.mean_prb_dl_10s is not None else None,
                "mean_sched_dl_mbps_wake_win": round(s.mean_sched_dl_10s, 3) if s.mean_sched_dl_10s is not None else None,

                "wake_ready": s.wake_ready,
            }
            for s in sleeping_cells
        ],

        "cells": [
            {
                "cell_id": s.cell_id,
                "state": s.state,
                "dwell_s": round(s.dwell_s, 1),
                "ue_count": s.ue_count_latest,

                "mean_prb_dl_sleep_win": round(s.mean_prb_dl_5m, 4) if s.mean_prb_dl_5m is not None else None,
                "mean_sched_dl_mbps_sleep_win": round(s.mean_sched_dl_5m, 3) if s.mean_sched_dl_5m is not None else None,

                "mean_prb_dl_wake_win": round(s.mean_prb_dl_10s, 4) if s.mean_prb_dl_10s is not None else None,
                "mean_sched_dl_mbps_wake_win": round(s.mean_sched_dl_10s, 3) if s.mean_sched_dl_10s is not None else None,

                "ho_arrival_rate_per_s": round(s.ho_arrival_rate, 4),

                "idle_ready": s.idle_ready,
                "wake_ready": s.wake_ready,
                "active_after_sleep": s.active_after_sleep,
                "min_active_cells": MIN_ACTIVE_CELLS,
            }
            for s in stats
        ],

        "instructions": (
            "Return ONLY the JSON object {'sleep': [cell_ids], 'wake': [cell_ids]}.\n"
            "Rules (MUST follow):\n"
            "1) Only include a cell in 'sleep' if idle_ready == true.\n"
            "2) Only include a cell in 'wake'  if wake_ready == true.\n"
            "3) Do NOT reduce active cells below MIN_ACTIVE_CELLS.\n"
            "4) Prefer fewer actions; avoid toggling.\n"
            "Notes:\n"
            "- *_sleep_win metrics are means over the last sleep_window_s seconds.\n"
            "- *_wake_win metrics are means over the last wake_window_s seconds.\n"
        ),
    }

    return json.dumps(payload, indent=2)


def _query_llm(stats: List[CellEnergyStats], snapshot: NetworkSnapshot) -> EnergyResponse:
    if not (USE_LLM and stats):
        return EnergyResponse()

    payload = _build_payload(stats, snapshot)
    _log_payload_preview("Energy LLM payload: ", payload)

    raw = structured_completion(
        model=os.getenv("ENERGY_LLM_MODEL", "google/gemini-2.5-flash"),
        system_prompt=(
        "You manage energy saving for LTE cells.\n"
        "Follow ALL rules and definitions in the user payload (thresholds/units/definitions/instructions).\n"
        "Return ONLY a JSON object that matches the provided schema. No extra text.\n"
        ),
        user_content=payload,
        schema_name="energy_response",
        schema=ENERGY_RESPONSE_SCHEMA,
    )
    if not raw:
        return EnergyResponse()
    if log.isEnabledFor(logging.DEBUG):
        log.debug("Energy LLM raw response: %s", raw)
    try:
        return EnergyResponse.model_validate(raw)
    except ValidationError as e:
        log.warning("Energy LLM validation error: %s", e)
        return EnergyResponse()


def _neighbor_headroom(snapshot: NetworkSnapshot, cell_id: int) -> Optional[float]:
    series = snapshot.cell_prb_util.get(cell_id, [])
    if not series:
        return None
    last = series[-1]
    try:
        util = float(last.dl)
    except Exception:
        return None
    return max(0.0, 1.0 - util)

def _offload_ues(snapshot: NetworkSnapshot, cell_id: int, banned_targets: Optional[set[int]] = None) -> List[HoAction]:
    actions: List[HoAction] = []
    banned_targets = set(banned_targets or set())
    sleeping_cells = set(getattr(snapshot, "sleeping_cells", set()) or set())

    for ue in snapshot.ues:
        if ue.serving_cell != cell_id:
            continue

        # Build candidate neighbor list (top-K). Fallback to legacy top-1.
        cand = []
        if getattr(ue, "neighbor_cells", None):
            for n in ue.neighbor_cells[:UE_NEIGHBOR_TOPK]:
                cand.append((int(n.cellid), float(n.rsrp_dbm)))
        elif ue.neighbor_cell is not None:
            cand.append((int(ue.neighbor_cell), float(ue.neighbor_rsrp or -140.0)))

        # Sort by RSRP desc
        cand.sort(key=lambda x: x[1], reverse=True)

        target = None
        for nbr_cell, rsrp_dbm in cand:
            if nbr_cell == ue.serving_cell:
                continue
            if nbr_cell in sleeping_cells:
                continue
            if nbr_cell in banned_targets:
                continue

            headroom = _neighbor_headroom(snapshot, nbr_cell)
            if headroom is None or headroom < HEADROOM_MIN:
                continue

            if rsrp_dbm < ENERGY_NEIGH_RSRP_MIN_DBM:
                continue

            target = nbr_cell
            break

        if target is None:
            log.debug("Energy: UE %s offload blocked (no feasible neighbor)", ue.ue_e2id)
            continue

        actions.append(HoAction(type="ho", ue_e2id=ue.ue_e2id, target_cellid=target))

    return actions


def _apply_decisions(snapshot: NetworkSnapshot, stats: List[CellEnergyStats], decisions: EnergyResponse) -> List:
    sim_ns = snapshot.sim_time_ns
    stats_by_cell = {s.cell_id: s for s in stats}
    actions: List = []

    # --- HARD CAP: never sleep below MIN_ACTIVE_CELLS ---
    awake_now = sum(1 for s in stats if s.state == "awake")
    max_sleep = max(0, awake_now - MIN_ACTIVE_CELLS)

    sleep_plan = list(decisions.sleep)
    wake_plan = list(decisions.wake)

    orig_sleep_requested = len(sleep_plan)

    if len(sleep_plan) > max_sleep:
        # Pick the most idle cells first (deterministic)
        def _rank(cid: int):
            st = stats_by_cell.get(cid)
            if not st:
                return (1e9, 1e9, 1e9, cid)
            prb = st.mean_prb_dl_5m if st.mean_prb_dl_5m is not None else 1e9
            sched = st.mean_sched_dl_5m if st.mean_sched_dl_5m is not None else 1e9
            ue = st.ue_count_latest
            return (prb, sched, ue, cid)

        sleep_plan = sorted(sleep_plan, key=_rank)[:max_sleep]
        log.info(
            "Energy: capped sleep list %d -> %d (awake_now=%d, MIN_ACTIVE_CELLS=%d)",
            orig_sleep_requested, len(sleep_plan), awake_now, MIN_ACTIVE_CELLS
        )

    sleep_requested = len(sleep_plan)
    wake_requested = len(wake_plan)

    sleep_blocks: Counter[str] = Counter()
    wake_blocks: Counter[str] = Counter()


    for cid in sleep_plan:
        st = stats_by_cell.get(cid)
        if not st:
            sleep_blocks["missing_stats"] += 1
            log.debug("Energy: skip sleep for cell %s (no stats)", cid)
            continue
        if not st.idle_ready:
            sleep_blocks["idle_guard"] += 1
            log.debug("Energy: skip sleep for cell %s (guard false)", cid)
            continue
        banned = set(sleep_plan) - {cid}
        ho_actions = _offload_ues(snapshot, cid, banned_targets=banned)
        if ho_actions:
            actions.extend(ho_actions)
        if any(ue.serving_cell == cid for ue in snapshot.ues):
            # UEs still attached; offload only this cycle and do not sleep yet
            sleep_blocks["sleep_pending_offload"] += 1
            continue
        actions.append(CellSleepAction(type="cell_sleep", cellid=cid))
        _cell_state[cid] = "asleep"
        _last_state_change_ns[cid] = sim_ns
        log.info("Energy: sleeping cell %s (ho=%d)", cid, len(ho_actions))

    for cid in wake_plan:
        st = stats_by_cell.get(cid)
        if not st:
            wake_blocks["missing_stats"] += 1
            log.debug("Energy: skip wake for cell %s (no stats)", cid)
            continue
        if not st.wake_ready:
            wake_blocks["wake_guard"] += 1
            log.debug("Energy: skip wake for cell %s (guard false)", cid)
            continue
        actions.append(CellWakeAction(type="cell_wake", cellid=cid))
        _cell_state[cid] = "awake"
        _last_state_change_ns[cid] = sim_ns
        log.info("Energy: waking cell %s", cid)

    executed_sleep = sum(1 for a in actions if isinstance(a, CellSleepAction))
    executed_wake = sum(1 for a in actions if isinstance(a, CellWakeAction))

    if sleep_blocks:
        log.info(
            "Energy: sleep guards blocked %d/%d cells (%s)",
            sum(sleep_blocks.values()),
            sleep_requested,
            summarize_counter(sleep_blocks),
        )
    if wake_blocks:
        log.info(
            "Energy: wake guards blocked %d/%d cells (%s)",
            sum(wake_blocks.values()),
            wake_requested,
            summarize_counter(wake_blocks),
        )
    if executed_sleep or executed_wake:
        log.info(
            "Energy: executed sleep=%d wake=%d (requested sleep=%d, wake=%d)",
            executed_sleep,
            executed_wake,
            sleep_requested,
            wake_requested,
        )
    elif sleep_requested or wake_requested:
        log.info(
            "Energy: no energy actions after guards (sleep_req=%d, wake_req=%d)",
            sleep_requested,
            wake_requested,
        )

    return actions


def propose_actions(snapshot: NetworkSnapshot) -> List:
    stats = _build_stats(snapshot)
    if not stats:
        return []

    sleep_ready = [s for s in stats if s.idle_ready]
    wake_ready = [s for s in stats if s.wake_ready]
    if not sleep_ready and not wake_ready:
        log.info("Energy: no cells pass sleep/wake guards this cycle")
        return []

    if USE_LLM:
        response = _query_llm(stats, snapshot)
        if not response.sleep and not response.wake:
            log.debug("Energy: LLM opted for no energy actions this cycle")
        else:
            log.info(
                "Energy: LLM plan -> sleep=%d wake=%d",
                len(response.sleep),
                len(response.wake),
            )
    else:
        log.info("Energy agent: LLM disabled; no automated energy actions")
        response = EnergyResponse()

    return _apply_decisions(snapshot, stats, response)
