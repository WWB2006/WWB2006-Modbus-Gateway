#include "gateway/transport/SerialTransport.h"
#include "gateway/util/Logger.h"
#include <thread>
#include <chrono>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#endif

namespace gateway::transport {

SerialTransport::SerialTransport(std::string portName, int baudRate, char parity, int dataBits, int stopBits)
    : m_portName(std::move(portName)), m_baudRate(baudRate), m_parity(parity),
      m_dataBits(dataBits), m_stopBits(stopBits),
      m_handle(INVALID_SERIAL_HANDLE), m_connected(false) {
    if (m_baudRate > 19200) {
        m_interCharDelayUs = 1750; // Fixed 1.75ms for > 19200 baud
    } else {
        // 3.5 character times = 3.5 * 11 bits / baudRate
        m_interCharDelayUs = static_cast<int>((3.5 * 11.0 * 1000000.0) / m_baudRate);
    }
}

SerialTransport::~SerialTransport() {
    disconnect();
}

bool SerialTransport::connect() {
    if (m_connected) {
        disconnect();
    }

#if defined(_WIN32)
    std::string fullPort = m_portName;
    if (fullPort.rfind("\\\\.\\", 0) != 0 && fullPort.rfind("COM", 0) == 0) {
        fullPort = "\\\\.\\" + fullPort;
    }

    m_handle = CreateFileA(fullPort.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        LOG_WARN("Could not open serial port " << fullPort << " (LastError=" << GetLastError() << ")");
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(m_handle, &dcb)) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    dcb.BaudRate = m_baudRate;
    dcb.ByteSize = static_cast<BYTE>(m_dataBits);

    switch (m_parity) {
        case 'E': dcb.Parity = EVENPARITY; break;
        case 'O': dcb.Parity = ODDPARITY; break;
        case 'N': default: dcb.Parity = NOPARITY; break;
    }

    dcb.StopBits = (m_stopBits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = (m_parity != 'N') ? TRUE : FALSE;

    if (!SetCommState(m_handle, &dcb)) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return false;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(m_handle, &timeouts);

#else
    m_handle = open(m_portName.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (m_handle == -1) {
        LOG_WARN("Could not open serial port " << m_portName);
        return false;
    }

    struct termios tty{};
    if (tcgetattr(m_handle, &tty) != 0) {
        close(m_handle);
        m_handle = INVALID_SERIAL_HANDLE;
        return false;
    }

    speed_t spd = B9600;
    switch (m_baudRate) {
        case 4800: spd = B4800; break;
        case 9600: spd = B9600; break;
        case 19200: spd = B19200; break;
        case 38400: spd = B38400; break;
        case 57600: spd = B57600; break;
        case 115200: spd = B115200; break;
        default: spd = B9600; break;
    }
    cfsetospeed(&tty, spd);
    cfsetispeed(&tty, spd);

    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= (m_dataBits == 7) ? CS7 : CS8;

    if (m_parity == 'E') {
        tty.c_cflag |= PARENB;
        tty.c_cflag &= ~PARODD;
    } else if (m_parity == 'O') {
        tty.c_cflag |= PARENB | PARODD;
    } else {
        tty.c_cflag &= ~PARENB;
    }

    if (m_stopBits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1; // 100ms timeout

    tcflush(m_handle, TCIFLUSH);
    if (tcsetattr(m_handle, TCSANOW, &tty) != 0) {
        close(m_handle);
        m_handle = INVALID_SERIAL_HANDLE;
        return false;
    }
#endif

    m_connected = true;
    return true;
}

void SerialTransport::disconnect() {
    if (m_handle != INVALID_SERIAL_HANDLE) {
#if defined(_WIN32)
        CloseHandle(m_handle);
#else
        close(m_handle);
#endif
        m_handle = INVALID_SERIAL_HANDLE;
    }
    m_connected = false;
}

bool SerialTransport::isConnected() const {
    return m_connected && m_handle != INVALID_SERIAL_HANDLE;
}

bool SerialTransport::send(const uint8_t* data, size_t len) {
    if (!isConnected()) return false;
    waitInterFrameDelay();

#if defined(_WIN32)
    DWORD bytesWritten = 0;
    BOOL res = WriteFile(m_handle, data, static_cast<DWORD>(len), &bytesWritten, nullptr);
    return res && bytesWritten == len;
#else
    ssize_t written = write(m_handle, data, len);
    return written == static_cast<ssize_t>(len);
#endif
}

bool SerialTransport::receive(uint8_t* buffer, size_t maxLen, size_t& bytesRead, int timeoutMs) {
    bytesRead = 0;
    if (!isConnected()) return false;

#if defined(_WIN32)
    COMMTIMEOUTS timeouts{};
    timeouts.ReadTotalTimeoutConstant = timeoutMs;
    SetCommTimeouts(m_handle, &timeouts);

    DWORD readCount = 0;
    BOOL res = ReadFile(m_handle, buffer, static_cast<DWORD>(maxLen), &readCount, nullptr);
    if (res && readCount > 0) {
        bytesRead = static_cast<size_t>(readCount);
        return true;
    }
    return false;
#else
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_handle, &readSet);

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int selRes = select(m_handle + 1, &readSet, nullptr, nullptr, &tv);
    if (selRes <= 0) return false;

    ssize_t n = read(m_handle, buffer, maxLen);
    if (n > 0) {
        bytesRead = static_cast<size_t>(n);
        return true;
    }
    return false;
#endif
}

std::string SerialTransport::getEndpointInfo() const {
    return m_portName + "@" + std::to_string(m_baudRate);
}

void SerialTransport::waitInterFrameDelay() {
    std::this_thread::sleep_for(std::chrono::microseconds(m_interCharDelayUs));
}

} // namespace gateway::transport
