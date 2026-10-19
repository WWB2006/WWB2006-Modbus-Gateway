#!/usr/bin/env python3
"""
Modbus Gateway Live Web Dashboard Server & Fault Injection Controller
Provides REST APIs, an integrated Modbus TCP simulator on :5020 with dynamic fault injection,
and serves the interactive frontend at http://127.0.0.1:8081
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
            data = sock.recv(260)
            if not data or len(data) < 7:
                break

            with FAULT_LOCK:
                mode = CURRENT_FAULT_MODE

            if mode == "disconnect":
                # Simulate physical wire disconnection
                break

            if mode == "delay":
                # Simulate weak network high latency (150ms ~ 350ms)
                time.sleep(random.uniform(0.15, 0.35))

            tx_id = data[:2]
            proto_id = data[2:4]
            length = int.from_bytes(data[4:6], "big")
            unit_id = data[6]
            func_code = data[7] if len(data) > 7 else 3

            if mode == "exception":
                # Return Modbus Exception 0x06 (Slave Device Busy)
                resp = tx_id + proto_id + b"\x00\x03" + bytes([unit_id, func_code | 0x80, 0x06])
                sock.sendall(resp)
                continue

            if mode == "crc":
                # Corrupt packet payload
                resp = tx_id + proto_id + b"\x00\x05" + bytes([unit_id, func_code, 0xFF, 0xFF, 0xFF])
                sock.sendall(resp)
                continue

            # Function 03: Read Holding Registers
            if func_code == 0x03 and len(data) >= 12:
                start_addr = int.from_bytes(data[8:10], "big")
                count = int.from_bytes(data[10:12], "big")

                with FAULT_LOCK:
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
                        val_bytes.extend(v.to_bytes(2, "big"))

                pdu_len = 1 + 1 + len(val_bytes)
                resp = tx_id + proto_id + (pdu_len).to_bytes(2, "big") + bytes([unit_id, 0x03, len(val_bytes)]) + val_bytes
                sock.sendall(resp)

            # Function 06: Write Single Register
            elif func_code == 0x06 and len(data) >= 12:
                write_addr = int.from_bytes(data[8:10], "big")
                write_val = int.from_bytes(data[10:12], "big")
                with FAULT_LOCK:
                    HOLDING_REGISTERS[write_addr] = write_val
                # Echo request as response
                resp = tx_id + proto_id + b"\x00\x06" + data[6:12]
                sock.sendall(resp)
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
        if not DB_PATH.exists():
            return {"totalRequests": 0, "successRate": 100.0, "p50LatencyMs": 0, "p95LatencyMs": 0, "recentLatencies": []}

        try:
            conn = sqlite3.connect(str(DB_PATH), timeout=1.0)
            cursor = conn.cursor()

            cursor.execute("SELECT COUNT(*), SUM(CASE WHEN status = 'OK' THEN 1 ELSE 0 END) FROM records")
            row = cursor.fetchone()
            total = row[0] or 0
            ok = row[1] or 0
            rate = (ok / total * 100.0) if total > 0 else 100.0

            # Get recent 60 latencies for chart
            cursor.execute("SELECT latency_ms FROM records WHERE status = 'OK' ORDER BY id DESC LIMIT 60")
            recent = [r[0] for r in cursor.fetchall()]
            recent.reverse()

            sorted_lat = sorted(recent) if recent else [0]
            n = len(sorted_lat)
            p50 = sorted_lat[int(n * 0.5)]
            p95 = sorted_lat[min(int(n * 0.95), n - 1)]
            p99 = sorted_lat[min(int(n * 0.99), n - 1)]

            conn.close()
            return {
                "totalRequests": total,
                "successfulRequests": ok,
                "successRate": rate,
                "p50LatencyMs": p50,
                "p95LatencyMs": p95,
                "p99LatencyMs": p99,
                "recentLatencies": recent
            }
        except Exception as e:
            return {"totalRequests": 0, "successRate": 100.0, "error": str(e), "recentLatencies": []}

    def get_records(self, limit=20):
        if not DB_PATH.exists():
            return []
        try:
            conn = sqlite3.connect(str(DB_PATH), timeout=1.0)
            cursor = conn.cursor()
            cursor.execute(
                "SELECT id, timestamp, device_name, function_code, address, count, values_str, latency_ms, status "
                "FROM records ORDER BY id DESC LIMIT ?", (limit,)
            )
            rows = cursor.fetchall()
            conn.close()

            results = []
            for r in rows:
                results.append({
                    "id": r[0],
                    "timestamp": r[1],
                    "deviceName": r[2],
                    "functionCode": r[3],
                    "address": r[4],
                    "count": r[5],
                    "valuesStr": r[6],
                    "latencyMs": r[7],
                    "status": r[8]
                })
            return results
        except Exception:
            return []

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

        return {"status": "OK", "address": addr, "value": val, "respLen": 6}

class ReusableHTTPServer(HTTPServer):
    allow_reuse_address = True

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Modbus Industrial Gateway Web Dashboard")
    parser.add_argument("-p", "--port", type=int, default=8081, help="Port to listen on (default: 8081)")
    args = parser.parse_args()

    # Start background Modbus TCP slave on 5020 with fault injector
    threading.Thread(target=modbus_tcp_slave_worker, daemon=True).start()
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
