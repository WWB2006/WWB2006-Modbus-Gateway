#pragma once

#ifdef QT_CORE_LIB
#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QSpinBox>
#include <QLineEdit>
#include "gateway/device/DeviceConfig.h"
#include "gateway/device/DeviceManager.h"

namespace gateway::ui {

class DevicePanel : public QWidget {
    Q_OBJECT
public:
    explicit DevicePanel(QWidget* parent = nullptr);

    void populateDevices(const std::vector<device::DeviceConfig>& devices);
    void updateDeviceStatus(const QString& devName, const QString& status);
    void updateMetrics(uint64_t total, double successRate);

signals:
    void startPollingRequested();
    void stopPollingRequested();
    void writeRegisterRequested(const QString& devName, uint16_t addr, uint16_t val);
    void readRegisterRequested(const QString& devName, uint16_t addr);

private:
    void setupUi();

    QTableWidget* m_deviceTable;
    QPushButton* m_startBtn;
    QPushButton* m_stopBtn;
    QLabel* m_statusLabel;

    QLineEdit* m_devNameEdit;
    QSpinBox* m_addrSpin;
    QSpinBox* m_valSpin;
    QPushButton* m_writeBtn;
    QPushButton* m_readBtn;
};

} // namespace gateway::ui
#endif
