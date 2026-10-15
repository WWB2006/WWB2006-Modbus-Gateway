#include "gateway/ui/RegisterTableModel.h"

#include <QBrush>
#include <QColor>
#include <QStringList>

#include <algorithm>

namespace gateway::ui {

RegisterTableModel::RegisterTableModel(QObject* parent) : QAbstractTableModel(parent) {}

int RegisterTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(cells_.size());
}

int RegisterTableModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return ColumnCount;
}

QVariant RegisterTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(cells_.size())) {
        return {};
    }
    const Cell& cell = cells_[static_cast<std::size_t>(index.row())];

    if (role == Qt::TextAlignmentRole) {
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }

    if (role == Qt::BackgroundRole) {
        // 数据来源不同设备时用不同底色，方便一眼看出这一行是谁采的。
        // 色值是淡色，保证深/浅主题下文字都可读。
        if (cell.valid && !cell.source.isEmpty()) {
            const int hash = qHash(cell.source) % 3;
            switch (hash) {
                case 0:
                    return QBrush(QColor(235, 245, 255));
                case 1:
                    return QBrush(QColor(255, 248, 235));
                default:
                    return QBrush(QColor(240, 255, 240));
            }
        }
        return {};
    }

    if (role != Qt::DisplayRole) {
        return {};
    }

    switch (index.column()) {
        case AddressColumn:
            return QStringLiteral("%1 (0x%2)")
                .arg(cell.address)
                .arg(cell.address, 4, 16, QLatin1Char('0'))
                .toUpper();
        case ValueColumn:
            return cell.valid ? QString::number(cell.value) : QStringLiteral("--");
        case HexColumn:
            return cell.valid ? QStringLiteral("0x%1")
                                    .arg(cell.value, 4, 16, QLatin1Char('0'))
                                    .toUpper()
                              : QStringLiteral("--");
        case UpdatedColumn:
            if (!cell.valid || cell.updatedMs == 0) {
                return QStringLiteral("--");
            }
            // 只显示「距上次更新多少毫秒」，比绝对时间更适合观察刷新频率。
            return QStringLiteral("%1 ms").arg(cell.updatedMs);
        default:
            return {};
    }
}

QVariant RegisterTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (orientation == Qt::Vertical) {
        return section;
    }
    switch (section) {
        case AddressColumn:
            return QStringLiteral("地址");
        case ValueColumn:
            return QStringLiteral("数值");
        case HexColumn:
            return QStringLiteral("十六进制");
        case UpdatedColumn:
            return QStringLiteral("最近更新");
        default:
            return {};
    }
}

int RegisterTableModel::rowForAddress(uint16_t address) const {
    const auto it = std::lower_bound(
        cells_.begin(), cells_.end(), address,
        [](const Cell& cell, uint16_t value) { return cell.address < value; });
    if (it == cells_.end() || it->address != address) {
        return -1;
    }
    return static_cast<int>(std::distance(cells_.begin(), it));
}

int RegisterTableModel::applyReadResult(const gateway::device::TransactionResult& result,
                                        uint64_t nowMs, const QString& sourceName) {
    if (!result.valid || result.values.empty()) {
        return 0;
    }
    // 结果里的 values[0] 对应 result.address，依次递增。
    // 按地址定位插入位置：读操作的地址范围通常连续，但多次读不同区间后表格里
    // 会出现空洞，因此必须逐地址插入而不是整段追加。
    int updated = 0;
    for (std::size_t i = 0; i < result.values.size(); ++i) {
        const uint16_t address = static_cast<uint16_t>(result.address + i);
        int row = rowForAddress(address);
        if (row < 0) {
            // 新地址：找到应插入的位置，保持升序。
            const auto it = std::lower_bound(
                cells_.begin(), cells_.end(), address,
                [](const Cell& cell, uint16_t value) { return cell.address < value; });
            const int insertAt = static_cast<int>(std::distance(cells_.begin(), it));
            beginInsertRows(QModelIndex(), insertAt, insertAt);
            Cell cell;
            cell.address = address;
            cells_.insert(it, cell);
            endInsertRows();
            row = insertAt;
        }
        Cell& cell = cells_[static_cast<std::size_t>(row)];
        cell.value = result.values[i];
        cell.updatedMs = nowMs;
        cell.source = sourceName;
        cell.valid = true;
        const QModelIndex topLeft = index(row, ValueColumn);
        const QModelIndex bottomRight = index(row, UpdatedColumn);
        emit dataChanged(topLeft, bottomRight);
        ++updated;
    }
    return updated;
}

bool RegisterTableModel::applySingleValue(uint16_t address, uint16_t value, uint64_t nowMs,
                                          const QString& sourceName) {
    const int row = rowForAddress(address);
    if (row < 0) {
        return false;
    }
    Cell& cell = cells_[static_cast<std::size_t>(row)];
    cell.value = value;
    cell.updatedMs = nowMs;
    cell.source = sourceName;
    cell.valid = true;
    emit dataChanged(index(row, ValueColumn), index(row, UpdatedColumn));
    return true;
}

void RegisterTableModel::clear() {
    if (cells_.empty()) {
        return;
    }
    beginResetModel();
    cells_.clear();
    endResetModel();
}

std::vector<uint16_t> RegisterTableModel::snapshot() const {
    std::vector<uint16_t> values;
    values.reserve(cells_.size());
    for (const Cell& cell : cells_) {
        values.push_back(cell.valid ? cell.value : 0);
    }
    return values;
}

}  // namespace gateway::ui
