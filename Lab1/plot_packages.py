import socket
import threading
import time
import re
import csv
import matplotlib.pyplot as plt

HOST = "127.0.0.1"
ADVERTISER_PORT = 60001
OBSERVER_PORT = 60002

sent_packets = {}
received_packets = {}

sent_re = re.compile(r"Sending advertising data: Temperature (\d+) C")
recv_re = re.compile(r"The temperature is (\d+) C")

running = True


def read_socket(port, name, regex, storage):
    global running

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.connect((HOST, port))
        print(f"Connected to {name} on port {port}")

        buffer = ""

        while running:
            data = sock.recv(1024)
            if not data:
                break

            buffer += data.decode(errors="ignore")

            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()

                match = regex.search(line)
                if match:
                    packet_id = int(match.group(1))
                    timestamp = time.time()

                    if packet_id not in storage:
                        storage[packet_id] = timestamp

                    print(f"{name}: packet {packet_id} at {timestamp:.6f}")


def main(duration=60, label="run"):
    global running

    t1 = threading.Thread(
        target=read_socket,
        args=(ADVERTISER_PORT, "advertiser", sent_re, sent_packets),
        daemon=True,
    )

    t2 = threading.Thread(
        target=read_socket,
        args=(OBSERVER_PORT, "observer", recv_re, received_packets),
        daemon=True,
    )

    t1.start()
    t2.start()

    print(f"Collecting data for {duration} seconds...")
    time.sleep(duration)

    running = False

    sent_ids = sorted(sent_packets.keys())
    received_ids = sorted(received_packets.keys())

    matched_ids = sorted(set(sent_ids) & set(received_ids))
    lost_ids = sorted(set(sent_ids) - set(received_ids))

    reliability = len(matched_ids) / len(sent_ids) if sent_ids else 0

    latencies = []
    for packet_id in matched_ids:
        latency = received_packets[packet_id] - sent_packets[packet_id]
        latencies.append((packet_id, latency))

    print("\n--- Results ---")
    print(f"Sent packets:      {len(sent_ids)}")
    print(f"Received packets:  {len(received_ids)}")
    print(f"Matched packets:   {len(matched_ids)}")
    print(f"Lost packets:      {len(lost_ids)}")
    print(f"Reliability:       {reliability * 100:.2f} %")

    if latencies:
        avg_latency = sum(l for _, l in latencies) / len(latencies)
        print(f"Average latency:   {avg_latency * 1000:.2f} ms")
    else:
        print("Average latency:   no packets matched")

    csv_name = f"results_{label}.csv"

    with open(csv_name, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["packet_id", "sent_time", "received_time", "latency_s", "received"])

        for packet_id in sent_ids:
            sent_time = sent_packets[packet_id]
            received_time = received_packets.get(packet_id)

            if received_time is not None:
                latency = received_time - sent_time
                received = 1
            else:
                latency = ""
                received = 0

            writer.writerow([packet_id, sent_time, received_time, latency, received])

    print(f"Saved CSV: {csv_name}")

    # Plot 1: Latency
    if latencies:
        packet_ids = [p for p, _ in latencies]
        latency_ms = [l * 1000 for _, l in latencies]

        plt.figure()
        plt.plot(packet_ids, latency_ms, marker="o")
        plt.xlabel("Packet ID / Temperature")
        plt.ylabel("Latency [ms]")
        plt.title(f"Latency per received packet - {label}")
        plt.grid(True)
        plt.savefig(f"latency_{label}.png")
        plt.show()

    # Plot 2: Reliability over time
    reliability_x = []
    reliability_y = []

    received_count = 0

    for i, packet_id in enumerate(sent_ids, start=1):
        if packet_id in received_packets:
            received_count += 1

        reliability_x.append(packet_id)
        reliability_y.append(received_count / i * 100)

    plt.figure()
    plt.plot(reliability_x, reliability_y, marker="o")
    plt.xlabel("Packet ID / Temperature")
    plt.ylabel("Reliability [%]")
    plt.title(f"Reliability over time - {label}")
    plt.grid(True)
    plt.ylim(0, 105)
    plt.savefig(f"reliability_{label}.png")
    plt.show()


if __name__ == "__main__":
    main(duration=30, label="distance_10")