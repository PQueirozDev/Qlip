#include "utils/Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "utils/Text.h"

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::start(const std::filesystem::path& logFile) {
    // Serializa start/stop: o join do stop termina antes de um novo start reutilizar worker_ e file_.
    std::lock_guard lifecycle(lifecycleMutex_);
    {
        std::lock_guard lock(mutex_);
        if (running_) return;
    }

    std::error_code ec;
    std::filesystem::create_directories(logFile.parent_path(), ec);
    file_.open(logFile, std::ios::app);
    if (!file_.is_open()) return;

    {
        std::lock_guard lock(mutex_);
        running_ = true;
    }
    worker_ = std::thread([this] { writerLoop(); });
}

void Logger::stop() {
    std::lock_guard lifecycle(lifecycleMutex_);
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Logger::log(Level level, const std::wstring& message) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);

    std::wostringstream line;
    line << std::put_time(&local, L"%Y-%m-%d %H:%M:%S") << L" [" << (level == Level::Info ? L"INFO" : level == Level::Warn ? L"WARN" : L"ERROR") << L"] " << message;

    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        pending_.push(line.str());
    }
    cv_.notify_one();
}

void Logger::writerLoop() {
    for (;;) {
        std::queue<std::wstring> batch;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return !pending_.empty() || !running_; });
            std::swap(batch, pending_);
            if (batch.empty() && !running_) break;
        }

        while (!batch.empty()) {
            if (file_.is_open()) file_ << toUtf8(batch.front()) << '\n';
            batch.pop();
        }
        if (file_.is_open()) file_.flush();
    }

    // Só o escritor toca em file_; fecha aqui para que um novo start possa reabrir o arquivo.
    file_.close();
}
