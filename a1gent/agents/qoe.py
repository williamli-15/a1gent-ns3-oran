import json
import os
from collections import Counter
from dataclasses import dataclass
from typing import Dict, List, Optional, Set, Tuple

from pydantic import BaseModel, Field, ValidationError

from ..common.models import NetworkSnapshot, HoAction, UeSnapshot, ThroughputSample, SinrSample
from ..common.util import (
    recent,
    mean_recent,
    percentile_recent,
    last,
    summarize_counter,
)
from ..common.log import setup_logger
from ..common.llm import get_openrouter_client, structured_completion

import logging

log = setup_logger("agent.qoe")


def _log_payload_preview(label: str, payload: str, max_chars: int = 400) -> None:
    if not log.isEnabledFor(logging.DEBUG):
        return
    suffix = ""
    if len(payload) > max_chars:
        payload = payload[:max_chars]
        suffix = "... (truncated)"
    log.debug("%s%s%s", label, payload, suffix)

# Rolling-window configuration (seconds)
QOE_ROLLING_WINDOW_S = float(os.getenv("QOE_ROLLING_WINDOW_S", "10.0"))

# Policy thresholds
DL_MIN_MBPS = float(os.getenv("QOE_DL_MIN_MBPS", "0.2"))
SINR_MIN_DB = float(os.getenv("QOE_SINR_MIN_DB", "12.0"))
MIN_DWELL_S = float(os.getenv("QOE_MIN_DWELL_S", "0.0"))
DISTRESS_RATIO_MIN = float(os.getenv("QOE_DISTRESS_RATIO_MIN", str(2.0 / 3.0)))
NEIGHBOR_HEADROOM_MIN = float(os.getenv("QOE_NEIGHBOR_HEADROOM_MIN", "0.05"))
UE_HO_BAN_S = float(os.getenv("QOE_UE_HO_BAN_S", "30.0"))
MAX_CANDIDATES = int(os.getenv("QOE_MAX_CANDIDATES", "6"))

USE_LLM = os.getenv("QOE_USE_LLM", "1").lower() not in ("0", "false", "no", "off")
# Cell-level cooldown after a CIO step (seconds). Set to 0 to disable.
CIO_HO_COOLDOWN_S = float(os.getenv("QOE_CIO_HO_COOLDOWN_S", "10.0"))

if USE_LLM:
    if get_openrouter_client():
        log.info("QoE agent: LLM enabled via OpenRouter (QOE_USE_LLM=1)")
    else:
        log.warning("QoE agent: disabling LLM (OpenRouter client unavailable)")
        USE_LLM = False
else:
    log.info("QoE agent: LLM disabled; no automated actions (QOE_USE_LLM=0)")


@dataclass
class Candidate:
    ue: UeSnapshot
    avg_dl: float
    avg_ul: float
    avg_sinr: float
    low_dl_ratio: float
    low_sinr_ratio: float
    dwell_s: float
    serving_prb_util: Optional[float]
    neighbor_prb_util: Optional[float]
    serving_headroom: Optional[float]
    neighbor_headroom: Optional[float]
    serving_sched_dl: Optional[float]
    neighbor_sched_dl: Optional[float]
    packet_loss_pct: float
    serving_rsrp: float
    neighbor_rsrp: Optional[float]
    ban_active: bool
    ban_remaining_s: float
    has_dwell_sample: bool


class HoDecision(BaseModel):
    ue_e2id: int = Field(..., description="Target UE E2 node id")
    target_cellid: int = Field(..., description="Neighbor cell to handover to")
    reason: str = Field(..., description="Short reason")


class QoeResponse(BaseModel):
    handovers: List[HoDecision]


_last_action_ns: Dict[int, int] = {}

QOE_RESPONSE_SCHEMA = {
    "type": "object",
    "properties": {
        "handovers": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "ue_e2id": {"type": "integer"},
                    "target_cellid": {"type": "integer"},
                    "reason": {"type": "string"},
                },
                "required": ["ue_e2id", "target_cellid", "reason"],
                "additionalProperties": False,
            },
            "default": [],
        }
    },
    "required": ["handovers"],
    "additionalProperties": False,
}


