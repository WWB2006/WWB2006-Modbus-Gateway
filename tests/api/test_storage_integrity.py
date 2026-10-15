import sqlite3
import time
import pytest

def test_storage_schema_integrity(running_system):
    """Verifies that all columns and types in the database schema match specifications."""
    time.sleep(1.0)
    conn = sqlite3.connect(str(running_system["db"]))
    cursor = conn.cursor()

    cursor.execute("PRAGMA table_info(records)")
    columns = {row[1]: row[2].upper() for row in cursor.fetchall()}

    expected_columns = {
        "id": "INTEGER",
        "timestamp": "INTEGER",
        "device_name": "TEXT",
        "unit_id": "INTEGER",
        "function_code": "INTEGER",
        "address": "INTEGER",
        "count": "INTEGER",
        "values_str": "TEXT",
        "latency_ms": "REAL",
        "status": "TEXT"
    }

    for col, col_type in expected_columns.items():
        assert col in columns, f"Column {col} missing from records table"
        assert columns[col] == col_type, f"Column {col} has type {columns[col]}, expected {col_type}"

    conn.close()

def test_timestamps_and_latency_validity(running_system):
    """Verifies that recorded timestamps are reasonable epoch ms and latency is positive."""
    time.sleep(1.0)
    conn = sqlite3.connect(str(running_system["db"]))
    cursor = conn.cursor()

    cursor.execute("SELECT timestamp, latency_ms FROM records WHERE status = 'OK' LIMIT 20")
    rows = cursor.fetchall()
    assert len(rows) > 0

    now_ms = int(time.time() * 1000)
    for ts, lat in rows:
        # Timestamp must be within the last hour
        assert abs(now_ms - ts) < 3600 * 1000
        assert lat >= 0.0

    conn.close()
