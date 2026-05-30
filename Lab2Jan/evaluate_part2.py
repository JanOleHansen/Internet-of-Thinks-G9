#!/usr/bin/env python3
"""
IoT Lab 2 – Part 2: Multi-Hop Latency & Reliability Evaluation
================================================================

Connects to the four Renode UART terminals (TCP ports 60001-60004),
parses the firmware's printk output in real-time, and produces:

  • Console summary  – per-hop latency stats + reliability table
  • Raw event CSV    – every timestamped event for offline analysis
  • PNG figure       – four panels:
        1. Violin plot of latency distribution per hop
        2. CDF of latency per hop
        3. Reliability (% messages received) per hop
        4. Latency time-series per sequence number

Usage
-----
  python3 evaluate_part2.py [options]

  --duration N   Collect for N seconds then exit (default 60, Ctrl-C to stop early)
  --label LABEL  Tag for this run; used in filenames and plot title
                   e.g. --label no_loss   or   --label with_loss
  --out DIR      Directory for output files (default: current directory)

Typical workflow
----------------
  1.  Start Renode:   renode Lab2/lab2.resc
  2.  In Renode:      start
  3.  Run this script:  python3 evaluate_part2.py --duration 90 --label no_loss
  4.  Press SW0 on device_1 in the Renode Monitor:
        mach set "device_1"
        gpio0 OnGPIO 11 false   # press
        gpio0 OnGPIO 11 true    # release
      Repeat at least 20 times.
  5.  Script exits after --duration seconds (or Ctrl-C) and saves results.
  6.  For the loss evaluation, switch the wireless function in lab2.resc
      (comment line 22, uncomment line 23) and repeat with --label with_loss.

Firmware log format expected
-----------------------------
  [INIT] Forming network  net_id=...
  [INIT] LED_ON  seq=N
  [INIT] LED_OFF seq=N
  [MEMBER] LED_ON  type=T seq=N
  [MEMBER] LED_OFF seq=N
"""

import argparse
import csv
import os
import re
import socket
import sys
import threading
import time
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")          # works without a display server
import matplotlib.pyplot as plt
import numpy as np

# ──────────────────────────────────────────────────────────────────────────────
#  Configuration
# ──────────────────────────────────────────────────────────────────────────────

PORTS       = [60001, 60002, 60003, 60004]
N_NODES     = 4

# Colours for hops 1, 2, 3
HOP_COLOURS = ["#2196F3", "#FF9800", "#4CAF50"]

# ──────────────────────────────────────────────────────────────────────────────
#  Regex patterns matching the firmware's printk lines
# ──────────────────────────────────────────────────────────────────────────────

_RE_FORM   = re.compile(r"\[INIT\]\s+Forming network")
_RE_INIT   = re.compile(r"\[INIT\]\s+(LED_ON|LED_OFF)\s+seq=(\d+)")
_RE_MEMBER = re.compile(r"\[MEMBER\]\s+(LED_ON|LED_OFF).*?seq=(\d+)")

# ──────────────────────────────────────────────────────────────────────────────
#  Shared state  (written by listener threads, read by analysis)
# ──────────────────────────────────────────────────────────────────────────────

_lock      = threading.Lock()
_stop      = threading.Event()

# _events[seq][device_idx] = (wall_time_s, kind_str)
_events    = defaultdict(dict)
_init_dev  = None     # which device index is the INITIATOR

# ──────────────────────────────────────────────────────────────────────────────
#  Per-device TCP listener
# ──────────────────────────────────────────────────────────────────────────────

def _parse_line(dev: int, line: str, ts: float) -> None:
    """Extract timing information from a single UART log line."""
    global _init_dev

    # INITIATOR: network formation → implicit seq = 0
    if _RE_FORM.search(line):
        with _lock:
            _init_dev = dev
            _events[0][dev] = (ts, "NET_FORM")
        return

    # INITIATOR: LED update
    m = _RE_INIT.search(line)
    if m:
        kind = m.group(1)
        seq  = int(m.group(2))
        with _lock:
            if _init_dev is None:
                _init_dev = dev
            _events[seq][dev] = (ts, kind)
        return

    # MEMBER: LED update
    m = _RE_MEMBER.search(line)
    if m:
        kind = m.group(1)
        seq  = int(m.group(2))
        with _lock:
            _events[seq][dev] = (ts, kind)
        return


