# MyselfModbusGateway

工业设备数据采集网关：把现场设备（PLC、电表、仪表）的 Modbus 数据稳定地采集回来，
做可视化、归档与自动化测试。面向 C++/Qt 工业软件与自动化测试岗位的作品项目。

> 当前进度：**阶段 0–11 已完成并通过验证**（工程骨架 → 协议层 → 通信层 → 从站模拟器 →
> 串口 RTU → 界面层 → 数据层 → 指标 → 异常注入 → 健壮性 → 测试体系与 CI → 部署与运维）。
> 仅剩阶段 12（工程化收尾）在整理中。
>
> **可移植测试 9 套共 653 项断言全绿**，端到端脚本 24 项、pymodbus 交叉验证 54 项通过。

与项目一（Linux epoll 服务器）的联系单独整理在
[docs/与项目一的对应关系.md](docs/与项目一的对应关系.md)，代码里用 `[与项目一对应]` 注释标注。

## 这个项目解决什么问题

工业软件的价值不在界面上，而在「把现场设备的数据稳定地拿回来」。本项目按工业现场的真实
约束设计：设备会掉线、响应会超时、帧会被截断、CRC 会出错、现场会有电磁干扰。
项目用自研从站模拟器把这些故障全部复现成可重复的用例，因此**不需要真实硬件**也能完成开发与测试。
从站模拟器（阶段 3）本身也是交付物：它独立成可执行文件，测试脚本可以反复启停它，
并用 pymodbus / QModMaster / mbpoll 做交叉验证。

三条贯穿始终的设计原则：

1. **协议层与设备层不依赖 Qt**。编解码只收字节数组、返回结构体；设备会话不持线程、
   不读系统时间（由外部 `tick()` 驱动）。因此核心可以在装 Qt 之前就用普通编译器验证，
   也能在没有图形环境的服务器上跑 —— 阶段 11 的常驻网关正是靠这条原则才不依赖 Qt。
2. **每一处结论都要有实测证据**。文档里的数字（断言数、延迟、耗时）都来自真实运行，
   不接受"应该没问题"。发现文档与代码不一致时，先改代码或改文档，不留模糊地带。
3. **故障要能被复现，才谈得上被修复**。六类链路故障（阶段 8）与断线重连（阶段 9）
   都有对应的假链路或注入规则，可以在毫秒级精确断言，不依赖"拔网线试试"。

## 技术栈

| 部分 | 选型 |
| --- | --- |
| 语言与标准 | C++17 |
| 构建 | CMake 3.16+（Qt 可选） |
| 界面 | Qt 6 Widgets（阶段 5） |
| 通信 | QTcpSocket（TCP）、QSerialPort（RS-485 RTU，阶段 4）；**无 Qt 构建下用自研 NativeTcpTransport** |
| 存储 | vendored SQLite 3.46.1（C API，WAL + 批量事务） |
| 测试 | 自研可移植测试框架（9 套件 / 653 断言）、Qt Test、pymodbus 交叉验证、Python 端到端脚本 |
| CI | GitHub Actions：Ubuntu（无 Qt / 全量）+ Windows（全量） |

## 目录结构

