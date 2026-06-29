#!/usr/bin/env python3
"""
Manual test script for ACT_NODE.
Connects directly to the actuator, sends commands, and prints ACKs and heartbeats.
Run with: python3 test_act_node.py
"""
import asyncio
from bleak import BleakClient, BleakScanner

ACT_NODE_NAME     = "ACT_NODE"
COMMAND_CHAR_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
ACK_CHAR_UUID     = "0000fff2-0000-1000-8000-00805f9b34fb"
HEARTBEAT_CHAR_UUID = "0000fff3-0000-1000-8000-00805f9b34fb"

NODE_ID          = 3
MSG_TYPE_COMMAND = 2
MSG_TYPE_ACK     = 3
MSG_TYPE_HEARTBEAT = 4

COMMAND_LIGHT_ON   = 1
COMMAND_LIGHT_OFF  = 2
COMMAND_HEATING_ON = 3
COMMAND_HEATING_OFF = 4

seq_num = 0

def build_command(command: int) -> bytearray:
    global seq_num
    seq_num += 1
    msg = bytearray()
    msg.append(NODE_ID)
    msg.append(MSG_TYPE_COMMAND)
    msg += seq_num.to_bytes(2, "little")
    msg += command.to_bytes(2, "little")
    return msg

def on_notification(sender, data):
    if len(data) < 2:
        return
    msg_type = data[1]
    seq = int.from_bytes(data[2:4], "little")
    if msg_type == MSG_TYPE_ACK:
        acked_seq = int.from_bytes(data[4:6], "little")
        print(f"  [ACK]       seq={seq}, acked_seq={acked_seq}")
    elif msg_type == MSG_TYPE_HEARTBEAT:
        print(f"  [HEARTBEAT] seq={seq}")
    else:
        print(f"  [UNKNOWN]   msg_type={msg_type}, data={data.hex()}")

async def send(client, command, label):
    msg = build_command(command)
    print(f"Sending {label} (seq={seq_num}, bytes={msg.hex()})...")
    await client.write_gatt_char(COMMAND_CHAR_UUID, msg, response=True)
    await asyncio.sleep(0.5)  # wait for ACK notification

async def main():
    print(f"Scanning for '{ACT_NODE_NAME}'...")
    device = await BleakScanner.find_device_by_name(ACT_NODE_NAME, timeout=10.0)
    if device is None:
        print("ACT_NODE not found. Is it powered on and advertising?")
        return

    print(f"Found {device.address} — connecting...")
    async with BleakClient(device) as client:
        print("Connected.\n")

        await client.start_notify(ACK_CHAR_UUID, on_notification)
        await client.start_notify(HEARTBEAT_CHAR_UUID, on_notification)
        print("Subscribed to ACK and heartbeat notifications.\n")
        print("Waiting 2s for first heartbeat...")
        await asyncio.sleep(2)

        print("\n--- Test 1: LIGHT ON ---")
        await send(client, COMMAND_LIGHT_ON, "LIGHT_ON")

        print("\n--- Test 2: HEATING ON ---")
        await send(client, COMMAND_HEATING_ON, "HEATING_ON")

        print("\nWaiting 3s (LED should be blinking, servo at 90°)...")
        await asyncio.sleep(3)

        print("\n--- Test 3: LIGHT OFF ---")
        await send(client, COMMAND_LIGHT_OFF, "LIGHT_OFF")

        print("\n--- Test 4: HEATING OFF ---")
        await send(client, COMMAND_HEATING_OFF, "HEATING_OFF")

        print("\nWaiting 3s (LED should be off, servo at -90°)...")
        await asyncio.sleep(3)

        print("\nAll tests done.")

if __name__ == "__main__":
    asyncio.run(main())