def _listener(dev: int, port: int) -> None:
    """Continuously read UART output from one Renode TCP terminal."""
    while not _stop.is_set():
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(2.0)
                s.connect(("localhost", port))
                print(f"[✓] device_{dev+1} connected (port {port})", flush=True)
                buf = ""
                while not _stop.is_set():
                    try:
                        data = s.recv(1024).decode("utf-8", errors="replace")
                        if not data:
                            break
                        buf += data
                        *complete_lines, buf = buf.split("\n")
                        for raw in complete_lines:
                            line = raw.strip("\r").strip()
                            if line:
                                ts = time.time()
                                print(f"  [d{dev+1}] {line}", flush=True)
                                _parse_line(dev, line, ts)
                    except socket.timeout:
                        pass
        except Exception as exc:
            if not _stop.is_set():
                print(f"[!] device_{dev+1} – {exc}; retry in 3 s", flush=True)
                time.sleep(3)

# ──────────────────────────────────────────────────────────────────────────────
#  Helpers
# ──────────────────────────────────────────────────────────────────────────────

def _hop(dev: int, init: int) -> int:
    """Hop distance from initiator to dev in a line topology."""
    return abs(dev - init)


def _devs_at_hop(init: int, h: int) -> list:
    """Device indices that are exactly h hops from initiator."""
    return [d for d in range(N_NODES) if abs(d - init) == h]

# ──────────────────────────────────────────────────────────────────────────────
#  Analysis
# ──────────────────────────────────────────────────────────────────────────────

def analyse_and_plot(out_dir: str, label: str) -> None:
    # Snapshot under lock so listener threads don't interfere
    with _lock:
        snap  = {seq: dict(dm) for seq, dm in _events.items()}
        init  = _init_dev

    if init is None:
        print("\n[!] No initiator detected. Was SW0 pressed during collection?")
        return

    max_hop = max(abs(d - init) for d in range(N_NODES))   # 3 for a 4-node line
    hops    = list(range(1, max_hop + 1))

    # ── Accumulate latencies and counts per hop ───────────────────────────────
    # latencies_ms[h] = [float, ...]  (milliseconds)
    latencies_ms = {h: [] for h in hops}
    n_possible   = {h: 0 for h in hops}   # total (seq × devices) opportunities
    n_received   = {h: 0 for h in hops}   # actually received
    n_total      = 0                        # sequences with an initiator record

    for seq, devmap in sorted(snap.items()):
        if init not in devmap:
            continue
        n_total += 1
        t0 = devmap[init][0]

        for h in hops:
            for d in _devs_at_hop(init, h):
                n_possible[h] += 1
                if d in devmap:
                    n_received[h] += 1
                    lat_ms = (devmap[d][0] - t0) * 1000.0
                    if lat_ms >= 0:          # sanity: only forward-in-time
                        latencies_ms[h].append(lat_ms)

    # ── Save raw events to CSV ────────────────────────────────────────────────
    csv_path = os.path.join(out_dir, f"lab2_part2_{label}_events.csv")
    _save_csv(csv_path, snap, init, hops)

    # ── Console summary ───────────────────────────────────────────────────────
    _print_summary(label, init, n_total, hops, latencies_ms, n_possible, n_received)

    # ── Generate plots ────────────────────────────────────────────────────────
    _make_plots(latencies_ms, n_possible, n_received, n_total,
                hops, snap, init, out_dir, label)


def _save_csv(path: str, snap: dict, init: int, hops: list) -> None:
    """Write every event as a row: seq, device, hop, timestamp, kind, latency_ms."""
    rows = []
    for seq, devmap in sorted(snap.items()):
        t0_init = devmap[init][0] if init in devmap else None
        for dev, (ts, kind) in sorted(devmap.items()):
            h = _hop(dev, init)
            lat = (ts - t0_init) * 1000.0 if t0_init is not None else ""
            rows.append({
                "seq":          seq,
                "device":       f"device_{dev+1}",
                "hop":          h,
                "timestamp_s":  f"{ts:.6f}",
                "kind":         kind,
                "latency_ms":   f"{lat:.3f}" if lat != "" else "",
            })
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys() if rows else [])
        writer.writeheader()
        writer.writerows(rows)
    print(f"[data] CSV saved  → {path}")


