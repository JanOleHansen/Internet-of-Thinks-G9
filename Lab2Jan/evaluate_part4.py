#!/usr/bin/env python3
"""
IoT Lab 2 – Part 4: Measurement Collection Evaluation
=======================================================

Connects to the SINK node's Renode UART terminal (TCP port 60001),
parses the measurement CSV lines printed by the firmware, and produces:

  • Console summary  – per-node latency stats + reliability table
  • Raw data CSV     – all received measurements
  • PNG figure       – four panels:
        1. Latency distribution per node (violin)
        2. CDF of tx latency per node
        3. Reliability (% of expected measurements received) per node
        4. % of measurements received in time (tx_ms ≤ 200 ms) per node

Usage
-----
  python3 evaluate_part4.py [--target N] [--label LABEL] [--out DIR]

  --target N   Stop after N readings from each remote node (default: 500)
  --label      Tag for filenames and plot title     (default: no_loss)
  --out DIR    Output directory                     (default: current dir)

Typical workflow
----------------
  1. Build NodeSink (auto-SINK) and Node1-4 (relay) firmwares.
  2. Start Renode:  renode Lab2/lab2.resc
  3. In Renode:     start
  4. Run this script (it connects automatically):
       python3 evaluate_part4.py --target 500 --label no_loss
  5. Script exits once every remote node has >= 500 readings (or Ctrl-C).
  6. For the lossy run: comment line 22, uncomment line 23 in lab2.resc,
     restart Renode, and repeat with --label with_loss.

Firmware CSV format (SINK UART output)
---------------------------------------
  <nodeID>;<counter>;<temp>;<humidity>;<timestamp_ms>;<tx_ms>

  nodeID      – low byte of source BLE address
  counter     – per-node measurement counter (starts at 0)
  temp        – °C with one decimal place  (e.g. -18.5 or 152.3)
  humidity    – % with one decimal place   (e.g. 67.8)
  timestamp   – k_uptime_get() at measurement time (ms since boot)
  tx_ms       – reception time at SINK minus timestamp; 0 for SINK's own
"""

import argparse
import csv
import os
import re
import socket
import threading
import time
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# ─────────────────────────────────────────────────────────────────────────────
#  Configuration
# ─────────────────────────────────────────────────────────────────────────────

SINK_PORT          = 60001
MEAS_INTERVAL_MS   = 200   # firmware measurement period
IN_TIME_THRESHOLD  = MEAS_INTERVAL_MS

# Colour palette (up to 4 nodes)
NODE_COLOURS = ["#2196F3", "#FF9800", "#4CAF50", "#9C27B0"]

# ─────────────────────────────────────────────────────────────────────────────
#  Regex for a single measurement line
# ─────────────────────────────────────────────────────────────────────────────

_RE_MEAS = re.compile(
    r"^(\d+);(\d+);(-?\d+\.\d+);(\d+\.\d+);(\d+);(\d+)\s*$"
)

# ─────────────────────────────────────────────────────────────────────────────
#  Shared state
# ─────────────────────────────────────────────────────────────────────────────

_lock  = threading.Lock()
_stop  = threading.Event()

# measurements[node_id] = [{"counter": int, "temp": float, "humidity": float,
#                            "ts_ms": int, "tx_ms": int}, ...]
_measurements: dict[int, list] = defaultdict(list)


def _parse_line(line: str) -> None:
    m = _RE_MEAS.match(line)
    if not m:
        return
    node_id  = int(m.group(1))
    counter  = int(m.group(2))
    temp     = float(m.group(3))
    humidity = float(m.group(4))
    ts_ms    = int(m.group(5))
    tx_ms    = int(m.group(6))
    with _lock:
        _measurements[node_id].append({
            "counter":  counter,
            "temp":     temp,
            "humidity": humidity,
            "ts_ms":    ts_ms,
            "tx_ms":    tx_ms,
        })


def _listener(port: int) -> None:
    """Continuously read UART output from the SINK's TCP terminal."""
    while not _stop.is_set():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(2.0)
                s.connect(("localhost", port))
                print(f"[✓] Connected to SINK UART (port {port})", flush=True)
                buf = ""
                while not _stop.is_set():
                    try:
                        data = s.recv(4096).decode("utf-8", errors="replace")
                        if not data:
                            break
                        buf += data
                        *lines, buf = buf.split("\n")
                        for raw in lines:
                            line = raw.strip("\r").strip()
                            if line:
                                print(f"  [sink] {line}", flush=True)
                                _parse_line(line)
                    except socket.timeout:
                        pass
        except Exception as exc:
            if not _stop.is_set():
                print(f"[!] {exc}; retry in 3 s", flush=True)
                time.sleep(3)


