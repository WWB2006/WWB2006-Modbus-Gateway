# 系统架构设计与线程模型说明 (Architecture & Threading Model)

## 1. 系统总体分层架构 (Layered Architecture)

Modbus 工业网关与自动化测试平台采用严格解耦的四层架构设计，各层单向依赖，核心通信与存储逻辑独立于图形界面。

```mermaid
flowchart TD
    subgraph UI ["界面与交互层 (Presentation Layer)"]
        MW["MainWindow (主窗口)"]
        DP["DevicePanel (设备管理)"]
        RTM["RegisterTableModel (寄存器模型)"]
        CP["ChartPanel (实时曲线)"]
        LV["LogView (日志视图)"]
    end

    subgraph Service ["调度与服务层 (Service & Orchestration)"]
        PS["PollScheduler (多设备轮询调度器)"]
        DM["DeviceManager (连接与重试管理)"]
        RP["ReconnectPolicy (指数退避算法)"]
        MET["Metrics (运行指标与统计)"]
    end

    subgraph Protocol ["协议与传输层 (Protocol & Transport Layer)"]
        MC["ModbusCodec (报文编解码器)"]
        CRC["Crc16 (查表法校验)"]
        TCP["TcpTransport (TCP Socket 链路)"]
        SER["SerialTransport (RTU 串口链路)"]
    end

    subgraph Storage ["数据归档与仿真层 (Storage & Simulation)"]
        SQL["SqliteStorage (WAL 批量存储引擎)"]
        SIM["SlaveSimulator (自研多功能从站)"]
        FI["FaultInjector (异常注入引擎)"]
    end

    MW --> DP
    MW --> RTM
    MW --> CP
    MW --> LV

    MW --> PS
    PS --> DM
    DM --> RP
    DM --> MET
    DM --> MC
    DM --> TCP
    DM --> SER
    MC --> CRC

    PS --> SQL
    SIM --> FI
    SIM --> MC
```

---

## 2. 线程隔离模型 (Thread Decoupling Model)

为了保证工业环境下图形界面的绝对流畅，本项目采用了彻底的跨线程解耦设计：

```mermaid
sequenceDiagram
    participant UI as Qt UI 主线程
    participant Worker as 设备专属采集线程 (Worker)
    participant HW as 工业从站设备 / 模拟器
    participant DB as SQLite 归档线程

    UI->>Worker: 启动调度 (startPolling)
    loop 每周期 (e.g. 500ms)
        Worker->>HW: 发送 Modbus 请求 (TCP/RTU)
        HW-->>Worker: 返回响应报文
        Worker->>Worker: 解码并计算耗时 (Latency)
        Worker-->>UI: emit registerUpdated / latency (Qt::QueuedConnection)
        Worker->>DB: 批量入库队列 (insertRecord)
    end
    DB->>DB: 每 500 条单事务提交 (COMMIT)
```

### 关键设计决策：
1. **设备独立线程模型**：每台设备分配独立的采集与重连逻辑，即使某一从站超时或断网，也不会对其他正常从站的轮询周期产生任何拖尾影响。
2. **跨线程通信规范**：全部数据交互通过值拷贝或 `const &` 配合 `Qt::QueuedConnection` 投递，UI 线程严禁直接操作任何 Socket 或数据库句柄，彻底杜绝界面冻结与多线程崩溃。
3. **日志与曲线内存封顶**：`LogView` 限制最多 500 行，`ChartPanel` 限制最多 200 点，时间复杂度与空间复杂度保持恒定 $O(1)$，确保系统连续稳定常驻运行数周内存不膨胀。

---

## 3. 指数退避断线重连状态机 (Reconnect State Machine)

当设备因网络波动或设备断电离线时，系统自动切入带随机抖动的指数退避重连状态机：

```mermaid
stateDiagram-v2
    [*] --> DISCONNECTED: 初始状态
    DISCONNECTED --> CONNECTING: 触发轮询任务
    CONNECTING --> CONNECTED: TCP/串口握手成功 (重置退避)
    CONNECTING --> ERROR_STATE: 握手失败 / 超时
    CONNECTED --> ERROR_STATE: 发送失败 / 接收超时
    ERROR_STATE --> CONNECTING: 退避时间到达 (Base * 2^attempt + Jitter)
    CONNECTED --> DISCONNECTED: 用户主动停止
```

- **退避公式**：
  $$\text{Delay} = \min(\text{MaxDelay},\, \text{BaseDelay} \times 2^{\text{attempt}}) + \text{RandomJitter}$$
- **抖动范围**：当前延时的 $0\% \sim 25\%$，有效打散大量从站同时重启造成的通信风暴。
