import json
import logging
import os
from collections import Counter
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

from pydantic import BaseModel, Field, ValidationError

from ..common.models import NetworkSnapshot, SetCioAction, HoAction, UeSnapshot
from ..common.util import (
    mean_recent,
    percentile_recent,
    recent,
    last,
    clamp,
    summarize_counter,
)
from ..common.log import setup_logger
from ..common.llm import get_openrouter_client, structured_completion

log = setup_logger("agent.load")


def _log_payload_preview(payload: str, max_chars: int = 400) -> None:
    if not log.isEnabledFor(logging.DEBUG):
        return
    suffix = ""
    if len(payload) > max_chars:
        payload = payload[:max_chars]
        suffix = "... (truncated)"
    log.debug("Load LLM payload: %s%s", payload, suffix)

# Rolling window (seconds) for cell/UE aggregates
LOAD_ROLLING_WINDOW_S = float(os.getenv("LOAD_ROLLING_WINDOW_S", "4.0"))

# Thresholds. Override via environment variables when needed.
HEADROOM_HOT_MAX = float(os.getenv("LOAD_HEADROOM_HOT_MAX", "0.10"))
HEADROOM_COOL_MIN = float(os.getenv("LOAD_HEADROOM_COOL_MIN", "0.30"))
CIO_STEP = float(os.getenv("LOAD_CIO_STEP_DB", "1.0"))
CIO_MIN = float(os.getenv("LOAD_CIO_MIN_DB", "-6.0"))
CIO_MAX = float(os.getenv("LOAD_CIO_MAX_DB", "6.0"))
HO_MIN_SINR_DB = float(os.getenv("LOAD_HO_MIN_SINR_DB", "-5.0"))
HO_TARGET_MCS_P50_MIN = float(os.getenv("LOAD_HO_TARGET_MCS_P50_MIN", "5.0"))
HO_TARGET_UL_INTERF_MAX_DBM = float(os.getenv("LOAD_HO_TARGET_UL_INTERF_MAX_DBM", "-85.0"))
CIO_INTERVAL = float(os.getenv("LOAD_CIO_INTERVAL_S", "15.0"))
MAX_PAIRS = int(os.getenv("LOAD_MAX_PAIRS", "6"))
ELEPHANTS_PER_PAIR = int(os.getenv("LOAD_ELEPHANTS_PER_PAIR", "1"))
HO_DWELL_MIN = float(os.getenv("LOAD_HO_DWELL_MIN_S", "5.0"))
HO_COOLDOWN = float(os.getenv("LOAD_HO_COOLDOWN_S", "30.0"))

USE_LLM = os.getenv("LOAD_USE_LLM", "1").lower() not in ("0", "false", "no", "off")

if USE_LLM:
    if get_openrouter_client():
        log.info("Load agent: LLM enabled via OpenRouter (LOAD_USE_LLM=1)")
    else:
        log.warning("Load agent: disabling LLM (OpenRouter client unavailable)")
        USE_LLM = False
else:
    log.info("Load agent: LLM disabled; no automated actions (LOAD_USE_LLM=0)")


@dataclass
class CellView:
    cell_id: int
    load_margin: Optional[float]
    prb_util: Optional[float]
    sched_dl: Optional[float]
    sched_ul: Optional[float]
    mcs_dl_mean: Optional[float]
    mcs_dl_p50: Optional[float]
    mcs_dl_p95: Optional[float]
    mcs_ul_p50: Optional[float]
    ul_interf_dbm: Optional[float]
    ue_count: Optional[int]
    cio_offset: float
    cooldown_remaining: float


@dataclass
class Elephant:
    ue: UeSnapshot
    avg_prb_dl: float
    avg_prb_ul: float
    avg_pdcp_dl: float
    median_sinr: Optional[float]


@dataclass
class OverloadPair:
    source: CellView
    target: CellView
    elephants: List[Elephant]


