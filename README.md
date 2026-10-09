# MyselfModbusGateway

工业设备数据采集网关：把现场设备（PLC、电表、仪表）的 Modbus 数据稳定地采集回来，
做可视化、归档与自动化测试。面向 C++/Qt 工业软件与自动化测试岗位的作品项目。

> 当前进度：**阶段 0（工程骨架）、阶段 1（协议编解码层）、阶段 2（通信层与设备会话）
> 已完成并通过验证**。后续阶段见文末路线。

与项目一（Linux epoll 服务器）的联系单独整理在
[docs/与项目一的对应关系.md](docs/与项目一的对应关系.md)，代码里用 `[与项目一对应]` 注释标注。

## 这个项目解决什么问题

工业软件的价值不在界面上，而在「把现场设备的数据稳定地拿回来」。本项目按工业现场的真实
约束设计：设备会掉线、响应会超时、帧会被截断、CRC 会出错、现场会有电磁干扰。
项目用自研从站模拟器把这些故障全部复现成可重复的用例，因此**不需要真实硬件**也能完成开发与测试。

## 技术栈

| 部分 | 选型 |
| --- | --- |
| 语言与标准 | C++17 |
| 构建 | CMake 3.16+ |
| 界面 | Qt 6 Widgets（阶段 5 起） |
| 通信 | QTcpSocket（TCP）、QSerialPort（RS-485 RTU） |
| 存储 | SQLite（阶段 6 起） |
| 测试 | Qt Test（单元测试）、pytest（端到端）、自研可移植测试 |

## 目录结构

```
MyselfModbusGateway/
|-- CMakeLists.txt
|-- README.md
|-- .clang-format
|-- .gitignore
|-- config/gateway.json          设备与轮询配置样例
|-- docs/
|   |-- 阶段0-1验收记录.md
|   |-- 阶段2验收记录.md
|   |-- 与项目一的对应关系.md     与 MyselfWebServer 的逐项对照
|   |-- protocol-matrix.md        功能码与异常码字节序对照表
|   `-- adr/                      架构决策记录
|-- include/gateway/
|   |-- protocol/                 协议层头文件（不依赖 Qt）
|   |-- transport/                链路抽象、接收缓冲、TCP 链路
|   `-- device/                   设备配置、会话状态机、设备管理
|-- src/
|   |-- protocol/                 Crc16、ModbusFrame、ModbusCodec、ExceptionCode
|   |-- transport/                TransportInterface、ByteAccumulator、TcpTransport
|   |-- device/                   DeviceSession、DeviceManager
|   |-- main.cpp                  界面入口（阶段 5 前只是一个空窗口）
|   `-- gateway_cli.cpp           无界面入口
|-- tests/
|   |-- portable/                 不依赖 Qt 的协议层与设备层测试（现在就能跑）
|   `-- unit/                     Qt Test 用例（装好 Qt 后跑）
`-- scripts/                      构建与验证脚本
```

**协议层与设备层都不依赖 Qt**：编解码只接收字节数组，设备会话不持有线程也不读系统时间。
因此整个核心可以在装 Qt 之前就用普通编译器验证，也能在没有图形环境的机器上跑。

分层依赖方向（只允许单向依赖）：

```
ui  ->  device  ->  transport  ->  protocol
                      |              ^
                      v              |
                   storage        sim
