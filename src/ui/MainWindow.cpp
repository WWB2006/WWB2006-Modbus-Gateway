#include "gateway/ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <fstream>
#include <sstream>

#include "gateway/config/GatewayConfig.h"
#include "gateway/transport/TransportFactory.h"
#include "gateway/ui/RegisterTableModel.h"

#include <QFileInfo>

namespace gateway::ui {
namespace {

QElapsedTimer g_clock;

uint64_t nowMs() {
    if (!g_clock.isValid()) {
        g_clock.start();
    }
    return static_cast<uint64_t>(g_clock.elapsed());
}

QString transportKindLabel(gateway::transport::TransportKind kind) {
    return kind == gateway::transport::TransportKind::Tcp ? QStringLiteral("TCP")
                                                          : QStringLiteral("串口 RTU");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    nowMs();  // 启动计时器
    buildUi();
    buildMenus();
    loadDevices();
    updateControlsEnabled();
}

MainWindow::~MainWindow() {
    if (manager_) {
        manager_->stopAll();
    }
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("Modbus 网关 - 设备数据采集"));

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);

    // —— 顶部：设备选择与链路参数 ——
    auto* deviceBox = new QGroupBox(QStringLiteral("设备与链路"), central);
    auto* deviceLayout = new QHBoxLayout(deviceBox);

    deviceCombo_ = new QComboBox(deviceBox);
    deviceCombo_->setObjectName(QStringLiteral("deviceCombo"));
    deviceCombo_->setMinimumWidth(160);

    hostEdit_ = new QLineEdit(QStringLiteral("127.0.0.1"), deviceBox);
    hostEdit_->setObjectName(QStringLiteral("hostEdit"));
    hostEdit_->setMaximumWidth(120);

    portSpin_ = new QSpinBox(deviceBox);
    portSpin_->setObjectName(QStringLiteral("portSpin"));
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(5020);

    unitIdSpin_ = new QSpinBox(deviceBox);
    unitIdSpin_->setObjectName(QStringLiteral("unitIdSpin"));
    unitIdSpin_->setRange(0, 247);
    unitIdSpin_->setValue(1);

    timeoutSpin_ = new QSpinBox(deviceBox);
    timeoutSpin_->setObjectName(QStringLiteral("timeoutSpin"));
    timeoutSpin_->setRange(10, 60000);
    timeoutSpin_->setValue(1000);
    timeoutSpin_->setSuffix(QStringLiteral(" ms"));

    connectButton_ = new QPushButton(QStringLiteral("连接"), deviceBox);
    connectButton_->setObjectName(QStringLiteral("connectButton"));
    disconnectButton_ = new QPushButton(QStringLiteral("断开"), deviceBox);
    disconnectButton_->setObjectName(QStringLiteral("disconnectButton"));

    deviceLayout->addWidget(new QLabel(QStringLiteral("设备:"), deviceBox));
    deviceLayout->addWidget(deviceCombo_);
    deviceLayout->addWidget(new QLabel(QStringLiteral("主机:"), deviceBox));
    deviceLayout->addWidget(hostEdit_);
    deviceLayout->addWidget(new QLabel(QStringLiteral("端口:"), deviceBox));
    deviceLayout->addWidget(portSpin_);
    deviceLayout->addWidget(new QLabel(QStringLiteral("从站:"), deviceBox));
    deviceLayout->addWidget(unitIdSpin_);
    deviceLayout->addWidget(new QLabel(QStringLiteral("超时:"), deviceBox));
    deviceLayout->addWidget(timeoutSpin_);
    deviceLayout->addStretch();
    deviceLayout->addWidget(connectButton_);
    deviceLayout->addWidget(disconnectButton_);

    rootLayout->addWidget(deviceBox);

    // —— 中部：寄存器表 + 操作面板，用分割器让用户能拖动 ——
    auto* splitter = new QSplitter(Qt::Horizontal, central);

    registerModel_ = std::make_unique<RegisterTableModel>(this);
    registerView_ = new QTableView(splitter);
    registerView_->setObjectName(QStringLiteral("registerView"));
    registerView_->setModel(registerModel_.get());
    registerView_->horizontalHeader()->setStretchLastSection(true);
    registerView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    registerView_->setAlternatingRowColors(true);
    registerView_->verticalHeader()->setVisible(false);

    auto* operationBox = new QGroupBox(QStringLiteral("读写操作"), splitter);
    auto* operationLayout = new QFormLayout(operationBox);

    addressSpin_ = new QSpinBox(operationBox);
    addressSpin_->setObjectName(QStringLiteral("addressSpin"));
    addressSpin_->setRange(0, 65535);
    addressSpin_->setValue(0);

    countSpin_ = new QSpinBox(operationBox);
    countSpin_->setObjectName(QStringLiteral("countSpin"));
    countSpin_->setRange(1, 125);
    countSpin_->setValue(10);

