#!/usr/bin/env python3
"""
Modbus Gateway Live Web Dashboard Server
Provides REST APIs and serves the interactive frontend at http://127.0.0.1:8080
"""

import os
import sys
import json
import time
import socket
import sqlite3
import subprocess
import webbrowser
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse, parse_qs

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = PROJECT_ROOT / "data" / "records.db"
WEB_DIR = PROJECT_ROOT / "web"
SLAVE_EXE = PROJECT_ROOT / ("slave_simulator.exe" if sys.platform == "win32" else "slave_simulator")

# Global handle for dynamic fault injection restarts
CURRENT_SLAVE_PROC = None

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
        global CURRENT_SLAVE_PROC
        fault_type = body.get("type", "normal")

        # Map to profile file
        profile_map = {
            "delay": "config/fault_profiles/delay.json",
            "crc": "config/fault_profiles/crc_error.json",
            "exception": "config/fault_profiles/exception.json",
            "disconnect": "config/fault_profiles/disconnect.json",
            "normal": None
        }

        profile_rel = profile_map.get(fault_type)
        profile_file = (PROJECT_ROOT / profile_rel) if profile_rel else None

        # Terminate any running slave simulator
        if sys.platform == "win32":
            subprocess.run(["taskkill", "/F", "/IM", "slave_simulator.exe"], capture_output=True)
        else:
            subprocess.run(["pkill", "-f", "slave_simulator"], capture_output=True)
        time.sleep(0.4)

        # Launch slave with profile
        cmd = [str(SLAVE_EXE), "-p", "5020", "-u", "1"]
        if profile_file and profile_file.exists():
            cmd.extend(["-f", str(profile_file)])

        CURRENT_SLAVE_PROC = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.3)

        msg_map = {
            "delay": "已注入弱网延时场景 (50~400ms 随机波动)",
            "crc": "已注入 CRC 校验位损坏 (20% 概率触发校验重试)",
            "exception": "已注入从站忙异常码 (0x06 异常响应)",
            "disconnect": "已注入物理断线 (每 25 次请求断开 3000ms)",
            "normal": "已恢复标准无故障运行模式"
        }
        return {"status": "OK", "message": msg_map.get(fault_type, "已生效")}

    def handle_manual_write(self, body):
        addr = int(body.get("address", 0))
        val = int(body.get("value", 0))

        # Send Modbus TCP Write Single Register (Func 06) frame directly
        # MBAP: Tx(2B) + Proto(00 00) + Len(00 06) + Unit(01) + Func(06) + Addr(2B) + Val(2B)
        frame = bytearray([
            0x00, 0x01, # Tx 1
            0x00, 0x00, # Proto 0
            0x00, 0x06, # Len 6
            0x01,       # Unit 1
            0x06,       # Func 06 Write Single Register
            (addr >> 8) & 0xFF, addr & 0xFF,
            (val >> 8) & 0xFF, val & 0xFF
        ])

        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1.0)
            s.connect(("127.0.0.1", 5020))
            s.sendall(frame)
            resp = s.recv(1024)
            s.close()
            return {"status": "OK", "address": addr, "value": val, "respLen": len(resp)}
        except Exception as e:
            return {"status": "ERROR", "message": str(e)}

class ReusableHTTPServer(HTTPServer):
    allow_reuse_address = True

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Modbus Industrial Gateway Web Dashboard")
    parser.add_argument("-p", "--port", type=int, default=8081, help="Port to listen on (default: 8081)")
    args = parser.parse_args()

    port = args.port
    server_address = ("0.0.0.0", port)
    httpd = ReusableHTTPServer(server_address, DashboardHandler)

    print("================================================================")
    print(f" Modbus Industrial Gateway Web Dashboard listening on port {port}")
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
