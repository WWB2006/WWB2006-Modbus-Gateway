// 无界面入口：所有功能都能在没有图形环境的情况下验证，这是阶段 11 无界面网关的雏形。
// 阶段 0-1 只做一件事：把协议层的编解码结果打印出来，证明核心可用。

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

#include "gateway/protocol/Crc16.h"
#include "gateway/protocol/ModbusCodec.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // 让控制台正确显示中文
#endif
    using namespace gateway::protocol;

    std::printf("modbus-gateway cli（阶段 0-1：协议层）\n\n");

    const auto pdu = buildReadHoldingRegisters(0, 10);
    const auto tcp = encodeTcpRequest(1, 1, pdu);
    const auto rtu = encodeRtuRequest(1, pdu);
    std::printf("读保持寄存器 0 起 10 个\n");
    std::printf("  PDU      : %s\n", toHex(pdu).c_str());
    std::printf("  TCP 请求 : %s\n", toHex(tcp).c_str());
    std::printf("  RTU 请求 : %s\n", toHex(rtu).c_str());

    const auto response = fromHex("01 03 04 00 0A 00 14 DA 3E");
    DecodeResult decoded;
    if (decodeRtuResponse(response.data(), response.size(), decoded) > 0) {
        const auto parsed = parseReadRegistersResponse(decoded.frame, 2);
        if (parsed.valid) {
            std::printf("\n解析 RTU 响应 %s\n", toHex(response).c_str());
            for (std::size_t i = 0; i < parsed.values.size(); ++i) {
                std::printf("  寄存器[%zu] = %u\n", i, parsed.values[i]);
            }
        } else {
            std::printf("解析失败：%s\n", parsed.error.c_str());
        }
    }

    const auto exceptionFrame = fromHex("01 83 02 C0 F1");
    DecodeResult exceptionDecoded;
    if (decodeRtuResponse(exceptionFrame.data(), exceptionFrame.size(), exceptionDecoded) > 0) {
        const auto parsed = parseReadRegistersResponse(exceptionDecoded.frame, 2);
        std::printf("\n异常响应 %s\n  %s\n", toHex(exceptionFrame).c_str(),
                    parsed.error.c_str());
    }
    return 0;
}
