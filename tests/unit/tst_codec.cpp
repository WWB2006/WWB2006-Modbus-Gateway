#include <QtTest>

#include "gateway/protocol/ModbusCodec.h"

using namespace gateway::protocol;

namespace {

std::size_t decodeHex(const char* hexText, DecodeResult& result, bool asResponse = true) {
    const auto bytes = fromHex(hexText);
    return asResponse ? decodeRtuResponse(bytes.data(), bytes.size(), result)
                      : decodeRtuRequest(bytes.data(), bytes.size(), result);
}

}  // namespace

class TestCodec : public QObject {
    Q_OBJECT

private slots:
    void pduBuilders();
    void pduBuilderLimits();
    void tcpFrameLayout();
    void rtuFrameLayout();
    void tcpDecodeAndReassembly();
    void tcpRejectsInvalidHeader();
    void rtuDecodeAndCrcFailure();
    void rtuRequestDecode();
    void exceptionResponses();
    void registerResponseParsing();
    void writeResponseParsing();
    void requestMatching();
};

void TestCodec::pduBuilders() {
    QCOMPARE(QString::fromStdString(toHex(buildReadHoldingRegisters(0, 10))),
             QStringLiteral("03 00 00 00 0A"));
    QCOMPARE(QString::fromStdString(toHex(buildWriteSingleRegister(1, 3))),
             QStringLiteral("06 00 01 00 03"));
    QCOMPARE(QString::fromStdString(toHex(buildWriteMultipleRegisters(0, {10, 20}))),
             QStringLiteral("10 00 00 00 02 04 00 0A 00 14"));
}

void TestCodec::pduBuilderLimits() {
    QVERIFY(buildReadHoldingRegisters(0, 0).empty());
    QVERIFY(buildReadHoldingRegisters(0, 126).empty());
    QVERIFY(buildWriteMultipleRegisters(0, {}).empty());
    QVERIFY(buildWriteMultipleRegisters(0, std::vector<uint16_t>(124, 1)).empty());
}

void TestCodec::tcpFrameLayout() {
    const auto frame = encodeTcpRequest(1, 1, buildReadHoldingRegisters(0, 10));
    QCOMPARE(QString::fromStdString(toHex(frame)),
             QStringLiteral("00 01 00 00 00 06 01 03 00 00 00 0A"));
    // 长度字段 = 单元号 + PDU，不是整帧长度
    QCOMPARE(static_cast<int>(frame[4] << 8 | frame[5]), 6);
    QVERIFY(encodeTcpRequest(1, 1, {}).empty());
}

void TestCodec::rtuFrameLayout() {
    const auto frame = encodeRtuRequest(1, buildReadHoldingRegisters(0, 10));
    QCOMPARE(QString::fromStdString(toHex(frame)),
             QStringLiteral("01 03 00 00 00 0A C5 CD"));
    QVERIFY(encodeRtuRequest(1, {}).empty());
}

void TestCodec::tcpDecodeAndReassembly() {
    const auto first = encodeTcpRequest(1, 1, buildReadHoldingRegisters(0, 10));
    const auto second = encodeTcpRequest(2, 1, buildWriteSingleRegister(1, 3));

    DecodeResult result;
    QCOMPARE(static_cast<int>(decodeTcp(first.data(), first.size() - 1, result)), 0);
    QCOMPARE(static_cast<int>(result.status), static_cast<int>(DecodeStatus::Incomplete));

    std::vector<uint8_t> merged = first;
    merged.insert(merged.end(), second.begin(), second.end());

    const std::size_t consumedFirst = decodeTcp(merged.data(), merged.size(), result);
    QCOMPARE(static_cast<int>(consumedFirst), 12);
    QCOMPARE(static_cast<int>(result.frame.transactionId), 1);
    QCOMPARE(QString::fromStdString(toHex(result.frame.pdu)), QStringLiteral("00 00 00 0A"));

    const std::size_t consumedSecond =
        decodeTcp(merged.data() + consumedFirst, merged.size() - consumedFirst, result);
    QCOMPARE(static_cast<int>(consumedSecond), 12);
    QCOMPARE(static_cast<int>(result.frame.transactionId), 2);
}

void TestCodec::tcpRejectsInvalidHeader() {
    auto frame = encodeTcpRequest(1, 1, buildReadHoldingRegisters(0, 10));
    DecodeResult result;

    auto badProtocol = frame;
    badProtocol[3] = 0x01;
    QCOMPARE(static_cast<int>(decodeTcp(badProtocol.data(), badProtocol.size(), result)), 0);
    QCOMPARE(static_cast<int>(result.status), static_cast<int>(DecodeStatus::BadProtocolId));

    auto badLength = frame;
    badLength[4] = 0x00;
    badLength[5] = 0x00;
    QCOMPARE(static_cast<int>(decodeTcp(badLength.data(), badLength.size(), result)), 0);
    QCOMPARE(static_cast<int>(result.status), static_cast<int>(DecodeStatus::BadLength));
}

