#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace gateway::protocol {

/**
 * @brief Modbus RTU CRC-16 calculation and verification.
 * Uses polynomial 0xA001 (reversed representation of 0x8005) with initial value 0xFFFF.
 * According to Modbus specifications, in RTU frames the low-order byte is sent first,
 * followed by the high-order byte.
 */
class Crc16 {
public:
    /**
     * @brief Computes the 16-bit CRC value for a buffer of bytes.
     * @param data Pointer to input data buffer
     * @param length Number of bytes to compute over
     * @return 16-bit CRC checksum
     */
    static uint16_t compute(const uint8_t* data, size_t length);

    /**
     * @brief Computes CRC for a vector of bytes.
     */
    static uint16_t compute(const std::vector<uint8_t>& data);

    /**
     * @brief Verifies whether the CRC at the end of an RTU frame is valid.
     * The last two bytes of the frame must be the low and high bytes of the CRC.
     * @param frame Complete RTU frame including 2-byte CRC
     * @return true if CRC matches, false otherwise
     */
    static bool verify(const uint8_t* frame, size_t length);

    /**
     * @brief Overload for vector frame.
     */
    static bool verify(const std::vector<uint8_t>& frame);

    /**
     * @brief Appends 2-byte CRC (low byte first, high byte second) to a frame buffer.
     * @param frame Frame data buffer to append CRC to in-place.
     */
    static void append(std::vector<uint8_t>& frame);

private:
    static const uint16_t CRC_TABLE[256];
};

} // namespace gateway::protocol
