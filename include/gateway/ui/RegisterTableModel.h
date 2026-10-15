#pragma once

#include <QAbstractTableModel>
#include <QString>
#include <cstdint>
#include <vector>

#include "gateway/device/DeviceSession.h"

namespace gateway::ui {

// 寄存器表格模型：把一次读取回来的寄存器值展示成「地址 / 值 / 十六进制 / 更新时间」四列。
//
// 为什么用自定义模型而不是 QTableWidget：
//   1. 寄存器是「按地址索引」的连续空间，用模型可以直接按地址定位、局部刷新一行；
//   2. 阶段 7 的实时曲线要拿同一份数据源，模型天然可被多个视图共用；
//   3. QTableWidget 每个格子都是一个 QTableWidgetItem 对象，万级寄存器会吃掉大量内存。
//
// 这个模型只负责「存值 + 通知视图」，不做任何协议解析 —— 数据由 MainWindow 从
// DeviceSession 的结果回调里喂进来。
class RegisterTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        AddressColumn = 0,
        ValueColumn,
        HexColumn,
        UpdatedColumn,
        ColumnCount,
    };

    explicit RegisterTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    // 用一次读操作的结果刷新表格。返回值是本次更新的寄存器个数。
    int applyReadResult(const gateway::device::TransactionResult& result, uint64_t nowMs,
                        const QString& sourceName);

    // 只更新一个寄存器（写操作的回显）。地址不存在时忽略。
    bool applySingleValue(uint16_t address, uint16_t value, uint64_t nowMs,
                          const QString& sourceName);

    void clear();

    // 供曲线视图取数据：返回按地址升序的值序列。
    std::vector<uint16_t> snapshot() const;

private:
    struct Cell {
        uint16_t address = 0;
        uint16_t value = 0;
        uint64_t updatedMs = 0;
        QString source;
        bool valid = false;
    };

    // 地址 -> 行号 的查找。表格按地址升序排列，二分即可。
    int rowForAddress(uint16_t address) const;

    std::vector<Cell> cells_;
};

}  // namespace gateway::ui
