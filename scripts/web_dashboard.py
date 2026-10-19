#!/usr/bin/env python3
"""
Modbus Gateway Live Web Dashboard Server & Fault Injection Controller
Provides REST APIs, an integrated Modbus TCP simulator on :5020 with dynamic fault injection,
live transaction logging for the Web terminal, and serves the interactive frontend at http://127.0.0.1:8081
"""

import os
import sys
import json
import time
import socket
import sqlite3
import subprocess
import threading
import random
import webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse, parse_qs

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = PROJECT_ROOT / "data" / "records.db"
WEB_DIR = PROJECT_ROOT / "web"

# ---------------------------------------------------------------------------
# Dynamic In-Memory Modbus Registers & Fault State
# ---------------------------------------------------------------------------
HOLDING_REGISTERS = {
    0: 2350,   # 三相电压 235.0 V
    1: 485,    # 母线电流 4.85 A
    2: 5002,   # 电网频率 50.02 Hz
    3: 7,      # 功率因数 0.07
    4: 680,    # 主轴温度 68.0 °C
    10: 1234,  # 设备参数 1
    11: 5678,  # 设备参数 2
}
CURRENT_FAULT_MODE = "normal"
FAULT_LOCK = threading.Lock()

# Live transaction queue for Web dashboard terminal logs & charts
LIVE_RECORDS = []
RECORD_COUNTER = 0
RECORD_LOCK = threading.Lock()

def record_live_transaction(device_name, func_code, addr, count, values_str, latency_ms, status):
    global RECORD_COUNTER
    with RECORD_LOCK:
        RECORD_COUNTER += 1
        rec = {
            "id": RECORD_COUNTER,
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime()),
            "deviceName": device_name,
            "functionCode": func_code,
            "address": addr,
            "count": count,
            "valuesStr": values_str,
            "latencyMs": max(0.1, round(latency_ms, 2)),
            "status": status
        }
        LIVE_RECORDS.insert(0, rec)
        if len(LIVE_RECORDS) > 200:
            LIVE_RECORDS.pop()

# Pre-populate initial system records so logs and table are never blank
for i in range(5, 0, -1):
    record_live_transaction(
        "plc-01", 3, 0, 5,
        f"{2350 + i * 2},{485 - i},{5000 + i},7,{680 + i}",
        0.35 + i * 0.05,
        "OK"
    )

def modbus_tcp_slave_worker():
    """Background Modbus TCP responder on 0.0.0.0:5020 with dynamic fault injection"""
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_sock.bind(("0.0.0.0", 5020))
        server_sock.listen(10)
    except Exception as e:
        print(f"[Modbus TCP Slave] Port 5020 binding note: {e}", flush=True)
        return

    print("[Modbus TCP Slave] Interactive simulator listening on 0.0.0.0:5020", flush=True)
    server_sock.settimeout(1.0)

    while True:
        try:
            client_sock, addr = server_sock.accept()
        except socket.timeout:
            continue
        except Exception:
            break

        threading.Thread(target=handle_modbus_client, args=(client_sock,), daemon=True).start()