def _print_summary(label, init, n_total, hops, latencies_ms,
                   n_possible, n_received) -> None:
    W = 72
    print(f"\n{'═'*W}")
    print(f"  Part 2 Summary  [{label}]")
    print(f"{'═'*W}")
    print(f"  Initiator : device_{init+1}   |   Total events sent : {n_total}")
    print()
    print(f"  {'Hop':<5} {'N':>5} {'Reliability':>13}"
          f" {'Mean':>9} {'Median':>9} {'Std':>9} {'P5':>7} {'P95':>8}")
    print(f"  {'-'*5} {'-'*5} {'-'*13}"
          f" {'-'*9} {'-'*9} {'-'*9} {'-'*7} {'-'*8}")
    for h in hops:
        rel = 100.0 * n_received[h] / n_possible[h] if n_possible[h] else 0.0
        arr = np.array(latencies_ms[h]) if latencies_ms[h] else None
        if arr is not None and len(arr) > 0:
            print(f"  {h:<5} {len(arr):>5} {rel:>12.1f}%"
                  f" {arr.mean():>9.1f} {np.median(arr):>9.1f}"
                  f" {arr.std():>9.1f} {np.percentile(arr,5):>7.1f}"
                  f" {np.percentile(arr,95):>8.1f}")
        else:
            print(f"  {h:<5} {'0':>5} {rel:>12.1f}%"
                  f" {'—':>9} {'—':>9} {'—':>9} {'—':>7} {'—':>8}")
    print(f"  (all latency values in ms)")
    print(f"{'═'*W}\n")

# ──────────────────────────────────────────────────────────────────────────────
#  Plots
# ──────────────────────────────────────────────────────────────────────────────