# ─────────────────────────────────────────────────────────────────────────────
#  Progress monitor (runs in main thread)
# ─────────────────────────────────────────────────────────────────────────────

def _monitor(target: int) -> None:
    """Print a progress line every 10 s; return when target is met."""
    while not _stop.is_set():
        time.sleep(10)
        with _lock:
            snap = {nid: len(rows) for nid, rows in _measurements.items()}
        remote = {nid: n for nid, n in snap.items() if n > 0}
        parts  = "  ".join(f"node {nid}: {n}" for nid, n in sorted(remote.items()))
        print(f"[progress] {parts}", flush=True)

        # Stop once every node that has started sending has reached the target.
        # We need at least 3 remote nodes (3 relay nodes in a 4-node network).
        remote_nodes = [nid for nid, n in remote.items() if n > 0]
        if len(remote_nodes) >= 3 and all(n >= target for n in remote.values()):
            print(f"\n[main] All nodes reached {target} readings – stopping.\n",
                  flush=True)
            _stop.set()
            return


# ─────────────────────────────────────────────────────────────────────────────
#  Analysis
# ─────────────────────────────────────────────────────────────────────────────

def _identify_sink(snap: dict) -> int | None:
    """Return the node_id whose measurements all have tx_ms == 0 (SINK)."""
    for nid, rows in snap.items():
        if rows and all(r["tx_ms"] == 0 for r in rows):
            return nid
    return None


def _reliability(rows: list) -> tuple[float, int, int]:
    """Return (reliability_pct, received, expected) for one node's rows."""
    if not rows:
        return 0.0, 0, 0
    counters = [r["counter"] for r in rows]
    min_c, max_c = min(counters), max(counters)
    expected = max_c - min_c + 1
    received = len(set(counters))
    return 100.0 * received / expected if expected else 0.0, received, expected


