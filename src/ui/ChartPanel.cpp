#ifdef QT_CORE_LIB
#include "gateway/ui/ChartPanel.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace gateway::ui {

ChartPanel::ChartPanel(QWidget* parent, size_t maxPoints)
    : QWidget(parent), m_maxPoints(maxPoints) {
    setMinimumHeight(150);
    setStyleSheet("background-color: #252526; border: 1px solid #3e3e42; border-radius: 4px;");
}

void ChartPanel::addDataPoint(double latencyMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_latencies.size() >= m_maxPoints) {
        m_latencies.pop_front();
    }
    m_latencies.push_back(latencyMs);

    double maxVal = 20.0;
    for (double val : m_latencies) {
        if (val > maxVal) maxVal = val;
    }
    m_maxLatency = maxVal * 1.2; // 20% margin
    update();
}

void ChartPanel::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_latencies.clear();
    m_maxLatency = 50.0;
    update();
}

void ChartPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int w = width();
    int h = height();
    int margin = 35;
    int plotW = w - margin - 15;
    int plotH = h - margin - 15;

    // Background
    painter.fillRect(rect(), QColor("#1e1e1e"));

    // Grid lines
    painter.setPen(QPen(QColor("#333333"), 1, Qt::DashLine));
    for (int i = 1; i <= 4; ++i) {
        int y = margin + (plotH * i) / 4;
        painter.drawLine(margin, y, margin + plotW, y);
        double val = m_maxLatency * (4 - i) / 4.0;
        painter.setPen(QColor("#888888"));
        painter.drawText(5, y + 4, QString::number(val, 'f', 1) + "ms");
        painter.setPen(QPen(QColor("#333333"), 1, Qt::DashLine));
    }

    // Axes
    painter.setPen(QPen(QColor("#555555"), 1));
    painter.drawLine(margin, margin, margin, margin + plotH);
    painter.drawLine(margin, margin + plotH, margin + plotW, margin + plotH);

    std::vector<double> points;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        points.assign(m_latencies.begin(), m_latencies.end());
    }

    if (points.size() < 2) {
        painter.setPen(QColor("#aaaaaa"));
        painter.drawText(rect(), Qt::AlignCenter, "Waiting for communication metrics...");
        return;
    }

    // Draw Curve
    QPainterPath path;
    double stepX = static_cast<double>(plotW) / (m_maxPoints - 1);
    size_t startOffset = m_maxPoints - points.size();

    for (size_t i = 0; i < points.size(); ++i) {
        double x = margin + (startOffset + i) * stepX;
        double normY = (m_maxLatency > 0.0) ? (points[i] / m_maxLatency) : 0.0;
        if (normY > 1.0) normY = 1.0;
        double y = margin + plotH - (normY * plotH);

        if (i == 0) {
            path.moveTo(x, y);
        } else {
            path.lineTo(x, y);
        }
    }

    painter.setPen(QPen(QColor("#007acc"), 2));
    painter.drawPath(path);

    // Current latency overlay text
    double lastVal = points.back();
    painter.setPen(QColor("#4ec9b0"));
    painter.drawText(w - 180, 25, QString("Current Latency: %1 ms").arg(lastVal, 0, 'f', 2));
}

} // namespace gateway::ui
#endif
