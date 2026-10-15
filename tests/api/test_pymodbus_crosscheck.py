import pytest
import time
from pymodbus.client import ModbusTcpClient

def call_with_slave_id(func, *args, **kwargs):
    """Handles both device_id (pymodbus 3.15+) and slave (pymodbus < 3.15)."""
    try:
        return func(*args, device_id=1, **kwargs)
    except TypeError:
        return func(*args, slave=1, **kwargs)

def test_pymodbus_crosscheck_registers(running_simulator):
    """Cross-checks that pymodbus can read holding & input registers from SlaveSimulator."""
    port = running_simulator["port"]
    client = ModbusTcpClient("127.0.0.1", port=port)
    assert client.connect()

    # Read Holding Registers (Func 03)
    rr = call_with_slave_id(client.read_holding_registers, 0, count=5)
    assert not rr.isError()
    assert len(rr.registers) == 5
    # Default values initialized in SlaveSimulator:
    # reg 0 = 235 (23.5V), reg 1 = 485, reg 2 = 5002, reg 3 = 1140, reg 4 = 68
    assert rr.registers[0] == 235
    assert rr.registers[1] == 485
    assert rr.registers[2] == 5002
    assert rr.registers[3] == 1140
    assert rr.registers[4] == 68

    # Read Input Registers (Func 04)
    ir = call_with_slave_id(client.read_input_registers, 0, count=4)
    assert not ir.isError()
    assert len(ir.registers) == 4
    assert ir.registers[0] == 500
    assert ir.registers[1] == 501

    client.close()

def test_pymodbus_crosscheck_coils(running_simulator):
    """Cross-checks that pymodbus can read and write coils from SlaveSimulator."""
    port = running_simulator["port"]
    client = ModbusTcpClient("127.0.0.1", port=port)
    assert client.connect()

    # Read Coils (Func 01)
    rc = call_with_slave_id(client.read_coils, 0, count=3)
    assert not rc.isError()
    assert rc.bits[0] == True  # Coil 0 default: true
    assert rc.bits[1] == False # Coil 1 default: false
    assert rc.bits[2] == True  # Coil 2 default: true

    # Write Single Coil (Func 05)
    wc = call_with_slave_id(client.write_coil, 1, True)
    assert not wc.isError()

    # Read back to verify
    rc2 = call_with_slave_id(client.read_coils, 1, count=1)
    assert rc2.bits[0] == True

    client.close()

def test_pymodbus_crosscheck_write_registers(running_simulator):
    """Cross-checks single and multiple register write from pymodbus."""
    port = running_simulator["port"]
    client = ModbusTcpClient("127.0.0.1", port=port)
    assert client.connect()

    # Write Single Register (Func 06)
    wr = call_with_slave_id(client.write_register, 10, 9999)
    assert not wr.isError()

    # Read back
    rr = call_with_slave_id(client.read_holding_registers, 10, count=1)
    assert rr.registers[0] == 9999

    # Write Multiple Registers (Func 10)
    wmr = call_with_slave_id(client.write_registers, 20, [111, 222, 333])
    assert not wmr.isError()

    # Read back
    rr2 = call_with_slave_id(client.read_holding_registers, 20, count=3)
    assert rr2.registers == [111, 222, 333]

    client.close()

def test_pymodbus_illegal_address_exception(running_simulator):
    """Verifies that reading beyond the register boundary triggers Exception 0x02."""
    port = running_simulator["port"]
    client = ModbusTcpClient("127.0.0.1", port=port)
    assert client.connect()

    # Address 9999 + count 10 exceeds 10000 limit
    rr = call_with_slave_id(client.read_holding_registers, 9995, count=10)
    assert rr.isError()

    client.close()