    writeValueSpin_ = new QSpinBox(operationBox);
    writeValueSpin_->setObjectName(QStringLiteral("writeValueSpin"));
    writeValueSpin_->setRange(0, 65535);
    writeValueSpin_->setValue(1234);

    readOnceButton_ = new QPushButton(QStringLiteral("读取一次"), operationBox);
    readOnceButton_->setObjectName(QStringLiteral("readOnceButton"));
    writeButton_ = new QPushButton(QStringLiteral("写入寄存器"), operationBox);
    writeButton_->setObjectName(QStringLiteral("writeButton"));

    operationLayout->addRow(new QLabel(QStringLiteral("起始地址:"), operationBox), addressSpin_);
    operationLayout->addRow(new QLabel(QStringLiteral("寄存器个数:"), operationBox), countSpin_);
    operationLayout->addRow(new QLabel(QStringLiteral("写入数值:"), operationBox), writeValueSpin_);
    operationLayout->addRow(readOnceButton_);
    operationLayout->addRow(writeButton_);
    operationLayout->addRow(new QLabel(QStringLiteral("提示：写入前请先连接设备。"), operationBox));

    splitter->addWidget(registerView_);
    splitter->addWidget(operationBox);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    rootLayout->addWidget(splitter, 1);

    // —— 底部：日志 ——
    auto* logBox = new QGroupBox(QStringLiteral("运行日志"), central);
    auto* logLayout = new QVBoxLayout(logBox);
    logView_ = new QPlainTextEdit(logBox);
    logView_->setObjectName(QStringLiteral("logView"));
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);  // 防止长时间运行吃掉内存
    clearLogButton_ = new QPushButton(QStringLiteral("清空日志"), logBox);
    clearLogButton_->setObjectName(QStringLiteral("clearLogButton"));
    logLayout->addWidget(logView_);
    logLayout->addWidget(clearLogButton_, 0, Qt::AlignRight);

    rootLayout->addWidget(logBox, 1);

    setCentralWidget(central);

    // —— 状态栏：连接状态与统计 ——
    stateLabel_ = new QLabel(QStringLiteral("未连接"), this);
    stateLabel_->setObjectName(QStringLiteral("stateLabel"));
    statsLabel_ = new QLabel(QStringLiteral("请求 0 / 响应 0 / 超时 0"), this);
    statsLabel_->setObjectName(QStringLiteral("statsLabel"));
    statusBar()->addWidget(stateLabel_);
    statusBar()->addPermanentWidget(statsLabel_);

    // —— 接线 ——
    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(disconnectButton_, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    connect(readOnceButton_, &QPushButton::clicked, this, &MainWindow::onReadOnceClicked);
    connect(writeButton_, &QPushButton::clicked, this, &MainWindow::onWriteClicked);
    connect(clearLogButton_, &QPushButton::clicked, this, &MainWindow::onClearLogClicked);
    connect(deviceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onDeviceComboChanged);

    // 定时器负责推进 DeviceSession 的时钟（超时判定、自动轮询都靠它）。
    // 100ms 一次：既够精确地体现 1s 超时，又不会空转烧 CPU。
    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(100);
    connect(tickTimer_, &QTimer::timeout, this, &MainWindow::onTimerTick);
    tickTimer_->start();

    resize(1100, 720);
}

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    QAction* quitAction = fileMenu->addAction(QStringLiteral("退出(&Q)"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("关于(&A)"));
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(
            this, QStringLiteral("关于"),
            QStringLiteral("Modbus 网关\n工业设备数据采集（阶段 5：界面层）\nQt %1")
                .arg(QString::fromLatin1(qVersion())));
    });
}