```

**协议层与设备层不依赖 Qt**，这是本项目最重要的设计决定：编解码只接收字节数组、返回结构体，
设备会话不持有线程也不读系统时间（由外部 tick 驱动），因此可以脱离网络与界面做单元测试，
同一份代码同时服务 TCP 与 RTU 两条链路，也可以移植到 MCU 侧。

## 构建与验证

### 方式一：还没装 Qt，先验证协议层（现在就能做）

只要有 C++17 编译器即可，不需要 Qt 与 CMake：

```bash
# Linux / WSL / Git Bash
./scripts/build_portable_tests.sh
```

```powershell
# Windows PowerShell
.\scripts\build_portable_tests.ps1
```

这个脚本会编译三个可执行文件并运行两套测试：`portable_tests`（协议层）、
`device_tests`（设备层，用假链路驱动）、`gateway_cli`（命令行演示）。

### 方式二：装好 Qt 6 之后的完整构建

```bash
# Ubuntu
./scripts/build_linux.sh
```

```powershell
# Windows
.\scripts\build_windows.ps1 -QtPrefix "C:/Qt/6.8.0/mingw_64"
```

### 方式三：CMake 已装但没有 Qt

顶层 `CMakeLists.txt` 把 Qt 设为可选：没装 Qt 时只构建 `gateway_core` 与 `gateway_cli`，
`ctest` 仍能运行可移植测试；装好 Qt 后重新执行 `cmake` 会自动启用界面程序与 Qt Test。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## 已完成的内容

**阶段 0 工程骨架**

- 顶层与 `src/` 的 CMake 配置，Qt 可选，AUTOMOC/AUTOUIC/AUTORCC 在检测到 Qt 后自动开启；
- `src/main.cpp`：空窗口 + Qt 版本号，用来验证工具链；
- `src/gateway_cli.cpp`：无界面入口，打印编解码结果；
- `.clang-format`、`.gitignore`、`config/gateway.json` 配置骨架。

**阶段 1 协议编解码层**

- `Crc16`：编译期生成 256 项查表，多项式 `0xA001`（0x8005 的反射形式），初值 `0xFFFF`；
  同时保留逐位实现用于对照测试；
- `ModbusCodec`：PDU 构造、TCP/RTU 组帧、TCP/RTU 解帧、响应解析、请求响应匹配；
- `ExceptionCode`：异常码枚举与中文提示（界面直接可用的可读说明）；
- 覆盖功能码 0x03 / 0x04 / 0x06 / 0x10 与异常码 0x01-0x06；
- 半包、粘包、坏 CRC、非法协议标识、非法长度字段都有明确的状态返回值。

**阶段 2 通信层与设备会话**

- `TransportInterface`：链路抽象（open/close/send + 字节/状态/消息回调），不依赖 Qt，
  因此 TCP、串口与测试用的假链路可以共用同一套设备层代码；
- `TcpTransport`：基于 QTcpSocket 的 TCP 链路，含连接超时与错误上报（**本机无 Qt，未编译**）；
- `ByteAccumulator`：接收缓冲区，对应项目一里的 `Buffer`；
- `DeviceSession`：请求 / 超时 / 重传 / 切帧 / 结果回调的完整状态机，一次只允许一个在途事务；
- `DeviceManager`：多设备注册、启动、时钟分发与结果派发；
- 链路抽象与设备层均可在无 Qt 环境下用假链路完整验证。

`config/gateway.json` 的字段与 `DeviceConfig` / `PollPoint` 一一对应：

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `name` | string | — | 设备名，回调里用它区分数据来源 |
| `transport` | `"tcp"` / `"rtu"` | `"tcp"` | 链路类型，同时决定用 MBAP 还是 CRC 组帧 |
| `host` / `port` | string / int | `127.0.0.1` / `502` | TCP 专用；`connectTimeoutMs` 默认 3000 |
| `serialPort` / `baudRate` / `parity` / `dataBits` / `stopBits` | — | `COM11` / 9600 / `N` / 8 / 1 | RTU 专用（阶段 4 使用） |
| `unitId` | int | 1 | 从站地址 |
| `timeoutMs` | int | 1000 | 单次响应超时；超时后重传 |
| `retry` | int | 3 | 最多重传次数，总尝试次数 = `retry + 1` |
| `pollIntervalMs` | int | 500 | 两次轮询之间的最小间隔 |
| `points[].func` | int | 0x03 | 功能码，支持 `3`（读保持寄存器）、`4`（读输入寄存器）、`6`（写单个寄存器） |
| `points[].address` | int | 0 | 起始地址（协议地址，不是手册上的 40001 口径） |
| `points[].count` | int | 10 | 读多少个寄存器；**写单寄存器时恒为 1，不表示写入值** |
| `points[].value` | int | 0 | 仅 `func = 6` 使用，表示要写进设备的数值 |

> 写单寄存器时 `count` 与 `value` 语义完全不同：前者是「读多少个」，后者是「写什么」。
> 代码里两者分别存放在 `PollPoint::count` 与 `PollPoint::value`，不能互相替代。

## 已验证的证据

两套可移植测试共 **219 项断言全部通过**（编译器：MinGW-W64 g++ 8.1.0，`-Wall -Wextra` 无告警）：

```
断言 131 项，失败 0 项     （协议层 portable_tests）
断言 88 项，失败 0 项      （设备层 device_tests）
结果：全部通过
```

协议层覆盖的关键用例：

| 类别 | 用例 |
| --- | --- |
| CRC 向量 | `01 03 00 00 00 0A` → `C5 CD`；空数据 → `FFFF`；`0x00` → `BF 40` |
| 实现对照 | 512 字节序列上查表版与逐位版结果一致 |
| TCP 组帧 | MBAP 长度字段 = 6（单元号 + PDU），不是整帧长度 |
| RTU 组帧 | `01 03 00 00 00 0A C5 CD`，CRC 低字节在前 |
| 半包 | 少 1 字节时返回 `Incomplete`，不产生假错误 |
| 粘包 | 一次送入两帧，循环解析出两帧并各自匹配事务号 |
| 非法报文 | 协议标识非 0 → `BadProtocolId`；长度字段为 0 → `BadLength` |
| 坏 CRC | 篡改数据后校验失败并丢弃本帧 |
| 半包 / 方向推断 | RTU 没有长度字段，请求与响应必须用不同函数解帧 |
| 异常码 | `01 83 02 C0 F1` 正确解析出异常码 0x02 并给出中文提示；0x01/0x03/0x04/0x06 逐码验证 |
| 异常功能码 | `0x83`、`0x84` 判定为异常帧，`0x03`、`0x00` 不是 |
| 错配防护 | 事务号、单元号或功能码不匹配时判为迟到 / 他人响应；异常功能码（0x83）正常放行 |

设备层用假链路（可脚本控制离线、坏 CRC、半包、异常码、事务号）覆盖的关键行为：

| 类别 | 用例 |
| --- | --- |
| 正常读写 | 读保持寄存器、写单个寄存器、寄存器值回读 |
| 超时重传 | 99ms 不重传 / 100ms 重传；重传次数等于 retry 配置；达到上限只上报一次超时 |
| 坏 CRC | 整帧丢弃、只计一次误码、不回调错误结果、继续等待 |
| 半包 | 只有半帧时不结束事务，凑齐后解析成功 |
| 粘包 | 一次到达两帧，第二帧按迟到响应计数 |
| 事务号错配 | 错配响应被丢弃；重传后拿到正确结果 |
| 异常响应 | 不重传，直接给出中文提示并计入异常计数 |
| 单在途事务 | 在途期间第二个请求被拒绝 |
| 自动轮询 | 到达轮询间隔才发起下一次，不重叠 |
| 轮询写点位 | 0x06 点位写入的是 `value` 而不是 `count`；未支持的功能码被跳过且不阻塞其它点位 |
| 多设备管理 | 两台设备分别发起，回调带正确设备名，链路状态可查询 |

详细记录见 `docs/阶段0-1验收记录.md` 与 `docs/阶段2验收记录.md`，
字节序对照见 `docs/protocol-matrix.md`，与项目一的对照见 `docs/与项目一的对应关系.md`。

## 常用命令

```bash
# 打印一组编解码结果（不需要 Qt）
./build_portable/gateway_cli

