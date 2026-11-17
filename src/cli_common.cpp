#include "cli_common.hpp"

#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace cli {

std::atomic<bool> g_interrupted { false };

namespace {

// シグナルハンドラから触れるのは lock-free の atomic だけにする。
extern "C" void on_sigint(int) {
    g_interrupted.store(true);
}

}    // namespace

SigintScope::SigintScope() {
    g_interrupted.store(false);
    prev_ = std::signal(SIGINT, on_sigint);
}

SigintScope::~SigintScope() {
    std::signal(SIGINT, prev_ == SIG_ERR ? SIG_DFL : prev_);
}

void say(std::FILE* f, const std::string& s) {
    std::fputs((s + "\n").c_str(), f);
    std::fflush(f);
}

Result<SessionFile> read_session_file(const std::string& path, std::FILE* log) {
    std::ifstream is(std::filesystem::u8path(path), std::ios::binary);
    if (!is) return Result<SessionFile>::failure("開けません: " + path);
    std::ostringstream ss;
    ss << is.rdbuf();
    Result<SessionFile> parsed = parse_session_json(ss.str());
    if (!parsed.ok()) return Result<SessionFile>::failure("読めません: " + path + "（" + parsed.error + "）");
    for (const std::string& n : parsed.value.notes) say(log, n);
    return parsed;
}

Status check_not_exists(const std::string& path, bool overwrite) {
    std::error_code ec;
    if (!overwrite && std::filesystem::exists(std::filesystem::u8path(path), ec))
        return Status::failure("保存先がすでにあります（上書きするなら --overwrite を付けてください）: " + path);
    return {};
}

Status save_session_file(const std::string& path, const SessionFile& s) {
    try {
        const std::string text = session_to_json(s);
        std::error_code   ec;
        std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
        std::ofstream os(std::filesystem::u8path(path), std::ios::binary);
        if (!os) return Status::failure("書き込めません: " + path);
        os << text;
        return os ? Status {} : Status::failure("書き込めません: " + path);
    } catch (const std::exception& e) {
        return Status::failure(e.what());
    }
}

}    // namespace cli