void MainWindow::loadDevices() {
    // 从 config/gateway.json 读设备；解析失败则用内置示例，保证界面一定能用。
    //
    // 工作目录可能不是仓库根（比如从 IDE 启动），因此按几个常见位置依次找：
    // 当前目录、可执行文件所在目录的上一级。找到就用真实的配置。
    const QStringList candidates = {
        QStringLiteral("config/gateway.json"),
        QApplication::applicationDirPath() + QStringLiteral("/config/gateway.json"),
        QApplication::applicationDirPath() + QStringLiteral("/../config/gateway.json"),
    };

    gateway::config::GatewayConfig parsed;
    std::string error;
    bool loaded = false;
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (!info.exists()) {
            continue;
        }
        if (gateway::config::loadGatewayConfig(candidate.toStdString(), parsed, error)) {
            configs_ = parsed.devices;
            appendLog(QStringLiteral("已从 %1 载入 %2 台设备")
                          .arg(info.absoluteFilePath())
                          .arg(configs_.size()));
            loaded = true;
            break;
        }
        appendLog(QStringLiteral("解析 %1 失败：%2")
                      .arg(info.absoluteFilePath())
                      .arg(QString::fromStdString(error)));
    }
    if (!loaded) {
        appendLog(QStringLiteral("未找到可用的 gateway.json，使用内置示例设备。"));
    }

    if (configs_.empty()) {
        gateway::device::DeviceConfig fallback;
        fallback.name = "sim-meter-01";
        fallback.transport = gateway::transport::TransportKind::Tcp;
        fallback.host = "127.0.0.1";
        fallback.port = 5020;
        fallback.unitId = 1;
        fallback.timeoutMs = 1000;
        fallback.retry = 2;
        fallback.pollIntervalMs = 500;
        gateway::device::PollPoint point;
        point.functionCode = 0x03;
        point.address = 0;
        point.count = 10;
        fallback.points.push_back(point);
        configs_.push_back(fallback);
    }

    deviceCombo_->clear();
    for (const auto& config : configs_) {
        deviceCombo_->addItem(QStringLiteral("%1 (%2)")
                                  .arg(QString::fromStdString(config.name))
                                  .arg(transportKindLabel(config.transport)));
    }

    if (!configs_.empty()) {
        applyConfigToControls(configs_.front());
    }
    appendLog(QStringLiteral("准备就绪：共 %1 台设备").arg(configs_.size()));
}

void MainWindow::applyConfigToControls(const gateway::device::DeviceConfig& config) {
    hostEdit_->setText(QString::fromStdString(config.host));
    portSpin_->setValue(config.port);
    unitIdSpin_->setValue(config.unitId);
    timeoutSpin_->setValue(config.timeoutMs);
    if (!config.points.empty()) {
        addressSpin_->setValue(config.points.front().address);
        countSpin_->setValue(config.points.front().count);
    }
}

void MainWindow::onDeviceComboChanged(int index) {
    if (index < 0 || index >= static_cast<int>(configs_.size())) {
        return;
    }
    if (connected_) {
        appendLog(QStringLiteral("切换设备前请先断开当前连接。"));
        return;
    }
    applyConfigToControls(configs_[static_cast<std::size_t>(index)]);
}

void MainWindow::onConnectClicked() {
    const int index = deviceCombo_->currentIndex();
    if (index < 0 || index >= static_cast<int>(configs_.size())) {
        return;
    }

    gateway::device::DeviceConfig config = configs_[static_cast<std::size_t>(index)];
    // 界面上的链路参数优先于配置文件：现场调试时改的是眼前这几个框。
    config.host = hostEdit_->text().toStdString();
    config.port = static_cast<uint16_t>(portSpin_->value());
    config.unitId = static_cast<uint8_t>(unitIdSpin_->value());
    config.timeoutMs = static_cast<uint16_t>(timeoutSpin_->value());

    std::string error;
    auto transport = gateway::transport::createTransport(config.toTransportConfig(), error);
    if (!transport) {
        appendLog(QStringLiteral("无法创建链路：%1").arg(QString::fromStdString(error)));
        QMessageBox::warning(this, QStringLiteral("连接失败"),
                             QString::fromStdString(error));
        return;
    }

    manager_ = std::make_unique<gateway::device::DeviceManager>();
    manager_->setResultCallback(
        [this](const std::string& name, const gateway::device::TransactionResult& result) {
            onResult(QString::fromStdString(name), result);
        });

    gateway::device::DeviceSession* session = manager_->addDevice(config, std::move(transport));
    if (session == nullptr) {
        appendLog(QStringLiteral("注册设备失败。"));
        return;
    }

    registerModel_->clear();
    if (!manager_->startAll()) {
        appendLog(QStringLiteral("启动链路失败，请检查配置。"));
    } else {
        appendLog(QStringLiteral("正在连接 %1 ...").arg(QString::fromStdString(config.name)));
    }
    setConnected(true);
    updateControlsEnabled();
}

void MainWindow::onDisconnectClicked() {
    if (manager_) {
        manager_->stopAll();
    }
    setConnected(false);
    updateControlsEnabled();
    appendLog(QStringLiteral("已断开连接。"));
}

void MainWindow::onReadOnceClicked() {
    gateway::device::DeviceSession* session = currentSession();
    if (session == nullptr) {
        appendLog(QStringLiteral("未找到设备会话。"));
        return;
    }
    const uint16_t address = static_cast<uint16_t>(addressSpin_->value());
    const uint16_t count = static_cast<uint16_t>(countSpin_->value());
    if (!session->requestReadHoldingRegisters(address, count)) {
        appendLog(QStringLiteral("请求被拒绝（可能有在途事务，或参数越界）。"));
        return;
    }
    appendLog(QStringLiteral("→ 读保持寄存器：地址 %1，数量 %2").arg(address).arg(count));
}