def _cell_prb_util(snapshot: NetworkSnapshot, cell_id: int, sim_ns: int, horizon_s: float) -> Optional[float]:
    series = snapshot.cell_prb_util.get(cell_id, [])
    if not series:
        return None
    return mean_recent(series, "dl", sim_ns, horizon_s)


def _cell_sched_dl(snapshot: NetworkSnapshot, cell_id: int, sim_ns: int, horizon_s: float) -> Optional[float]:
    series = snapshot.cell_sched_tp.get(cell_id, [])
    if not series:
        return None
    return mean_recent(series, "dl", sim_ns, horizon_s)


def _low_ratio(samples: List[ThroughputSample], threshold: float, sim_ns: int, horizon_s: float, attr: str) -> float:
    recent_samples = recent(samples, sim_ns, horizon_s)
    if not recent_samples:
        return 0.0
    below = sum(1 for s in recent_samples if getattr(s, attr) < threshold)
    return below / len(recent_samples)


def _sinr_low_ratio(samples: List[SinrSample], threshold: float, sim_ns: int, horizon_s: float) -> float:
    recent_samples = recent(samples, sim_ns, horizon_s)
    if not recent_samples:
        return 0.0
    below = sum(1 for s in recent_samples if s.sinr_db < threshold)
    return below / len(recent_samples)


def _recent_cio_cells(snapshot: NetworkSnapshot, sim_ns: int) -> Set[int]:
    if CIO_HO_COOLDOWN_S <= 0.0 or sim_ns <= 0:
        return set()
    cio_map = getattr(snapshot, "cio_last_ns", {}) or {}
    cool_ns = int(CIO_HO_COOLDOWN_S * 1e9)
    if cool_ns <= 0:
        return set()
    recent: Set[int] = set()
    for cell_id, ts in cio_map.items():
        if ts is None:
            continue
        try:
            delta = sim_ns - int(ts)
        except (TypeError, ValueError):
            continue
        if 0 <= delta < cool_ns:
            recent.add(int(cell_id))
    return recent