```
MyselfModbusGateway/
|-- CMakeLists.txt
|-- README.md
|-- .github/workflows/ci.yml     双平台 CI（无 Qt 核心 / Linux 全量 / Windows 全量）
|-- config/
|   |-- gateway.json             设备与轮询配置样例
|   `-- slave-simulator.json     从站模拟器配置（寄存器初值）
|-- deploy/
|   `-- gateway-daemon.service   systemd unit（含安全加固与优雅退出配置）
|-- docs/
|   |-- 阶段0-1验收记录.md
|   |-- 阶段2验收记录.md
|   |-- 阶段3验收记录.md          含 pymodbus 交叉验证记录
|   |-- 部署与运维.md             安装、systemd 托管、日常运维、故障排查
|   |-- 与项目一的对应关系.md     与 MyselfWebServer 的逐项对照
|   |-- protocol-matrix.md        功能码与异常码字节序对照表
|   |-- 运行环境与测试验证说明.md
|   `-- adr/                      架构决策记录（0001 分层 / 0002 时间模型 /
|                                 0003 存储选型 / 0004 链路状态所有权 / 0005 无界面网关）
|-- include/gateway/
|   |-- protocol/                 协议层（不依赖 Qt）
|   |-- transport/                链路抽象、接收缓冲、TCP/串口/原生链路、链路注册表
|   |-- device/                   设备配置、会话状态机、设备管理
|   |-- sim/                      从站模拟器与故障注入（不依赖 Qt）
|   |-- storage/                  存储接口、内存实现、SQLite 实现
|   |-- metrics/                  滑动窗口与延迟分位数
|   |-- json/                     通用 JSON 解析器（配置层共用）
|   |-- config/                   网关配置解析
|   `-- ui/                       Qt 界面（主窗口、寄存器表模型）
|-- src/                          与 include/ 一一对应的实现
|   |-- main.cpp                  界面入口
|   |-- gateway_cli.cpp           协议层自检（无界面）
|   |-- gateway_daemon.cpp        常驻采集网关（无界面、不依赖 Qt）
|-- tests/
|   |-- portable/                 不依赖 Qt 的 9 套测试（现在就能跑）
|   `-- unit/                     Qt Test 用例（装好 Qt 后跑）
|-- third_party/sqlite/           vendored SQLite amalgamation（3.46.1）
`-- scripts/
    |-- build_portable_tests.sh / .ps1   不依赖 Qt 与 CMake 的构建+测试
    |-- run_portable_tests.sh            只跑已构建的测试并汇总断言数
    |-- e2e_gateway.py                   端到端验收（模拟器→网关→SQLite→CSV）
    |-- verify_simulator.py              pymodbus 交叉验证
    |-- build_linux.sh / build_windows.ps1
```

分层依赖方向（只允许单向依赖）：

```
ui  ->  device  ->  transport  ->  protocol
                      |              ^
                      v              |
                   storage        sim
```

## 构建与验证

### 方式一：还没装 Qt，先验证核心（现在就能做）

只要有 C++17 编译器与（可选）CMake，不需要 Qt：

```bash
# Linux / WSL / Git Bash
./scripts/build_portable_tests.sh
```

```powershell
# Windows PowerShell（注意：脚本是 UTF-8 with BOM，
# 否则 Windows PowerShell 5.1 会按 ANSI 解码中文注释导致语法错误）
.\scripts\build_portable_tests.ps1
```

脚本会编译 12 个可执行文件（9 套测试 + `gateway_cli` + `slave_simulator` + `gateway_daemon`）
并运行全部 9 套测试。只想重跑不重编时用：

```bash
./scripts/run_portable_tests.sh            # 汇总断言数
./scripts/run_portable_tests.sh build-linux  # 也能跑 CMake 的产物
```

### 方式二：装好 Qt 6 之后的完整构建

```bash
# Ubuntu（需要 qt6-base-dev / libqt6serialport6-dev）
./scripts/build_linux.sh
```

```powershell
# Windows
.\scripts\build_windows.ps1 -QtPrefix "C:/Qt/6.8.0/mingw_64"
```

### 方式三：CMake 已装但没有 Qt

顶层 `CMakeLists.txt` 把 Qt 设为可选：没装 Qt 时构建 `gateway_core`、`gateway_cli`、
`slave_simulator` 与 `gateway_daemon`，`ctest` 仍能运行全部 9 套可移植测试；
装好 Qt 后重新执行 `cmake` 会自动启用界面程序与 Qt Test。

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

> Windows + MinGW 提示：如果仓库路径含非 ASCII 字符（例如 `项目`），
> `mingw32-make` 会因编码问题读不出 `VerifyGlobs.cmake`。
> 用 Ninja 生成器（`-G Ninja`）即可，它按 UTF-8 处理路径。

### 端到端验收

```bash
python scripts/e2e_gateway.py --build-dir build_portable
```

它会真起两个进程（从站模拟器 + 常驻网关）、走真实 loopback socket、采集 3 秒，
然后用**独立的 sqlite3 模块**去查库，校验三层：

