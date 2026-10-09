# -*- coding: utf-8 -*-
"""
用 pymodbus 作为第三方参考客户端，交叉验证自研从站模拟器（阶段 3 验收要求）。

为什么必须用第三方工具：
    实现指导里写得很直白 ——「模拟器自己实现了错的字节序，把主站也带偏，
    所以一定要用第三方工具交叉验证」。自研主站与自研从站的闭环测试只能证明
    「两边对规范的理解一致」，如果两边一起理解错了，测试还是全绿。
    pymodbus 是独立实现，它读出来的值对了，才说明字节序真的是对的。

用法：
    python verify_simulator.py [host] [port]
"""
import sys
import struct

# 注意：pymodbus 3.15 起统一用 device_id（旧版本叫 slave）。
from pymodbus.client import ModbusTcpClient

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5020

# 与 config/slave-simulator.json 中的初值一致
EXPECT_HOLDING = {0: 100, 1: 250, 2: 380, 3: 7, 4: 65535, 10: 1234, 11: 5678, 100: 42}
EXPECT_INPUT = {0: 220, 1: 50, 2: 3}
EXPECT_COILS = {0: True, 1: False, 2: True, 10: True}
EXPECT_DISCRETE = {0: True, 3: True}

checks = 0
failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if ok:
        print(f"  [OK]   {what}")
    else:
        failures += 1
        print(f"  [FAIL] {what}")


def raw_request(host, port, pdu, unit_id=1, transaction_id=0x0001, timeout=2.0):
    """绕过 pymodbus 的客户端校验，直接发一条原始 Modbus TCP 帧。

    为什么需要它：pymodbus 会在客户端就拒绝 count>125 这类非法请求，
    于是我们永远测不到「从站收到非法请求会怎么回」。
    而现场真正的坏主站是会发出这种帧的，从站的处理方式必须被验证。

    返回响应 PDU（功能码之后的部分），超时返回 None。
    """
    import socket

    mbap = struct.pack(">HHHB", transaction_id, 0, len(pdu) + 1, unit_id)
    frame = mbap + pdu
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        try:
            sock.connect((host, port))
            sock.sendall(frame)
            header = sock.recv(7)
            if len(header) < 7:
                return None
            length = struct.unpack(">H", header[4:6])[0]
            body = b""
            while len(body) < length - 1:
                chunk = sock.recv(length - 1 - len(body))
                if not chunk:
                    break
                body += chunk
            return body
        except (socket.timeout, ConnectionResetError, OSError):
            return None