def _build_candidates(
    snapshot: NetworkSnapshot,
    recent_cio_cells: Set[int],
    sleeping_cells: Set[int],
) -> Tuple[List[Candidate], Counter[str], int]:
    sim_ns = snapshot.sim_time_ns
    candidates: List[Candidate] = []
    skip_stats: Counter[str] = Counter()
    total = 0

    for ue in snapshot.ues:
        total += 1
        if ue.neighbor_cell is None:
            skip_stats["no_neighbor"] += 1
            continue  # no neighbor to handover to
        if ue.neighbor_cell == ue.serving_cell:
            skip_stats["self_neighbor"] += 1
            continue  # avoid HO-to-self proposals
        if ue.serving_cell in recent_cio_cells or ue.neighbor_cell in recent_cio_cells:
            skip_stats["cio_cooldown"] += 1
            continue
        if ue.neighbor_cell in sleeping_cells:
            skip_stats["neighbor_sleeping"] += 1
            continue

        pdcp_series = snapshot.ue_pdcp_tp.get(ue.ue_e2id, [])
        sinr_series = snapshot.ue_sinr.get(ue.ue_e2id, [])
        if not pdcp_series or not sinr_series:
            skip_stats["missing_pdcp_sinr"] += 1
            continue

        avg_dl = mean_recent(pdcp_series, "dl", sim_ns, QOE_ROLLING_WINDOW_S)
        avg_ul = mean_recent(pdcp_series, "ul", sim_ns, QOE_ROLLING_WINDOW_S)
        avg_sinr = mean_recent(sinr_series, "sinr_db", sim_ns, QOE_ROLLING_WINDOW_S)
        if avg_dl is None or avg_sinr is None:
            skip_stats["missing_avg"] += 1
            continue

        low_dl_ratio = _low_ratio(pdcp_series, DL_MIN_MBPS, sim_ns, QOE_ROLLING_WINDOW_S, "dl")
        low_sinr_ratio = _sinr_low_ratio(sinr_series, SINR_MIN_DB, sim_ns, QOE_ROLLING_WINDOW_S)

        if low_dl_ratio < DISTRESS_RATIO_MIN and low_sinr_ratio < DISTRESS_RATIO_MIN:
            skip_stats["healthy_metrics"] += 1
            continue  # neither throughput nor SINR persistently bad

        dwell_series = snapshot.ue_dwell.get(ue.ue_e2id, [])
        dwell_sample = last(dwell_series)
        has_dwell = bool(dwell_series) or ue.dwell_s is not None
        dwell_s = dwell_sample.dwell_s if dwell_sample else (ue.dwell_s if ue.dwell_s is not None else 0.0)
        if MIN_DWELL_S > 0.0 and dwell_s < MIN_DWELL_S:
            skip_stats["dwell_short"] += 1
            continue

        neighbor_headroom = ue.neighbor_headroom
        if neighbor_headroom is None or neighbor_headroom < NEIGHBOR_HEADROOM_MIN:
            skip_stats["neighbor_headroom"] += 1
            continue

        serving_headroom = ue.serving_headroom
        serving_prb_util = _cell_prb_util(snapshot, ue.serving_cell, sim_ns, QOE_ROLLING_WINDOW_S)
        neighbor_prb_util = _cell_prb_util(snapshot, ue.neighbor_cell, sim_ns, QOE_ROLLING_WINDOW_S)
        serving_sched_dl = _cell_sched_dl(snapshot, ue.serving_cell, sim_ns, QOE_ROLLING_WINDOW_S)
        neighbor_sched_dl = _cell_sched_dl(snapshot, ue.neighbor_cell, sim_ns, QOE_ROLLING_WINDOW_S)

        last_action = _last_action_ns.get(ue.ue_e2id)
        ban_active = False
        ban_remaining_s = 0.0
        if last_action is not None:
            delta_s = (sim_ns - last_action) / 1e9
            if delta_s < UE_HO_BAN_S:
                ban_active = True
                ban_remaining_s = UE_HO_BAN_S - delta_s
        if ban_active:
            skip_stats["ban_active"] += 1
            continue

        candidates.append(
            Candidate(
                ue=ue,
                avg_dl=avg_dl,
                avg_ul=avg_ul or 0.0,
                avg_sinr=avg_sinr,
                low_dl_ratio=low_dl_ratio,
                low_sinr_ratio=low_sinr_ratio,
                dwell_s=dwell_s,
                serving_prb_util=serving_prb_util,
                neighbor_prb_util=neighbor_prb_util,
                serving_headroom=serving_headroom,
                neighbor_headroom=neighbor_headroom,
                serving_sched_dl=serving_sched_dl,
                neighbor_sched_dl=neighbor_sched_dl,
                packet_loss_pct=ue.packet_loss_pct,
                serving_rsrp=ue.serving_rsrp,
                neighbor_rsrp=ue.neighbor_rsrp,
                ban_active=ban_active,
                ban_remaining_s=ban_remaining_s,
                has_dwell_sample=has_dwell,
            )
        )

    candidates.sort(key=lambda c: c.avg_dl)
    if skip_stats and log.isEnabledFor(logging.DEBUG):
        log.debug(
            "QoE skips (total=%d kept=%d): %s",
            total,
            len(candidates),
            dict(skip_stats),
        )
    return candidates[:MAX_CANDIDATES], skip_stats, total


