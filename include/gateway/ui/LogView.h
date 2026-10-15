#pragma once

#ifdef QT_CORE_LIB
#include <QWidget>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QPushButton>
#include <QHBoxLayout>

namespace gateway::ui {

class LogView : public QWidget {
    Q_OBJECT
public:
    explicit LogView(QWidget* parent = nullptr, int maxLines = 500);

    void appendLog(const QString& line);
    void clear();

private:
    QPlainTextEdit* m_textEdit;
    int m_maxLines;
};

} // namespace gateway::ui
#endif