def main():
    client = ModbusTcpClient(HOST, port=PORT, timeout=3)
    if not client.connect():
        print(f"无法连接到 {HOST}:{PORT}，请先启动 slave_simulator")
        return 2
    print(f"已连接 {HOST}:{PORT}\n")

    # ---------------------------------------------------------- 读保持寄存器
    print("─ 读保持寄存器（0x03）")
    rr = client.read_holding_registers(address=0, count=12, device_id=1)
    if rr.isError():
        check(False, f"读保持寄存器返回错误：{rr}")
    else:
        for addr in range(12):
            expected = EXPECT_HOLDING.get(addr, 0)
            check(rr.registers[addr] == expected,
                  f"HR[{addr}] 期望 {expected} 实际 {rr.registers[addr]}")
    # 单独验 100 号（跨越了默认一批读的范围）
    rr = client.read_holding_registers(address=100, count=1, device_id=1)
    check(not rr.isError() and rr.registers[0] == 42, "HR[100] 期望 42")
    # 65535 是「全 1」，字节序写反会变成 65535 以外的值
    rr = client.read_holding_registers(address=4, count=1, device_id=1)
    check(not rr.isError() and rr.registers[0] == 65535, "HR[4] 期望 65535（边界全 1）")

    # ---------------------------------------------------------- 读输入寄存器
    print("\n─ 读输入寄存器（0x04）")
    rr = client.read_input_registers(address=0, count=3, device_id=1)
    if rr.isError():
        check(False, f"读输入寄存器返回错误：{rr}")
    else:
        for addr in range(3):
            check(rr.registers[addr] == EXPECT_INPUT[addr],
                  f"IR[{addr}] 期望 {EXPECT_INPUT[addr]} 实际 {rr.registers[addr]}")

    # --------------------------------------------------------------- 读线圈
    print("\n─ 读线圈（0x01）")
    rr = client.read_coils(address=0, count=12, device_id=1)
    if rr.isError():
        check(False, f"读线圈返回错误：{rr}")
    else:
        for addr in range(12):
            expected = EXPECT_COILS.get(addr, False)
            check(bool(rr.bits[addr]) == expected,
                  f"COIL[{addr}] 期望 {expected} 实际 {bool(rr.bits[addr])}")

    # ------------------------------------------------------------ 读离散输入
    print("\n─ 读离散输入（0x02）")
    rr = client.read_discrete_inputs(address=0, count=8, device_id=1)
    if rr.isError():
        check(False, f"读离散输入返回错误：{rr}")
    else:
        for addr in range(8):
            expected = EXPECT_DISCRETE.get(addr, False)
            check(bool(rr.bits[addr]) == expected,
                  f"DI[{addr}] 期望 {expected} 实际 {bool(rr.bits[addr])}")

    # ------------------------------------------------------------ 写单个寄存器
    print("\n─ 写单个寄存器（0x06）")
    wr = client.write_register(address=20, value=4321, device_id=1)
    check(not wr.isError(), f"写 HR[20] = 4321 应成功（{wr}）")
    rr = client.read_holding_registers(address=20, count=1, device_id=1)
    check(not rr.isError() and rr.registers[0] == 4321, "回读 HR[20] 期望 4321")

    # ------------------------------------------------------------ 写多个寄存器
    print("\n─ 写多个寄存器（0x10）")
    wr = client.write_registers(address=30, values=[11, 22, 33, 44], device_id=1)
    check(not wr.isError(), f"写 HR[30..33] 应成功（{wr}）")
    rr = client.read_holding_registers(address=30, count=4, device_id=1)
    check(not rr.isError() and rr.registers == [11, 22, 33, 44],
          f"回读期望 [11, 22, 33, 44] 实际 {rr.registers if not rr.isError() else rr}")

    # -------------------------------------------------------------- 写单个线圈
    print("\n─ 写单个线圈（0x05）")
    wr = client.write_coil(address=5, value=True, device_id=1)
    check(not wr.isError(), f"写 COIL[5] = ON 应成功（{wr}）")
    rr = client.read_coils(address=5, count=1, device_id=1)
    check(not rr.isError() and bool(rr.bits[0]) is True, "回读 COIL[5] 应为 True")
    wr = client.write_coil(address=5, value=False, device_id=1)
    check(not wr.isError(), f"写 COIL[5] = OFF 应成功（{wr}）")
    rr = client.read_coils(address=5, count=1, device_id=1)
    check(not rr.isError() and bool(rr.bits[0]) is False, "回读 COIL[5] 应为 False")

    # ------------------------------------------------------------ 写多个线圈
    print("\n─ 写多个线圈（0x0F）")
    wr = client.write_coils(address=40, values=[True, False, True, True], device_id=1)
    check(not wr.isError(), f"写 COIL[40..43] 应成功（{wr}）")
    rr = client.read_coils(address=40, count=4, device_id=1)
    check(not rr.isError() and [bool(b) for b in rr.bits[:4]] == [True, False, True, True],
          f"回读期望 [T,F,T,T] 实际 {[bool(b) for b in rr.bits[:4]] if not rr.isError() else rr}")

    # --------------------------------------------------------------- 异常路径
    print("\n─ 异常路径（应返回异常码，不是成功也不是超时）")
    # 地址越界：holding size = 128，读 200 起 1 个
    rr = client.read_holding_registers(address=200, count=1, device_id=1)
    check(rr.isError(), "读越界地址应返回异常")
    if rr.isError():
        # pymodbus 的字符串形如 ExceptionResponse(..., function_code=131, exception_code=2)。
        # 直接读 exception_code 属性比匹配字符串可靠。
        code = getattr(rr, "exception_code", None)
        check(code == 2, f"异常码应是 0x02（非法地址），实际 {code}（{rr}）")
        check(getattr(rr, "function_code", None) == 0x83,
              "异常响应的功能码应是 0x83（0x03 | 0x80）")

    # 数量超规范上限：读 126 个（>125）。
    # 注意 pymodbus 客户端自己就会拒绝 count>125 的请求（抛 ValueError），
    # 这本身说明规范上限是共识。为了验证**从站**也会拒绝，这里直接发原始帧。
    print("\n─ 从站侧数量上限（绕过客户端校验，直接发原始帧）")
    raw = raw_request(HOST, PORT, pdu=bytes([0x03, 0x00, 0x00, 0x00, 0x7E]))  # 读 126 个
    check(raw is not None, "原始帧应收到响应")
    if raw is not None:
        check(raw[:2] == bytes([0x83, 0x03]),
              f"从站应回 0x83 0x03（非法数据值），实际 {raw.hex(' ').upper()}")

    # 单元号不匹配：本从站是 1 号，请求 9 号 -> 不应答 -> 超时
    print("\n─ 单元号过滤（请求 9 号从站，本模拟器是 1 号，应超时）")
    # 用原始帧更干净：直接确认「从站一个字都没回」。
    raw = raw_request(HOST, PORT, pdu=bytes([0x03, 0x00, 0x00, 0x00, 0x01]), unit_id=9)
    check(raw is None, "单元号不匹配时从站应保持沉默（无任何响应）")
    # 再用 pymodbus 走一遍：不匹配时它重试后抛超时，这也是「没有响应」的另一种表现。
    try:
        rr = client.read_holding_registers(address=0, count=1, device_id=9)
        check(rr.isError(), "单元号不匹配应无有效响应")
    except Exception as exc:  # noqa: BLE001 - 任何形式的「没拿到响应」都算通过
        check("No response" in str(exc) or "timeout" in str(exc).lower(),
              f"单元号不匹配应表现为超时，实际：{type(exc).__name__}")

    client.close()

    print(f"\n{'='*60}")
    print(f"断言 {checks} 项，失败 {failures} 项")
    print("结果：全部通过" if failures == 0 else "结果：失败")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