class CioDecision(BaseModel):
    source_cell: int = Field(..., description="Overloaded cell to bias away from")
    target_cell: int = Field(..., description="Neighbour cell receiving traffic")
    delta_db: float = Field(..., description="Apply +/- step to the CIO offset of the source cell")
    reason: str


class LoadHoDecision(BaseModel):
    ue_e2id: int
    source_cell: int
    target_cell: int
    reason: str


class LoadResponse(BaseModel):
    cio: List[CioDecision] = Field(default_factory=list)
    handovers: List[LoadHoDecision] = Field(default_factory=list)


# src_cell -> (current_neighbor_cell, offset_db)
_cio_state: Dict[int, Tuple[Optional[int], float]] = {}
_last_cio_action_ns: Dict[int, int] = {}
_last_ho_action_ns: Dict[int, int] = {}

LOAD_RESPONSE_SCHEMA = {
    "type": "object",
    "properties": {
        "cio": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "source_cell": {"type": "integer"},
                    "target_cell": {"type": "integer"},
                    "delta_db": {"type": "number"},
                    "reason": {"type": "string"},
                },
                "required": ["source_cell", "target_cell", "delta_db", "reason"],
                "additionalProperties": False,
            },
            "default": [],
        },
        "handovers": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "ue_e2id": {"type": "integer"},
                    "source_cell": {"type": "integer"},
                    "target_cell": {"type": "integer"},
                    "reason": {"type": "string"},
                },
                "required": ["ue_e2id", "source_cell", "target_cell", "reason"],
                "additionalProperties": False,
            },
            "default": [],
        },
    },
    "required": ["cio", "handovers"],
    "additionalProperties": False,
}


def _get_cio_offset(cell_id: int) -> float:
    st = _cio_state.get(cell_id)
    return st[1] if st else 0.0


def _cio_cooldown(cell_id: int, sim_ns: int) -> float:
    last_ns = _last_cio_action_ns.get(cell_id)
    if last_ns is None:
        return 0.0
    delta_s = (sim_ns - last_ns) / 1e9
    return max(0.0, CIO_INTERVAL - delta_s)


def _build_cell_views(snapshot: NetworkSnapshot) -> Dict[int, CellView]:
    sim_ns = snapshot.sim_time_ns
    views: Dict[int, CellView] = {}

    for cell in snapshot.cells:
        series_prb = snapshot.cell_prb_util.get(cell.cell_id, [])
        avg_prb_util = mean_recent(series_prb, "dl", sim_ns, LOAD_ROLLING_WINDOW_S)
        if avg_prb_util is None:
            avg_prb_util = cell.prb_dl_util
        load_margin = None
        if avg_prb_util is not None:
            load_margin = max(0.0, 1.0 - avg_prb_util)

        sched_series = snapshot.cell_sched_tp.get(cell.cell_id, [])
        sched_dl = mean_recent(sched_series, "dl", sim_ns, LOAD_ROLLING_WINDOW_S)
        sched_ul = mean_recent(sched_series, "ul", sim_ns, LOAD_ROLLING_WINDOW_S)
        if sched_dl is None:
            sched_dl = cell.sched_dl_mbps
        if sched_ul is None:
            sched_ul = cell.sched_ul_mbps

        mcs_series = snapshot.cell_mcs.get(cell.cell_id, [])
        mcs_sample = last(mcs_series)
        ul_interf_series = snapshot.cell_ul_interf.get(cell.cell_id, [])
        ul_interf_sample = last(ul_interf_series)

        views[cell.cell_id] = CellView(
            cell_id=cell.cell_id,
            load_margin=load_margin,
            prb_util=avg_prb_util,
            sched_dl=sched_dl,
            sched_ul=sched_ul,
            mcs_dl_mean=(mcs_sample.dl_mean if mcs_sample else cell.mcs_dl_mean),
            mcs_dl_p50=(mcs_sample.dl_p50 if mcs_sample else cell.mcs_dl_p50),
            mcs_dl_p95=(mcs_sample.dl_p95 if mcs_sample else cell.mcs_dl_p95),
            mcs_ul_p50=(mcs_sample.ul_p50 if mcs_sample else cell.mcs_ul_p50),
            ul_interf_dbm=(ul_interf_sample.p95_dbm if ul_interf_sample else cell.ul_interf_dbm),
            ue_count=cell.ue_count,
            cio_offset=_get_cio_offset(cell.cell_id),
            cooldown_remaining=_cio_cooldown(cell.cell_id, sim_ns),
        )

    return views


