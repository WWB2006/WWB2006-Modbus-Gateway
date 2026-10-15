#include "gateway/protocol/Crc16.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <iomanip>

using namespace gateway::protocol;

int main() {
    std::cout << "[TEST] Running tst_crc..." << std::endl;

    // Test 1: Known frame 1 - Read Holding Registers (Func 03, Addr 0, Count 10)
    // 01 03 00 00 00 0A -> expected CRC low: 0xC5, high: 0xCD -> 0xCDC5
    {
        std::vector<uint8_t> frame = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
        uint16_t crc = Crc16::compute(frame);
        std::cout << "  Frame 1 CRC: 0x" << std::hex << crc << std::dec << std::endl;
        assert(crc == 0xCDC5);

        // Append test
        std::vector<uint8_t> rtuFrame = frame;
        Crc16::append(rtuFrame);
        assert(rtuFrame.size() == 8);
        assert(rtuFrame[6] == 0xC5); // Low byte
        assert(rtuFrame[7] == 0xCD); // High byte

        // Verify test
        assert(Crc16::verify(rtuFrame));

        // Corrupted frame test
        rtuFrame[7] ^= 0x01;
        assert(!Crc16::verify(rtuFrame));
    }

    // Test 2: Known frame 2 - Write Single Register (Func 06, Addr 1, Value 100/0x0064)
    // 01 06 00 01 00 64 -> expected CRC low: 0xD9, high: 0xE1 -> 0xE1D9
    {
        std::vector<uint8_t> frame = {0x01, 0x06, 0x00, 0x01, 0x00, 0x64};
        uint16_t crc = Crc16::compute(frame);
        std::cout << "  Frame 2 CRC: 0x" << std::hex << crc << std::dec << std::endl;
        assert(crc == 0xE1D9);

        std::vector<uint8_t> rtuFrame = frame;
        Crc16::append(rtuFrame);
        assert(rtuFrame[6] == 0xD9); // Low byte
        assert(rtuFrame[7] == 0xE1); // High byte
        assert(Crc16::verify(rtuFrame));
    }

    // Test 3: Read Coils response
    // 01 01 01 05 -> Unit 1, Func 1, ByteCount 1, Data 0x05 (Coils 1 & 3 ON)
    // CRC is 0x8B91 (low 0x91, high 0x8B)
    {
        std::vector<uint8_t> frame = {0x01, 0x01, 0x01, 0x05};
        uint16_t crc = Crc16::compute(frame);
        assert(crc == 0x8B91);
        std::vector<uint8_t> rtuFrame = frame;
        Crc16::append(rtuFrame);
        assert(Crc16::verify(rtuFrame));
    }

    // Test 4: Exception response
    // 01 83 02 -> Unit 1, Func 83 (03 | 0x80), ExCode 02 (Illegal Data Address)
    // CRC: 0xC0F1 (low 0xC0, high 0xF1)
    {
        std::vector<uint8_t> frame = {0x01, 0x83, 0x02};
        uint16_t crc = Crc16::compute(frame);
        std::cout << "  Exception frame CRC: 0x" << std::hex << crc << std::dec << std::endl;
        assert(crc == 0xF1C0);
        std::vector<uint8_t> rtuFrame = frame;
        Crc16::append(rtuFrame);
        assert(Crc16::verify(rtuFrame));
    }

    // Test 5: Verify edge cases
    {
        std::vector<uint8_t> empty;
        assert(!Crc16::verify(empty));
        std::vector<uint8_t> twoBytes = {0x01, 0x02};
        assert(!Crc16::verify(twoBytes));
    }

    std::cout << "[PASS] tst_crc passed all 5 test scenarios!" << std::endl;
    return 0;
}
