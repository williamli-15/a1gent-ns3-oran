# a1gent/common/db.py
import sqlite3, time, re, os
from collections import defaultdict
import pandas as pd
from typing import Dict, List, Tuple, Optional
from .models import (
    UeSnapshot,
    CellSnapshot,
    NetworkSnapshot,
    DlUlSample,
    ThroughputSample,
    PrbSample,
    UtilizationSample,
    SinrSample,
    McsSample,
    InterferenceSample,
    CountSample,
    DwellSample,
    HoEvent,
)
from .log import setup_logger

log = setup_logger("snapshot")

# Snapshot pulls from SQLite tables populated by the ns-3 reporters:
#   nodelocation, lteuecell, lteuersrprsrq, nodeapploss,
#   lteue_pdcp_tp, lteue_prb, lteuesinr, lteue_dwell,
#   lteenbprbutilization, lteenb_sched_tp, lteenb_mcs,
#   lteenb_ul_interf, lteenb_uecount, lte_ho_events.

def latest_sim_time_ns(con) -> int:
    """Max simulator time (ns) we see in the DB (from a table that always advances)."""
    row = con.execute("SELECT MAX(simulationtime) FROM lteuecell").fetchone()
    return int(row[0] or 0)

def build_network_snapshot(db_path: str, window_s: float = 300.0) -> NetworkSnapshot:
    """
    Build a rich network snapshot from the last `window_s` seconds of **simulation** time.
    The snapshot fuses per-UE and per-cell reporters so the Python rApps can make
    multi-metric decisions without additional DB queries during a cycle.
    """
    t0 = time.perf_counter()
    now = time.time()

    con = sqlite3.connect(db_path, timeout=10)
    try:
        # 1) Connection pragmas and read-side indexes
        # SQLite reader pragmas
        con.execute("PRAGMA journal_mode=WAL")
        con.execute("PRAGMA synchronous=NORMAL")
        con.execute("PRAGMA temp_store=MEMORY")
        con.execute("PRAGMA busy_timeout=1000")

        # SQLite indexes
        index_statements = (
            "CREATE INDEX IF NOT EXISTS i_cell_node_time  ON lteuecell(nodeid, simulationtime)",
            "CREATE INDEX IF NOT EXISTS i_meas_node_time  ON lteuersrprsrq(nodeid, simulationtime)",
            "CREATE INDEX IF NOT EXISTS i_loss_node_time  ON nodeapploss(nodeid, simulationtime)",
            "CREATE INDEX IF NOT EXISTS i_pos_node_time   ON nodelocation(nodeid, simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteenbprbutil_time ON lteenbprbutilization(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteenb_sched_tp_time ON lteenb_sched_tp(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteue_pdcp_tp_time ON lteue_pdcp_tp(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteenb_mcs_time ON lteenb_mcs(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteue_prb_time ON lteue_prb(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteenb_ul_interf_time ON lteenb_ul_interf(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lte_ho_events_time ON lte_ho_events(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteue_dwell_time ON lteue_dwell(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteenb_uecount_time ON lteenb_uecount(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteuesinr_time ON lteuesinr(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lmaction_time ON lmaction(simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteue_pdcp_tp_ueid_time ON lteue_pdcp_tp(ueid, simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteue_prb_ueid_time      ON lteue_prb(ueid, simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteuesinr_nodeid_time    ON lteuesinr(nodeid, simulationtime)",
            "CREATE INDEX IF NOT EXISTS idx_lteue_dwell_ueid_time    ON lteue_dwell(ueid, simulationtime)",
        )
        for stmt in index_statements:
            try:
                con.execute(stmt)
            except sqlite3.OperationalError:
                # Older SQLite versions may not support some composite indexes; warn once.
                log.debug("index create failed (non fatal): %s", stmt)

        # Use simulator time (ns) as the ground truth
        max_sim = latest_sim_time_ns(con)
        if max_sim == 0:
            dt = (time.perf_counter() - t0) * 1000
            log.info(f"snapshot: ues=0 cells=0 sim_t=0s | {dt:.1f} ms")
            snap = NetworkSnapshot(time_s=now, sim_time_ns=0, ues=[], cells=[])
            setattr(snap, "_sim_ts_ns", 0)
            return snap

        from_sim = max(0, max_sim - int(window_s * 1e9))
        # Keep UE series shorter than the global window (helps perf with no KPI loss)
        ue_window_s = min(window_s, float(os.getenv("UE_SERIES_WINDOW_S", "60")))
        from_sim_ue = max(0, max_sim - int(ue_window_s * 1e9))

        # 2) Latest UE attachment map
        # Latest UE attachment within window (keeps stale UEs out)
        df_cell = pd.read_sql_query(
            """
            SELECT t.nodeid, t.cellid, t.rnti
            FROM lteuecell t
            JOIN (
              SELECT nodeid, MAX(simulationtime) AS mx
              FROM lteuecell
              WHERE simulationtime >= ?
              GROUP BY nodeid
            ) last
              ON t.nodeid = last.nodeid AND t.simulationtime = last.mx
            """,
            con,
            params=(from_sim,),
        )

        nodeids: Tuple[int, ...] = tuple(int(x) for x in df_cell["nodeid"].tolist())
        if nodeids:
            if len(nodeids) == 1:
                nid_list = f"({nodeids[0]})"   # Important: no trailing comma (valid SQL IN clause)
            else:
                nid_list = str(nodeids)        # Example: "(1, 2, 3)"
        else:
            nid_list = None

        def empty_df(columns: List[str]) -> pd.DataFrame:
            return pd.DataFrame(columns=columns)

        if nid_list:
            # 3) UE-side latest serving metrics and neighbor summaries
            # --- Serving cell (latest) ---
            df_serving = pd.read_sql_query(
                f"""
                SELECT nodeid, rsrp
                FROM (
                SELECT nodeid, rsrp, simulationtime,
                        ROW_NUMBER() OVER (PARTITION BY nodeid ORDER BY simulationtime DESC) rn
                FROM lteuersrprsrq
                WHERE serving=1 AND nodeid IN {nid_list} AND simulationtime >= ?
                ) WHERE rn=1
                """,
                con,
                params=(from_sim,),
            )

            UE_NEIGHBOR_TOPK = int(os.getenv("UE_NEIGHBOR_TOPK", "3"))

            # --- Neighbor cells: avg_RSRP_{k,n} over UE window ---
            df_neigh_avg = pd.read_sql_query(
                f"""
                SELECT nodeid, cellid,
                    AVG(rsrp) AS rsrp_dbm,
                    MAX(simulationtime) AS last_ts
                FROM lteuersrprsrq
                WHERE serving=0 AND nodeid IN {nid_list}
                    AND simulationtime >= ? AND rsrp > -200.0
                GROUP BY nodeid, cellid
                """,
                con,
                params=(from_sim_ue,),
            )

            if not df_neigh_avg.empty:
                df_neigh_avg = df_neigh_avg.merge(
                    df_cell[["nodeid", "cellid"]].rename(columns={"cellid": "serving_cell"}),
                    on="nodeid",
                    how="left",
                )
                df_neigh_avg = df_neigh_avg[df_neigh_avg["cellid"] != df_neigh_avg["serving_cell"]]

                # sort by avg RSRP desc, last_ts tie-break
                df_neigh_avg = df_neigh_avg.sort_values(
                    ["nodeid", "rsrp_dbm", "last_ts"],
                    ascending=[True, False, False],
                )

                # top-1 for legacy neighbor_cell fields
                df_neigh_top1 = df_neigh_avg.groupby("nodeid", as_index=False).head(1).copy()

                # top-K map for energy
                neigh_map: Dict[int, List[tuple]] = {}
                for row in df_neigh_avg.itertuples(index=False):
                    nid = int(row.nodeid)
                    neigh_map.setdefault(nid, [])
                    if len(neigh_map[nid]) >= UE_NEIGHBOR_TOPK:
                        continue
                    neigh_map[nid].append((int(row.cellid), float(row.rsrp_dbm)))
            else:
                df_neigh_top1 = empty_df(["nodeid", "cellid", "rsrp_dbm"])
                neigh_map = {}


            df_loss = pd.read_sql_query(
                f"""
                SELECT nodeid, loss
                FROM (
                  SELECT nodeid, loss, simulationtime,
                         ROW_NUMBER() OVER (PARTITION BY nodeid ORDER BY simulationtime DESC) rn
                  FROM nodeapploss
                  WHERE nodeid IN {nid_list} AND simulationtime >= ?
                ) WHERE rn=1
                """,
                con,
                params=(from_sim,),
            )

            df_pos = pd.read_sql_query(
                f"""
                SELECT nodeid, x, y
                FROM (
                  SELECT nodeid, x, y, simulationtime,
                         ROW_NUMBER() OVER (PARTITION BY nodeid ORDER BY simulationtime DESC) rn
                  FROM nodelocation
                  WHERE nodeid IN {nid_list} AND simulationtime >= ?
                ) WHERE rn=1
                """,
                con,
                params=(from_sim,),
            )
        else:
            df_serving = empty_df(["nodeid", "rsrp"])
            df_neigh_top1 = empty_df(["nodeid", "cellid", "rsrp_dbm"])

            df_loss = empty_df(["nodeid", "loss"])
            df_pos = empty_df(["nodeid", "x", "y"])
            neigh_map = {}

        # 4) Build UE-level records
        # Join UE-level observers
        df_join = (
            df_cell
            .merge(df_serving, how="left", on="nodeid")
            .merge(df_loss, how="left", on="nodeid")
            .merge(df_pos, how="left", on="nodeid")
            .merge(
                df_neigh_top1.rename(columns={"cellid": "neighbor_cell", "rsrp_dbm": "neighbor_rsrp"}),
                how="left",
                on="nodeid",
            )
        )


        # If neighbor == serving (transient), keep the UE but null the neighbor.
        if "neighbor_cell" in df_join.columns and "cellid" in df_join.columns:
            mask_self = (
                df_join["neighbor_cell"].notna()
                & (df_join["neighbor_cell"] == df_join["cellid"])
            )
            if mask_self.any():
                df_join.loc[mask_self, ["neighbor_cell", "neighbor_rsrp"]] = [pd.NA, pd.NA]
                log.debug("snapshot: nulled %d self-neighbour rows", int(mask_self.sum()))

        ues: List[UeSnapshot] = []
        for _, r in df_join.iterrows():
            neighbor_cell = int(r["neighbor_cell"]) if pd.notna(r["neighbor_cell"]) else None
            neighbor_rsrp = float(r["neighbor_rsrp"]) if pd.notna(r["neighbor_rsrp"]) else None

            # top-K neighbors list for energy offload
            nbr_list = []
            for (cid, rsrp) in neigh_map.get(int(r["nodeid"]), []):
                nbr_list.append({"cellid": cid, "rsrp_dbm": rsrp})

                
            ues.append(
                UeSnapshot(
                    ue_e2id=int(r["nodeid"]),
                    serving_cell=int(r["cellid"]),
                    rnti=int(r["rnti"]),
                    serving_rsrp=float(r["rsrp"]) if pd.notna(r["rsrp"]) else -140.0,
                    packet_loss_pct=float(r["loss"] * 100.0) if pd.notna(r["loss"]) else 0.0,
                    x=float(r["x"]) if pd.notna(r["x"]) else 0.0,
                    y=float(r["y"]) if pd.notna(r["y"]) else 0.0,
                    neighbor_cell=neighbor_cell,
                    neighbor_rsrp=neighbor_rsrp,
                    neighbor_cells=nbr_list,
                )
            )

        by_cell: Dict[int, List[int]] = defaultdict(list)
        for ue in ues:
            by_cell[ue.serving_cell].append(ue.ue_e2id)

        # Map cellId -> e2nodeid (from lteenb)
        df_cell_map = pd.read_sql_query(
            "SELECT nodeid, cellid FROM lteenb",
            con,
        )
        cell_to_e2 = {
            int(row.cellid): int(row.nodeid)
            for row in df_cell_map.itertuples(index=False)
            if pd.notna(row.cellid) and pd.notna(row.nodeid)
        }

        def collect_dl_ul_samples(
            df: pd.DataFrame,
            key_col: str,
            dl_col: str,
            ul_col: str,
            sample_cls,
            cell_col: Optional[str] = "cellid",
            node_col: Optional[str] = "nodeid",
        ):
            out: Dict[int, List[DlUlSample]] = defaultdict(list)
            for row in df.itertuples(index=False):
                key = int(getattr(row, key_col))
                sample = sample_cls(
                    sim_ns=int(row.simulationtime),
                    dl=float(getattr(row, dl_col)),
                    ul=float(getattr(row, ul_col)),
                    window_ms=int(getattr(row, "window_ms")) if hasattr(row, "window_ms") else None,
                    count=int(getattr(row, "ttis_observed")) if hasattr(row, "ttis_observed") else None,
                    cellid=int(getattr(row, cell_col)) if cell_col and hasattr(row, cell_col) else None,
                    e2nodeid=int(getattr(row, node_col)) if node_col and hasattr(row, node_col) else None,
                )
                out[key].append(sample)
            for lst in out.values():
                lst.sort(key=lambda s: s.sim_ns)
            return dict(out)
        ue_pdcp_tp: Dict[int, List[ThroughputSample]] = {}
        ue_prb: Dict[int, List[PrbSample]] = {}
        ue_sinr: Dict[int, List[SinrSample]] = {}
        ue_dwell: Dict[int, List[DwellSample]] = {}

        if nid_list:
            # UE series (use from_sim_ue)
            df_pdcp = pd.read_sql_query(
                f"""
                SELECT ueid, cellid, dlmbps, ulmbps, simulationtime
                FROM lteue_pdcp_tp
                WHERE simulationtime >= ? AND ueid IN {nid_list}
                ORDER BY simulationtime
                """,
                con,
                params=(from_sim_ue,),
            )

            df_prb = pd.read_sql_query(
                f"""
                SELECT ueid, cellid, dl_prbs, ul_prbs, simulationtime
                FROM lteue_prb
                WHERE simulationtime >= ? AND ueid IN {nid_list}
                ORDER BY simulationtime
                """,
                con,
                params=(from_sim_ue,),
            )

            df_sinr = pd.read_sql_query(
                f"""
                SELECT nodeid, cellid, rnti, sinr_linear, sinr_db, rsrp_mw, rsrp_dbm, simulationtime
                FROM lteuesinr
                WHERE simulationtime >= ? AND nodeid IN {nid_list}
                ORDER BY simulationtime
                """,
                con,
                params=(from_sim_ue,),
            )

            df_dwell = pd.read_sql_query(
                f"""
                SELECT ueid, cellid, dwell_s, simulationtime
                FROM lteue_dwell
                WHERE simulationtime >= ? AND ueid IN {nid_list}
                ORDER BY simulationtime
                """,
                con,
                params=(from_sim_ue,),
            )


            ue_pdcp_tp = collect_dl_ul_samples(df_pdcp, "ueid", "dlmbps", "ulmbps", ThroughputSample, cell_col="cellid", node_col=None)
            ue_prb = collect_dl_ul_samples(df_prb, "ueid", "dl_prbs", "ul_prbs", PrbSample, cell_col="cellid", node_col=None)

            out_sinr: Dict[int, List[SinrSample]] = defaultdict(list)
            for row in df_sinr.itertuples(index=False):
                ue_id = int(row.nodeid)
                out_sinr[ue_id].append(
                    SinrSample(
                        sim_ns=int(row.simulationtime),
                        sinr_db=float(row.sinr_db),
                        sinr_linear=float(row.sinr_linear),
                        rsrp_dbm=float(row.rsrp_dbm),
                        rsrp_mw=float(row.rsrp_mw),
                        cellid=int(row.cellid),
                        rnti=int(row.rnti),
                    )
                )
            for lst in out_sinr.values():
                lst.sort(key=lambda s: s.sim_ns)
            ue_sinr = dict(out_sinr)

            out_dwell: Dict[int, List[DwellSample]] = defaultdict(list)
            for row in df_dwell.itertuples(index=False):
                ue_id = int(row.ueid)
                out_dwell[ue_id].append(
                    DwellSample(
                        sim_ns=int(row.simulationtime),
                        dwell_s=float(row.dwell_s),
                        cellid=int(row.cellid),
                    )
                )
            for lst in out_dwell.values():
                lst.sort(key=lambda s: s.sim_ns)
            ue_dwell = dict(out_dwell)

        cell_prb_util_df = pd.read_sql_query(
            """
            SELECT nodeid, cellid, dlutil, ulutil, window_ms, ttis_observed, simulationtime
            FROM lteenbprbutilization
            WHERE simulationtime >= ?
            ORDER BY simulationtime
            """,
            con,
            params=(from_sim,),
        )
        cell_sched_tp_df = pd.read_sql_query(
            """
            SELECT nodeid, cellid, dl_mbps, ul_mbps, window_ms, ttis_observed, simulationtime
            FROM lteenb_sched_tp
            WHERE simulationtime >= ?
            ORDER BY simulationtime
            """,
            con,
            params=(from_sim,),
        )
        cell_mcs_df = pd.read_sql_query(
            """
            SELECT e2nodeid, cellid, dl_mean, dl_p50, dl_p95,
                   ul_mean, ul_p50, ul_p95, simulationtime
            FROM lteenb_mcs
            WHERE simulationtime >= ?
            ORDER BY simulationtime
            """,
            con,
            params=(from_sim,),
        )
        cell_ul_interf_df = pd.read_sql_query(
            """
            SELECT e2nodeid, cellid, p95mw, p95dbm, simulationtime
            FROM lteenb_ul_interf
            WHERE simulationtime >= ?
            ORDER BY simulationtime
            """,
            con,
            params=(from_sim,),
        )
        cell_uecount_df = pd.read_sql_query(
            """
            SELECT e2nodeid, cellid, ue_count, simulationtime
            FROM lteenb_uecount
            WHERE simulationtime >= ?
            ORDER BY simulationtime
            """,
            con,
            params=(from_sim,),
        )
        ho_events_df = pd.read_sql_query(
            """
            SELECT imsi, ueid, srcCell, dstCell, event, simulationtime
            FROM lte_ho_events
            WHERE simulationtime >= ?
            ORDER BY simulationtime
            """,
            con,
            params=(from_sim,),
        )

        cell_prb_util = collect_dl_ul_samples(
            cell_prb_util_df, "cellid", "dlutil", "ulutil", UtilizationSample, cell_col="cellid", node_col="nodeid"
        )
        cell_sched_tp = collect_dl_ul_samples(
            cell_sched_tp_df, "cellid", "dl_mbps", "ul_mbps", ThroughputSample, cell_col="cellid", node_col="nodeid"
        )

        cell_mcs: Dict[int, List[McsSample]] = defaultdict(list)
        for row in cell_mcs_df.itertuples(index=False):
            cell_id = int(row.cellid)
            cell_mcs[cell_id].append(
                McsSample(
                    sim_ns=int(row.simulationtime),
                    e2nodeid=int(row.e2nodeid),
                    dl_mean=float(row.dl_mean),
                    dl_p50=float(row.dl_p50),
                    dl_p95=float(row.dl_p95),
                    ul_mean=float(row.ul_mean),
                    ul_p50=float(row.ul_p50),
                    ul_p95=float(row.ul_p95),
                )
            )
        for lst in cell_mcs.values():
            lst.sort(key=lambda s: s.sim_ns)

        cell_ul_interf: Dict[int, List[InterferenceSample]] = defaultdict(list)
        for row in cell_ul_interf_df.itertuples(index=False):
            cell_ul_interf[int(row.cellid)].append(
                InterferenceSample(
                    sim_ns=int(row.simulationtime),
                    e2nodeid=int(row.e2nodeid),
                    p95_mw=float(row.p95mw),
                    p95_dbm=float(row.p95dbm),
                )
            )
        for lst in cell_ul_interf.values():
            lst.sort(key=lambda s: s.sim_ns)

        cell_ue_count: Dict[int, List[CountSample]] = defaultdict(list)
        for row in cell_uecount_df.itertuples(index=False):
            cell_ue_count[int(row.cellid)].append(
                CountSample(
                    sim_ns=int(row.simulationtime),
                    value=int(row.ue_count),
                    cellid=int(row.cellid),
                    e2nodeid=int(row.e2nodeid),
                )
            )
        for lst in cell_ue_count.values():
            lst.sort(key=lambda s: s.sim_ns)

        ho_events: List[HoEvent] = [
            HoEvent(
                sim_ns=int(row.simulationtime),
                imsi=int(row.imsi),
                ueid=int(row.ueid),
                src_cell=int(row.srcCell),
                dst_cell=int(row.dstCell),
                event=str(row.event),
            )
            for row in ho_events_df.itertuples(index=False)
        ]

        # Track recent LM actions (CIO tweaks, sleep/wake) to support coordination guards.
        cio_last_ns: Dict[int, int] = {}
        sleeping_cells: set[int] = set()
        sleep_last_ns: Dict[int, int] = {}

        try:
            # Scan last N actions (by entryid DESC), independent of snapshot time window.
            LM_SCAN_LIMIT = int(os.getenv("LM_ACTION_SCAN_LIMIT", "20000"))
            if LM_SCAN_LIMIT <= 0:
                raise ValueError("LM_ACTION_SCAN_LIMIT <= 0 (skip)")

            lm_actions_df = pd.read_sql_query(
                """
                SELECT entryid, simulationtime, description
                FROM lmaction
                WHERE (
                        description LIKE 'SET_CIO %'
                    OR  description LIKE 'CELL_SLEEP %'
                    OR  description LIKE 'CELL_WAKE %'
                    OR  description LIKE 'AUTO_WAKE %'
                )
                ORDER BY entryid DESC
                LIMIT ?
                """,
                con,
                params=(LM_SCAN_LIMIT,),
            )

            # Because we scan DESC, the first time we see a cell is its latest action.
            for row in lm_actions_df.itertuples(index=False):
                desc = str(getattr(row, "description", "") or "")
                m = re.search(r"cell=(\d+)", desc)
                if not m:
                    continue

                cell_id = int(m.group(1))
                ts = int(getattr(row, "simulationtime", 0) or 0)

                # CIO cooldown: keep latest only
                if desc.startswith("SET_CIO"):
                    if cell_id not in cio_last_ns:
                        cio_last_ns[cell_id] = ts
                    continue

                # Sleep/wake: keep latest only
                if desc.startswith("CELL_SLEEP") or desc.startswith("CELL_WAKE") or desc.startswith("AUTO_WAKE"):
                    if cell_id not in sleep_last_ns:
                        sleep_last_ns[cell_id] = ts
                        if desc.startswith("CELL_SLEEP"):
                            sleeping_cells.add(cell_id)
                        else:
                            sleeping_cells.discard(cell_id)

        except (sqlite3.OperationalError, pd.errors.DatabaseError, ValueError) as e:
            log.debug("lmaction scan skipped: %s", e)
            # Older runs may lack lmaction; or scan disabled; treat as no recent actions.
            pass


        # Compose cell set (include empty cells that still have reporters)
        cell_ids = set(by_cell.keys())
        cell_ids.update(cell_prb_util.keys())
        cell_ids.update(cell_sched_tp.keys())
        cell_ids.update(cell_mcs.keys())
        cell_ids.update(cell_ul_interf.keys())
        cell_ids.update(cell_ue_count.keys())
        cell_ids.update(cell_to_e2.keys())

        cells: List[CellSnapshot] = []
        for cid in sorted(cell_ids):
            cells.append(
                CellSnapshot(
                    cell_id=cid,
                    e2nodeid=cell_to_e2.get(cid),
                    attached_ues=sorted(by_cell.get(cid, [])),
                )
            )

        snap = NetworkSnapshot(
            time_s=now,
            sim_time_ns=max_sim,
            ues=ues,
            cells=cells,
            ue_pdcp_tp=ue_pdcp_tp,
            ue_prb=ue_prb,
            ue_sinr=ue_sinr,
            ue_dwell=ue_dwell,
            cell_prb_util=cell_prb_util,
            cell_sched_tp=cell_sched_tp,
            cell_mcs=dict(cell_mcs),
            cell_ul_interf=dict(cell_ul_interf),
            cell_ue_count=dict(cell_ue_count),
            ho_events=ho_events,
            cio_last_ns=cio_last_ns,
            sleeping_cells=sleeping_cells,
            sleep_last_ns=sleep_last_ns,
        )
        setattr(snap, "_sim_ts_ns", max_sim)

        def latest(series):
            return series[-1] if series else None

        for ue in snap.ues:
            pdcp_sample = latest(snap.ue_pdcp_tp.get(ue.ue_e2id, []))
            if pdcp_sample:
                ue.pdcp_dl_mbps = pdcp_sample.dl
                ue.pdcp_ul_mbps = pdcp_sample.ul
            prb_sample = latest(snap.ue_prb.get(ue.ue_e2id, []))
            if prb_sample:
                ue.dl_prbs = prb_sample.dl
                ue.ul_prbs = prb_sample.ul
            sinr_sample = latest(snap.ue_sinr.get(ue.ue_e2id, []))
            if sinr_sample:
                ue.sinr_db = sinr_sample.sinr_db
                ue.sinr_linear = sinr_sample.sinr_linear
                ue.rsrp_dbm = sinr_sample.rsrp_dbm
                ue.rsrp_mw = sinr_sample.rsrp_mw
            dwell_sample = latest(snap.ue_dwell.get(ue.ue_e2id, []))
            if dwell_sample:
                ue.dwell_s = dwell_sample.dwell_s
            serving_util = latest(snap.cell_prb_util.get(ue.serving_cell, []))
            if serving_util:
                ue.serving_headroom = max(0.0, 1.0 - serving_util.dl)
            if ue.neighbor_cell is not None:
                neighbor_util = latest(snap.cell_prb_util.get(ue.neighbor_cell, []))
                if neighbor_util:
                    ue.neighbor_headroom = max(0.0, 1.0 - neighbor_util.dl)

        for cell in snap.cells:
            util_sample = latest(snap.cell_prb_util.get(cell.cell_id, []))
            if util_sample:
                cell.prb_dl_util = util_sample.dl
                cell.prb_ul_util = util_sample.ul
                if util_sample.e2nodeid is not None:
                    cell.e2nodeid = util_sample.e2nodeid
            sched_sample = latest(snap.cell_sched_tp.get(cell.cell_id, []))
            if sched_sample:
                cell.sched_dl_mbps = sched_sample.dl
                cell.sched_ul_mbps = sched_sample.ul
                if sched_sample.e2nodeid is not None and cell.e2nodeid is None:
                    cell.e2nodeid = sched_sample.e2nodeid
            mcs_sample = latest(snap.cell_mcs.get(cell.cell_id, []))
            if mcs_sample:
                cell.mcs_dl_mean = mcs_sample.dl_mean
                cell.mcs_ul_mean = mcs_sample.ul_mean
                cell.mcs_dl_p50 = mcs_sample.dl_p50
                cell.mcs_dl_p95 = mcs_sample.dl_p95
                cell.mcs_ul_p50 = mcs_sample.ul_p50
                cell.mcs_ul_p95 = mcs_sample.ul_p95
                if cell.e2nodeid is None:
                    cell.e2nodeid = mcs_sample.e2nodeid
            interf_sample = latest(snap.cell_ul_interf.get(cell.cell_id, []))
            if interf_sample:
                cell.ul_interf_dbm = interf_sample.p95_dbm
                cell.ul_interf_mw = interf_sample.p95_mw
                if cell.e2nodeid is None:
                    cell.e2nodeid = interf_sample.e2nodeid
            ue_count_sample = latest(snap.cell_ue_count.get(cell.cell_id, []))
            if ue_count_sample:
                cell.ue_count = ue_count_sample.value
                if cell.e2nodeid is None:
                    cell.e2nodeid = ue_count_sample.e2nodeid

        dt = (time.perf_counter() - t0) * 1000
        missing_pdcp = sum(1 for ue in snap.ues if ue.pdcp_dl_mbps is None)
        missing_sinr = sum(1 for ue in snap.ues if ue.sinr_db is None)
        missing_neighbor = sum(1 for ue in snap.ues if ue.neighbor_cell is None)
        log.info(
            "snapshot: ues=%d cells=%d sim_t=%.0fs window=%.0fs | %.1f ms | missing pdcp=%d sinr=%d neighbor=%d",
            len(snap.ues),
            len(snap.cells),
            max_sim / 1e9,
            window_s,
            dt,
            missing_pdcp,
            missing_sinr,
            missing_neighbor,
        )
        if snap.ues:
            u0 = snap.ues[0]
            pdcp_text = f"{u0.pdcp_dl_mbps:.2f}" if u0.pdcp_dl_mbps is not None else "n/a"
            sinr_text = f"{u0.sinr_db:.1f}" if u0.sinr_db is not None else "n/a"
            log.debug(
                "first UE: id=%d scell=%d rsrp=%.1f loss=%.1f%% pdcp=%s Mbps sinr=%s dB",
                u0.ue_e2id,
                u0.serving_cell,
                u0.serving_rsrp,
                u0.packet_loss_pct,
                pdcp_text,
                sinr_text,
            )
        return snap
    finally:
        con.close()