void TestCodec::rtuDecodeAndCrcFailure() {
    DecodeResult result;
    QCOMPARE(static_cast<int>(decodeHex("01 03 04 00 0A 00 14 DA 3E", result)), 9);
    QCOMPARE(static_cast<int>(result.status), static_cast<int>(DecodeStatus::Ok));
    QCOMPARE(static_cast<int>(result.frame.unitId), 1);
    QCOMPARE(static_cast<int>(result.frame.functionCode), 3);

    const auto partial = fromHex("01 03 04 00 0A 00 14 DA");
    QCOMPARE(static_cast<int>(decodeRtuResponse(partial.data(), partial.size(), result)), 0);
    QCOMPARE(static_cast<int>(result.status), static_cast<int>(DecodeStatus::Incomplete));

    auto corrupted = fromHex("01 03 04 00 0A 00 14 DA 3F");
    QCOMPARE(static_cast<int>(decodeRtuResponse(corrupted.data(), corrupted.size(), result)), 0);
    QCOMPARE(static_cast<int>(result.status), static_cast<int>(DecodeStatus::BadCrc));
}

void TestCodec::rtuRequestDecode() {
    DecodeResult result;
    QCOMPARE(static_cast<int>(decodeHex("01 03 00 00 00 0A C5 CD", result, false)), 8);
    QCOMPARE(static_cast<int>(result.frame.functionCode), 3);

    QCOMPARE(static_cast<int>(decodeHex("01 10 00 00 00 02 04 00 0A 00 14 D3 A2", result, false)),
             13);
    QCOMPARE(QString::fromStdString(toHex(result.frame.pdu)),
             QStringLiteral("00 00 00 02 04 00 0A 00 14"));

    // 未知功能码：不猜长度，直接判非法
    QCOMPARE(static_cast<int>(decodeHex("01 45 00 00", result)), 0);
    QCOMPARE(static_cast<int>(result.status), static_cast<int>(DecodeStatus::BadFunction));
}

void TestCodec::exceptionResponses() {
    DecodeResult rtuResult;
    QCOMPARE(static_cast<int>(decodeHex("01 83 02 C0 F1", rtuResult)), 5);
    QVERIFY(rtuResult.frame.isException());
    QCOMPARE(static_cast<int>(rtuResult.frame.exceptionCode()),
             static_cast<int>(ExceptionCode::IllegalDataAddress));

    const auto tcpFrame = fromHex("00 01 00 00 00 03 01 83 02");
    DecodeResult tcpResult;
    QCOMPARE(static_cast<int>(decodeTcp(tcpFrame.data(), tcpFrame.size(), tcpResult)), 9);
    QVERIFY(tcpResult.frame.isException());
    QCOMPARE(static_cast<int>(tcpResult.frame.exceptionCode()),
             static_cast<int>(ExceptionCode::IllegalDataAddress));

    const auto hint = QString::fromStdString(toChineseHint(ExceptionCode::IllegalDataAddress));
    QVERIFY(!hint.isEmpty());
}

void TestCodec::registerResponseParsing() {
    DecodeResult result;
    decodeHex("01 03 04 00 0A 00 14 DA 3E", result);

    const auto parsed = parseReadRegistersResponse(result.frame, 2);
    QVERIFY(parsed.valid);
    QCOMPARE(static_cast<int>(parsed.values.size()), 2);
    QCOMPARE(static_cast<int>(parsed.values[0]), 10);
    QCOMPARE(static_cast<int>(parsed.values[1]), 20);

    const auto mismatch = parseReadRegistersResponse(result.frame, 3);
    QVERIFY(!mismatch.valid);
    QVERIFY(!mismatch.error.empty());
}

void TestCodec::writeResponseParsing() {
    DecodeResult singleResult;
    decodeHex("01 06 00 01 00 03 98 0B", singleResult);
    const auto single = parseWriteSingleResponse(singleResult.frame);
    QVERIFY(single.valid);
    QCOMPARE(static_cast<int>(single.address), 1);
    QCOMPARE(static_cast<int>(single.value), 3);

    DecodeResult multiResult;
    decodeHex("01 10 00 00 00 02 41 C8", multiResult);
    const auto multi = parseWriteMultipleResponse(multiResult.frame);
    QVERIFY(multi.valid);
    QCOMPARE(static_cast<int>(multi.address), 0);
    QCOMPARE(static_cast<int>(multi.count), 2);
}

void TestCodec::requestMatching() {
    const auto frame = fromHex("00 07 00 00 00 05 01 03 02 00 0A");
    DecodeResult result;
    decodeTcp(frame.data(), frame.size(), result);

    QVERIFY(matchesRequest(result.frame, Transport::Tcp, 7, 1, 0x03));
    QVERIFY(!matchesRequest(result.frame, Transport::Tcp, 8, 1, 0x03));
    QVERIFY(!matchesRequest(result.frame, Transport::Tcp, 7, 2, 0x03));
    QVERIFY(!matchesRequest(result.frame, Transport::Tcp, 7, 1, 0x04));

    // 异常响应（0x83 = 0x03 | 0x80）必须能与请求 0x03 匹配
    DecodeResult exceptionResult;
    const auto exceptionFrame = fromHex("00 07 00 00 00 03 01 83 02");
    decodeTcp(exceptionFrame.data(), exceptionFrame.size(), exceptionResult);
    QVERIFY(matchesRequest(exceptionResult.frame, Transport::Tcp, 7, 1, 0x03));

    DecodeResult rtuResult;
    decodeHex("01 03 02 00 0A 38 43", rtuResult);
    QVERIFY(matchesRequest(rtuResult.frame, Transport::Rtu, 0, 1, 0x03));
    QVERIFY(!matchesRequest(rtuResult.frame, Transport::Rtu, 0, 3, 0x03));
    QVERIFY(!matchesRequest(rtuResult.frame, Transport::Rtu, 0, 1, 0x06));
}

QTEST_APPLESS_MAIN(TestCodec)

#include "tst_codec.moc"
