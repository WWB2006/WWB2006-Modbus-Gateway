#pragma once

#include <QMainWindow>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

#include "gateway/device/DeviceConfig.h"
#include "gateway/device/DeviceManager.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableView;
class QTimer;

namespace gateway::ui {

class RegisterTableModel;

// 主窗口：设备面板 + 寄存器表 + 运行日志三块。
//
// 界面层刻意做得很薄 —— 所有逻辑都在 device / protocol / sim 里，
// 这里只做三件事：把配置变成 DeviceManager 的调用、把结果回调转成界面更新、
// 把日志追加到文本框。这样界面层可以整个替换掉（阶段 11 的无界面网关就是
// 去掉这一层、直接驱动 DeviceManager）。
//
// 依赖方向：ui -> device -> transport -> protocol，界面只能调用下层，下层绝不反向依赖界面。
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onReadOnceClicked();
    void onWriteClicked();
    void onClearLogClicked();
    void onTimerTick();
    void onDeviceComboChanged(int index);

private:
    void buildUi();
    void buildMenus();
    // 把 config/gateway.json 里的设备读进来；失败时用内置示例兜底。
    void loadDevices();
    void appendLog(const QString& line);
    void refreshStats();
    void setConnected(bool connected);
    // 控件可用状态随连接状态变化，集中在一处改，避免漏改某个按钮。
    void updateControlsEnabled();
    // 选择设备后把该设备的链路参数填进控件。
    void applyConfigToControls(const gateway::device::DeviceConfig& config);
    // 设备层结果回调的界面侧处理：按结果类型更新表格与日志。
    void onResult(const QString& deviceName,
                  const gateway::device::TransactionResult& result);
    // 当前选中设备对应的会话；没有则返回 nullptr。
    gateway::device::DeviceSession* currentSession();

    // —— 界面控件（阶段 5 要求至少十个可操作元素）——
    QComboBox* deviceCombo_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QSpinBox* addressSpin_ = nullptr;
    QSpinBox* countSpin_ = nullptr;
    QSpinBox* writeValueSpin_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QSpinBox* unitIdSpin_ = nullptr;
    QSpinBox* timeoutSpin_ = nullptr;
    QPushButton* readOnceButton_ = nullptr;
    QPushButton* writeButton_ = nullptr;
    QPushButton* clearLogButton_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QTableView* registerView_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;

    QTimer* tickTimer_ = nullptr;

    std::vector<gateway::device::DeviceConfig> configs_;
    std::unique_ptr<gateway::device::DeviceManager> manager_;
    std::unique_ptr<RegisterTableModel> registerModel_;
    uint64_t startedMs_ = 0;
    bool connected_ = false;
};

}  // namespace gateway::ui
