#ifdef QT_CORE_LIB
#include <QApplication>
#include <QFile>
#include <QDebug>
#include "gateway/ui/MainWindow.h"
#include "gateway/util/AppConfig.h"
#include "gateway/util/Logger.h"
#include "gateway/storage/SqliteStorage.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("ModbusIndustrialGateway");
    app.setApplicationVersion("2.1.0");

    // Modern professional Dark Theme stylesheet
    QString darkQss = R"(
        QMainWindow, QWidget {
            background-color: #1e1e1e;
            color: #d4d4d4;
            font-family: "Segoe UI", Arial, sans-serif;
            font-size: 13px;
        }
        QTabWidget::pane {
            border: 1px solid #333333;
            background-color: #1e1e1e;
        }
        QTabBar::tab {
            background: #2d2d2d;
            color: #cccccc;
            padding: 8px 16px;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: #007acc;
            color: white;
            font-weight: bold;
        }
        QTableView, QTableWidget {
            background-color: #252526;
            alternate-background-color: #2d2d30;
            gridline-color: #3e3e42;
            color: #ffffff;
            selection-background-color: #094771;
            border: 1px solid #3e3e42;
        }
        QHeaderView::section {
            background-color: #2d2d2d;
            color: #ffffff;
            font-weight: bold;
            padding: 5px;
            border: 1px solid #3e3e42;
        }
        QPushButton {
            background-color: #0e639c;
            color: white;
            border: none;
            padding: 6px 14px;
            border-radius: 3px;
        }
        QPushButton:hover {
            background-color: #1177bb;
        }
        QPushButton:pressed {
            background-color: #094771;
        }
        QLineEdit, QSpinBox {
            background-color: #3c3c3c;
            border: 1px solid #555555;
            color: white;
            padding: 4px;
            border-radius: 2px;
        }
        QGroupBox {
            border: 1px solid #3e3e42;
            border-radius: 4px;
            margin-top: 10px;
            font-weight: bold;
            color: #007acc;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
        }
        QMenuBar {
            background-color: #2d2d2d;
            color: #cccccc;
        }
        QMenuBar::item:selected {
            background-color: #3e3e42;
        }
        QStatusBar {
            background-color: #007acc;
            color: white;
            font-weight: bold;
        }
    )";
    app.setStyleSheet(darkQss);

    gateway::device::GatewayAppConfig config;
    gateway::util::AppConfig::loadFromFile("config/gateway.json", config);

    auto devManager = std::make_shared<gateway::device::DeviceManager>();
    auto storage = std::make_shared<gateway::storage::SqliteStorage>(config.storage.database, config.storage.batchSize);
    storage->init();

    gateway::ui::MainWindow mainWindow(config, devManager, storage);
    mainWindow.show();

    return app.exec();
}
#else
#include <iostream>
int main() {
    std::cout << "Qt6 GUI requires Qt6 desktop installation. Use 'gateway_cli.exe' for headless daemon mode.\n";
    return 0;
}
#endif
