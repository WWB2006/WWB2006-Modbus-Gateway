#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""阶段 10：端到端验收脚本（模拟器 → 常驻网关 → SQLite → CSV）。

为什么要有这个脚本：
    单元测试各自只证明「自己那一层对」。真正会出问题的是**层与层之间**：
    配置解析出来的端口有没有真的被用上、链路收发的字节有没有真的落进数据库、
    CSV 导出的列顺序跟导入能不能对上。这些问题在单层测试里全是绿的。
    所以这里真起两个进程、走真实 socket、最后用独立的 sqlite3 模块去查库 ——
    「不依赖被测代码自己的说法」是这条链路上唯一的可信证据。

三层校验，缺一不可：
    1. 进程层：模拟器与网关都能起来、网关能优雅退出（退出码 0）；
    2. 数据层：库里真的有样本，且**值**与从站配置一致（不是"有行就算过"）；
    3. 导出层：CSV 能被重新解析，列数与表头一致。

用法：
    python scripts/e2e_gateway.py [--build-dir build_portable] [--keep]

    --build-dir  存放 slave_simulator / gateway_daemon 的目录（默认 build_portable）
    --keep       保留临时目录，便于失败后手工排查
"""

import argparse
import json
import os
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

# 与 config/slave-simulator.json 的初值保持一致。
# 端到端要断言的正是「从站里的这个值，经过链路与存储之后，在库里还是这个值」。
EXPECTED_HOLDING = {0: 100, 1: 250, 2: 380, 3: 7}

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
    return ok


def find_free_port():
    """让操作系统分配一个空闲端口。

    不用写死端口：CI 上并行跑多个 job 时写死的端口会互相撞，
    表现为「本地能过、CI 偶发失败」这类最难查的问题。
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(host, port, timeout=10.0):
    """轮询直到端口可连接。返回是否成功。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.3)
            if sock.connect_ex((host, port)) == 0:
                return True
        time.sleep(0.1)
    return False


def executable(build_dir, name):
    """Windows 上是 name.exe，类 Unix 上是 name。自动兼容 build 根目录与 build/src 子目录。"""
    candidates = [
        os.path.join(build_dir, name + ".exe"),
        os.path.join(build_dir, name),
        os.path.join(build_dir, "src", name + ".exe"),
        os.path.join(build_dir, "src", name),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build_portable")
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build_dir = os.path.join(repo_root, args.build_dir)

    simulator_bin = executable(build_dir, "slave_simulator")
    daemon_bin = executable(build_dir, "gateway_daemon")

    print("=" * 66)
    print("阶段 10：端到端验收（模拟器 → 常驻网关 → SQLite → CSV）")
    print("=" * 66)

    if not check(simulator_bin is not None, f"找到 slave_simulator（{build_dir}）"):
        print("  提示：先运行 ./scripts/build_portable_tests.sh")
        return 1
    if not check(daemon_bin is not None, f"找到 gateway_daemon（{build_dir}）"):
        print("  提示：先运行 ./scripts/build_portable_tests.sh")
        return 1

    work_dir = tempfile.mkdtemp(prefix="gateway_e2e_")
    port = find_free_port()
    db_path = os.path.join(work_dir, "e2e.db")
    csv_path = os.path.join(work_dir, "e2e.csv")
    config_path = os.path.join(work_dir, "gateway.json")

    print(f"\n临时目录 : {work_dir}")
    print(f"模拟器端口: {port}")

    # ------------------------------------------------------------------
    # 1. 写一份专用配置：单设备、TCP、100ms 轮询、读 HR[0..3]。
    #    轮询间隔取 100ms 是为了让 3 秒的采集窗口能拿到足够样本，
    #    同时不至于把测试跑成性能测试。
    # ------------------------------------------------------------------
    gateway_config = {
        "devices": [
            {
                "name": "e2e-plc",
                "transport": "tcp",
                "host": "127.0.0.1",
                "port": port,
                "unitId": 1,
                "pollIntervalMs": 100,
                "timeoutMs": 800,
                "retry": 2,
                "points": [{"func": 3, "address": 0, "count": 4}],
            }
        ],
        "storage": {"database": db_path, "batchSize": 16},
        "logging": {"level": "info", "rotateSizeMb": 8},
    }
    with open(config_path, "w", encoding="utf-8") as handle:
        json.dump(gateway_config, handle, ensure_ascii=False, indent=2)

    simulator_proc = None
    daemon_proc = None
    try:
        # --------------------------------------------------------------
        # 2. 起从站模拟器
        # --------------------------------------------------------------
        simulator_log = open(os.path.join(work_dir, "simulator.log"), "w", encoding="utf-8")
        simulator_cmd = [
            simulator_bin,
            os.path.join(repo_root, "config", "slave-simulator.json"),
            "--port",
            str(port),
            "--bind",
            "127.0.0.1",
            "--unit",
            "1",
        ]
        print(f"\n启动模拟器 : {' '.join(simulator_cmd)}")
        simulator_proc = subprocess.Popen(
            simulator_cmd, cwd=repo_root, stdout=simulator_log, stderr=subprocess.STDOUT
        )

        if not check(wait_for_port("127.0.0.1", port), "模拟器端口在 10 秒内可连接"):
            print("  模拟器日志：")
            simulator_log.flush()
            with open(os.path.join(work_dir, "simulator.log"), encoding="utf-8") as handle:
                print(handle.read()[-2000:])
            return 1

        # --------------------------------------------------------------
        # 3. 起常驻网关，采 3 秒后自动退出
        # --------------------------------------------------------------
        daemon_log_path = os.path.join(work_dir, "daemon.log")
        daemon_log = open(daemon_log_path, "w", encoding="utf-8")
        daemon_cmd = [
            daemon_bin,
            config_path,
            "--seconds",
            "3",
            "--quiet",
            "--export-csv",
            csv_path,
        ]
        print(f"启动网关   : {' '.join(daemon_cmd)}")
        daemon_proc = subprocess.Popen(
            daemon_cmd, cwd=repo_root, stdout=daemon_log, stderr=subprocess.STDOUT
        )

        try:
            exit_code = daemon_proc.wait(timeout=40)
        except subprocess.TimeoutExpired:
            daemon_proc.kill()
            daemon_proc.wait()
            exit_code = -1
        daemon_log.flush()
        daemon_log.close()

        with open(daemon_log_path, encoding="utf-8", errors="replace") as handle:
            daemon_output = handle.read()

        check(exit_code == 0, f"网关优雅退出，退出码 {exit_code}")
        check("已运行 3 秒，按计划退出" in daemon_output, "网关按 --seconds 计划退出")
        check("正在落盘剩余数据" in daemon_output, "退出前执行了落盘")

        # --------------------------------------------------------------
        # 4. 数据层校验：用独立的 sqlite3 模块查库
        #    刻意不调用被测代码的查询接口 —— 那等于让被考的人自己批卷。
        # --------------------------------------------------------------
        print("\n─ 数据层校验（直接用 sqlite3 模块查库）")
        if not check(os.path.isfile(db_path), f"数据库文件已生成（{os.path.basename(db_path)}）"):
            print(daemon_output[-3000:])
            return 1

        connection = sqlite3.connect(db_path)
        cursor = connection.cursor()
        cursor.execute("SELECT COUNT(*) FROM samples")
        total_rows = cursor.fetchone()[0]
        check(total_rows > 0, f"库里有样本（{total_rows} 行）")

        # 表结构：确认主键与索引真的建出来了。
        cursor.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='samples'")
        check(cursor.fetchone() is not None, "samples 表存在")

        cursor.execute("PRAGMA table_info(samples)")
        columns = {row[1] for row in cursor.fetchall()}
        expected_columns = {"device", "ts_ms", "address", "value", "function", "valid", "note"}
        check(expected_columns <= columns, f"samples 表含全部期望列（实际 {sorted(columns)}）")

        cursor.execute("PRAGMA index_list(samples)")
        index_names = {row[1] for row in cursor.fetchall()}
        check(
            "idx_samples_device_ts" in index_names,
            f"按设备+时间的索引存在（实际 {sorted(index_names)}）",
        )

        # 值的校验：这才是端到端的重点。
        # 「有行」很容易满足（哪怕是全 0 的垃圾数据）；「值等于从站里的值」才能证明
        # 请求组帧、链路收发、响应解析、存储写入这一整条链路都是对的。
        cursor.execute(
            "SELECT DISTINCT address, value FROM samples WHERE device='e2e-plc' AND valid=1"
        )
        observed = {}
        for address, value in cursor.fetchall():
            # 同一个地址可能因为不同时间点采到多次；这里只关心值是否正确。
            observed.setdefault(address, set()).add(value)

        for address, expected in sorted(EXPECTED_HOLDING.items()):
            actual = observed.get(address)
            check(
                actual == {expected},
                f"HR[{address}] 库里读到的值应为 {expected}，实际 {actual}",
            )

        # 设备名与功能码：确认存进去的不是别的东西。
        cursor.execute("SELECT DISTINCT device FROM samples")
        devices = {row[0] for row in cursor.fetchall()}
        check(devices == {"e2e-plc"}, f"库里只有配置里的那台设备（实际 {sorted(devices)}）")

        cursor.execute("SELECT DISTINCT function FROM samples")
        functions = {row[0] for row in cursor.fetchall()}
        check(functions == {3}, f"功能码都是 0x03（实际 {sorted(functions)}）")

        # 时间戳单调：同一次运行的样本，时间戳应覆盖 3 秒窗口。
        cursor.execute("SELECT MIN(ts_ms), MAX(ts_ms) FROM samples")
        low, high = cursor.fetchone()
        check(high > low, "时间戳有跨度（不是同一毫秒的重复写入）")

        connection.close()

        # --------------------------------------------------------------
        # 5. 导出层校验：CSV 能被重新解析，列数与表头一致
        # --------------------------------------------------------------
        print("\n─ 导出层校验（CSV）")
        if check(os.path.isfile(csv_path), "CSV 文件已导出"):
            with open(csv_path, encoding="utf-8") as handle:
                lines = [line.rstrip("\n") for line in handle if line.strip()]
            check(len(lines) >= 2, f"CSV 有表头与数据（{len(lines)} 行）")
            if lines:
                header = lines[0].split(",")
                check(
                    header == ["timestamp_ms", "device", "address", "value", "function", "valid", "note"],
                    f"CSV 表头正确（实际 {header}）",
                )
                data_rows = lines[1:]
                check(
                    all(len(row.split(",")) == len(header) for row in data_rows),
                    "CSV 每行列数与表头一致",
                )
                check(
                    len(data_rows) == total_rows,
                    f"CSV 行数与库里行数一致（CSV {len(data_rows)} / 库 {total_rows}）",
                )

        # --------------------------------------------------------------
        # 6. 优雅退出的证据：库里不应有半截数据
        # --------------------------------------------------------------
        print("\n─ 落盘完整性")
        check("数据库共" in daemon_output, "退出时打印了总条数")

    finally:
        for proc in (daemon_proc, simulator_proc):
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.wait()

        if args.keep:
            print(f"\n临时目录已保留：{work_dir}")
        else:
            shutil.rmtree(work_dir, ignore_errors=True)

    print("\n" + "=" * 66)
    print(f"断言 {checks} 项，失败 {failures} 项")
    print("结果：全部通过" if failures == 0 else "结果：失败")
    print("=" * 66)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
