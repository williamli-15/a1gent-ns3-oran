#!/usr/bin/env python3
# PRB-based energy estimate with optional sleep parsing
import argparse, sqlite3, re, math

def parse_sleep_timeline(con):
    """Return {cell: [(t_ns, sleeping_bool), ...]} from lmaction descriptions."""
    rows = con.execute(
        "SELECT entryid, simulationtime, description "
        "FROM lmaction "
        "WHERE description LIKE 'CELL_SLEEP %' OR description LIKE 'CELL_WAKE %' OR description LIKE 'AUTO_WAKE %' "
        "ORDER BY entryid ASC"
    ).fetchall()

    by_cell = {}
    for entryid, t, desc in rows:
        m = re.search(r"cell=(\d+)", desc or "")
        if not m:
            continue
        cid = int(m.group(1))
        sleeping = ("CELL_SLEEP" in desc)
        by_cell.setdefault(cid, []).append((int(t), sleeping))
    return by_cell

def is_sleeping(t, timeline):
    """Binary search last toggle <= t, return True if sleeping, else False."""
    if not timeline: 
        return False
    lo, hi = 0, len(timeline)-1
    if t < timeline[0][0]: 
        return False
    while lo <= hi:
        mid = (lo + hi)//2
        if timeline[mid][0] <= t: lo = mid + 1
        else: hi = mid - 1
    return timeline[hi][1]

def estimate_db(db_path, p0=60.0, pmax=120.0, psleep=8.0, mode="dl"):
    """
    P_active(t) = P0 + (Pmax-P0)*util  (util from lteenbprbutilization)
    P_sleep(t)  = Psleep               (if bridge logged CELL_SLEEP)
    mode: 'dl' uses dlutil; 'max' uses max(dl,ul); 'mean' mean(dl,ul)
    """
    con = sqlite3.connect(db_path)
    sleep = parse_sleep_timeline(con)
    total_j = 0.0
    per_cell = {}
    # note: window_ms and ttis_observed were added by your PRB reporter; fall back to 1s if missing
    rows = con.execute(
        "SELECT cellid, dlutil, ulutil, window_ms, ttis_observed, simulationtime "
        "FROM lteenbprbutilization ORDER BY cellid, simulationtime"
    ).fetchall()
    for cid, dl, ul, win_ms, ttis, t in rows:
        dl = float(dl or 0.0); ul = float(ul or 0.0)
        util = dl if mode=="dl" else (max(dl,ul) if mode=="max" else (dl+ul)/2.0)
        dt = (win_ms / 1000.0) if (win_ms and win_ms > 0) else 1.0
        p = psleep if is_sleeping(int(t), sleep.get(cid)) else (p0 + (pmax - p0)*max(0.0, min(1.0, util)))
        e = p * dt
        total_j += e
        per_cell[cid] = per_cell.get(cid, 0.0) + e
    con.close()
    return total_j, per_cell

def main():
    ap = argparse.ArgumentParser(description="Estimate RU energy (J) from oran-repository.db")
    ap.add_argument("db", nargs="+", help="one or two DB paths (baseline then rApp)")
    ap.add_argument("--p0", type=float, default=60.0, help="idle Watts (active) per sector")
    ap.add_argument("--pmax", type=float, default=120.0, help="full-load Watts per sector")
    ap.add_argument("--psleep", type=float, default=8.0, help="sleep/standby Watts per sector")
    ap.add_argument("--mode", choices=["dl","max","mean"], default="dl", help="util model")
    args = ap.parse_args()

    if len(args.db) == 1:
        tot, per = estimate_db(args.db[0], args.p0, args.pmax, args.psleep, args.mode)
        print(f"{args.db[0]}: total_energy = {tot:,.0f} J  (cells={len(per)})")
        for cid in sorted(per):
            print(f"  cell {cid:>3}: {per[cid]:,.0f} J")
        return

    base_tot, _ = estimate_db(args.db[0], args.p0, args.pmax, args.psleep, args.mode)
    app_tot,  _ = estimate_db(args.db[1], args.p0, args.pmax, args.psleep, args.mode)
    delta = base_tot - app_tot
    pct = (delta/base_tot*100.0) if base_tot>0 else float("nan")
    print(f"Baseline      : {args.db[0]}\nLLM (rApp)   : {args.db[1]}")
    print(f"Total energy  : baseline={base_tot:,.0f} J   rApp={app_tot:,.0f} J   Δ={delta:,.0f} J ({pct:.1f}%)")

if __name__ == "__main__":
    main()
