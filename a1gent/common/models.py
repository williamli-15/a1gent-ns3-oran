from typing import Optional, List, Literal, Dict, Set
from pydantic import BaseModel, Field


class DlUlSample(BaseModel):
    sim_ns: int
    dl: float
    ul: float
    window_ms: Optional[int] = None
    count: Optional[int] = None
    cellid: Optional[int] = None
    e2nodeid: Optional[int] = None


class ThroughputSample(DlUlSample):
    """DL/UL Mbps sample aggregated over a 1s window."""


class PrbSample(DlUlSample):
    """DL/UL PRB usage sample aggregated over a 1s window."""


class UtilizationSample(DlUlSample):
    """DL/UL PRB utilization sample (0..1) aggregated over a 1s window."""


class SinrSample(BaseModel):
    sim_ns: int
    sinr_db: float
    sinr_linear: float
    rsrp_dbm: float
    rsrp_mw: float
    cellid: int
    rnti: int


class McsSample(BaseModel):
    sim_ns: int
    e2nodeid: int
    dl_mean: float
    dl_p50: float
    dl_p95: float
    ul_mean: float
    ul_p50: float
    ul_p95: float


class InterferenceSample(BaseModel):
    sim_ns: int
    e2nodeid: int
    p95_mw: float
    p95_dbm: float


class CountSample(BaseModel):
    sim_ns: int
    value: int
    cellid: Optional[int] = None
    e2nodeid: Optional[int] = None


class DwellSample(BaseModel):
    sim_ns: int
    dwell_s: float
    cellid: int

# --- UE neighbor measurement (from lteuersrprsrq serving=0 rows) ---
class NeighborCellMeas(BaseModel):
    cellid: int
    rsrp_dbm: float


# -------- DB snapshot types --------

class UeSnapshot(BaseModel):
    ue_e2id: int
    serving_cell: int
    rnti: int
    serving_rsrp: float  # dBm (from lteuersrprsrq)
    packet_loss_pct: float
    x: float
    y: float
    # optional neighbor sample (latest):
    neighbor_cell: Optional[int] = None
    neighbor_rsrp: Optional[float] = None  # dBm (top-1 neighbor)
    neighbor_cells: List[NeighborCellMeas] = Field(default_factory=list)  # top-K neighbors
    # enriched metrics (latest sample within snapshot window)
    pdcp_dl_mbps: Optional[float] = None
    pdcp_ul_mbps: Optional[float] = None
    sinr_db: Optional[float] = None
    sinr_linear: Optional[float] = None
    rsrp_dbm: Optional[float] = None
    rsrp_mw: Optional[float] = None
    dl_prbs: Optional[float] = None
    ul_prbs: Optional[float] = None
    dwell_s: Optional[float] = None
    neighbor_headroom: Optional[float] = None
    serving_headroom: Optional[float] = None


class CellSnapshot(BaseModel):
    cell_id: int
    e2nodeid: Optional[int] = None
    attached_ues: List[int] = Field(default_factory=list)
    prb_dl_util: Optional[float] = None
    prb_ul_util: Optional[float] = None
    sched_dl_mbps: Optional[float] = None
    sched_ul_mbps: Optional[float] = None
    ue_count: Optional[int] = None
    mcs_dl_mean: Optional[float] = None
    mcs_ul_mean: Optional[float] = None
    mcs_dl_p50: Optional[float] = None
    mcs_dl_p95: Optional[float] = None
    mcs_ul_p50: Optional[float] = None
    mcs_ul_p95: Optional[float] = None
    ul_interf_dbm: Optional[float] = None
    ul_interf_mw: Optional[float] = None


class NetworkSnapshot(BaseModel):
    time_s: float
    sim_time_ns: int
    ues: List[UeSnapshot]
    cells: List[CellSnapshot]
    ue_pdcp_tp: Dict[int, List[ThroughputSample]] = Field(default_factory=dict)
    ue_prb: Dict[int, List[PrbSample]] = Field(default_factory=dict)
    ue_sinr: Dict[int, List[SinrSample]] = Field(default_factory=dict)
    ue_dwell: Dict[int, List[DwellSample]] = Field(default_factory=dict)
    cell_prb_util: Dict[int, List[UtilizationSample]] = Field(default_factory=dict)
    cell_sched_tp: Dict[int, List[ThroughputSample]] = Field(default_factory=dict)
    cell_mcs: Dict[int, List[McsSample]] = Field(default_factory=dict)
    cell_ul_interf: Dict[int, List[InterferenceSample]] = Field(default_factory=dict)
    cell_ue_count: Dict[int, List[CountSample]] = Field(default_factory=dict)
    ho_events: List["HoEvent"] = Field(default_factory=list)
    cio_last_ns: Dict[int, int] = Field(default_factory=dict)
    sleeping_cells: Set[int] = Field(default_factory=set)
    sleep_last_ns: Dict[int, int] = Field(default_factory=dict)


class HoEvent(BaseModel):
    sim_ns: int
    imsi: int
    ueid: int
    src_cell: int
    dst_cell: int
    event: str

# -------- action schemas (what LM_CommandBridge consumes) --------

class HoAction(BaseModel):
    type: Literal["ho"]
    ue_e2id: int
    target_cellid: int

class SetCioAction(BaseModel):
    type: Literal["set_cio"]
    cellid: int
    neighbor_cellid: Optional[int] = None
    offset_db: float  # dB (policy clamp, e.g. [-6, +6] by LOAD_CIO_MIN_DB/MAX_DB)

class CellSleepAction(BaseModel):
    type: Literal["cell_sleep"]
    cellid: int


class CellWakeAction(BaseModel):
    type: Literal["cell_wake"]
    cellid: int


Action = HoAction | SetCioAction | CellSleepAction | CellWakeAction

class CommandFile(BaseModel):
    ts: float
    commands: List[Action]
