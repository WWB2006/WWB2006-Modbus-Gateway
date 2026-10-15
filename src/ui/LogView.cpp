#ifdef QT_CORE_LIB
#include "gateway/ui/LogView.h"
#include <QScrollBar>

namespace gateway::ui {

LogView::LogView(QWidget* parent, int maxLines)
    : QWidget(parent), m_maxLines(maxLines) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto topBar = new QHBoxLayout();
    auto clearBtn = new QPushButton("Clear Logs", this);
    connect(clearBtn, &QPushButton::clicked, this, &LogView::clear);
    topBar->addStretch();
    topBar->addWidget(clearBtn);

    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_textEdit->setMaximumBlockCount(m_maxLines);
    m_textEdit->setStyleSheet("background-color: #1e1e1e; color: #d4d4d4; font-family: Consolas, monospace; font-size: 11px;");

    layout->addLayout(topBar);
    layout->addWidget(m_textEdit);
}

void LogView::appendLog(const QString& line) {
    m_textEdit->appendPlainText(line);
    m_textEdit->verticalScrollBar()->setValue(m_textEdit->verticalScrollBar()->maximum());
}

void LogView::clear() {
    m_textEdit->clear();
}

} // namespace gateway::ui
#endif
