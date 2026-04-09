# a1gent/common/apt.py
from __future__ import annotations

import os
import math
from dataclasses import dataclass
from typing import Any, Callable, Deque, Dict, List, Optional, Tuple
from collections import deque, Counter

from .util import percentile, mean_or_none


@dataclass
class Observation:
    t: float
    phase: str

    # QoE proxies
    ue_p10_pdcp_dl_mbps: Optional[float] = None

    # Load proxy
    overload_frac: Optional[float] = None  # fraction of active cells considered "hot"

    # Energy proxy
    sleep_frac: Optional[float] = None     # sleeping_cells / total_cells
    sleeping_cells: int = 0
    total_cells: int = 0

    # action counts in this tick
    ho: int = 0
    cio: int = 0
    sleep: int = 0
    wake: int = 0


@dataclass
class Knob:
    name: str
    module: Any
    attr: str
    lo: float
    hi: float
    step: float
    kind: str = "float"   # "float" | "int"
    preferred_dir: int = +1  # +1 or -1
    post_set: Optional[Callable[[Any], None]] = None


@dataclass
class Trial:
    knob: Knob
    old: float
    new: float
    start_t: float
    base_score: float
    phase: str
    reason: str


def _env_f(name: str, default: float) -> float:
    v = os.getenv(name)
    if v is None or v == "":
        return float(default)
    try:
        return float(v)
    except ValueError:
        return float(default)


def _env_i(name: str, default: int) -> int:
    v = os.getenv(name)
    if v is None or v == "":
        return int(default)
    try:
        return int(v)
    except ValueError:
        return int(default)


def _clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))


def _mean(xs: List[Optional[float]]) -> Optional[float]:
    vals = [x for x in xs if x is not None]
    return mean_or_none(vals)


