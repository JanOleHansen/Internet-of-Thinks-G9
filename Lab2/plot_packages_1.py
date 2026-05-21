import socket
import threading
import time
import re
import csv
import argparse
from collections import defaultdict
import matplotlib.pyplot as plt

HOST = "127.0.0.1"

PORTS = {
    "node1": 60001,
    "node2": 60002,
    "node3": 60003,
    "node4": 60004,
}

TX_RE = re.compile(r"TX type=(\d+) seq=(\d+) ttl=(\d+)")
RX_RE = re.compile(r"RX type=(\d+) seq=(\d+) ttl=(\d+)")

running = True
events = []
lock = threading.Lock()


def read_terminal(node_name, port):
    global running

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.connect((HOST, port))
        print(f"Connected to {node_name} on port {port}")

        buffer = ""

        while running:
            try:
                data = sock.recv(1024)
            except OSError:
                break

            if not data:
                break

            buffer += data.decode(errors="ignore")

            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                timestamp = time.time()

                tx = TX_RE.search(line)
                rx = RX_RE.search(line)

                if tx:
                    msg_type, seq, ttl = map(int, tx.groups())
                    with lock:
                        events.append({
                            "time": timestamp,
                            "node": node_name,
                            "direction": "TX",
                            "type": msg_type,
                            "seq": seq,
                            "ttl": ttl,
                            "line": line,
                        })
                    print(f"{node_name} TX type={msg_type} seq={seq} ttl={ttl}")

                elif rx:
                    msg_type, seq, ttl = map(int, rx.groups())
                    with lock:
                        events.append({
                            "time": timestamp,
                            "node": node_name,
                            "direction": "RX",
                            "type": msg_type,
                            "seq": seq,
                            "ttl": ttl,
                            "line": line,
                        })
                    print(f"{node_name} RX type={msg_type} seq={seq} ttl={ttl}")


def analyse(label):
    tx_events = defaultdict(list)
    rx_events = defaultdict(list)

    for e in events:
        key = (e["type"], e["seq"])

        if e["direction"] == "TX":
            tx_events[key].append(e)
        else:
            rx_events[key].append(e)

    rows = []

    for key, txs in tx_events.items():
        msg_type, seq = key
        first_tx = min(txs, key=lambda x: x["time"])
        rx_nodes = {e["node"]: e for e in rx_events.get(key, [])}

        row = {
            "type": msg_type,
            "seq": seq,
            "tx_node": first_tx["node"],
            "tx_time": first_tx["time"],
        }

        for node in PORTS:
            if node in rx_nodes:
                latency = rx_nodes[node]["time"] - first_tx["time"]
                row[f"{node}_received"] = 1
                row[f"{node}_latency_s"] = latency
            else:
                row[f"{node}_received"] = 0
                row[f"{node}_latency_s"] = ""

        rows.append(row)

    csv_name = f"evaluation_{label}.csv"

    fieldnames = [
        "type", "seq", "tx_node", "tx_time",
    ]

    for node in PORTS:
        fieldnames += [f"{node}_received", f"{node}_latency_s"]

    with open(csv_name, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print(f"\nSaved CSV: {csv_name}")

    print("\n--- Reliability per node ---")
    total_packets = len(rows)

    for node in PORTS:
        received = sum(1 for r in rows if r[f"{node}_received"] == 1)
        reliability = received / total_packets * 100 if total_packets else 0
        print(f"{node}: {received}/{total_packets} = {reliability:.2f}%")

    print("\n--- Average latency per node ---")

    avg_latencies = {}

    for node in PORTS:
        latencies = [
            r[f"{node}_latency_s"]
            for r in rows
            if r[f"{node}_latency_s"] != ""
        ]

        if latencies:
            avg = sum(latencies) / len(latencies)
            avg_latencies[node] = avg * 1000
            print(f"{node}: {avg * 1000:.2f} ms")
        else:
            print(f"{node}: no packets received")

    if avg_latencies:
        plt.figure()
        plt.bar(avg_latencies.keys(), avg_latencies.values())
        plt.xlabel("Node")
        plt.ylabel("Average latency [ms]")
        plt.title(f"Average propagation latency - {label}")
        plt.grid(True)
        plt.savefig(f"latency_{label}.png")
        plt.show()

    reliability_values = {}

    for node in PORTS:
        received = sum(1 for r in rows if r[f"{node}_received"] == 1)
        reliability_values[node] = received / total_packets * 100 if total_packets else 0

    plt.figure()
    plt.bar(reliability_values.keys(), reliability_values.values())
    plt.xlabel("Node")
    plt.ylabel("Reliability [%]")
    plt.ylim(0, 105)
    plt.title(f"Reliability per node - {label}")
    plt.grid(True)
    plt.savefig(f"reliability_{label}.png")
    plt.show()


def main(duration, label):
    global running

    threads = []

    for node, port in PORTS.items():
        t = threading.Thread(target=read_terminal, args=(node, port), daemon=True)
        t.start()
        threads.append(t)

    print(f"Collecting data for {duration} seconds...")
    time.sleep(duration)

    running = False
    time.sleep(1)

    analyse(label)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--label", type=str, default="no_loss")
    args = parser.parse_args()

    main(args.duration, args.label)