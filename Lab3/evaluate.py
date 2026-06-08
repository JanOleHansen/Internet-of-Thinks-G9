import argparse
import csv
import re
import threading
import time
import statistics
from collections import defaultdict

import serial

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


def analyse(label):
    reqs = {}
    acks = {}
    server_rx = {}
    server_resp = {}
    rtts = []

    for e in events:
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
            rtts.append(e["rtt_ms"])

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
            "rtt_ms": next((e["rtt_ms"] for e in events if e["event"] == "RTT" and e["mid"] == mid), ""),
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