def _build_llm_payload(candidates: List[Candidate], snapshot: NetworkSnapshot) -> str:
    sleeping_cells = sorted((getattr(snapshot, "sleeping_cells", set()) or set()))
    recent_cio_cells = sorted(_recent_cio_cells(snapshot, snapshot.sim_time_ns))

    payload = {
        "sim_time_s": round(snapshot.sim_time_ns / 1e9, 3),

        "windows_s": {
            "rolling_s": QOE_ROLLING_WINDOW_S,
            "cio_ho_cooldown_s": CIO_HO_COOLDOWN_S,
            "ue_ho_ban_s": UE_HO_BAN_S,
        },

        "units": {
            "avg_pdcp_dl_mbps": "Mbps",
            "avg_pdcp_ul_mbps": "Mbps",
            "avg_sinr_db": "dB",
            "serving_rsrp_dbm": "dBm",
            "neighbor_rsrp_dbm": "dBm",
            "headroom": "fraction in [0,1] (1 - prb_util)",
            "prb_util": "fraction in [0,1]",
            "sched_dl_mbps": "Mbps (MAC scheduled DL throughput)",
            "dwell_s": "seconds",
        },

        "definitions": {
            "low_dl_ratio": "fraction of PDCP samples below dl_min_mbps within rolling_s",
            "low_sinr_ratio": "fraction of SINR samples below sinr_min_db within rolling_s",
            "serving_headroom": "1 - serving_prb_util (same for neighbor_headroom)",
            "cio_cooldown_cells": "cells that had a recent CIO action; avoid HO touching these cells",
        },

        "thresholds": {
            "DISTRESS_RATIO_MIN": DISTRESS_RATIO_MIN,
            "DL_MIN_MBPS": DL_MIN_MBPS,
            "SINR_MIN_DB": SINR_MIN_DB,
            "MIN_DWELL_S": MIN_DWELL_S,
            "NEIGHBOR_HEADROOM_MIN": NEIGHBOR_HEADROOM_MIN,
            "UE_HO_BAN_S": UE_HO_BAN_S,
        },

        "sleeping_cells": sleeping_cells,
        "cio_cooldown_cells": recent_cio_cells,

        "candidates": [
            {
                "ue_e2id": c.ue.ue_e2id,
                "serving_cell": c.ue.serving_cell,
                "neighbor_cell": c.ue.neighbor_cell,

                "avg_pdcp_dl_mbps": round(c.avg_dl, 3),
                "avg_pdcp_ul_mbps": round(c.avg_ul, 3),
                "avg_sinr_db": round(c.avg_sinr, 2),

                "low_dl_ratio": round(c.low_dl_ratio, 3),
                "low_sinr_ratio": round(c.low_sinr_ratio, 3),

                "dwell_s": round(c.dwell_s, 2),

                "serving_headroom": round(c.serving_headroom or 0.0, 3) if c.serving_headroom is not None else None,
                "neighbor_headroom": round(c.neighbor_headroom or 0.0, 3) if c.neighbor_headroom is not None else None,

                "serving_prb_util": round(c.serving_prb_util, 3) if c.serving_prb_util is not None else None,
                "neighbor_prb_util": round(c.neighbor_prb_util, 3) if c.neighbor_prb_util is not None else None,

                "serving_sched_dl_mbps": round(c.serving_sched_dl, 3) if c.serving_sched_dl is not None else None,
                "neighbor_sched_dl_mbps": round(c.neighbor_sched_dl, 3) if c.neighbor_sched_dl is not None else None,

                "serving_rsrp_dbm": round(c.serving_rsrp, 1),
                "neighbor_rsrp_dbm": round(c.neighbor_rsrp, 1) if c.neighbor_rsrp is not None else None,
            }
            for c in candidates
        ],

        "instructions": (
            "Return ONLY the JSON object {'handovers': [...]}.\n"
            "Only include UEs from candidates.\n"
            "Avoid handing to sleeping_cells or cio_cooldown_cells.\n"
            "Prefer fewer actions; if uncertain, return an empty handovers list.\n"
            "Reason must explain why the HO improves QoE.\n"
        ),
    }

    return json.dumps(payload, indent=2)


def _query_llm(candidates: List[Candidate], snapshot: NetworkSnapshot) -> List[HoDecision]:
    if not (USE_LLM and candidates):
        return []

    payload = _build_llm_payload(candidates, snapshot)
    _log_payload_preview("QoE LLM payload: ", payload)

    raw = structured_completion(
        model=os.getenv("QOE_LLM_MODEL", "google/gemini-2.5-flash"),
        system_prompt=(
        "You manage QoE firefighting handovers for LTE UEs.\n"
        "Follow ALL rules and definitions in the user payload (thresholds/units/definitions/instructions).\n"
        "Return ONLY a JSON object that matches the provided schema. No extra text.\n"
        ),
        user_content=payload,
        schema_name="qoe_response",
        schema=QOE_RESPONSE_SCHEMA,
    )
    if not raw:
        return []
    log.debug("QoE LLM raw response: %s", raw)
    try:
        parsed = QoeResponse.model_validate(raw)
        return parsed.handovers
    except ValidationError as e:
        log.warning("QoE LLM validation error: %s", e)
        return []