void MainWindow::onWriteClicked() {
    gateway::device::DeviceSession* session = currentSession();
    if (session == nullptr) {
        appendLog(QStringLiteral("未找到设备会话。"));
        return;
    }
    const uint16_t address = static_cast<uint16_t>(addressSpin_->value());
    const uint16_t value = static_cast<uint16_t>(writeValueSpin_->value());
    if (!session->requestWriteSingleRegister(address, value)) {
        appendLog(QStringLiteral("写请求被拒绝（可能有在途事务）。"));
        return;
    }
    appendLog(QStringLiteral("→ 写单个寄存器：地址 %1，数值 %2").arg(address).arg(value));
}

gateway::device::DeviceSession* MainWindow::currentSession() {
    if (!manager_) {
        return nullptr;
    }
    const int index = deviceCombo_->currentIndex();
    if (index < 0 || index >= static_cast<int>(configs_.size())) {
        return nullptr;
    }
    return manager_->session(configs_[static_cast<std::size_t>(index)].name);
}

void MainWindow::onClearLogClicked() { logView_->clear(); }

void MainWindow::onTimerTick() {
    if (!manager_) {
        return;
    }
    manager_->tick(nowMs());
    refreshStats();
}

void MainWindow::onResult(const QString& deviceName,
                          const gateway::device::TransactionResult& result) {
    const uint64_t stamp = nowMs();

    if (result.timedOut) {
        appendLog(QStringLiteral("✗ %1 超时：%2")
                      .arg(deviceName)
                      .arg(QString::fromStdString(result.error)));
        return;
    }
    if (result.exception != gateway::protocol::ExceptionCode::None) {
        appendLog(QStringLiteral("⚠ %1 设备异常：%2")
                      .arg(deviceName)
                      .arg(QString::fromStdString(result.error)));
        return;
    }
    if (!result.valid) {
        appendLog(QStringLiteral("✗ %1 失败：%2")
                      .arg(deviceName)
                      .arg(QString::fromStdString(result.error)));
        return;
    }

    if (result.functionCode == 0x03 || result.functionCode == 0x04) {
        const int updated = registerModel_->applyReadResult(result, stamp, deviceName);
        appendLog(QStringLiteral("✓ %1 读到 %2 个寄存器（地址 %3 起），表格更新 %4 行")
                      .arg(deviceName)
                      .arg(result.values.size())
                      .arg(result.address)
                      .arg(updated));
    } else if (result.functionCode == 0x06) {
        const bool touched = registerModel_->applySingleValue(result.address, result.value, stamp,
                                                             deviceName);
        appendLog(QStringLiteral("✓ %1 写入成功：地址 %2 = %3%4")
                      .arg(deviceName)
                      .arg(result.address)
                      .arg(result.value)
                      .arg(touched ? QString() : QStringLiteral("（该地址尚未读入表格）")));
    }
    refreshStats();
}

void MainWindow::refreshStats() {
    if (!manager_) {
        return;
    }
    const int index = deviceCombo_->currentIndex();
    if (index < 0 || index >= static_cast<int>(configs_.size())) {
        return;
    }
    gateway::device::DeviceSession* session =
        manager_->session(configs_[static_cast<std::size_t>(index)].name);
    if (session == nullptr) {
        return;
    }
    const auto& stats = session->stats();
    statsLabel_->setText(QStringLiteral("请求 %1 / 响应 %2 / 超时 %3 / 重传 %4 / 异常 %5")
                             .arg(stats.requests)
                             .arg(stats.responses)
                             .arg(stats.timeouts)
                             .arg(stats.retries)
                             .arg(stats.exceptions));

    const auto state = manager_->state(configs_[static_cast<std::size_t>(index)].name);
    QString text;
    switch (state) {
        case gateway::transport::TransportState::Open:
            text = QStringLiteral("已连接");
            break;
        case gateway::transport::TransportState::Connecting:
            text = QStringLiteral("连接中...");
            break;
        case gateway::transport::TransportState::Error:
            text = QStringLiteral("链路错误");
            break;
        case gateway::transport::TransportState::Closed:
            text = QStringLiteral("未连接");
            break;
    }
    stateLabel_->setText(text);
}

void MainWindow::setConnected(bool connected) { connected_ = connected; }

void MainWindow::updateControlsEnabled() {
    connectButton_->setEnabled(!connected_);
    disconnectButton_->setEnabled(connected_);
    readOnceButton_->setEnabled(connected_);
    writeButton_->setEnabled(connected_);
    deviceCombo_->setEnabled(!connected_);
    hostEdit_->setEnabled(!connected_);
    portSpin_->setEnabled(!connected_);
    unitIdSpin_->setEnabled(!connected_);
}

void MainWindow::appendLog(const QString& line) {
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    logView_->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, line));
}

}  // namespace gateway::ui