1. 进程层：两个进程都能起来、网关能优雅退出（退出码 0）；
2. 数据层：库里有样本，且**值**与从站配置一致（HR[0..3] = 100/250/380/7）、
   表结构含全部期望列、索引真的建出来了、功能码与设备名都对；
3. 导出层：CSV 表头正确、每行列数与表头一致、行数与库里一致。

## 已完成的内容

**阶段 0 工程骨架** — 顶层与 `src/` 的 CMake 配置（Qt 可选、AUTOMOC 按需开启）、
`gateway_cli` 无界面入口、`.clang-format`、`.gitignore`、配置骨架。

**阶段 1 协议编解码层** — `Crc16`（编译期 256 项查表，多项式 `0xA001`，初值 `0xFFFF`，
另留逐位实现用于对照）、`ModbusCodec`（PDU 构造、TCP/RTU 组帧解帧、响应解析、请求响应匹配）、
`ExceptionCode`（异常码枚举 + 中文提示）。覆盖功能码 `0x01/0x02/0x03/0x04/0x05/0x06/0x0F/0x10`
与异常码 `0x01`–`0x06`；半包、粘包、坏 CRC、非法协议标识、非法长度都有明确状态返回值。

**阶段 2 通信层与设备会话** — `TransportInterface`（链路抽象，不依赖 Qt）、
`TcpTransport`（QTcpSocket，含连接超时与错误上报）、`ByteAccumulator`（接收缓冲）、
`DeviceSession`（请求/超时/重传/切帧/结果回调的完整状态机，一次只允许一个在途事务）、
`DeviceManager`（多设备注册、启动、时钟分发、结果派发）。

**阶段 3 从站模拟器** — `RegisterMap`（四区独立地址空间，越界与超量分别返回 `0x02`/`0x03`）、
`RequestHandler`（八个功能码 + 三类异常，位区按规范低位在前打包）、
`SlaveSimulator`（不依赖 Qt，单线程 select 轮询，独立可执行文件可反复启停）、
`SimulatorConfig`（从 JSON 读初值，地址越界直接报错而不是静默丢弃）。

**阶段 4 串口 RTU 链路** — `SerialTransport`（QSerialPort 封装，条件编译：
没有 Qt SerialPort 模块时不编入，而不是整个构建失败）；
RTU 解帧分请求/响应两个方向（RTU 没有长度字段，必须知道方向才能推断帧长）；
**帧间静默估算** `estimateFrameGapMs()` 按 3.5 字符时间计算并夹在 [1,50]ms 内
（9600 8E1 → 5ms）。35 项断言覆盖组帧字节序、解帧方向、半包拼齐、坏 CRC 丢弃、
异常响应不重传、单元号过滤。

**阶段 5 Qt 界面层** — `MainWindow`（三栏：设备面板 / 寄存器表 / 运行日志，
15 个可操作控件，100ms `QTimer` 驱动 `manager.tick()`）、
`RegisterTableModel`（四列：地址/数值/十六进制/最近更新；按地址二分定位插入、保序、
按来源设备底色区分）、`TransportFactory`（按配置造链路，串口不可用时给出可操作提示）。

**阶段 6 数据层** — `StorageInterface`（存储契约）、`MemoryStorage`（测试用，零临时文件）、
`SqliteStorage`（vendored SQLite 3.46.1，**C API 而非 Qt SQL**，因此能在无 Qt 的网关里复用）。
关键设计：主键 `(device, ts_ms, address)` + `INSERT OR REPLACE` 去重、
`BEGIN IMMEDIATE` + 预编译语句复用、WAL + `synchronous=NORMAL`、`close()` 兜底 flush。
另有 CSV 导出（引号翻倍转义，回读可还原）。设计取舍见
[ADR 0003](docs/adr/0003-存储选型与批量落盘.md)。

**阶段 7 运行指标** — `MetricsCollector`（固定容量滑动窗口，默认 1000）。
`Snapshot` 给出成功率/超时率与 P50/P95/P99/最小/最大/均值。
两处刻意的语义选择：**成功率的分母是窗口内样本数**（反映"现在稳不稳"，
不被几小时前的正常状态稀释）；**超时样本不计入延迟分位数**（超时延迟恒等于阈值，
算进去会让 P95 永远等于阈值，看不出真实分布）。分位数用最近秩法（延迟是整数毫秒，
插值出的小数没有物理意义）。

