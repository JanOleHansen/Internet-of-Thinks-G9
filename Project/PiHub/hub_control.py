#!/usr/bin/env python3
import asyncio
from rule_engine import *
from bleak import BleakClient, BleakScanner
from datetime import *
from state import system_state
ACT_NODE_NAME = "ACT_NODE"
COMMAND_CHAR_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"
ACK_CHAR_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"
ENV_NODE_NAME = "ENV_NODE"
SENSOR_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef1"
HEARTBEAT_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef2"

WIN_NODE_NAME = "WIN_NODE"

NODE_ID = 3
MSG_TYPE_SENSOR = 1
MSG_TYPE_COMMAND = 2
MSG_TYPE_ACK = 3
MSG_TYPE_HEARTBEAT = 4
MSG_TYPE_ERROR = 5

COMMAND_LIGHT_ON = 1
COMMAND_LIGHT_OFF = 2
COMMAND_HEATING_ON = 3
COMMAND_HEATING_OFF = 4

TEMP_SENSOR = 1
HUM_SENSOR = 2
LIGHT_SENSOR = 3
MOTION_SENSOR = 4
WINDOW_SENSOR = 5

BLINK_SECONDS = 5

#light_on = None
# Time since light was on and the last motion in the room was detected
light_on_time = None

'''# Other global sensor data
curr_temp = None
curr_hum = None
curr_window = None
curr_motion = None'''

seq_num = 0
pending_acks = {}
ACK_TIMEOUT = 2.0
MAX_RETRIES = 3

# Sending commands
def build_message(sequence_number: int, command: int) -> bytearray:
    message = bytearray()
    message.append(NODE_ID)
    message.append(MSG_TYPE_COMMAND)
    message += sequence_number.to_bytes(2, "little")
    message += command.to_bytes(2, "little")
    return message

# Parse ACK Packets
def parse_ack_packet(data: bytearray):
    node_id = data[0]
    msg_type = data[1]
    seq = int.from_bytes(data[2:4], "little")

    acked_seq = int.from_bytes(data[4:6], "little")
    status = data[6]

    return node_id, msg_type, seq, acked_seq, status

# Function to parse incoming data from ACT_NODE
def handle_ack_notification(sender, data):
    node_id, msg_type, seq, acked_seq, status = parse_ack_packet(data)

    if msg_type != MSG_TYPE_ACK:
        return

    print(f"ACK received: acked_seq={acked_seq}, status={status}")

    future = pending_acks.get(acked_seq)

    if future and not future.done():
        future.set_result(status)

# Sending commands and search for associated ACKs
async def send_command_with_retry(client: BleakClient, command: int) -> bool:
    for attempt in range(1, MAX_RETRIES + 1):
        global seq_num
        seq_num += 1

        loop = asyncio.get_running_loop()
        ack_future = loop.create_future()
        pending_acks[seq_num] = ack_future

        print(f"Sending command={command}, seq={seq_num}, attempt={attempt}")

        await client.write_gatt_char(
            COMMAND_CHAR_UUID,
            build_message(seq_num, command),
            response=True
        )

        try:
            status = await asyncio.wait_for(ack_future, timeout=ACK_TIMEOUT)

            pending_acks.pop(seq_num, None)

        except asyncio.TimeoutError:
            pending_acks.pop(seq_num, None)
            print(f"No ACK for seq={seq_num}, retrying...")

    print(f"Command failed after {MAX_RETRIES} attempts")
    return False

# Function to parse incoming sensor packet
def parse_sensor_message(data: bytearray):
    node_id = data[0]
    msg_type = data[1]
    seq = int.from_bytes(data[2:4], "little")

    sensor_type = int.from_bytes(data[4:6], "little")
    value1 = int.from_bytes(data[6:8], "little")
    value2 = int.from_bytes(data[8:10], "little")

    return node_id, msg_type, seq, sensor_type, value1, value2

