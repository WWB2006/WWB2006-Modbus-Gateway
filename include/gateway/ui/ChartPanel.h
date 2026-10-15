#pragma once

#ifdef QT_CORE_LIB
#include <QWidget>
#include <vector>
#include <mutex>
#include <deque>

namespace gateway::ui {

class ChartPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChartPanel(QWidget* parent = nullptr, size_t maxPoints = 200);

    void addDataPoint(double latencyMs);
    void clear();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    size_t m_maxPoints;
    std::deque<double> m_latencies;
    std::mutex m_mutex;
    double m_maxLatency = 50.0;
};

} // namespace gateway::ui
#endif