**阶段 8 异常注入引擎** — `FaultInjector` 六类故障：`NoResponse`（不应答）、
`CorruptCrc`（篡改 CRC）、`ExceptionCode`（返回异常码）、`TruncatedFrame`（截断帧）、
`BadProtocolId`（非法协议标识）、`DelayedResponse`（延迟响应）。
规则支持 `interval` / `startAfter` / `maxInjections` / `functionCode` 过滤与优先级。
79 项断言覆盖六类的决策与字节效果、计数与上限、功能码过滤、JSON 规则解析。

**阶段 9 健壮性：退避重连** — `ReconnectPolicy`（指数退避，默认 1s→2s→4s…封顶 30s，
前 2 次失败不进入退避以便瞬时抖动快速恢复）、`LinkEvent{Down, Reconnecting, Recovered}`。
**修复了一个静默 bug**：`DeviceSession::attach` 与 `DeviceManager::addDevice` 都注册
`transport->onState`，后者覆盖前者导致掉线重连完全失效 —— 界面能正确显示"链路断开"，
但永远不会重连，且不报任何错。确立「链路状态唯一所有者 = 会话」原则，
`DeviceManager::state()` 改为读 `session->linkState()`。设计取舍见
[ADR 0004](docs/adr/0004-链路状态所有权与退避重连.md)。40 项断言，含退避序列 100→200→400→800ms。

**阶段 10 测试体系与 CI** — 9 套可移植测试（653 断言）+ Qt Test + 端到端脚本 +
双平台 GitHub Actions。CI 分三个 job，各自回答一个不同的问题：
`core-no-qt`（不装 Qt 能不能构建测试 —— 这是对外的核心承诺）、
`full-linux`（部署目标机 Ubuntu 上能不能全量构建）、
`full-windows`（开发机上的界面与串口能不能编）。
端到端脚本刻意放在 `core-no-qt` 里跑，等于顺手证明了整条采集链路在裸机上可用。

**阶段 11 部署与运维** — `gateway_daemon`（常驻、无界面、**不依赖 Qt**）：
单调时钟主循环（`steady_clock`，避免 NTP 校时把超时算错）、
`--seconds` / `--stop-file` / `--db` / `--poll` / `--export-csv` / `--dry-run` / `--quiet`、
优雅退出（`stopAll()` → `flush()` → `close()`，顺序不能反）。
为此引入 `TransportRegistry`（无 Qt 的链路注册表）与 `NativeTcpTransport`
（阻塞 socket + `select` 轮询，带超时的非阻塞 connect），
并给链路接口加 `pump()` 主动驱动入口（Qt 链路是空实现，原生链路真正去读 socket）。
设计取舍见 [ADR 0005](docs/adr/0005-无界面网关与原生链路.md)。
systemd unit 与完整运维手册见 [docs/部署与运维.md](docs/部署与运维.md)。

## 已验证的证据

### 可移植测试：9 套 653 项断言全绿

编译器 Windows MinGW-W64 g++ 8.1.0，`-Wall -Wextra` 零告警：

```
断言 131 项，失败 0 项     （协议层 portable_tests）
断言  88 项，失败 0 项     （设备层 device_tests，假链路驱动）
断言  87 项，失败 0 项     （从站模拟器 simulator_tests，与主站侧闭环）
断言  35 项，失败 0 项     （串口 RTU serial_tests）
断言  72 项，失败 0 项     （配置解析 config_tests）
断言  80 项，失败 0 项     （存储层与指标 storage_tests）
断言  79 项，失败 0 项     （异常注入 fault_tests）
断言  40 项，失败 0 项     （退避重连 recovery_tests）
断言  41 项，失败 0 项     （原生 TCP 链路与注册表 native_transport_tests）
合计 653 项，失败 0 项
```

