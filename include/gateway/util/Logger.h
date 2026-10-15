#pragma once

#include <string>
#include <mutex>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace gateway::util {

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERR = 3
};

class Logger {
public:
    static Logger& instance() {
        static Logger s_instance;
        return s_instance;
    }

    void setLevel(LogLevel level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_level = level;
    }

    void setOutputFile(const std::string& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_filePath = path;
        if (m_fileStream.is_open()) {
            m_fileStream.close();
        }
        m_fileStream.open(path, std::ios::app);
    }

    void log(LogLevel level, const std::string& message) {
        if (level < m_level) return;

        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tmNow{};
#if defined(_WIN32)
        localtime_s(&tmNow, &timeT);
#else
        localtime_r(&timeT, &tmNow);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tmNow, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count()
            << " [" << levelToString(level) << "] "
            << message;
        std::string line = oss.str();

        std::lock_guard<std::mutex> lock(m_mutex);
        std::cout << line << std::endl;
        if (m_fileStream.is_open()) {
            m_fileStream << line << std::endl;
        }
    }

    static const char* levelToString(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERR:   return "ERROR";
            default:              return "LOG  ";
        }
    }

private:
    Logger() : m_level(LogLevel::INFO) {}
    ~Logger() {
        if (m_fileStream.is_open()) {
            m_fileStream.close();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel m_level;
    std::string m_filePath;
    std::ofstream m_fileStream;
    std::mutex m_mutex;
};

#define LOG_DEBUG(msg) do { std::ostringstream _oss; _oss << msg; gateway::util::Logger::instance().log(gateway::util::LogLevel::DEBUG, _oss.str()); } while(0)
#define LOG_INFO(msg)  do { std::ostringstream _oss; _oss << msg; gateway::util::Logger::instance().log(gateway::util::LogLevel::INFO,  _oss.str()); } while(0)
#define LOG_WARN(msg)  do { std::ostringstream _oss; _oss << msg; gateway::util::Logger::instance().log(gateway::util::LogLevel::WARN,  _oss.str()); } while(0)
#define LOG_ERROR(msg) do { std::ostringstream _oss; _oss << msg; gateway::util::Logger::instance().log(gateway::util::LogLevel::ERR,   _oss.str()); } while(0)

} // namespace gateway::util