def _make_plots(latencies_ms, n_possible, n_received, n_total,
                hops, snap, init, out_dir, label) -> None:

    colours    = HOP_COLOURS[:len(hops)]
    hop_labels = [f"Hop {h}" for h in hops]
    lat_arrays = {h: np.array(latencies_ms[h]) for h in hops}

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(
        f"IoT Lab 2 – Part 2: Multi-Hop Evaluation  [{label}]\n"
        f"Initiator: device_{init+1}   |   Button events: {n_total}",
        fontsize=13, fontweight="bold"
    )

    # ── Panel 1: Violin / distribution ───────────────────────────────────────
    ax = axes[0, 0]
    plot_data   = [lat_arrays[h] for h in hops if len(lat_arrays[h]) > 1]
    plot_labels = [hop_labels[h-1] for h in hops if len(lat_arrays[h]) > 1]
    plot_cols   = [colours[h-1] for h in hops if len(lat_arrays[h]) > 1]

    if plot_data:
        vp = ax.violinplot(plot_data, showmedians=True, showextrema=True)
        for body, c in zip(vp["bodies"], plot_cols):
            body.set_facecolor(c)
            body.set_alpha(0.65)
            body.set_edgecolor("black")
            body.set_linewidth(0.8)
        for part in ("cmedians", "cmins", "cmaxes", "cbars"):
            vp[part].set_color("black")
            vp[part].set_linewidth(1.5)
        ax.set_xticks(range(1, len(plot_labels) + 1))
        ax.set_xticklabels(plot_labels)

        # Overlay individual data points (jittered)
        for i, (arr, c) in enumerate(zip(plot_data, plot_cols), start=1):
            jitter = np.random.uniform(-0.08, 0.08, size=len(arr))
            ax.scatter(i + jitter, arr, color=c, alpha=0.35,
                       s=12, zorder=2, edgecolors="none")
    else:
        ax.text(0.5, 0.5, "Not enough data", ha="center", va="center",
                transform=ax.transAxes, fontsize=12, color="grey")

    ax.set_title("Latency Distribution per Hop")
    ax.set_xlabel("Hop")
    ax.set_ylabel("Latency (ms)")
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    # ── Panel 2: CDF ─────────────────────────────────────────────────────────
    ax = axes[0, 1]
    for h, c, lbl in zip(hops, colours, hop_labels):
        arr = lat_arrays[h]
        if len(arr) == 0:
            continue
        s   = np.sort(arr)
        cdf = np.arange(1, len(s) + 1) / len(s)
        ax.step(s, cdf, where="post", label=lbl, color=c, linewidth=2.2)

        # Mark median
        med = float(np.median(s))
        ax.axvline(med, color=c, linestyle=":", linewidth=1.2, alpha=0.7)

    ax.set_title("Latency CDF per Hop")
    ax.set_xlabel("Latency (ms)")
    ax.set_ylabel("P(latency ≤ x)")
    ax.set_ylim(0, 1.05)
    ax.legend(loc="lower right")
    ax.grid(linestyle="--", alpha=0.5)

    # ── Panel 3: Reliability bar chart ───────────────────────────────────────
    ax = axes[1, 0]
    rel_vals = [
        100.0 * n_received[h] / n_possible[h] if n_possible[h] else 0.0
        for h in hops
    ]
    x = np.arange(len(hops))
    bars = ax.bar(x, rel_vals, color=colours, alpha=0.85,
                  edgecolor="black", linewidth=0.8, width=0.5)
    for bar, rv, h in zip(bars, rel_vals, hops):
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            bar.get_height() + 1.5,
            f"{rv:.1f}%\n({n_received[h]}/{n_possible[h]})",
            ha="center", va="bottom", fontsize=9, fontweight="bold"
        )
    ax.set_xticks(x)
    ax.set_xticklabels(hop_labels)
    ax.set_ylim(0, 120)
    ax.axhline(100, color="grey", linestyle="--", linewidth=1)
    ax.set_title("Reliability per Hop")
    ax.set_xlabel("Hop")
    ax.set_ylabel("Messages Received (%)")
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    # Annotate total sent
    ax.text(0.98, 0.02, f"Events sent by initiator: {n_total}",
            ha="right", va="bottom", transform=ax.transAxes,
            fontsize=8, color="grey")

    # ── Panel 4: Latency time-series per sequence number ─────────────────────
    ax = axes[1, 1]
    for h, c, lbl in zip(hops, colours, hop_labels):
        devs = _devs_at_hop(init, h)
        for d in devs:
            xs, ys = [], []
            for seq, devmap in sorted(snap.items()):
                if init in devmap and d in devmap:
                    lat = (devmap[d][0] - devmap[init][0]) * 1000.0
                    if lat >= 0:
                        xs.append(seq)
                        ys.append(lat)
            if xs:
                suffix = f" (d{d+1})" if len(devs) > 1 else ""
                ax.plot(xs, ys, "o-", color=c, markersize=4,
                        linewidth=1.2, alpha=0.8,
                        label=f"{lbl}{suffix}")

    ax.set_title("Latency per Sequence Number")
    ax.set_xlabel("Sequence Number")
    ax.set_ylabel("Latency (ms)")
    ax.legend(fontsize=8, loc="upper left")
    ax.grid(linestyle="--", alpha=0.5)

    # ── Save ─────────────────────────────────────────────────────────────────
    plt.tight_layout()
    png_path = os.path.join(out_dir, f"lab2_part2_{label}.png")
    fig.savefig(png_path, dpi=150, bbox_inches="tight")
    print(f"[plot] PNG saved  → {png_path}")
    plt.close(fig)

# ──────────────────────────────────────────────────────────────────────────────
#  Entry point
# ──────────────────────────────────────────────────────────────────────────────

def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--duration", type=int, default=60,
                    help="Collection time in seconds (default: 60; Ctrl-C to stop early)")
    ap.add_argument("--label",    default="no_loss",
                    help="Run label – used in filenames and plot title (default: no_loss)")
    ap.add_argument("--out",      default=".",
                    help="Output directory for PNG and CSV files (default: .)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)

    print("═" * 55)
    print(" IoT Lab 2 – Part 2 Evaluator")
    print("═" * 55)
    print(f" Duration  : {args.duration} s")
    print(f" Label     : {args.label}")
    print(f" Output    : {os.path.abspath(args.out)}")
    print()
    print(" Connecting to Renode UART terminals (ports 60001-60004)…")
    print(" Ctrl-C at any time to stop early and generate plots.\n")

    threads = [
        threading.Thread(target=_listener, args=(i, p), daemon=True)
        for i, p in enumerate(PORTS)
    ]
    for t in threads:
        t.start()

    try:
        time.sleep(args.duration)
    except KeyboardInterrupt:
        print("\n[main] Interrupted – stopping collection…")

    _stop.set()
    time.sleep(0.3)   # let threads flush final lines

    analyse_and_plot(args.out, args.label)


if __name__ == "__main__":
    main()
