#pragma once

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <queue>

// Logger assíncrono: as chamadas só enfileiram a mensagem; uma thread dedicada grava em disco.
class Logger {
public:
    enum class Level { Info, Warn, Error };

    static Logger& instance();

    void start(const std::filesystem::path& logFile);
    void stop();

    void log(Level level, const std::wstring& message);

private:
    Logger() = default;
    void writerLoop();

    std::mutex lifecycleMutex_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::wstring> pending_;
    std::thread worker_;
    std::ofstream file_;
    bool running_ = false;
};

#define QLOG_INFO(msg)  Logger::instance().log(Logger::Level::Info, (msg))
#define QLOG_WARN(msg)  Logger::instance().log(Logger::Level::Warn, (msg))
#define QLOG_ERROR(msg) Logger::instance().log(Logger::Level::Error, (msg))
