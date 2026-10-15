# 异常注入引擎与自动化测试指南 (Fault Injection & Testing Guide)

## 1. 为什么需要自研异常注入引擎？

在工业现场，上位机面临的最棘手问题往往并非“正常通信”，而是“偶发异常”：
1. **弱网与链路波动**：电磁干扰导致 CRC 损坏，通信延迟忽长忽短；
2. **设备瞬时过载**：从站忙碌返回 `0x06` 异常码；
3. **物理断线与设备重启**：网线松动、交换机断电导致长达数秒的连接丢失。

传统的现场调试通常依赖真实设备拔插网线或人为制造断电，成本极高且**不可控、不可重现**。本项目自研的异常注入引擎（`FaultInjector`）与从站模拟器（`SlaveSimulator`）完美解决了这一痛点，通过 JSON 配置文件即可精确按概率复现全部工业现场边缘场景。

---

## 2. 四类异常场景配置与实现机制

所有异常场景均在 `config/fault_profiles/` 目录下提供标准 JSON 模板：

### 场景一：通信响应延时波动 (Latency Jitter)
- **配置文件**：`config/fault_profiles/delay.json`
```json
{
  "name": "delay-fault",
  "delayMs": {
    "min": 50,
    "max": 400,
    "ratio": 0.3
  }
}
```
- **注入机制**：模拟器在收到请求后，以 `30%` 的概率触发延时，在 `[50ms, 400ms]` 均匀分布区间内休眠，检验网关超时配置与实时曲线渲染的动态表现。

### 场景二：数据链路 CRC 校验错误 (CRC Corruption)
- **配置文件**：`config/fault_profiles/crc_error.json`
```json
{
  "name": "crc-error-fault",
  "crcError": { "ratio": 0.20 }
}
```
- **注入机制**：针对 Modbus RTU 串口链路，模拟器以 `20%` 概率故意将帧尾最后一个字节取反 (`frame.back() ^= 0xFF`)，验证网关 `Crc16::verify()` 拦截能力与重试机制。

### 场景三：从站异常响应码 (Modbus Exception)
- **配置文件**：`config/fault_profiles/exception.json`
```json
{
  "name": "exception-fault",
  "exception": { "ratio": 0.25, "code": 6 }
}
```
- **注入机制**：模拟器以 `25%` 概率返回 `Func | 0x80` 及异常码 `0x06` (Slave Device Busy)，网关解析为异常响应并执行退避，而不判定为链路断开。

### 场景四：物理级断线与掉电 (Link Disconnect)
- **配置文件**：`config/fault_profiles/disconnect.json`
```json
{
  "name": "disconnect-fault",
  "disconnect": { "afterRequests": 25, "durationMs": 3000 }
}
```
- **注入机制**：模拟器每正常处理 25 次请求，主动断开 TCP 连接并拒绝响应达 3000ms，验证网关指数退避重连算法与自动续采能力。

---

## 3. 一键加载与复现验证命令

在无需编写代码的情况下，通过一条命令行即可启动带故障特征的仿真环境：

```powershell
# Windows
.\scripts\run_slave.ps1 -Port 5020 -Unit 1 -Profile "config/fault_profiles/composite.json"

# Linux
./scripts/run_slave.sh 5020 1 "config/fault_profiles/composite.json"
```

---

## 4. 自动化回归测试矩阵

本项目编写了超过 60 条测试断言，覆盖 C++ 单元级与端到端集成级：

```
tests/
|-- unit/ (C++ CTest 单元测试 - 纯协议与算法，秒级通过)
|   |-- tst_crc.cpp         (Modbus 多项式查表校验与边界检验)
|   |-- tst_codec.cpp       (8种常用功能码编解码与异常帧解析)
|   |-- tst_reconnect.cpp   (指数退避算法、随机抖动与重试上限截断)
|   |-- tst_storage.cpp     (SQLite WAL 初始化、2000条批量写入、分页与CSV)
|   `-- tst_scheduler.cpp   (多设备异步调度队列与模拟器端到端连通)
`-- api/ (Pytest 端到端自动化套件 - 真实进程启动)
    |-- test_normal_polling.py        (正常轮询与数据持久化校验)
    |-- test_fault_injection.py       (延时注入与异常码注入断言)
    |-- test_pymodbus_crosscheck.py   (pymodbus 3.x 官方参考实现交叉核验)
    |-- test_recovery.py              (杀从站重启恢复自动断言)
    `-- test_storage_integrity.py     (数据库结构与数据时间戳连续性)
```

执行命令：
```bash
# 执行全部 C++ 单元测试
ctest --test-dir build --output-on-failure

# 执行全部 Python 自动化测试
pytest tests/api -v
```
