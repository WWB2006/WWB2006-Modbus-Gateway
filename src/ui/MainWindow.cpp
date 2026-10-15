#ifdef QT_CORE_LIB
#include "gateway/ui/MainWindow.h"
#include "gateway/util/Logger.h"
#include "gateway/util/Metrics.h"
#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHeaderView>

namespace gateway::ui {

MainWindow::MainWindow(device::GatewayAppConfig config,
                       std::shared_ptr<device::DeviceManager> devManager,
                       std::shared_ptr<storage::RecordRepository> storage,
                       QWidget* parent)
    : QMainWindow(parent), m_config(std::move(config)),
      m_devManager(std::move(devManager)), m_storage(std::move(storage)) {

    m_scheduler = std::make_unique<device::PollScheduler>(*m_devManager, *m_storage);

    setupUi();
    setupMenus();

    // Hook scheduler callbacks to GUI via Qt::QueuedConnection
    m_scheduler->setRecordCallback([this](const storage::CommunicationRecord& rec) {
        QMetaObject::invokeMethod(this, [this, rec]() {
            this->onRecordReceived(rec);
        }, Qt::QueuedConnection);
    });

    m_scheduler->setStateCallback([this](const std::string& dev, device::DeviceState st) {
        QString qdev = QString::fromStdString(dev);
        QString qstate = QString::fromStdString(device::deviceStateToString(st));
        QMetaObject::invokeMethod(this, [this, qdev, qstate]() {
            this->onDeviceStateChanged(qdev, qstate);
        }, Qt::QueuedConnection);
    });

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::onTimerRefresh);
    m_refreshTimer->start(500); // 2Hz refresh for metrics & charts
}

MainWindow::~MainWindow() {
    if (m_scheduler && m_scheduler->isRunning()) {
        m_scheduler->stop();
    }
}

void MainWindow::setupUi() {
    setWindowTitle("Modbus Industrial Gateway & Automated Testing Platform - Qt6");
    resize(1050, 700);

    m_tabWidget = new QTabWidget(this);

    // Tab 1: Device Management
    m_devicePanel = new DevicePanel(this);
    m_devicePanel->populateDevices(m_config.devices);
    connect(m_devicePanel, &DevicePanel::startPollingRequested, this, &MainWindow::onStartPolling);
    connect(m_devicePanel, &DevicePanel::stopPollingRequested, this, &MainWindow::onStopPolling);
    connect(m_devicePanel, &DevicePanel::writeRegisterRequested, this, &MainWindow::onWriteRegister);
    connect(m_devicePanel, &DevicePanel::readRegisterRequested, this, &MainWindow::onReadRegister);
    m_tabWidget->addTab(m_devicePanel, "Device Management");

    // Tab 2: Registers & Data Points
    auto regContainer = new QWidget(this);
    auto regLayout = new QVBoxLayout(regContainer);
    m_regModel = new RegisterTableModel(this);
    m_regTableView = new QTableView(this);
    m_regTableView->setModel(m_regModel);
    m_regTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_regTableView->setAlternatingRowColors(true);
    regLayout->addWidget(m_regTableView);
    m_tabWidget->addTab(regContainer, "Real-Time Registers");

    // Tab 3: Real-Time Charts
    auto chartContainer = new QWidget(this);
    auto chartLayout = new QVBoxLayout(chartContainer);
    m_chartPanel = new ChartPanel(this);
    chartLayout->addWidget(m_chartPanel);
    m_tabWidget->addTab(chartContainer, "Latency Charts");

    // Tab 4: Logs
    m_logView = new LogView(this);
    m_tabWidget->addTab(m_logView, "Communication Logs");

    setCentralWidget(m_tabWidget);
    statusBar()->showMessage("Gateway ready. Press 'Start Polling' to initiate acquisition.");
}

void MainWindow::setupMenus() {
    auto fileMenu = menuBar()->addMenu("&File");
    auto exportAction = fileMenu->addAction("&Export CSV Records...");
    connect(exportAction, &QAction::triggered, this, &MainWindow::onExportCsv);

    fileMenu->addSeparator();
    auto exitAction = fileMenu->addAction("E&xit");
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto helpMenu = menuBar()->addMenu("&Help");
    auto aboutAction = helpMenu->addAction("&About Modbus Gateway");
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, "About Modbus Gateway",
            "Modbus Industrial Gateway & Automated Testing Platform\n"
            "C++17 / Qt 6 Cross-Platform Implementation\n"
            "Features: Modbus TCP/RTU, WAL SQLite Storage, Fault Injection Engine");
    });
}

void MainWindow::onStartPolling() {
    if (m_scheduler->start(m_config.devices)) {
        statusBar()->showMessage("Polling active on all configured devices.");
        m_logView->appendLog("[SYSTEM] Started poll scheduler.");
    }
}

void MainWindow::onStopPolling() {
    m_scheduler->stop();
    statusBar()->showMessage("Polling stopped. System idle.");
    m_logView->appendLog("[SYSTEM] Stopped poll scheduler.");
}

void MainWindow::onRecordReceived(const storage::CommunicationRecord& rec) {
    m_regModel->updateOrAddRegister(rec.deviceName, rec.functionCode, rec.address, rec.valuesStr, rec.status);
    m_chartPanel->addDataPoint(rec.latencyMs);

    QString logLine = QString("[%1] %2 Func=0x%3 Addr=%4 -> %5 (%6 ms)")
        .arg(QString::fromStdString(rec.deviceName))
        .arg(QString::fromStdString(rec.status))
        .arg(rec.functionCode, 2, 16, QChar('0')).toUpper()
        .arg(rec.address)
        .arg(QString::fromStdString(rec.valuesStr))
        .arg(rec.latencyMs, 0, 'f', 2);
    m_logView->appendLog(logLine);
}

void MainWindow::onDeviceStateChanged(const QString& name, const QString& state) {
    m_devicePanel->updateDeviceStatus(name, state);
    m_logView->appendLog(QString("[DEVICE] %1 state -> %2").arg(name, state));
}

void MainWindow::onWriteRegister(const QString& devName, uint16_t addr, uint16_t val) {
    storage::CommunicationRecord rec;
    bool ok = m_devManager->writeSingleRegister(devName.toStdString(), addr, val, rec);
    m_logView->appendLog(QString("[MANUAL WRITE] %1 Addr=%2 Val=%3 -> %4")
        .arg(devName).arg(addr).arg(val).arg(QString::fromStdString(rec.status)));
}

void MainWindow::onReadRegister(const QString& devName, uint16_t addr) {
    device::DataPointConfig pt{3, addr, 1};
    storage::CommunicationRecord rec;
    bool ok = m_devManager->pollPoint(devName.toStdString(), pt, rec);
    onRecordReceived(rec);
}

void MainWindow::onExportCsv() {
    QString fileName = QFileDialog::getSaveFileName(this, "Export Records to CSV", "records_export.csv", "CSV Files (*.csv)");
    if (!fileName.isEmpty()) {
        storage::QueryFilter f;
        if (m_storage->exportCsv(fileName.toStdString(), f)) {
            QMessageBox::information(this, "Export Succeeded", "Records exported successfully to " + fileName);
        } else {
            QMessageBox::warning(this, "Export Failed", "Failed to write CSV file.");
        }
    }
}

void MainWindow::onTimerRefresh() {
    auto snap = util::Metrics::instance().snapshot();
    m_devicePanel->updateMetrics(snap.totalRequests, snap.successRate());
}

} // namespace gateway::ui
#endif
