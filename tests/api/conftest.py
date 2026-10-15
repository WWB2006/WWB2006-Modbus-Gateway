import os
import sys
import time
import sqlite3
import subprocess
import pytest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
SLAVE_EXE = PROJECT_ROOT / ("slave_simulator.exe" if sys.platform == "win32" else "slave_simulator")
CLI_EXE = PROJECT_ROOT / ("gateway_cli.exe" if sys.platform == "win32" else "gateway_cli")

@pytest.fixture(scope="session")
def project_root():
    return PROJECT_ROOT

@pytest.fixture
def running_simulator():
    """Starts a clean standalone SlaveSimulator on port 5020."""
    port = 5020
    proc = subprocess.Popen([str(SLAVE_EXE), "-p", str(port), "-u", "1"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.6) # Allow socket to bind
    yield {"port": port, "proc": proc}
    try:
        proc.terminate()
        proc.wait(timeout=2)
    except Exception:
        proc.kill()

@pytest.fixture
def running_system(tmp_path, running_simulator):
    """Starts both SlaveSimulator and Gateway CLI, pointing to a temporary test DB."""
    test_db = tmp_path / "test_records.db"
    cli_proc = subprocess.Popen([
        str(CLI_EXE),
        "-c", str(PROJECT_ROOT / "config" / "gateway_test.json"),
        "--db", str(test_db),
        "--metrics-interval", "1"
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    time.sleep(1.0) # Wait for initial polling cycles
    yield {
        "db": test_db,
        "slave": running_simulator["proc"],
        "cli": cli_proc,
        "port": running_simulator["port"]
    }

    try:
        cli_proc.terminate()
        cli_proc.wait(timeout=2)
    except Exception:
        cli_proc.kill()