两条构建路径都验证过：`scripts/build_portable_tests.sh`（手写源文件列表）
与 CMake + ctest（`100% tests passed, 9/9`）。这两条路径的存在是有意义的 ——
它们编出的东西不同（一个用 g++ 直接编、一个走 CMake target），
**阶段 11 就抓到过"脚本能过、CMake 链接失败"**：`src/metrics/` 新增后忘了加进 CMake 的
源文件收集列表，表现为一堆 `undefined reference`。现在 CMake 改为整体 glob 目录，
这类"加了新目录却忘了改构建脚本"不会再发生。

### 端到端：24 项断言

`python scripts/e2e_gateway.py` —— 真起两个进程、真实 socket、3 秒采集，
用独立 sqlite3 模块查库：

```
  [OK]   网关优雅退出，退出码 0
  [OK]   库里有样本（100 行）
  [OK]   samples 表含全部期望列
  [OK]   按设备+时间的索引存在
  [OK]   HR[0] 库里读到的值应为 100，实际 {100}
  [OK]   HR[1] 库里读到的值应为 250，实际 {250}
  [OK]   HR[2] 库里读到的值应为 380，实际 {380}
  [OK]   HR[3] 库里读到的值应为 7，实际 {7}
  [OK]   CSV 表头正确
  [OK]   CSV 行数与库里行数一致（CSV 100 / 库 100）
断言 24 项，失败 0 项
```

值的校验是这一步的重点：「有行」很容易满足（哪怕是全 0 的垃圾数据），
「值等于从站里的值」才能证明请求组帧、链路收发、响应解析、存储写入这一整条链路都对。

### pymodbus 交叉验证：54 项断言

```
python scripts/verify_simulator.py        # pymodbus 3.15.0
断言 54 项，失败 0 项
```

这一步不能省略。自研主站与自研从站的闭环测试只能证明「两边对规范的理解一致」，
如果两边一起把字节序理解错了，测试依然全绿。pymodbus 是独立实现，
它读出的值与预期一致，才说明字节序、位打包顺序、异常码真的是对的。

### 其他防线

Qt Test（`tst_crc16` / `tst_codec`）在 Ubuntu 22.04 + Qt 6.2.4 下 `ctest` 通过，
与自研可移植测试互为备份。完整环境记录见
[docs/运行环境与测试验证说明.md](docs/运行环境与测试验证说明.md)。

### 关键用例摘录

协议层：

| 类别 | 用例 |
| --- | --- |
| CRC 向量 | `01 03 00 00 00 0A` → `C5 CD`；空数据 → `FFFF`；`0x00` → `BF 40` |
| 实现对照 | 512 字节序列上查表版与逐位版结果一致 |
| TCP 组帧 | MBAP 长度字段 = 6（单元号 + PDU），不是整帧长度 |
| RTU 组帧 | `01 03 00 00 00 0A C5 CD`，CRC 低字节在前 |
| 半包 / 粘包 | 少 1 字节返回 `Incomplete` 不产生假错误；一次送两帧能循环解析出两帧 |
| 非法报文 | 协议标识非 0 → `BadProtocolId`；长度字段为 0 → `BadLength` |
| 错配防护 | 事务号、单元号**或功能码**不匹配时判为迟到/他人响应；异常功能码 `0x83` 正常放行 |

设备层（假链路可脚本控制离线、坏 CRC、半包、异常码、事务号）：

| 类别 | 用例 |
| --- | --- |
| 超时重传 | 99ms 不重传 / 100ms 重传；重传次数等于 `retry`；达上限只上报一次超时 |
| 单在途事务 | 在途期间第二个请求被拒绝 |
| 轮询写点位 | `0x06` 点位写入的是 `value` 而不是 `count`；未支持功能码被跳过且不阻塞其它点位 |
| 重连退避 | 掉线后间隔 100→200→400→800ms（封顶）；`stop()` 后不再重连；策略关闭时不重连 |
| 链路所有权 | 管理器正确转发会话的链路事件；`pump()` 转达到链路 |

存储层：