def _collect_neighbour_pairs(snapshot: NetworkSnapshot) -> List[Tuple[int, int]]:
    pairs = set()
    for ue in snapshot.ues:
        if ue.neighbor_cell is None or ue.neighbor_cell == ue.serving_cell:
            continue
        pairs.add((ue.serving_cell, ue.neighbor_cell))
    return list(pairs)


def _elephants_for_pair(snapshot: NetworkSnapshot, source: int, target: int) -> List[Elephant]:
    sim_ns = snapshot.sim_time_ns
    elephants: List[Elephant] = []

    for ue in snapshot.ues:
        if ue.serving_cell != source:
            continue
        if ue.neighbor_cell != target:
            continue

        prb_series = snapshot.ue_prb.get(ue.ue_e2id, [])
        avg_prb_dl = mean_recent(prb_series, "dl", sim_ns, LOAD_ROLLING_WINDOW_S) or 0.0
        avg_prb_ul = mean_recent(prb_series, "ul", sim_ns, LOAD_ROLLING_WINDOW_S) or 0.0
        if avg_prb_dl <= 0.0:
            continue

        pdcp_series = snapshot.ue_pdcp_tp.get(ue.ue_e2id, [])
        avg_pdcp_dl = mean_recent(pdcp_series, "dl", sim_ns, LOAD_ROLLING_WINDOW_S) or 0.0
        sinr_series = snapshot.ue_sinr.get(ue.ue_e2id, [])
        median_sinr = percentile_recent(sinr_series, "sinr_db", sim_ns, LOAD_ROLLING_WINDOW_S, 50.0)

        elephants.append(
            Elephant(
                ue=ue,
                avg_prb_dl=avg_prb_dl,
                avg_prb_ul=avg_prb_ul,
                avg_pdcp_dl=avg_pdcp_dl,
                median_sinr=median_sinr,
            )
        )

    elephants.sort(key=lambda e: e.avg_prb_dl, reverse=True)
    return elephants[:ELEPHANTS_PER_PAIR]


def _build_pairs(snapshot: NetworkSnapshot) -> Tuple[List[OverloadPair], Counter[str], int]:
    cell_views = _build_cell_views(snapshot)
    pairs: List[OverloadPair] = []
    skip_stats: Counter[str] = Counter()
    inspected = 0

    for src_cell, tgt_cell in _collect_neighbour_pairs(snapshot):
        inspected += 1
        src_view = cell_views.get(src_cell)
        tgt_view = cell_views.get(tgt_cell)
        if not src_view or not tgt_view:
            skip_stats["missing_view"] += 1
            continue
        if src_view.load_margin is None or tgt_view.load_margin is None:
            skip_stats["missing_margin"] += 1
            continue
        if src_view.load_margin > HEADROOM_HOT_MAX:
            skip_stats["source_not_hot"] += 1
            continue  # source not overloaded enough
        if tgt_view.load_margin < HEADROOM_COOL_MIN:
            skip_stats["target_not_cool"] += 1
            continue  # target not light enough

        elephants = _elephants_for_pair(snapshot, src_cell, tgt_cell)
        pairs.append(OverloadPair(source=src_view, target=tgt_view, elephants=elephants))

    pairs.sort(key=lambda p: p.source.load_margin if p.source.load_margin is not None else 1.0)
    return pairs[:MAX_PAIRS], skip_stats, inspected