def _enforce_guards(
    decisions: List[HoDecision],
    candidates: List[Candidate],
    snapshot: NetworkSnapshot,
    recent_cio_cells: Set[int],
    sleeping_cells: Set[int],
) -> List[HoAction]:
    sim_ns = snapshot.sim_time_ns
    by_ue = {c.ue.ue_e2id: c for c in candidates}
    actions: List[HoAction] = []
    reject_stats: Counter[str] = Counter()

    for d in decisions:
        cand = by_ue.get(d.ue_e2id)
        if not cand:
            reject_stats["not_candidate"] += 1
            continue
        if cand.ue.neighbor_cell != d.target_cellid:
            reject_stats["target_mismatch"] += 1
            continue
        if d.target_cellid == cand.ue.serving_cell:
            reject_stats["self_target"] += 1
            continue
        if cand.ue.serving_cell in recent_cio_cells or d.target_cellid in recent_cio_cells:
            reject_stats["cio_cooldown"] += 1
            continue
        if d.target_cellid in sleeping_cells:
            reject_stats["target_sleeping"] += 1
            continue
        if cand.neighbor_headroom is None or cand.neighbor_headroom < NEIGHBOR_HEADROOM_MIN:
            reject_stats["neighbor_headroom"] += 1
            continue
        if cand.has_dwell_sample and MIN_DWELL_S > 0.0 and cand.dwell_s < MIN_DWELL_S:
            reject_stats["dwell_short"] += 1
            continue

        actions.append(HoAction(type="ho", ue_e2id=d.ue_e2id, target_cellid=d.target_cellid))
        _last_action_ns[d.ue_e2id] = sim_ns
        log.info(
            "QoE HO -> UE %s to cell %s (dl=%.2f Mbps sinr=%.1f dB headroom=%.2f reason=%s)",
            d.ue_e2id,
            d.target_cellid,
            cand.avg_dl,
            cand.avg_sinr,
            cand.neighbor_headroom,
            d.reason,
        )

    if reject_stats and log.isEnabledFor(logging.DEBUG):
        log.debug("QoE guard rejects: %s", dict(reject_stats))

    if reject_stats:
        log.info("QoE guard rejects: %s", summarize_counter(reject_stats))

    return actions


def propose_actions(snapshot: NetworkSnapshot) -> List[HoAction]:
    sim_ns = snapshot.sim_time_ns
    recent_cio_cells = _recent_cio_cells(snapshot, sim_ns)
    sleeping_cells = set(getattr(snapshot, "sleeping_cells", set()) or set())

    candidates, skip_stats, inspected = _build_candidates(snapshot, recent_cio_cells, sleeping_cells)
    if not candidates:
        log.info(
            "QoE: no distressed UEs (checked=%d, reasons=%s)",
            inspected,
            summarize_counter(skip_stats),
        )
        return []

    worst = candidates[0]
    log.info(
        "QoE: candidates=%d/checked=%d worst UE %s scell=%s->%s dl=%.2fMbps sinr=%.1fdB dwell=%.1fs nHead=%s (skips=%s)",
        len(candidates),
        inspected,
        worst.ue.ue_e2id,
        worst.ue.serving_cell,
        worst.ue.neighbor_cell,
        worst.avg_dl,
        worst.avg_sinr,
        worst.dwell_s,
        f"{worst.neighbor_headroom:.2f}" if worst.neighbor_headroom is not None else "n/a",
        summarize_counter(skip_stats),
    )
    for c in candidates:
        log.debug(
            "QoE candidate UE %s scell=%s -> ncell=%s | dl=%.2fMbps sinr=%.1fdB dwell=%.1fs nHead=%s",
            c.ue.ue_e2id,
            c.ue.serving_cell,
            c.ue.neighbor_cell,
            c.avg_dl,
            c.avg_sinr,
            c.dwell_s,
            f"{c.neighbor_headroom:.2f}" if c.neighbor_headroom is not None else "n/a",
        )

    llm_decisions = _query_llm(candidates, snapshot)
    if not llm_decisions:
        log.info(
            "QoE: LLM kept current serving cells (candidates=%d, worst_dl=%.2f)",
            len(candidates),
            worst.avg_dl,
        )

    return _enforce_guards(llm_decisions, candidates, snapshot, recent_cio_cells, sleeping_cells)
