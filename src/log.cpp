#include "log.hpp"

#include <ctime>
#include <utility>

namespace applog {

namespace {
std::vector<std::string> g_lines;
}    // namespace

void add(std::string msg) {
    const std::time_t tt = std::time(nullptr);
    char              buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&tt));
    g_lines.emplace_back("[" + std::string(buf) + "] " + std::move(msg));
}

const std::vector<std::string>& lines() {
    return g_lines;
}

void clear() {
    g_lines.clear();
}

}    // namespace applog
