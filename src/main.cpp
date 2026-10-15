// 界面入口（阶段 5 起）：创建主窗口。
//
// 阶段 0 时这个文件只是一个空 QLabel，用来验证工具链与 AUTOMOC；
// 阶段 5 换成真正的三栏界面（设备面板 / 寄存器表 / 运行日志）。
//
// 注意：这个入口只在编译出 Qt Widgets 时才存在（见 src/CMakeLists.txt）。
// 无图形环境的运行方式是 gateway_daemon（阶段 11）或 gateway_cli。

#include <QApplication>
#include <QString>
#include <QtGlobal>

#include "gateway/ui/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ModbusGateway"));
    app.setApplicationVersion(QStringLiteral("0.5.0-stage5"));
    app.setOrganizationName(QStringLiteral("WWB2006"));

    gateway::ui::MainWindow window;
    window.show();

    return app.exec();
}