def _pairs_payload(pairs: List[OverloadPair], snapshot: NetworkSnapshot) -> str:
    sleeping_cells = sorted((getattr(snapshot, "sleeping_cells", set()) or set()))
    active_cells = sorted({c.cell_id for c in snapshot.cells if c.cell_id not in sleeping_cells})

    def _avg(values: List[Optional[float]]) -> Optional[float]:
        vals = [v for v in values if v is not None]
        if not vals:
            return None
        return sum(vals) / len(vals)

    active_prb = _avg([
        mean_recent(snapshot.cell_prb_util.get(cell_id, []), "dl", snapshot.sim_time_ns, LOAD_ROLLING_WINDOW_S)
        for cell_id in active_cells
    ])
    active_sched = _avg([
        mean_recent(snapshot.cell_sched_tp.get(cell_id, []), "dl", snapshot.sim_time_ns, LOAD_ROLLING_WINDOW_S)
        for cell_id in active_cells
    ])
    total_ue = sum(c.ue_count or 0 for c in snapshot.cells)

    payload = {
        "sim_time_s": round(snapshot.sim_time_ns / 1e9, 3),

        # tell LLM the lookback horizon
        "windows_s": {"rolling_s": LOAD_ROLLING_WINDOW_S},

        # units table (LLM stops guessing)
        "units": {
            "prb_util": "fraction in [0,1]",
            "load_margin": "fraction in [0,1] (1 - prb_util)",
            "sched_dl_mbps": "Mbps (MAC scheduled DL throughput)",
            "ul_interf_dbm": "dBm (UL interference p95)",
            "cio_offset_db": "dB",
            "avg_dl_prbs_per_1s_mean": "DL PRBs per 1s UE PRB-sample, then averaged over rolling_s",
            "avg_ul_prbs_per_1s_mean": "UL PRBs per 1s UE PRB-sample, then averaged over rolling_s",
            "median_sinr_db": "dB (UE serving-cell SINR)",
        },

        # Definitions
        "definitions": {
            "rolling_s": "lookback horizon for means/percentiles in this payload",
            "elephant": "a heavy UE (high DL PRB usage) under an overloaded source cell",
        },

        "thresholds": {
            "HEADROOM_HOT_MAX": HEADROOM_HOT_MAX,
            "HEADROOM_COOL_MIN": HEADROOM_COOL_MIN,
            "CIO_MIN_DB": CIO_MIN,
            "CIO_MAX_DB": CIO_MAX,
            "CIO_STEP": CIO_STEP,
            "HO_MIN_SINR_DB": HO_MIN_SINR_DB,
            "HO_TARGET_MCS_P50_MIN": HO_TARGET_MCS_P50_MIN,
            "HO_TARGET_UL_INTERF_MAX_DBM": HO_TARGET_UL_INTERF_MAX_DBM,
            "CIO_INTERVAL": CIO_INTERVAL,
        },

        "summary": {
            "active_cells": active_cells,
            "sleeping_cells": sleeping_cells,
            "total_ue": total_ue,
            "active_prb_dl_mean": round(active_prb, 4) if active_prb is not None else None,
            "active_sched_dl_mean": round(active_sched, 3) if active_sched is not None else None,
        },

        "pairs": [
            {
                "source_cell": p.source.cell_id,
                "target_cell": p.target.cell_id,
                "source": {
                    "load_margin": round(p.source.load_margin, 3) if p.source.load_margin is not None else None,
                    "prb_util": round(p.source.prb_util, 3) if p.source.prb_util is not None else None,
                    "sched_dl_mbps": round(p.source.sched_dl, 3) if p.source.sched_dl is not None else None,
                    "ue_count": p.source.ue_count,
                    "cio_offset_db": round(p.source.cio_offset, 2),
                    "cooldown_remaining_s": round(p.source.cooldown_remaining, 1),
                },
                "target": {
                    "load_margin": round(p.target.load_margin, 3) if p.target.load_margin is not None else None,
                    "prb_util": round(p.target.prb_util, 3) if p.target.prb_util is not None else None,
                    "sched_dl_mbps": round(p.target.sched_dl, 3) if p.target.sched_dl is not None else None,
                    "mcs_dl_p50": round(p.target.mcs_dl_p50, 2) if p.target.mcs_dl_p50 is not None else None,
                    "ul_interf_dbm": round(p.target.ul_interf_dbm, 2) if p.target.ul_interf_dbm is not None else None,
                    "ue_count": p.target.ue_count,
                },

                "elephants": [
                    {
                        "ue_e2id": e.ue.ue_e2id,
                        "avg_dl_prbs_per_1s_mean": round(e.avg_prb_dl, 2),
                        "avg_ul_prbs_per_1s_mean": round(e.avg_prb_ul, 2),
                        "avg_pdcp_dl_mbps": round(e.avg_pdcp_dl, 3),
                        "median_sinr_db": round(e.median_sinr, 2) if e.median_sinr is not None else None,
                        "neighbor_cell": e.ue.neighbor_cell,
                    }
                    for e in p.elephants
                ],
            }
            for p in pairs
        ],

        "instructions": (
            "Return ONLY the JSON object {'cio': [...], 'handovers': [...]}.\n"
            "For each overloaded source cell decide whether to apply a CIO step (delta_db of +/- CIO_STEP) "
            "or to move one elephant UE to the target cell.\n"
            "Choose at most ONE action type in this cycle: either CIO or HO, not both.\n"
            "delta_db must be exactly +CIO_STEP or -CIO_STEP (no other magnitudes).\n"
            "Resulting cio_offset_db must stay within [CIO_MIN_DB, CIO_MAX_DB].\n"
            "Respect the cooldown (if cooldown_remaining_s > 0, skip CIO).\n"
            "Only handover a UE if ALL guards hold: median_sinr_db >= HO_MIN_SINR_DB, "
            "target mcs_dl_p50 >= HO_TARGET_MCS_P50_MIN, and target ul_interf_dbm <= HO_TARGET_UL_INTERF_MAX_DBM.\n"
            "Prefer fewer actions; if uncertain, return empty arrays.\n"
        ),
    }

    return json.dumps(payload, indent=2)



