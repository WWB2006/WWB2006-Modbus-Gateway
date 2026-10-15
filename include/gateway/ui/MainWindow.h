#pragma once

#ifdef QT_CORE_LIB
#include <QMainWindow>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include "gateway/ui/DevicePanel.h"
#include "gateway/ui/RegisterTableModel.h"
#include "gateway/ui/ChartPanel.h"
#include "gateway/ui/LogView.h"
#include "gateway/device/DeviceManager.h"
#include "gateway/device/PollScheduler.h"
#include "gateway/storage/RecordRepository.h"
#include "gateway/util/AppConfig.h"

namespace gateway::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(device::GatewayAppConfig config,
                        std::shared_ptr<device::DeviceManager> devManager,
                        std::shared_ptr<storage::RecordRepository> storage,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onStartPolling();
    void onStopPolling();
    void onRecordReceived(const storage::CommunicationRecord& rec);
    void onDeviceStateChanged(const QString& name, const QString& state);
    void onWriteRegister(const QString& devName, uint16_t addr, uint16_t val);
    void onReadRegister(const QString& devName, uint16_t addr);
    void onExportCsv();
    void onTimerRefresh();

private:
    void setupUi();
    void setupMenus();

    device::GatewayAppConfig m_config;
    std::shared_ptr<device::DeviceManager> m_devManager;
    std::shared_ptr<storage::RecordRepository> m_storage;
    std::unique_ptr<device::PollScheduler> m_scheduler;

    QTabWidget* m_tabWidget;
    DevicePanel* m_devicePanel;
    RegisterTableModel* m_regModel;
    QTableView* m_regTableView;
    ChartPanel* m_chartPanel;
    LogView* m_logView;
    QTimer* m_refreshTimer;
};

} // namespace gateway::ui
#endif
