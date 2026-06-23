#!/usr/bin/env python3
import time
from pathlib import Path

import pymodbus
from pymodbus.client import ModbusSerialClient


GPIO_NUM = 496
GPIO_PATH = Path(f"/sys/class/gpio/gpio{GPIO_NUM}")
POWER_BOARD_PORT = "/dev/ttyS0"
POWER_BOARD_BAUDRATE = 9600
POWER_BOARD_SLAVE_ID = 1
POWER_REGISTER = 0x10
POWER_BITS = range(5)
POWER_BIT_MASK = sum(1 << bit_index for bit_index in POWER_BITS)
POWER_WRITE_ATTEMPTS = 3
POWER_WRITE_RETRY_DELAY_SEC = 0.2


def set_gpio_value(value: int) -> bool:
    value_path = GPIO_PATH / "value"
    try:
        value_path.write_text(f"{value}\n", encoding="ascii")
        print(f"gpio {GPIO_NUM} set to {value}")
        return True
    except Exception as exc:
        print(f"warning: failed to set gpio {GPIO_NUM} to {value}: {exc}")
        return False


def open_modbus_client() -> ModbusSerialClient:
    return ModbusSerialClient(
        port=POWER_BOARD_PORT,
        baudrate=POWER_BOARD_BAUDRATE,
        bytesize=8,
        parity="N",
        stopbits=1,
        timeout=1,
    )


def read_power_register(client: ModbusSerialClient) -> int | None:
    read_result = client.read_holding_registers(
        POWER_REGISTER,
        count=1,
        device_id=POWER_BOARD_SLAVE_ID,
    )
    if read_result.isError():
        print(f"warning: failed to read power register: {read_result}")
        return None
    return int(read_result.registers[0])


def write_power_bits(value: int) -> bool:
    client = open_modbus_client()
    try:
        if not client.connect():
            print(f"warning: failed to open {POWER_BOARD_PORT}")
            return False

        current = read_power_register(client)
        if current is None:
            return False

        if value:
            new_value = current | POWER_BIT_MASK
        else:
            new_value = current & ~POWER_BIT_MASK

        write_result = client.write_register(
            POWER_REGISTER,
            new_value,
            device_id=POWER_BOARD_SLAVE_ID,
        )
        if write_result.isError():
            print(f"warning: failed to write power register: {write_result}")
            return False

        time.sleep(0.1)
        verify = read_power_register(client)
        if verify is None:
            return False
        if (verify & POWER_BIT_MASK) != (new_value & POWER_BIT_MASK):
            print(
                "warning: power register verify mismatch: "
                f"expected bits 0x{new_value & POWER_BIT_MASK:04X}, "
                f"got 0x{verify & POWER_BIT_MASK:04X}, "
                f"register=0x{verify:04X}"
            )
            return False

        print(
            f"power bits mask=0x{POWER_BIT_MASK:04X} -> {value}, "
            f"register 0x{current:04X} -> 0x{new_value:04X}, "
            f"verify=0x{verify:04X}"
        )
        return True
    except Exception as exc:
        print(f"warning: power bits restore failed: {exc}")
        return False
    finally:
        client.close()


def set_power_bits(value: int) -> bool:
    for attempt in range(1, POWER_WRITE_ATTEMPTS + 1):
        if write_power_bits(value):
            return True
        if attempt < POWER_WRITE_ATTEMPTS:
            print(
                f"warning: retry power bits value={value} "
                f"attempt={attempt + 1}/{POWER_WRITE_ATTEMPTS}"
            )
            time.sleep(POWER_WRITE_RETRY_DELAY_SEC)
    return False


def main() -> int:
    print(f"pymodbus version: {pymodbus.__version__}")
    ok = True

    ok = set_gpio_value(1) and ok
    time.sleep(0.5)
    ok = set_gpio_value(1) and ok
    ok = set_power_bits(0) and ok
    time.sleep(5)

    ok = set_gpio_value(0) and ok
    time.sleep(1)
    ok = set_power_bits(1) and ok

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