# Function to handle incoming notifications from ENV_NODE
def handle_env_notification(sender, data):
    node_id, msg_type, seq, sensor_type, value1, value2 = parse_sensor_message(data)
    if node_id != 1:
        print("Didn't received expected node_id")
        return
    environment = system_state["environment"]
    if msg_type == MSG_TYPE_SENSOR:
        global light_on_time
        if sensor_type == TEMP_SENSOR:
            temp = str(value1) + "." + str(value2)
            environment["temperature"] = float(temp)
        elif sensor_type == HUM_SENSOR:
            humidity = str(value1) + "." + str(value2)
            environment["humidity"] = float(humidity)
        elif sensor_type == LIGHT_SENSOR:
            if value1 == 0:
                light = "on"
                if not environment["light"]: # Light was not on before, so start timer
                    environment["light"] = True 
                    light_on_time = datetime.now(None) 
            else:
                light = "off"
                environment["light"] = False
        elif sensor_type == MOTION_SENSOR:
            if value1 == 1:
                motion = "yes"
                if environment["light"]: # Update time because there is still motion in the room
                    light_on_time = datetime.now(None)
                environment["motion"] = True
            else:
                motion = "no"
                environment["motion"] = False
        else:
            print("Didn't received expected msg_type")
    elif msg_type == MSG_TYPE_HEARTBEAT:
        environment["last_seen"] = datetime.now(None).isoformat()
        

# Function to handle incoming notifications from WIN_NODE
def handle_window_notification(sender, data):
    node_id, msg_type, seq, sensor_type, value1, value2 = parse_sensor_message(data)
    if node_id != 2:
        print("Didn't received expected node_id")
        return
    window = system_state["window"]
    if msg_type == MSG_TYPE_SENSOR:
        if value1 == 1:
            window_state = "open"
            window["state"] = "open"
        else:
            window_state = "close"
            window["state"] = "close"
    elif msg_type == MSG_TYPE_HEARTBEAT:
        window["last_seen"] = datetime.now(None).isoformat()

async def maintain_node(name, notification_handler):
    while True:
        try:
            print(f"Scanning for '{name}'...")
            device = await BleakScanner.find_device_by_name(name, timeout=10.0)

            if device is None:
                print(f"{name} not found; retrying...")
                await asyncio.sleep(5)
                continue

            disconnected = asyncio.Event()

            def on_disconnect(_client):
                print(f"{name} disconnected")
                disconnected.set()

            async with BleakClient(device, disconnected_callback=on_disconnect) as client:
                print(f"{name} connected")
                await client.start_notify(SENSOR_CHAR_UUID,notification_handler)
                await client.start_notify(HEARTBEAT_CHAR_UUID,notification_handler)

                # Let the connection open
                await disconnected.wait()

        except asyncio.CancelledError:
            raise
        except Exception as exc:
            print(f"{name}: {exc}")

        await asyncio.sleep(3)

async def evaluate_rules():
    global light_on_time
    environment = system_state["environment"]
    window = system_state["window"]
    while True:
        if all(v is not None for v in
               (window["state"], environment["humidity"], environment["motion"], 
                environment["temperature"], environment["light"], light_on_time)):
            print("Window CMD:", checkWindow(window["state"], environment["temperature"], 
                                             environment["humidity"]))
            print("Heating CMD:", checkHeating(environment["temperature"], window["state"]))
            print("Light CMD:", checkLight(
                environment["light"], environment["motion"], light_on_time, datetime.now()
            ))

        await asyncio.sleep(1)

async def main():
    # Actuator Node
    print(f"Scanning for '{ACT_NODE_NAME}'...")
    device = await BleakScanner.find_device_by_name(ACT_NODE_NAME, timeout=10.0)
    if device is None:
        print(f"No device named '{ACT_NODE_NAME}' found")
        return

    print(f"Connecting to {device.address}...")
    async with BleakClient(device.address) as client:
        print("Connected")

        await client.write_gatt_char(COMMAND_CHAR_UUID, build_message(1, COMMAND_LIGHT_ON), response=True)
        print("Light ON")
        await client.write_gatt_char(COMMAND_CHAR_UUID, build_message(1, COMMAND_HEATING_ON), response=True)
        print("Heating ON")

        await asyncio.sleep(BLINK_SECONDS)

        await client.write_gatt_char(COMMAND_CHAR_UUID, build_message(2, COMMAND_LIGHT_OFF), response=True)
        print("Light OFF")
        await client.write_gatt_char(COMMAND_CHAR_UUID, build_message(2, COMMAND_HEATING_OFF), response=True)
        print("Heating OFF")

    # Environment Node
    env_task = asyncio.create_task(
        maintain_node(
            ENV_NODE_NAME,
            handle_env_notification
        )
    )

    # Window Node
    window_task = asyncio.create_task(
        maintain_node(
            WIN_NODE_NAME,
            handle_window_notification
        )
    )

    await asyncio.gather(env_task, window_task, evaluate_rules())
    
if __name__ == "__main__":
    asyncio.run(main())