def _query_llm(pairs: List[OverloadPair], snapshot: NetworkSnapshot) -> LoadResponse:
    if not (USE_LLM and pairs):
        return LoadResponse()

    payload = _pairs_payload(pairs, snapshot)
    _log_payload_preview(payload)

    raw = structured_completion(
        model=os.getenv("LOAD_LLM_MODEL", "google/gemini-2.5-flash"),
        system_prompt=(
        "You manage load balancing for LTE cells.\n"
        "Follow ALL rules and definitions in the user payload (thresholds/units/definitions/instructions).\n"
        "Return ONLY a JSON object that matches the provided schema. No extra text.\n"
        ),
        user_content=payload,
        schema_name="load_response",
        schema=LOAD_RESPONSE_SCHEMA,
    )
    if not raw:
        return LoadResponse()
    if log.isEnabledFor(logging.DEBUG):
        log.debug("Load LLM raw response: %s", raw)
    try:
        return LoadResponse.model_validate(raw)
    except ValidationError as e:
        log.warning("Load LLM validation error: %s", e)
        return LoadResponse()


def _apply_cio_decisions(decisions: List[CioDecision], pairs: List[OverloadPair], snapshot: NetworkSnapshot) -> List[SetCioAction]:
    sim_ns = snapshot.sim_time_ns
    by_cell = {p.source.cell_id: p for p in pairs}
    actions: List[SetCioAction] = []
    drop_stats: Counter[str] = Counter()

    for d in decisions:
        pair = by_cell.get(d.source_cell)
        if not pair:
            drop_stats["unknown_source"] += 1
            continue
        if pair.source.cooldown_remaining > 0.0:
            drop_stats["cooldown_active"] += 1
            continue

        delta = clamp(d.delta_db, -CIO_STEP, CIO_STEP)

        if abs(delta) < 1e-6:
            drop_stats["cio_zero_delta"] += 1
            continue
        delta = CIO_STEP if delta > 0 else -CIO_STEP

        cur_nbr, cur_off = _cio_state.get(d.source_cell, (None, 0.0))
        nbr_changed = (cur_nbr != d.target_cell)

        # Neighbor changed: reset base to 0.0 (aligned with bridge ClearSrc semantics)
        base = cur_off if not nbr_changed else 0.0
        updated = clamp(base + delta, CIO_MIN, CIO_MAX)

        # Only skip when the neighbor is unchanged AND the value remains the same.
        # If the neighbor changed, we must emit (to trigger ClearSrc on the bridge side).
        emit = nbr_changed or (abs(updated - base) > 1e-3)

        # Maintain state for the next cycle (used for delta accumulation)
        _cio_state[d.source_cell] = (d.target_cell, updated)

        if not emit:
            log.debug("CIO noop: src=%s nbr=%s offset=%.2f", d.source_cell, d.target_cell, updated)
            continue

        _last_cio_action_ns[d.source_cell] = sim_ns
        actions.append(SetCioAction(
            type="set_cio",
            cellid=d.source_cell,
            neighbor_cellid=d.target_cell,
            offset_db=updated,
        ))

        if nbr_changed and abs(updated) < 1e-3:
            log.info("CIO rebinding(clear): src=%s -> nbr=%s offset=0.00 (%s)", d.source_cell, d.target_cell, d.reason)
        else:
            log.info("CIO step: src=%s -> nbr=%s new=%.2f (delta=%.2f) %s",
                     d.source_cell, d.target_cell, updated, delta, d.reason)

    if drop_stats:
        log.info(
            "Load CIO guards dropped %d/%d proposals (%s)",
            sum(drop_stats.values()),
            len(decisions),
            summarize_counter(drop_stats),
        )

    return actions


