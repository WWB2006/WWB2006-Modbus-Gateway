// 界面入口：只创建空窗口并打印 Qt 版本，用来验证工具链与 AUTOMOC 配置。
// 自阶段 0 起未改动 —— 真正的界面要到阶段 5 才出现在这个文件里。

#include <QApplication>
#include <QLabel>
#include <QString>
#include <QtGlobal>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QLabel label;
    label.setAlignment(Qt::AlignCenter);
    label.setText(QStringLiteral("Modbus Gateway\n阶段 0 工程骨架\nQt %1")
                      .arg(QString::fromLatin1(qVersion())));
    label.resize(420, 180);
    label.show();

    return app.exec();
}