class AptTuner:
    """
    Stage-0 self-heal + Stage-1 trial-based optimizer.

    - observe(): record lightweight KPIs + action counts.
    - maybe_tune(): every interval, either finish an in-flight trial (accept/revert)
      or start a new trial (self-heal first, else optimize).

    Current runtime model:
    - single process
    - single orchestrator
    - long-running online adaptation
    - knob values live as module globals in the agent modules and are updated via setattr()

    This is intentional for the current experiment workflow. If the runtime model changes
    to multi-process execution, concurrent experiments, or stronger test isolation, move
    knob state into an explicit per-run config object instead of mutating module globals.
    """

    def __init__(
        self,
        *,
        energy_mod: Any,
        qoe_mod: Any,
        load_mod: Any,
        log_fn: Optional[Callable[..., None]] = None,
    ):
        self.energy = energy_mod
        self.qoe = qoe_mod
        self.load = load_mod
        self.log = log_fn or (lambda *a, **k: None)

        self.enabled = (os.getenv("APT_ENABLE", "1").lower() not in ("0", "false", "no", "off"))
        self.interval_s = _env_f("APT_INTERVAL_S", 30.0)   # how often we may start/finish trials
        self.eval_s = _env_f("APT_EVAL_S", 40.0)           # how long to wait before judging a trial
        self.min_start_s = _env_f("APT_MIN_START_S", 20.0) # don't tune too early
        self.eps = _env_f("APT_EPS", 0.01)                 # minimum score improvement to accept
        self.self_heal_cooldown_s = _env_f("APT_SELF_HEAL_COOLDOWN_S", 60.0)

        # store ~10 minutes @ 2s tick => 300 points
        self.hist: Deque[Observation] = deque(maxlen=_env_i("APT_MAX_OBS", 600))

        self.trial: Optional[Trial] = None
        self._last_tune_t = -1e18
        self._last_self_heal_t = -1e18

        # per-knob bookkeeping (deterministic)
        self._rr_idx: Dict[str, int] = {"normal": 0, "emergency": 0, "recovery": 0}
        self._last_outcome: Dict[str, str] = {}   # knob.name -> "accept"|"reject"
        self._last_dir: Dict[str, int] = {}       # knob.name -> +1|-1

        self.knobs_normal: List[Knob] = self._build_knobs_normal()
        self.knobs_emergency: List[Knob] = self._build_knobs_emergency()
        self.knobs_recovery: List[Knob] = self._build_knobs_recovery()
        self.overload_prb_th = _env_f("APT_OVERLOAD_PRB_TH", 0.90)

    # ------------------------- public API -------------------------

    def observe(self, sim_sec: float, snap, actions: List[Any], phase: str) -> None:
        if not self.enabled:
            return

        c = Counter(getattr(a, "type", None) for a in actions)
        obs = Observation(
            t=float(sim_sec),
            phase=str(phase),
            ue_p10_pdcp_dl_mbps=self._ue_p10_pdcp_dl(snap),
            overload_frac=self._overload_frac(snap),
            sleeping_cells=len(getattr(snap, "sleeping_cells", set()) or set()),
            total_cells=len(getattr(snap, "cells", []) or []),
            ho=int(c.get("ho", 0)),
            cio=int(c.get("set_cio", 0)),
            sleep=int(c.get("cell_sleep", 0)),
            wake=int(c.get("cell_wake", 0)),
        )
        obs.sleep_frac = (obs.sleeping_cells / obs.total_cells) if obs.total_cells > 0 else None
        self.hist.append(obs)

    def maybe_tune(self, sim_sec: float, snap, phase: str) -> List[str]:
        if not self.enabled:
            return []
        if sim_sec < self.min_start_s:
            return []

        # phase changed during a trial: revert to be safe/deterministic
        if self.trial is not None and self.trial.phase != phase:
            msgs = [f"trial_abort (phase change): {self.trial.knob.name} revert {self.trial.new} -> {self.trial.old}"]
            self._set_knob(self.trial.knob, self.trial.old)
            self.trial = None
            return msgs

        # Finish an in-flight trial if ready
        if self.trial is not None:
            if (sim_sec - self.trial.start_t) < self.eval_s:
                return []
            return self._finish_trial(sim_sec, phase)

        # Gate start cadence
        if (sim_sec - self._last_tune_t) < self.interval_s:
            return []

        # 1) Stage-0 self-heal (only if "stuck" and cooldown passed)
        if (sim_sec - self._last_self_heal_t) >= self.self_heal_cooldown_s:
            heal = self._maybe_start_self_heal(sim_sec, snap, phase)
            if heal:
                self._last_tune_t = sim_sec
                self._last_self_heal_t = sim_sec
                return heal

        # 2) Stage-1 optimize: start a deterministic trial (round-robin per phase)
        opt = self._start_opt_trial(sim_sec, snap, phase)
        if opt:
            self._last_tune_t = sim_sec
        return opt

    # ------------------------- knobs -------------------------

    def _build_knobs_normal(self) -> List[Knob]:
        # Normal: prioritize Energy + keep QoE/load sane.
        return [
            # Energy: nudge sleep eligibility (score will reject if hurts QoE/load)
            Knob("energy.PRB_SLEEP_MAX", self.energy, "PRB_SLEEP_MAX", lo=0.03, hi=0.50, step=0.03, preferred_dir=+1),
            Knob("energy.SCHED_SLEEP_MAX", self.energy, "SCHED_SLEEP_MAX", lo=0.05, hi=1.00, step=0.05, preferred_dir=+1),

            # Load: allow mild steering if needed
            Knob("load.CIO_INTERVAL", self.load, "CIO_INTERVAL", lo=5.0, hi=60.0, step=5.0, preferred_dir=-1),
            Knob("load.HEADROOM_HOT_MAX", self.load, "HEADROOM_HOT_MAX", lo=0.05, hi=0.30, step=0.02, preferred_dir=+1),
            Knob("load.HEADROOM_COOL_MIN", self.load, "HEADROOM_COOL_MIN", lo=0.05, hi=0.60, step=0.02, preferred_dir=-1),

            # QoE: relax headroom if rescue stalls
            Knob("qoe.NEIGHBOR_HEADROOM_MIN", self.qoe, "NEIGHBOR_HEADROOM_MIN", lo=0.02, hi=0.40, step=0.02, preferred_dir=-1),
        ]

    def _build_knobs_emergency(self) -> List[Knob]:
        # Emergency: focus QoE + Load; Energy weight ~ 0 in score.
        return [
            Knob("load.CIO_INTERVAL", self.load, "CIO_INTERVAL", lo=5.0, hi=60.0, step=5.0, preferred_dir=-1),
            Knob("load.CIO_STEP", self.load, "CIO_STEP", lo=0.5, hi=2.0, step=0.25, preferred_dir=+1),
            Knob("load.HEADROOM_HOT_MAX", self.load, "HEADROOM_HOT_MAX", lo=0.05, hi=0.30, step=0.02, preferred_dir=+1),
            Knob("load.HEADROOM_COOL_MIN", self.load, "HEADROOM_COOL_MIN", lo=0.05, hi=0.60, step=0.02, preferred_dir=-1),

            Knob("qoe.DL_MIN_MBPS", self.qoe, "DL_MIN_MBPS", lo=0.05, hi=3.0, step=0.10, preferred_dir=+1),
            Knob("qoe.NEIGHBOR_HEADROOM_MIN", self.qoe, "NEIGHBOR_HEADROOM_MIN", lo=0.02, hi=0.40, step=0.02, preferred_dir=-1),
            Knob("qoe.UE_HO_BAN_S", self.qoe, "UE_HO_BAN_S", lo=5.0, hi=120.0, step=5.0, preferred_dir=+1),
        ]

    def _build_knobs_recovery(self) -> List[Knob]:
        # Recovery: unwind congestion and then return to energy saving.
        return [
            Knob("load.CIO_INTERVAL", self.load, "CIO_INTERVAL", lo=5.0, hi=60.0, step=5.0, preferred_dir=+1),
            Knob("load.CIO_STEP", self.load, "CIO_STEP", lo=0.5, hi=2.0, step=0.25, preferred_dir=-1),
            Knob("energy.PRB_SLEEP_MAX", self.energy, "PRB_SLEEP_MAX", lo=0.03, hi=0.50, step=0.03, preferred_dir=+1),
            Knob("qoe.UE_HO_BAN_S", self.qoe, "UE_HO_BAN_S", lo=5.0, hi=120.0, step=5.0, preferred_dir=-1),
        ]

    def _knobs_for_phase(self, phase: str) -> List[Knob]:
        if phase == "emergency":
            return self.knobs_emergency
        if phase == "recovery":
            return self.knobs_recovery
        return self.knobs_normal

    # ------------------------- Stage-0 self-heal -------------------------

    def _maybe_start_self_heal(self, sim_sec: float, snap, phase: str) -> List[str]:
        """
        Only kicks in when the system is clearly 'stuck':
        - Load: overload persists but CIO is rare
        - QoE: p10 DL is low but HO is rare
        - Energy: no sleeping cells even though there are clearly idle-looking cells
        """
        window_s = 60.0
        w = self._window(sim_sec, window_s)
        if not w:
            return []

        overload = _mean([o.overload_frac for o in w]) or 0.0
        p10 = _mean([o.ue_p10_pdcp_dl_mbps for o in w]) or 0.0
        cio_cnt = sum(o.cio for o in w)
        ho_cnt = sum(o.ho for o in w)

        # 1) Load self-heal: overload > 25% but almost no CIO
        if overload > 0.25 and cio_cnt <= 1:
            # most effective "wake up" knob: allow more frequent CIO
            return self._start_trial(sim_sec, phase, self._find_knob("load.CIO_INTERVAL", phase), reason="self_heal: overload_high_cio_rare")

        # 2) QoE self-heal: p10 low but HO rare -> relax neighbor headroom guard
        #    (only if emergency/normal; in recovery we don't want to thrash)
        if phase in ("normal", "emergency") and p10 > 0 and p10 < 0.35 and ho_cnt == 0:
            return self._start_trial(sim_sec, phase, self._find_knob("qoe.NEIGHBOR_HEADROOM_MIN", phase), reason="self_heal: qoe_tail_low_ho_rare")

        # 3) Energy self-heal: no sleeping cells, but some cells remain well below the idle thresholds
        if phase != "emergency":
            if self._sleeping_is_zero(w) and self._has_idle_cells_below_threshold(snap):
                return self._start_trial(sim_sec, phase, self._find_knob("energy.PRB_SLEEP_MAX", phase), reason="self_heal: idle_cells_no_sleep")

        return []

    def _sleeping_is_zero(self, w: List[Observation]) -> bool:
        return all((o.sleeping_cells or 0) == 0 for o in w)

    def _has_idle_cells_below_threshold(self, snap) -> bool:
        # Strict idle hint: DL<=0.05 and UL<=0.05 and UE_count<=1
        idle = 0
        for cell in getattr(snap, "cells", []) or []:
            dl = getattr(cell, "prb_dl_util", None)
            ul = getattr(cell, "prb_ul_util", None)
            ue = getattr(cell, "ue_count", None)
            if dl is None or ul is None or ue is None:
                continue
            if dl <= 0.05 and ul <= 0.05 and ue <= 1:
                idle += 1
        return idle >= 1

    # ------------------------- Stage-1 optimize (trial / accept / revert) -------------------------

    def _start_opt_trial(self, sim_sec: float, snap, phase: str) -> List[str]:
        knobs = self._knobs_for_phase(phase)
        if not knobs:
            return []

        i = self._rr_idx.get(phase, 0) % len(knobs)
        self._rr_idx[phase] = i + 1
        k = knobs[i]
        return self._start_trial(sim_sec, phase, k, reason="optimize_round_robin")

    def _start_trial(self, sim_sec: float, phase: str, knob: Optional[Knob], reason: str) -> List[str]:
        if knob is None:
            return []

        old = self._get_knob(knob)
        if old is None:
            return []

        # pick direction deterministically, with "retry other direction" if last time rejected
        last_dir = self._last_dir.get(knob.name, knob.preferred_dir)
        last_out = self._last_outcome.get(knob.name, "")
        direction = last_dir
        if last_out == "reject":
            direction = -last_dir  # flip direction next time
        self._last_dir[knob.name] = direction

        new = old + direction * knob.step
        new = _clamp(new, knob.lo, knob.hi)
        if abs(new - old) < 1e-9:
            return []

        base_score = self._score_recent(sim_sec, phase, horizon_s=self.eval_s)

        self._set_knob(knob, new)
        self.trial = Trial(
            knob=knob,
            old=float(old),
            new=float(new),
            start_t=float(sim_sec),
            base_score=float(base_score),
            phase=str(phase),
            reason=reason,
        )
        return [f"trial_start {knob.name}: {old:.4g} -> {new:.4g} ({reason})"]

    def _finish_trial(self, sim_sec: float, phase: str) -> List[str]:
        assert self.trial is not None
        tr = self.trial

        new_score = self._score_recent(sim_sec, phase, horizon_s=self.eval_s)
        improved = (new_score > (tr.base_score + self.eps))

        msgs: List[str] = []
        if improved:
            self._last_outcome[tr.knob.name] = "accept"
            msgs.append(
                f"trial_accept {tr.knob.name}: keep {tr.new:.4g} (score {tr.base_score:.4g} -> {new_score:.4g})"
            )
        else:
            self._set_knob(tr.knob, tr.old)
            self._last_outcome[tr.knob.name] = "reject"
            msgs.append(
                f"trial_reject {tr.knob.name}: revert {tr.new:.4g} -> {tr.old:.4g} (score {tr.base_score:.4g} -> {new_score:.4g})"
            )

        self.trial = None
        return msgs

    # ------------------------- score & metrics -------------------------

    def _weights(self, phase: str) -> Tuple[float, float, float]:
        # (w_qoe, w_load, w_energy)
        if phase == "emergency":
            return (1.0, 0.8, 0.0)
        if phase == "recovery":
            return (0.4, 1.0, 0.6)
        return (0.5, 0.5, 1.0)

    def _score_recent(self, now_t: float, phase: str, horizon_s: float) -> float:
        w = self._window(now_t, horizon_s)
        if not w:
            return 0.0

        w_qoe, w_load, w_energy = self._weights(phase)

        qoe = _mean([o.ue_p10_pdcp_dl_mbps for o in w]) or 0.0
        load = _mean([o.overload_frac for o in w]) or 0.0
        energy = _mean([o.sleep_frac for o in w]) or 0.0

        # churn penalties (per-second)
        ho_rate = (sum(o.ho for o in w) / max(horizon_s, 1e-6))
        cio_rate = (sum(o.cio for o in w) / max(horizon_s, 1e-6))

        # use log1p so QoE doesn't dominate scale
        score = (
            w_qoe * math.log1p(max(0.0, qoe))
            - w_load * load
            + w_energy * energy
            - 0.20 * ho_rate
            - 0.05 * cio_rate
        )
        return float(score)

    def _window(self, now_t: float, horizon_s: float) -> List[Observation]:
        out: List[Observation] = []
        for o in reversed(self.hist):
            if (now_t - o.t) > horizon_s:
                break
            out.append(o)
        out.reverse()
        return out

    def _ue_p10_pdcp_dl(self, snap) -> Optional[float]:
        vals: List[float] = []
        for ue in getattr(snap, "ues", []) or []:
            v = getattr(ue, "pdcp_dl_mbps", None)
            if v is None:
                continue
            try:
                vals.append(float(v))
            except Exception:
                continue
        if len(vals) < 5:
            return None
        return float(percentile(vals, 10.0) or 0.0)

    def _overload_frac(self, snap) -> Optional[float]:
        cells = getattr(snap, "cells", []) or []
        if not cells:
            return None

        sleeping = set(getattr(snap, "sleeping_cells", set()) or set())
        active_cells = [c for c in cells if int(getattr(c, "cell_id", 0) or 0) not in sleeping]
        if not active_cells:
            return 0.0

        hot_util_th = self.overload_prb_th  # fixed KPI threshold, independent of tuned knobs

        hot = 0
        total = 0
        for c in active_cells:
            dl = getattr(c, "prb_dl_util", None)
            if dl is None:
                continue
            total += 1
            if float(dl) >= hot_util_th:
                hot += 1
        if total == 0:
            return None
        return float(hot / total)

    # ------------------------- knob I/O -------------------------

    def _find_knob(self, name: str, phase: str) -> Optional[Knob]:
        for k in self._knobs_for_phase(phase):
            if k.name == name:
                return k
        # also search other phase lists (self-heal may request a knob not in this phase list)
        for lst in (self.knobs_normal, self.knobs_emergency, self.knobs_recovery):
            for k in lst:
                if k.name == name:
                    return k
        return None

    def _get_knob(self, knob: Knob) -> Optional[float]:
        if not hasattr(knob.module, knob.attr):
            return None
        v = getattr(knob.module, knob.attr)
        try:
            return float(v)
        except Exception:
            return None

    def _set_knob(self, knob: Knob, value: float) -> None:
        # Runtime knob mutation is intentional in the current single-process,
        # single-orchestrator design. The updated module global is consumed by
        # the next control cycles without rebuilding the agents.
        v = float(value)
        v = _clamp(v, knob.lo, knob.hi)
        if knob.kind == "int":
            v = int(round(v))
        setattr(knob.module, knob.attr, v)
        if knob.post_set:
            try:
                knob.post_set(knob.module)
            except Exception:
                pass
