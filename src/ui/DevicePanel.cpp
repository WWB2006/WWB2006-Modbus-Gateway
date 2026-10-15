#ifdef QT_CORE_LIB
#include "gateway/ui/DevicePanel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>

namespace gateway::ui {

DevicePanel::DevicePanel(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void DevicePanel::setupUi() {
    auto mainLayout = new QVBoxLayout(this);

    // Toolbar
    auto toolbarLayout = new QHBoxLayout();
    m_startBtn = new QPushButton("Start Polling", this);
    m_startBtn->setStyleSheet("background-color: #2ecc71; color: white; font-weight: bold; padding: 6px 12px;");
    m_stopBtn = new QPushButton("Stop Polling", this);
    m_stopBtn->setStyleSheet("background-color: #e74c3c; color: white; font-weight: bold; padding: 6px 12px;");
    m_stopBtn->setEnabled(false);

    m_statusLabel = new QLabel("System Idle | Ready", this);
    m_statusLabel->setStyleSheet("font-weight: bold; color: #007acc;");

    toolbarLayout->addWidget(m_startBtn);
    toolbarLayout->addWidget(m_stopBtn);
    toolbarLayout->addSpacing(20);
    toolbarLayout->addWidget(m_statusLabel);
    toolbarLayout->addStretch();

    mainLayout->addLayout(toolbarLayout);

    // Device Table
    m_deviceTable = new QTableWidget(this);
    m_deviceTable->setColumnCount(6);
    m_deviceTable->setHorizontalHeaderLabels({"Device Name", "Transport", "Endpoint", "Unit ID", "Interval (ms)", "Status"});
    m_deviceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_deviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_deviceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);

    mainLayout->addWidget(m_deviceTable);

    // Manual Read/Write Group
    auto ioGroup = new QGroupBox("Manual Modbus Point Read / Write", this);
    auto ioLayout = new QHBoxLayout(ioGroup);

    m_devNameEdit = new QLineEdit("plc-01", this);
    m_devNameEdit->setPlaceholderText("Device Name");

    m_addrSpin = new QSpinBox(this);
    m_addrSpin->setRange(0, 65535);
    m_addrSpin->setPrefix("Addr: ");

    m_valSpin = new QSpinBox(this);
    m_valSpin->setRange(0, 65535);
    m_valSpin->setPrefix("Val: ");

    m_readBtn = new QPushButton("Read Register (03)", this);
    m_writeBtn = new QPushButton("Write Register (06)", this);

    ioLayout->addWidget(new QLabel("Device:", this));
    ioLayout->addWidget(m_devNameEdit);
    ioLayout->addWidget(m_addrSpin);
    ioLayout->addWidget(m_valSpin);
    ioLayout->addWidget(m_readBtn);
    ioLayout->addWidget(m_writeBtn);

    mainLayout->addWidget(ioGroup);

    // Signals
    connect(m_startBtn, &QPushButton::clicked, this, [this]() {
        m_startBtn->setEnabled(false);
        m_stopBtn->setEnabled(true);
        emit startPollingRequested();
    });

    connect(m_stopBtn, &QPushButton::clicked, this, [this]() {
        m_startBtn->setEnabled(true);
        m_stopBtn->setEnabled(false);
        emit stopPollingRequested();
    });

    connect(m_writeBtn, &QPushButton::clicked, this, [this]() {
        emit writeRegisterRequested(m_devNameEdit->text(), static_cast<uint16_t>(m_addrSpin->value()), static_cast<uint16_t>(m_valSpin->value()));
    });

    connect(m_readBtn, &QPushButton::clicked, this, [this]() {
        emit readRegisterRequested(m_devNameEdit->text(), static_cast<uint16_t>(m_addrSpin->value()));
    });
}

void DevicePanel::populateDevices(const std::vector<device::DeviceConfig>& devices) {
    m_deviceTable->setRowCount(static_cast<int>(devices.size()));
    for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
        const auto& d = devices[i];
        m_deviceTable->setItem(i, 0, new QTableWidgetItem(QString::fromStdString(d.name)));
        m_deviceTable->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(d.transport).toUpper()));
        QString ep = (d.transport == "tcp") ? QString::fromStdString(d.host) + ":" + QString::number(d.port)
                                            : QString::fromStdString(d.serialPort) + "@" + QString::number(d.baudRate);
        m_deviceTable->setItem(i, 2, new QTableWidgetItem(ep));
        m_deviceTable->setItem(i, 3, new QTableWidgetItem(QString::number(d.unitId)));
        m_deviceTable->setItem(i, 4, new QTableWidgetItem(QString::number(d.pollIntervalMs)));
        m_deviceTable->setItem(i, 5, new QTableWidgetItem("DISCONNECTED"));
    }
}

void DevicePanel::updateDeviceStatus(const QString& devName, const QString& status) {
    for (int i = 0; i < m_deviceTable->rowCount(); ++i) {
        if (m_deviceTable->item(i, 0)->text() == devName) {
            auto item = m_deviceTable->item(i, 5);
            item->setText(status);
            if (status == "CONNECTED") {
                item->setForeground(QColor("#2ecc71"));
            } else if (status == "CONNECTING") {
                item->setForeground(QColor("#f39c12"));
            } else {
                item->setForeground(QColor("#e74c3c"));
            }
            break;
        }
    }
}

void DevicePanel::updateMetrics(uint64_t total, double successRate) {
    m_statusLabel->setText(QString("Total Requests: %1 | Success Rate: %2%").arg(total).arg(successRate, 0, 'f', 1));
}

} // namespace gateway::ui
#endif
