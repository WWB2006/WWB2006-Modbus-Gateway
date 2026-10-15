#ifdef QT_CORE_LIB
#include "gateway/ui/RegisterTableModel.h"
#include <QDateTime>
#include <QColor>

namespace gateway::ui {

RegisterTableModel::RegisterTableModel(QObject* parent)
    : QAbstractTableModel(parent) {
}

int RegisterTableModel::rowCount(const QModelIndex&) const {
    return static_cast<int>(m_rows.size());
}

int RegisterTableModel::columnCount(const QModelIndex&) const {
    return 6; // Device, Function, Address, Value, Status, Last Updated
}

QVariant RegisterTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(m_rows.size())) {
        return QVariant();
    }

    const auto& row = m_rows[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0: return QString::fromStdString(row.deviceName);
            case 1: return QString("0x%1").arg(row.functionCode, 2, 16, QChar('0')).toUpper();
            case 2: return QString::number(row.address);
            case 3: return QString::fromStdString(row.value);
            case 4: return QString::fromStdString(row.status);
            case 5: return QString::fromStdString(row.updateTime);
            default: return QVariant();
        }
    } else if (role == Qt::ForegroundRole && index.column() == 4) {
        if (row.status == "OK") {
            return QColor("#2ecc71"); // Green
        } else {
            return QColor("#e74c3c"); // Red
        }
    }

    return QVariant();
}

QVariant RegisterTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
            case 0: return "Device";
            case 1: return "Function";
            case 2: return "Address";
            case 3: return "Value";
            case 4: return "Status";
            case 5: return "Last Updated";
            default: return QVariant();
        }
    }
    return QVariant();
}

void RegisterTableModel::updateOrAddRegister(const std::string& dev, uint8_t func, uint16_t addr, const std::string& val, const std::string& status) {
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    for (size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].deviceName == dev && m_rows[i].functionCode == func && m_rows[i].address == addr) {
            m_rows[i].value = val;
            m_rows[i].status = status;
            m_rows[i].updateTime = timeStr.toStdString();
            emit dataChanged(index(static_cast<int>(i), 0), index(static_cast<int>(i), 5));
            return;
        }
    }

    // Add new row
    beginInsertRows(QModelIndex(), static_cast<int>(m_rows.size()), static_cast<int>(m_rows.size()));
    RegisterRow r;
    r.deviceName = dev;
    r.functionCode = func;
    r.address = addr;
    r.value = val;
    r.status = status;
    r.updateTime = timeStr.toStdString();
    m_rows.push_back(r);
    endInsertRows();
}

void RegisterTableModel::clear() {
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

} // namespace gateway::ui
#endif
