import argparse
import csv
import re
import threading
import time
import statistics

import serial
import matplotlib.pyplot as plt

REQ_RE = re.compile(r"REQ MID=(\d+)")
ACK_RE = re.compile(r"ACK MID=(\d+)")
RTT_RE = re.compile(r"RTT=(\d+) ms MID=(\d+)")
RX_RE = re.compile(r"RX MID=(\d+)")
RESP_RE = re.compile(r"RESP MID=(\d+)")

running = True
events = []
lock = threading.Lock()


def read_serial(name, port, baud):
    global running

    with serial.Serial(port, baud, timeout=1) as ser:
        print(f"Connected to {name} on {port}")

        while running:
            line = ser.readline().decode(errors="ignore").strip()
            if not line:
                continue

            ts = time.time()
            print(f"[{name}] {line}")

            patterns = [
                ("REQ", REQ_RE),
                ("ACK", ACK_RE),
                ("RTT", RTT_RE),
                ("RX", RX_RE),
                ("RESP", RESP_RE),
            ]

            for event_type, regex in patterns:
                m = regex.search(line)
                if not m:
                    continue

                mid = int(m.group(2) if event_type == "RTT" else m.group(1))
                rtt = int(m.group(1)) if event_type == "RTT" else None

                with lock:
                    events.append({
                        "time": ts,
                        "node": name,
                        "event": event_type,
                        "mid": mid,
                        "rtt_ms": rtt,
                        "line": line,
                    })


def save_latency_plot(rows, label):
    rtt_points = [
        (row["mid"], row["rtt_ms"])
        for row in rows
        if row["rtt_ms"] != ""
    ]

    if not rtt_points:
        print("No RTT values available for plotting.")
        return

    mids = [p[0] for p in rtt_points]
    rtts = [p[1] for p in rtt_points]

    plt.figure()
    plt.plot(mids, rtts, marker="o")
    plt.xlabel("Message ID")
    plt.ylabel("RTT [ms]")
    plt.title(f"CoAP RTT over time - {label}")
    plt.grid(True)
    plt.tight_layout()
    filename = f"coap_rtt_{label}.png"
    plt.savefig(filename)
    print(f"Saved plot: {filename}")


def save_loss_plot(sent, received_by_server, acked_by_client, label):
    if sent == 0:
        print("No requests available for loss plot.")
        return

    values = [
        100 * received_by_server / sent,
        100 * acked_by_client / sent,
    ]

    labels = [
        "Server received",
        "Client received ACK",
    ]

    plt.figure()
    plt.bar(labels, values)
    plt.ylabel("Success rate [%]")
    plt.ylim(0, 105)
    plt.title(f"CoAP reliability - {label}")
    plt.grid(True, axis="y")
    plt.tight_layout()
    filename = f"coap_reliability_{label}.png"
    plt.savefig(filename)
    print(f"Saved plot: {filename}")


def analyse(label):
    reqs = {}
    acks = {}
    server_rx = {}
    server_resp = {}
    rtts_by_mid = {}

    with lock:
        collected_events = list(events)

    for e in collected_events:
        mid = e["mid"]

        if e["event"] == "REQ":
            reqs[mid] = e
        elif e["event"] == "ACK":
            acks[mid] = e
        elif e["event"] == "RX":
            server_rx[mid] = e
        elif e["event"] == "RESP":
            server_resp[mid] = e
        elif e["event"] == "RTT":
            rtts_by_mid[mid] = e["rtt_ms"]

    rows = []

    for mid, req in sorted(reqs.items()):
        ack = acks.get(mid)
        rx = server_rx.get(mid)
        resp = server_resp.get(mid)

        rows.append({
            "mid": mid,
            "client_req_time": req["time"],
            "server_received": 1 if rx else 0,
            "server_response_sent": 1 if resp else 0,
            "client_ack_received": 1 if ack else 0,
            "rtt_ms": rtts_by_mid.get(mid, ""),
        })

    csv_name = f"coap_eval_{label}.csv"

    with open(csv_name, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "mid",
            "client_req_time",
            "server_received",
            "server_response_sent",
            "client_ack_received",
            "rtt_ms",
        ])
        writer.writeheader()
        writer.writerows(rows)

    sent = len(reqs)
    received_by_server = len(server_rx)
    acked_by_client = len(acks)
    rtts = [r for r in rtts_by_mid.values() if r is not None]

    print("\n========== RESULTS ==========")
    print(f"Requests sent by client: {sent}")
    print(f"Requests received by server: {received_by_server}")
    print(f"Responses received by client: {acked_by_client}")

    if sent:
        uplink_loss = 100 * (sent - received_by_server) / sent
        end_to_end_loss = 100 * (sent - acked_by_client) / sent

        print(f"Client -> Server loss: {uplink_loss:.2f}%")
        print(f"End-to-end CoAP loss: {end_to_end_loss:.2f}%")

    if rtts:
        print(f"Average RTT: {statistics.mean(rtts):.2f} ms")
        print(f"Min RTT: {min(rtts)} ms")
        print(f"Max RTT: {max(rtts)} ms")
        print(f"Median RTT: {statistics.median(rtts):.2f} ms")

    print(f"\nSaved CSV: {csv_name}")

    save_latency_plot(rows, label)
    save_loss_plot(sent, received_by_server, acked_by_client, label)


def main():
    global running

    parser = argparse.ArgumentParser()
    parser.add_argument("--client-port", required=True)
    parser.add_argument("--server-port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=int, default=120)
    parser.add_argument("--label", default="test")
    args = parser.parse_args()

    threads = [
        threading.Thread(target=read_serial, args=("client", args.client_port, args.baud), daemon=True),
        threading.Thread(target=read_serial, args=("server", args.server_port, args.baud), daemon=True),
    ]

    for t in threads:
        t.start()

    print(f"Collecting data for {args.duration} seconds...")
    time.sleep(args.duration)

    running = False
    time.sleep(1)

    analyse(args.label)


if __name__ == "__main__":
    main()