def analyse_and_plot(out_dir: str, label: str, target: int) -> None:
    with _lock:
        snap = {nid: list(rows) for nid, rows in _measurements.items()}

    if not snap:
        print("[!] No measurement data collected.")
        return

    sink_id      = _identify_sink(snap)
    node_ids     = sorted(snap.keys())
    remote_ids   = [nid for nid in node_ids if nid != sink_id]

    # Assign a colour to every node
    colour_map = {nid: NODE_COLOURS[i % len(NODE_COLOURS)]
                  for i, nid in enumerate(node_ids)}

    # Infer hop count for remote nodes by ranking median tx_ms
    medians = {nid: float(np.median([r["tx_ms"] for r in snap[nid]]))
               for nid in remote_ids if snap[nid]}
    hop_rank = {nid: i + 1
                for i, (nid, _) in enumerate(sorted(medians.items(), key=lambda x: x[1]))}

    def _label(nid: int) -> str:
        if nid == sink_id:
            return f"node {nid} (SINK)"
        hop = hop_rank.get(nid, "?")
        return f"node {nid} (hop {hop})"

    # ── Save raw CSV ──────────────────────────────────────────────────────────
    csv_path = os.path.join(out_dir, f"lab2_part4_{label}_measurements.csv")
    rows_out = []
    for nid in node_ids:
        for r in snap[nid]:
            rows_out.append({
                "node_id":   nid,
                "role":      "sink" if nid == sink_id else f"hop{hop_rank.get(nid, '?')}",
                "counter":   r["counter"],
                "temp":      r["temp"],
                "humidity":  r["humidity"],
                "ts_ms":     r["ts_ms"],
                "tx_ms":     r["tx_ms"],
            })
    rows_out.sort(key=lambda x: (x["node_id"], x["counter"]))
    if rows_out:
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=rows_out[0].keys())
            writer.writeheader()
            writer.writerows(rows_out)
        print(f"[data] CSV saved → {csv_path}")

    # ── Console summary ───────────────────────────────────────────────────────
    W = 78
    print(f"\n{'═'*W}")
    print(f"  Part 4 Summary  [{label}]   target={target} readings/node")
    print(f"{'═'*W}")
    print(f"  SINK node: {sink_id}   |   Remote nodes: {remote_ids}")
    print()
    hdr = f"  {'Node':<20} {'Rcvd':>6} {'Expct':>6} {'Reliab':>8} {'InTime':>8}"
    hdr += f" {'Mean':>8} {'Med':>8} {'P5':>7} {'P95':>8}"
    print(hdr)
    print(f"  {'-'*20} {'-'*6} {'-'*6} {'-'*8} {'-'*8} {'-'*8} {'-'*8} {'-'*7} {'-'*8}")
    for nid in node_ids:
        rows = snap[nid]
        rel, rcvd, expct = _reliability(rows)
        tx_vals = np.array([r["tx_ms"] for r in rows if r["tx_ms"] > 0])
        in_time = sum(1 for r in rows if r["tx_ms"] <= IN_TIME_THRESHOLD)
        in_time_pct = 100.0 * in_time / len(rows) if rows else 0.0
        if len(tx_vals) > 0:
            row_str = (f"  {_label(nid):<20} {rcvd:>6} {expct:>6} {rel:>7.1f}%"
                       f" {in_time_pct:>7.1f}%"
                       f" {tx_vals.mean():>8.1f} {float(np.median(tx_vals)):>8.1f}"
                       f" {float(np.percentile(tx_vals,5)):>7.1f}"
                       f" {float(np.percentile(tx_vals,95)):>8.1f}")
        else:
            row_str = (f"  {_label(nid):<20} {rcvd:>6} {expct:>6} {rel:>7.1f}%"
                       f" {in_time_pct:>7.1f}%"
                       f" {'—':>8} {'—':>8} {'—':>7} {'—':>8}")
        print(row_str)
    print(f"  (latency columns in ms; SINK self-latency is always 0 and excluded)")
    print(f"{'═'*W}\n")

    # ── Plots ─────────────────────────────────────────────────────────────────
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(
        f"IoT Lab 2 – Part 4: Measurement Collection [{label}]\n"
        f"SINK node: {sink_id}   |   target: {target} readings/node",
        fontsize=13, fontweight="bold"
    )

    remote_rows  = {nid: snap[nid] for nid in remote_ids if snap[nid]}
    remote_order = sorted(remote_rows.keys(),
                          key=lambda nid: medians.get(nid, 0))

    tx_arrays = {nid: np.array([r["tx_ms"] for r in remote_rows[nid]])
                 for nid in remote_order}
    labels    = [_label(nid) for nid in remote_order]
    colours   = [colour_map[nid] for nid in remote_order]

    # Panel 1 – Violin latency distribution
    ax = axes[0, 0]
    valid = [(arr, lbl, col) for arr, lbl, col
             in zip(tx_arrays.values(), labels, colours) if len(arr) > 1]
    if valid:
        arrs, lbls, cols = zip(*valid)
        vp = ax.violinplot(arrs, showmedians=True, showextrema=True)
        for body, c in zip(vp["bodies"], cols):
            body.set_facecolor(c); body.set_alpha(0.65)
            body.set_edgecolor("black"); body.set_linewidth(0.8)
        for part in ("cmedians", "cmins", "cmaxes", "cbars"):
            vp[part].set_color("black"); vp[part].set_linewidth(1.5)
        ax.set_xticks(range(1, len(lbls) + 1))
        ax.set_xticklabels(lbls, fontsize=8)
        for i, (arr, c) in enumerate(zip(arrs, cols), 1):
            jitter = np.random.uniform(-0.07, 0.07, size=len(arr))
            ax.scatter(i + jitter, arr, color=c, alpha=0.3, s=8, zorder=2,
                       edgecolors="none")
    ax.axhline(IN_TIME_THRESHOLD, color="red", linestyle="--",
               linewidth=1.2, label=f"200 ms threshold")
    ax.legend(fontsize=8)
    ax.set_title("Latency Distribution per Node")
    ax.set_xlabel("Node"); ax.set_ylabel("TX latency (ms)")
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    # Panel 2 – CDF
    ax = axes[0, 1]
    for nid, lbl, col in zip(remote_order, labels, colours):
        arr = tx_arrays[nid]
        if len(arr) == 0:
            continue
        s   = np.sort(arr)
        cdf = np.arange(1, len(s) + 1) / len(s)
        ax.step(s, cdf, where="post", label=lbl, color=col, linewidth=2.0)
        ax.axvline(float(np.median(s)), color=col, linestyle=":", linewidth=1.0,
                   alpha=0.6)
    ax.axvline(IN_TIME_THRESHOLD, color="red", linestyle="--",
               linewidth=1.2, label="200 ms threshold")
    ax.set_title("Latency CDF per Node")
    ax.set_xlabel("TX latency (ms)"); ax.set_ylabel("P(latency ≤ x)")
    ax.set_ylim(0, 1.05); ax.legend(fontsize=8, loc="lower right")
    ax.grid(linestyle="--", alpha=0.5)

    # Panel 3 – Reliability bar chart
    ax = axes[1, 0]
    all_ids = node_ids
    all_lbls   = [_label(nid) for nid in all_ids]
    all_cols   = [colour_map[nid] for nid in all_ids]
    all_rel    = []
    all_rcvd   = []
    all_expct  = []
    for nid in all_ids:
        r, rcvd, expct = _reliability(snap[nid])
        all_rel.append(r); all_rcvd.append(rcvd); all_expct.append(expct)
    x = np.arange(len(all_ids))
    bars = ax.bar(x, all_rel, color=all_cols, alpha=0.85,
                  edgecolor="black", linewidth=0.8, width=0.5)
    for bar, rv, rcvd, expct in zip(bars, all_rel, all_rcvd, all_expct):
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 1.5,
                f"{rv:.1f}%\n({rcvd}/{expct})",
                ha="center", va="bottom", fontsize=8, fontweight="bold")
    ax.set_xticks(x); ax.set_xticklabels(all_lbls, fontsize=8)
    ax.set_ylim(0, 120); ax.axhline(100, color="grey", linestyle="--", linewidth=1)
    ax.set_title("Reliability per Node")
    ax.set_xlabel("Node"); ax.set_ylabel("Measurements received (%)")
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    # Panel 4 – % received in time
    ax = axes[1, 1]
    in_time_pcts = []
    for nid in all_ids:
        rows = snap[nid]
        if not rows:
            in_time_pcts.append(0.0)
            continue
        cnt = sum(1 for r in rows if r["tx_ms"] <= IN_TIME_THRESHOLD)
        in_time_pcts.append(100.0 * cnt / len(rows))
    bars2 = ax.bar(x, in_time_pcts, color=all_cols, alpha=0.85,
                   edgecolor="black", linewidth=0.8, width=0.5)
    for bar, pct in zip(bars2, in_time_pcts):
        ax.text(bar.get_x() + bar.get_width() / 2,
                bar.get_height() + 1.5,
                f"{pct:.1f}%",
                ha="center", va="bottom", fontsize=9, fontweight="bold")
    ax.set_xticks(x); ax.set_xticklabels(all_lbls, fontsize=8)
    ax.set_ylim(0, 120); ax.axhline(100, color="grey", linestyle="--", linewidth=1)
    ax.set_title(f"% Received in Time (tx ≤ {IN_TIME_THRESHOLD} ms)")
    ax.set_xlabel("Node"); ax.set_ylabel("Measurements received in time (%)")
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    plt.tight_layout()
    png_path = os.path.join(out_dir, f"lab2_part4_{label}.png")
    fig.savefig(png_path, dpi=150, bbox_inches="tight")
    print(f"[plot] PNG saved → {png_path}")
    plt.close(fig)


# ─────────────────────────────────────────────────────────────────────────────
#  Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--target", type=int, default=500,
                    help="Stop after N readings from each remote node (default 500)")
    ap.add_argument("--label",  default="no_loss",
                    help="Run label used in filenames and title (default: no_loss)")
    ap.add_argument("--out",    default=".",
                    help="Output directory for PNG and CSV (default: .)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    print("═" * 60)
    print(" IoT Lab 2 – Part 4 Evaluator")
    print("═" * 60)
    print(f" Target   : {args.target} readings per remote node")
    print(f" Label    : {args.label}")
    print(f" Output   : {os.path.abspath(args.out)}")
    print()
    print(" Connecting to SINK UART (port 60001)…")
    print(" Ctrl-C at any time to stop early and generate plots.\n")

    listener = threading.Thread(target=_listener, args=(SINK_PORT,), daemon=True)
    listener.start()

    monitor = threading.Thread(target=_monitor, args=(args.target,), daemon=True)
    monitor.start()

    try:
        while not _stop.is_set():
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[main] Interrupted – stopping collection…")
        _stop.set()

    time.sleep(0.5)
    analyse_and_plot(args.out, args.label, args.target)


if __name__ == "__main__":
    main()
