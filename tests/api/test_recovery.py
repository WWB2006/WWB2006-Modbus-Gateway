import time
import sqlite3
import subprocess
import pytest
from pathlib import Path

def test_gateway_recovers_after_slave_restart(running_system, project_root):
    """Kills the slave simulator and verifies gateway automatically recovers after restart."""
    db_path = str(running_system["db"])
    time.sleep(1.0)

    conn = sqlite3.connect(db_path)
    count_before = conn.execute("SELECT COUNT(*) FROM records WHERE status = 'OK'").fetchone()[0]
    conn.close()
    assert count_before > 0

    # Kill slave simulator
    running_system["slave"].terminate()
    try:
        running_system["slave"].wait(timeout=2)
    except Exception:
        running_system["slave"].kill()

    # Wait for gateway to observe disconnect and enter reconnect backoff
    time.sleep(2.5)

    # Restart slave simulator on port 5020
    slave_exe = project_root / "slave_simulator.exe"
    new_slave = subprocess.Popen([str(slave_exe), "-p", str(running_system["port"]), "-u", "1"])
    time.sleep(3.5) # Allow backoff period to trigger reconnection and polling

    conn = sqlite3.connect(db_path)
    count_after = conn.execute("SELECT COUNT(*) FROM records WHERE status = 'OK'").fetchone()[0]
    conn.close()

    new_slave.terminate()
    try:
        new_slave.wait(timeout=2)
    except Exception:
        new_slave.kill()

    assert count_after > count_before, f"Expected records to increase after recovery. Before={count_before}, After={count_after}"
