import time
import sqlite3
import subprocess
import pytest
from pathlib import Path

def test_delay_fault_injection(tmp_path, project_root):
    """Verifies that injecting delay into slave increases recorded latency."""
    port = 5025
    delay_profile = project_root / "config" / "fault_profiles" / "delay.json"
    slave_exe = project_root / "slave_simulator.exe"
    cli_exe = project_root / "gateway_cli.exe"

    slave_proc = subprocess.Popen([
        str(slave_exe), "-p", str(port), "-u", "1", "-f", str(delay_profile)
    ])
    time.sleep(0.5)

    test_db = tmp_path / "delay_test.db"

    # Temporary config pointing to port 5025
    cfg_content = f'''{{
      "devices": [
        {{
          "name": "delay-plc",
          "transport": "tcp",
          "host": "127.0.0.1",
          "port": {port},
          "unitId": 1,
          "pollIntervalMs": 100,
          "timeoutMs": 1500,
          "retry": 3,
          "points": [ {{ "func": 3, "address": 0, "count": 2 }} ]
        }}
      ],
      "storage": {{ "database": "{test_db.as_posix()}", "batchSize": 1 }},
      "logging": {{ "level": "info", "file": "logs/delay_test.log" }}
    }}'''

    cfg_file = tmp_path / "delay_cfg.json"
    cfg_file.write_text(cfg_content)

    cli_proc = subprocess.Popen([str(cli_exe), "-c", str(cfg_file), "--db", str(test_db)])
    time.sleep(2.0)

    cli_proc.terminate()
    slave_proc.terminate()
    cli_proc.wait(timeout=2)
    slave_proc.wait(timeout=2)

    conn = sqlite3.connect(str(test_db))
    cursor = conn.cursor()
    cursor.execute("SELECT MAX(latency_ms) FROM records WHERE status = 'OK'")
    max_lat = cursor.fetchone()[0]
    conn.close()

    assert max_lat is not None
    assert max_lat >= 40.0, f"Expected injected latency >= 40ms, got {max_lat}ms"

def test_exception_fault_injection(tmp_path, project_root):
    """Verifies that injecting exception code 6 into slave records EXCEPTION_6 status."""
    port = 5026
    ex_profile = project_root / "config" / "fault_profiles" / "exception.json"
    slave_exe = project_root / "slave_simulator.exe"
    cli_exe = project_root / "gateway_cli.exe"

    slave_proc = subprocess.Popen([
        str(slave_exe), "-p", str(port), "-u", "1", "-f", str(ex_profile)
    ])
    time.sleep(0.5)

    test_db = tmp_path / "ex_test.db"
    cfg_content = f'''{{
      "devices": [
        {{
          "name": "ex-plc",
          "transport": "tcp",
          "host": "127.0.0.1",
          "port": {port},
          "unitId": 1,
          "pollIntervalMs": 80,
          "timeoutMs": 1000,
          "retry": 3,
          "points": [ {{ "func": 3, "address": 0, "count": 2 }} ]
        }}
      ],
      "storage": {{ "database": "{test_db.as_posix()}", "batchSize": 1 }},
      "logging": {{ "level": "info", "file": "logs/ex_test.log" }}
    }}'''

    cfg_file = tmp_path / "ex_cfg.json"
    cfg_file.write_text(cfg_content)

    cli_proc = subprocess.Popen([str(cli_exe), "-c", str(cfg_file), "--db", str(test_db)])
    time.sleep(2.0)

    cli_proc.terminate()
    slave_proc.terminate()
    cli_proc.wait(timeout=2)
    slave_proc.wait(timeout=2)

    conn = sqlite3.connect(str(test_db))
    cursor = conn.cursor()
    cursor.execute("SELECT COUNT(*) FROM records WHERE status LIKE 'EXCEPTION_%'")
    ex_count = cursor.fetchone()[0]
    conn.close()

    assert ex_count > 0, "Expected at least one injected Modbus exception record"