| 类别 | 用例 |
| --- | --- |
| 双实现同契约 | 内存实现与 SQLite 实现跑同一批断言（含排序、时间范围闭区间、`limit`） |
| 主键去重 | 同一 `(device, ts_ms, address)` 重写不增加条数，值被替换 |
| 批量语义 | `batchSize=3` 时 7 条样本的落盘与缓冲计数；未达阈值时缓冲留住数据 |
| close 兜底 | 写入后不 flush 直接 `close()`，重开库仍能读到 |
| CSV 转义 | 含逗号/引号/中文标点的 `note` 能正确转义并回读 |
| 分位数 | 最近秩法；超时样本不计入延迟分位；滑动窗口超容量后丢最旧 |

## 常用命令

```bash
# 打印一组编解码结果（不需要 Qt）
./build_portable/gateway_cli

# 9 套可移植测试（不需要 Qt）；也可直接跑单套，如 ./build_portable/storage_tests <仓库根>
./scripts/run_portable_tests.sh

# 端到端验收（不需要 Qt）
python scripts/e2e_gateway.py --build-dir build_portable

# 常驻网关：只检查配置不采集（有设备链路不可用时退出码为 1，可直接用于部署脚本拦截）
./build_portable/gateway_daemon config/gateway.json --dry-run

# 常驻网关：采集 30 秒并导出 CSV
./build_portable/gateway_daemon config/gateway.json --seconds 30 --export-csv out.csv

# 从站模拟器（第三方工具可连）
./build_portable/slave_simulator config/slave-simulator.json --port 5020

# 全部测试（含 Qt Test；Linux 构建目录是 build-linux，Windows 是 build）
ctest --test-dir build-linux --output-on-failure
```

## 后续阶段路线

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| ~~0~~ | ~~工程骨架~~ | 已完成 |
| ~~1~~ | ~~协议编解码层~~ | 已完成 · 131 断言 |
| ~~2~~ | ~~通信层与设备会话~~ | 已完成 · 88 断言 |
| ~~3~~ | ~~从站模拟器~~ | 已完成 · 87 断言 + pymodbus 54 项 |
| ~~4~~ | ~~串口 RTU（QSerialPort、帧间静默）~~ | 已完成 · 35 断言 |
| ~~5~~ | ~~界面层（设备面板、寄存器表、日志）~~ | 已完成 · 15 个可操作控件 |
| ~~6~~ | ~~数据层（SQLite 归档、查询、CSV 导出）~~ | 已完成 · 含在 80 断言内 |
| ~~7~~ | ~~实时指标（滑动窗口、P50/P95/P99）~~ | 已完成 · 含在 80 断言内 |
| ~~8~~ | ~~异常注入引擎（六类故障）~~ | 已完成 · 79 断言 |
| ~~9~~ | ~~健壮性与并发（退避重连）~~ | 已完成 · 40 断言 |
| ~~10~~ | ~~测试体系（单元、端到端、双平台 CI）~~ | 已完成 · 653 + 24 断言，CI 三 job |
| ~~11~~ | ~~部署与运维（无界面网关、systemd）~~ | 已完成 · 41 断言 + 运维手册 |
| 12 | 工程化收尾（README、架构图、演示、简历条目） | 进行中 |

## 提交规范

每个阶段结束提交一次，提交信息格式：

```
stage-N: 标题

- 主要改动
- 验证结果（断言数 / 通过的检查）
```

历史提交示例：

```
stage-3: 从站模拟器（四区寄存器模型与异常码）

- 新增 RegisterMap/RequestHandler/SlaveSimulator/SimulatorConfig 与 slave_simulator
- 不依赖 Qt（POSIX/WinSock 套接字），配置从 JSON 读寄存器初值
- 模拟器测试 87 项断言通过；pymodbus 交叉验证 54 项通过
```

## 许可证与来源

本项目为**参考 Modbus 协议规范独立实现**，未基于任何开源仓库二次开发。
`third_party/sqlite/` 下为 SQLite 官方 amalgamation（公有领域），
仅作为 vendored 依赖随仓库分发，不属于本项目著作权范围。

协议细节（功能码、异常码、CRC 多项式、寄存器四区模型）来自
Modbus Application Protocol Specification V1.1b3 与 Modbus over Serial Line V1.02，
属于公开标准，已在 [docs/protocol-matrix.md](docs/protocol-matrix.md) 中整理为字节序对照表。

代码以 **MIT 许可证**发布，全文见 [LICENSE](LICENSE)。
