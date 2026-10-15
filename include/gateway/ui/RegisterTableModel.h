#pragma once

#ifdef QT_CORE_LIB
#include <QAbstractTableModel>
#include <vector>
#include <string>
#include <cstdint>

namespace gateway::ui {

struct RegisterRow {
    std::string deviceName;
    uint8_t functionCode = 3;
    uint16_t address = 0;
    std::string value;
    std::string updateTime;
    std::string status;
};

class RegisterTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit RegisterTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void updateOrAddRegister(const std::string& dev, uint8_t func, uint16_t addr, const std::string& val, const std::string& status);
    void clear();

private:
    std::vector<RegisterRow> m_rows;
};

} // namespace gateway::ui
#endif