def _apply_ho_decisions(
    decisions: List[LoadHoDecision],
    pairs: List[OverloadPair],
    sim_ns: int,
) -> List[HoAction]:
    actions: List[HoAction] = []
    pair_by_src = {(p.source.cell_id, p.target.cell_id): p for p in pairs}
    drop_stats: Counter[str] = Counter()
    cooldown_ns = int(HO_COOLDOWN * 1e9) if HO_COOLDOWN > 0.0 else 0

    for d in decisions:
        pair = pair_by_src.get((d.source_cell, d.target_cell))
        if not pair:
            log.debug("Load HO drop: pair (%s -> %s) not in overload set", d.source_cell, d.target_cell)
            drop_stats["pair_not_overloaded"] += 1
            continue

        if cooldown_ns > 0:
            last_ns = _last_ho_action_ns.get(d.ue_e2id)
            if last_ns is not None and (sim_ns - last_ns) < cooldown_ns:
                drop_stats["ho_cooldown"] += 1
                log.debug(
                    "Load HO drop: UE %s still in cooldown (%.2f s remaining)",
                    d.ue_e2id,
                    (cooldown_ns - (sim_ns - last_ns)) / 1e9,
                )
                continue

        elephant = next((e for e in pair.elephants if e.ue.ue_e2id == d.ue_e2id), None)
        if not elephant:
            log.debug("Load HO drop: UE %s not in elephant list for %s->%s", d.ue_e2id, d.source_cell, d.target_cell)
            drop_stats["ue_not_elephant"] += 1
            continue

        dwell_s = elephant.ue.dwell_s or 0.0
        if HO_DWELL_MIN > 0.0 and dwell_s < HO_DWELL_MIN:
            log.debug("Load HO drop: UE %s dwell %.1f < %.1f s", d.ue_e2id, dwell_s, HO_DWELL_MIN)
            drop_stats["dwell_short"] += 1
            continue
        if elephant.median_sinr is None or elephant.median_sinr < HO_MIN_SINR_DB:
            log.debug("Load HO drop: UE %s sinr %.1f < HO_MIN_SINR_DB", d.ue_e2id, elephant.median_sinr or -999.0)
            drop_stats["sinr_guard"] += 1
            continue
        if (pair.target.mcs_dl_p50 or 0.0) < HO_TARGET_MCS_P50_MIN:
            log.debug("Load HO drop: target cell %s mcs %.1f < %.1f", d.target_cell, pair.target.mcs_dl_p50 or -1.0, HO_TARGET_MCS_P50_MIN)
            drop_stats["mcs_guard"] += 1
            continue
        if (pair.target.ul_interf_dbm or -200.0) > HO_TARGET_UL_INTERF_MAX_DBM:
            log.debug("Load HO drop: target cell %s UL interference %.1f dBm > %.1f", d.target_cell, pair.target.ul_interf_dbm or 0.0, HO_TARGET_UL_INTERF_MAX_DBM)
            drop_stats["interference_guard"] += 1
            continue

        actions.append(HoAction(type="ho", ue_e2id=d.ue_e2id, target_cellid=d.target_cell))
        _last_ho_action_ns[d.ue_e2id] = sim_ns
        log.info(
            "Load HO: UE %s from %s -> %s (%s)",
            d.ue_e2id,
            d.source_cell,
            d.target_cell,
            d.reason,
        )

    if drop_stats:
        log.info(
            "Load HO guards dropped %d/%d proposals (%s)",
            sum(drop_stats.values()),
            len(decisions),
            summarize_counter(drop_stats),
        )

    return actions


