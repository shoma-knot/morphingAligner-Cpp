#include "log.hpp"

#include <ctime>
#include <mutex>
#include <utility>

namespace applog {

namespace {
std::mutex               g_mutex;    // ワーカースレッドからも add できるように保護する
std::vector<std::string> g_lines;
}    // namespace

void add(std::string msg) {
    const std::time_t tt = std::time(nullptr);
    char              buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&tt));
    std::string line = "[" + std::string(buf) + "] " + std::move(msg);

    const std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.push_back(std::move(line));
}

std::vector<std::string> lines() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_lines;    // スナップショットを返す（表示側はロック不要）
}

void clear() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.clear();
}

}    // namespace applog
