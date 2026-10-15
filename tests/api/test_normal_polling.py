import sqlite3
import time
import pytest

def test_records_are_written(running_system):
    """Verifies that gateway polls the simulator and records data into SQLite."""
    time.sleep(1.5)
    db_path = str(running_system["db"])
    assert db_path is not None

    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()

    # Query record count
    cursor.execute("SELECT COUNT(*) FROM records WHERE status = 'OK'")
    count = cursor.fetchone()[0]
    assert count >= 3, f"Expected at least 3 successful records, got {count}"

    # Query last record content
    cursor.execute("SELECT device_name, function_code, values_str, latency_ms FROM records WHERE status = 'OK' ORDER BY timestamp DESC LIMIT 1")
    row = cursor.fetchone()
    assert row is not None
    dev_name, func_code, values_str, latency_ms = row

    assert dev_name == "test-plc"
    assert func_code == 3
    assert "235" in values_str # Default voltage register 0
    assert latency_ms >= 0.0
    conn.close()

def test_sqlite_pragmas_and_indexes(running_system):
    """Verifies that database schema has WAL mode and expected composite indexes."""
    time.sleep(0.5)
    conn = sqlite3.connect(str(running_system["db"]))
    cursor = conn.cursor()

    cursor.execute("PRAGMA journal_mode")
    journal_mode = cursor.fetchone()[0].upper()
    assert journal_mode == "WAL", f"Expected WAL mode, got {journal_mode}"

    cursor.execute("SELECT name FROM sqlite_master WHERE type = 'index'")
    indexes = [r[0] for r in cursor.fetchall()]
    assert "idx_records_dev_time" in indexes
    assert "idx_records_time" in indexes
    conn.close()