def propose_actions(snapshot: NetworkSnapshot) -> List:
    if not USE_LLM:
        log.info("Load agent: LLM disabled; no automated load balancing")
        return []

    pairs, skip_stats, inspected = _build_pairs(snapshot)
    if not pairs:
        log.info(
            "Load agent: no overload pairs (checked=%d, reasons=%s)",
            inspected,
            summarize_counter(skip_stats),
        )
        return []

    worst_pair = min(
        pairs,
        key=lambda p: p.source.load_margin if p.source.load_margin is not None else 1.0,
    )
    worst_src_margin = (
        f"{worst_pair.source.load_margin:.2f}" if worst_pair.source.load_margin is not None else "n/a"
    )
    worst_tgt_margin = (
        f"{worst_pair.target.load_margin:.2f}" if worst_pair.target.load_margin is not None else "n/a"
    )
    log.info(
        "Load agent: overload pairs=%d/checked=%d worst source cell %s margin=%s -> target %s margin=%s (skips=%s)",
        len(pairs),
        inspected,
        worst_pair.source.cell_id,
        worst_src_margin,
        worst_pair.target.cell_id,
        worst_tgt_margin,
        summarize_counter(skip_stats),
    )

    for p in pairs:
        log.debug(
            "Load pair %s -> %s | source LM=%.2f target LM=%.2f elephants=%d",
            p.source.cell_id,
            p.target.cell_id,
            p.source.load_margin if p.source.load_margin is not None else -1.0,
            p.target.load_margin if p.target.load_margin is not None else -1.0,
            len(p.elephants),
        )

    response = _query_llm(pairs, snapshot)
    if response.cio or response.handovers:
        log.info(
            "Load agent: LLM plan -> CIO=%d HO=%d",
            len(response.cio),
            len(response.handovers),
        )
    else:
        log.debug("Load agent: LLM selected no load-balancing actions this cycle")

    ho_actions = _apply_ho_decisions(response.handovers, pairs, snapshot.sim_time_ns)

    # infer HO source cells from current snapshot (executed HOs only)
    ho_ueids = {a.ue_e2id for a in ho_actions}
    ho_sources = {ue.serving_cell for ue in snapshot.ues if ue.ue_e2id in ho_ueids}

    filtered_cio = [c for c in response.cio if c.source_cell not in ho_sources]
    cio_actions = _apply_cio_decisions(filtered_cio, pairs, snapshot)

    return cio_actions + ho_actions
