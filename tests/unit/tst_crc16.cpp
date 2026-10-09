#include <QtTest>

#include <vector>

#include "gateway/protocol/Crc16.h"
#include "gateway/protocol/ModbusCodec.h"

using namespace gateway::protocol;

namespace {

uint16_t crcOf(const char* hexText) {
    const auto bytes = fromHex(hexText);
    return crc16(bytes.data(), bytes.size());
}

}  // namespace

class TestCrc16 : public QObject {
    Q_OBJECT

private slots:
    void knownVectors();
    void tableMatchesBitwise();
    void frameAppendOrder();
    void verifyDetectsCorruption();
};

void TestCrc16::knownVectors() {
    QCOMPARE(crc16(nullptr, 0), static_cast<uint16_t>(0xFFFF));

    const std::vector<uint8_t> zero{0x00};
    QCOMPARE(crc16(zero.data(), zero.size()), static_cast<uint16_t>(0x40BF));

    // 标准测试向量：手算结果写在注释里，便于面试时复述
    QCOMPARE(crcOf("01 03 00 00 00 0A"), static_cast<uint16_t>(0xCDC5));
    QCOMPARE(crcOf("01 03 04 00 0A 00 14"), static_cast<uint16_t>(0x3EDA));
    QCOMPARE(crcOf("01 06 00 01 00 03"), static_cast<uint16_t>(0x0B98));
    QCOMPARE(crcOf("01 10 00 00 00 02 04 00 0A 00 14"), static_cast<uint16_t>(0xA2D3));
}

void TestCrc16::tableMatchesBitwise() {
    std::vector<uint8_t> sample;
    sample.reserve(512);
    for (int i = 0; i < 512; ++i) {
        sample.push_back(static_cast<uint8_t>((i * 37 + 11) & 0xFF));
    }
    QCOMPARE(crc16(sample.data(), sample.size()), crc16Bitwise(sample.data(), sample.size()));
}

void TestCrc16::frameAppendOrder() {
    auto frame = fromHex("01 03 00 00 00 0A");
    appendCrc16(frame);
    QCOMPARE(QString::fromStdString(toHex(frame)), QStringLiteral("01 03 00 00 00 0A C5 CD"));
}

void TestCrc16::verifyDetectsCorruption() {
    auto frame = fromHex("01 03 00 00 00 0A");
    appendCrc16(frame);
    QVERIFY(verifyCrc16(frame.data(), frame.size()));

    auto broken = frame;
    broken[3] ^= 0x01;
    QVERIFY(!verifyCrc16(broken.data(), broken.size()));

    QVERIFY(!verifyCrc16(frame.data(), 2));
}

QTEST_APPLESS_MAIN(TestCrc16)

#include "tst_crc16.moc"
