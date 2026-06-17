#!/usr/bin/env python3
import asyncio
from bleak import BleakClient, BleakScanner

ACT_NODE_NAME = "ACT_NODE"
COMMAND_CHAR_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"

NODE_ID = 3
MSG_TYPE_COMMAND = 2
COMMAND_LIGHT_ON = 1
COMMAND_LIGHT_OFF = 2
COMMAND_HEATING_ON = 3
COMMAND_HEATING_OFF = 4

BLINK_SECONDS = 5


def build_message(sequence_number: int, command: int) -> bytearray:
    message = bytearray()
    message.append(NODE_ID)
    message.append(MSG_TYPE_COMMAND)
    message += sequence_number.to_bytes(2, "little")
    message += command.to_bytes(2, "little")
    return message


async def main():
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


if __name__ == "__main__":
    asyncio.run(main())