# 两套可移植测试
./build_portable/portable_tests
./build_portable/device_tests

# 全部测试
ctest --test-dir build --output-on-failure
```

## 后续阶段路线

| 阶段 | 内容 | 验证方式 |
| --- | --- | --- |
| ~~2~~ | ~~通信层与设备会话~~（已完成） | 假链路驱动 88 项断言通过 |
| 3 | 从站模拟器（四区寄存器模型、异常码返回） | 无硬件下复现故障 |
| 4 | 串口 RTU（QSerialPort、虚拟串口） | com0com / socat 上完成读写 |
| 5 | 界面层（设备面板、寄存器表、日志） | 十个以上界面元素可操作 |
| 6 | 数据层（SQLite 归档、查询、CSV 导出） | 百万级写入与查询 |
| 7 | 实时曲线与运行指标 | 长稳刷新内存不增长 |
| 8 | 异常注入引擎（六类故障） | 每种异常都能复现 |
| 9 | 健壮性与并发（退避重连、并发调度） | 断线后自动恢复 |
| 10 | 测试体系（单元、端到端、双平台 CI） | 用例数达标，CI 全绿 |
| 11 | 部署与运维（无界面网关、systemd、打包） | 无图形环境稳定运行 |
| 12 | 工程化收尾（README、架构图、演示、简历条目） | 链接可直接发给面试官 |

## 提交规范

每个阶段结束提交一次，提交信息格式：

```
stage-1: 协议编解码层与 CRC16 查表实现

- 新增 Crc16/ModbusCodec/ExceptionCode
- 覆盖 0x03/0x04/0x06/0x10 与异常码 0x01-0x06
- 可移植测试 131 项断言通过
```

```
stage-2: 通信层抽象与设备会话状态机

- 新增 TransportInterface/ByteAccumulator/TcpTransport/DeviceSession/DeviceManager
- 超时由外部 tick 驱动，一次只允许一个在途事务
- 设备层测试 88 项断言通过（假链路驱动）
```

## 许可证与来源

本项目为**参考 Modbus 协议规范独立实现**，未基于任何开源仓库二次开发，因此不含上游
版权声明需要保留。协议细节（功能码、异常码、CRC 多项式、寄存器四区模型）来自
Modbus Application Protocol Specification V1.1b3 与 Modbus over Serial Line V1.02，
属于公开标准，已在 `docs/protocol-matrix.md` 中整理为字节序对照表。

代码以 **MIT 许可证**发布，全文见 [LICENSE](LICENSE)。