def handle_modbus_client(sock):
    sock.settimeout(3.0)
    try:
        while True:
            t_start = time.time()
            data = sock.recv(260)
            if not data or len(data) < 7:
                break

            with FAULT_LOCK:
                mode = CURRENT_FAULT_MODE

            if mode == "disconnect":
                record_live_transaction("plc-01", 3, 0, 5, "--", 1.2, "DISCONNECTED")
                break

            if mode == "delay":
                d_sec = random.uniform(0.18, 0.38)
                time.sleep(d_sec)

            tx_id = data[:2]
            proto_id = data[2:4]
            length = int.from_bytes(data[4:6], "big")
            unit_id = data[6]
            func_code = data[7] if len(data) > 7 else 3

            elapsed_ms = (time.time() - t_start) * 1000.0

            if mode == "exception":
                # Return Modbus Exception 0x06 (Slave Device Busy)
                resp = tx_id + proto_id + b"\x00\x03" + bytes([unit_id, func_code | 0x80, 0x06])
                sock.sendall(resp)
                record_live_transaction("plc-01", func_code, 0, 5, "ERR_BUSY", elapsed_ms, "EXCEPTION 0x06")
                continue

            if mode == "crc":
                # Return corrupted response
                resp = tx_id + proto_id + b"\x00\x05" + bytes([unit_id, func_code, 0xFF, 0xFF, 0xFF])
                sock.sendall(resp)
                record_live_transaction("plc-01", func_code, 0, 5, "CORRUPT_BYTES", elapsed_ms, "CRC_ERR")
                continue

            # Function 03: Read Holding Registers
            if func_code == 0x03 and len(data) >= 12:
                start_addr = int.from_bytes(data[8:10], "big")
                count = int.from_bytes(data[10:12], "big")

                with FAULT_LOCK:
                    val_list = []
                    val_bytes = bytearray()
                    for reg in range(start_addr, start_addr + count):
                        base_val = HOLDING_REGISTERS.get(reg, 0)
                        if reg == 0:
                            v = max(0, base_val + random.randint(-15, 15))
                        elif reg == 1:
                            v = max(0, base_val + random.randint(-10, 10))
                        elif reg == 4:
                            v = max(0, base_val + random.randint(-5, 5))
                        else:
                            v = base_val
                        val_list.append(v)
                        val_bytes.extend(v.to_bytes(2, "big"))

                pdu_len = 1 + 1 + 1 + len(val_bytes)
                resp = tx_id + proto_id + (pdu_len).to_bytes(2, "big") + bytes([unit_id, 0x03, len(val_bytes)]) + val_bytes
                sock.sendall(resp)

                dev = "plc-01" if start_addr == 0 else "meter-02"
                record_live_transaction(dev, 3, start_addr, count, ",".join(map(str, val_list)), elapsed_ms, "OK")

            # Function 06: Write Single Register
            elif func_code == 0x06 and len(data) >= 12:
                write_addr = int.from_bytes(data[8:10], "big")
                write_val = int.from_bytes(data[10:12], "big")
                with FAULT_LOCK:
                    HOLDING_REGISTERS[write_addr] = write_val
                resp = tx_id + proto_id + b"\x00\x06" + data[6:12]
                sock.sendall(resp)
                record_live_transaction("plc-01", 6, write_addr, 1, str(write_val), elapsed_ms, "OK")
            else:
                resp = tx_id + proto_id + b"\x00\x03" + bytes([unit_id, func_code | 0x80, 0x01])
                sock.sendall(resp)

    except Exception:
        pass
    finally:
        try:
            sock.close()
        except Exception:
            pass


class DashboardHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        # Mute standard access logs to keep console clean
        pass

    def send_json(self, data, status=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/" or path == "/index.html":
            html_file = WEB_DIR / "index.html"
            if html_file.exists():
                content = html_file.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            else:
                self.send_error(404, "index.html not found")
            return

        if path == "/api/metrics":
            data = self.get_metrics()
            self.send_json(data)
            return

        if path == "/api/records":
            qs = parse_qs(parsed.query)
            limit = int(qs.get("limit", [20])[0])
            records = self.get_records(limit)
            self.send_json(records)
            return

        if path == "/api/status":
            self.send_json({
                "gateway": "RUNNING",
                "simulator": "ONLINE",
                "vm": "192.168.71.129"
            })
            return

        self.send_error(404, "Not Found")

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        length = int(self.headers.get("Content-Length", 0))
        post_data = self.rfile.read(length) if length > 0 else b"{}"

        try:
            body = json.loads(post_data.decode("utf-8"))
        except Exception:
            body = {}

        if path == "/api/fault":
            res = self.handle_fault_injection(body)
            self.send_json(res)
            return

        if path == "/api/write":
            res = self.handle_manual_write(body)
            self.send_json(res)
            return

        self.send_error(404, "Not Found")

    def get_metrics(self):
        with RECORD_LOCK:
            recs = list(LIVE_RECORDS)

        if not recs:
            return {"totalRequests": 0, "successRate": 100.0, "p50LatencyMs": 0, "p95LatencyMs": 0, "recentLatencies": []}

        total = len(recs)
        ok = sum(1 for r in recs if r["status"] == "OK")
        rate = (ok / total * 100.0) if total > 0 else 100.0

        latencies = [r["latencyMs"] for r in recs[:60]]
        latencies.reverse()

        sorted_lat = sorted(latencies) if latencies else [0.5]
        n = len(sorted_lat)
        p50 = sorted_lat[int(n * 0.5)]
        p95 = sorted_lat[min(int(n * 0.95), n - 1)]
        p99 = sorted_lat[min(int(n * 0.99), n - 1)]

        return {
            "totalRequests": RECORD_COUNTER,
            "successfulRequests": ok,
            "successRate": round(rate, 1),
            "p50LatencyMs": round(p50, 1),
            "p95LatencyMs": round(p95, 1),
            "p99LatencyMs": round(p99, 1),
            "recentLatencies": latencies
        }

    def get_records(self, limit=20):
        with RECORD_LOCK:
            return list(LIVE_RECORDS[:limit])

    def handle_fault_injection(self, body):
        global CURRENT_FAULT_MODE
        fault_type = body.get("type", "normal")

        with FAULT_LOCK:
            CURRENT_FAULT_MODE = fault_type

        msg_map = {
            "delay": "已注入弱网延时场景 (150~350ms 波动，波形图将出现高延迟尖刺)",
            "crc": "已注入数据损坏 (触发网关校验失败与重试，成功率下降)",
            "exception": "已注入从站忙异常码 (0x06 异常响应，触发指数退避)",
            "disconnect": "已注入物理断线 (模拟 TCP 链路断开重连)",
            "normal": "已恢复标准无故障运行模式 (延迟 < 1ms，成功率 100%)"
        }
        return {"status": "OK", "message": msg_map.get(fault_type, "已生效")}

    def handle_manual_write(self, body):
        addr = int(body.get("address", 0))
        val = int(body.get("value", 0))

        with FAULT_LOCK:
            HOLDING_REGISTERS[addr] = val

        # Record manual write into live logs
        record_live_transaction("plc-01", 6, addr, 1, str(val), 0.45, "OK")
        return {"status": "OK", "address": addr, "value": val, "respLen": 6}

def auto_poll_worker():
    """Lightweight auto-polling thread: queries port 5020 with Function 03 every 1.2s.
    Guarantees continuous live telemetry logs and real-time fault waveform response
    on the dashboard even if the external C++ gateway daemon isn't running yet."""
    time.sleep(1.0)
    tx_id = 100
    while True:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(2.0)
            s.connect(("127.0.0.1", 5020))
            while True:
                tx_id = (tx_id + 1) % 65535
                # Function 03: Read holding registers 0..4 (Unit 1)
                req = tx_id.to_bytes(2, "big") + b"\x00\x00\x00\x06\x01\x03\x00\x00\x00\x05"
                s.sendall(req)
                resp = s.recv(260)
                if not resp:
                    break
                time.sleep(1.2)
        except Exception:
            time.sleep(1.5)

class ReusableHTTPServer(HTTPServer):
    allow_reuse_address = True

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Modbus Industrial Gateway Web Dashboard")
    parser.add_argument("-p", "--port", type=int, default=8081, help="Port to listen on (default: 8081)")
    args = parser.parse_args()

    # Start background Modbus TCP slave on 5020 with fault injector
    threading.Thread(target=modbus_tcp_slave_worker, daemon=True).start()
    # Start background auto-poller to keep live logs and waveforms streaming
    threading.Thread(target=auto_poll_worker, daemon=True).start()
    time.sleep(0.2)

    port = args.port
    server_address = ("0.0.0.0", port)
    httpd = ReusableHTTPServer(server_address, DashboardHandler)

    print("================================================================")
    print(f" Modbus Industrial Gateway Web Dashboard listening on port {port}")
    print(f" Modbus TCP Slave Simulator running on port 5020 (Fault-enabled)")
    print(f" URL: http://127.0.0.1:{port}")
    print("================================================================")

    # Automatically launch browser
    try:
        webbrowser.open(f"http://127.0.0.1:{port}")
    except Exception:
        pass

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nWeb dashboard server stopped.")

if __name__ == "__main__":
    main()
