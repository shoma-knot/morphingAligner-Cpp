#include "log.hpp"

#include <algorithm>
#include <ctime>
#include <deque>
#include <mutex>
#include <utility>

namespace applog {

namespace {

std::mutex              g_mutex;    // ワーカースレッドからも add できるように保護する
std::deque<std::string> g_lines;    // 古い順。kMaxLines を超えたら先頭から捨てる

// 現在時刻の "HH:MM:SS"。localtime はスレッド安全でないので、OS ごとの再入可能版を使う。
std::string now_hms() {
    const std::time_t tt = std::time(nullptr);
    std::tm           tm {};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    return buf;
}

}    // namespace

void add(std::string msg) {
    std::string line = "[" + now_hms() + "] " + std::move(msg);

    const std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.push_back(std::move(line));
    while (g_lines.size() > kMaxLines) g_lines.pop_front();
}

std::size_t size() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_lines.size();
}

std::vector<std::string> lines(std::size_t first, std::size_t count) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (first >= g_lines.size()) return {};
    const std::size_t last = std::min(g_lines.size(), first + count);
    // 表示側がロックせずに使えるようコピーを返す。
    return std::vector<std::string>(g_lines.begin() + static_cast<std::ptrdiff_t>(first),
                                    g_lines.begin() + static_cast<std::ptrdiff_t>(last));
}

void clear() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.clear();
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}    // namespace applog
