#include "speech_tools.hpp"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "freqscale.hpp"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <algorithm>
    #include <csignal>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

// UTF-8 の文字列をパスにする（Windows で std::string から直接作ると ANSI 扱いになる）。
fs::path u8path(const std::string& s) {
    return fs::u8path(s);
}

// 候補のうち最初に存在するものの絶対パス（UTF-8）。無ければ空。
std::string first_existing(std::initializer_list<const char*> candidates) {
    std::error_code ec;
    for (const char* c : candidates) {
        const fs::path p = u8path(c);
        if (fs::is_regular_file(p, ec)) return fs::absolute(p, ec).u8string();
    }
    return {};
}

// 呼び出しごとの作業ディレクトリ（一時ディレクトリ直下に一意な名前で作る）。
// base/target を同時に解析しても衝突しないよう、連番と乱数を混ぜる。
fs::path make_work_dir() {
    static std::atomic<unsigned> counter { 0 };
    std::random_device           rd;
    const fs::path base = fs::temp_directory_path();
    for (int attempt = 0; attempt < 16; ++attempt) {
        const fs::path dir =
          base / ("morphaligner_" + std::to_string(rd()) + "_" + std::to_string(counter++));
        if (fs::create_directory(dir)) return dir;
    }
    throw std::runtime_error("一時ディレクトリを作れません");
}

// ログファイルの末尾 n 行（空行は除く）。失敗時の手がかりとしてエラーに添える。
std::string log_tail(const fs::path& log, std::size_t n) {
    std::ifstream            is(log);
    std::vector<std::string> lines;
    for (std::string line; std::getline(is, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    std::string out;
    for (std::size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); ++i)
        out += "\n  " + lines[i];
    return out;
}

// ── 子プロセスの起動 ─────────────────────────────────────────
// args[0] を引数 args で起動し、標準出力と標準エラーを log に書かせて終了を待つ。
// 終了コードを返す（起動できなければ例外）。
//
// アプリ終了時に実行中の子（と MFA などの孫）をまとめて止められるよう、起動した子を
// 記録しておく（Windows は Job Object、Linux はプロセスグループ）。止めないと、ワーカーの
// future が子の終了を待つため、MFA の実行中にウィンドウを閉じると数十秒固まる。
std::mutex g_proc_mutex;          // 起動と停止の排他（下の状態を守る）
bool       g_shutdown = false;    // terminate_speech_tools 後は新たに起動しない

#ifdef _WIN32

HANDLE g_job = nullptr;    // 子を入れる Job Object（閉じる/止めると中の全プロセスが終わる）

// Job Object を用意する（g_proc_mutex を持った状態で呼ぶ）。
HANDLE job_object() {
    if (g_job) return g_job;
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        // アプリが異常終了してハンドルが閉じられた場合も、中のプロセスを道連れにする。
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof info);
    }
    return g_job;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// CommandLineToArgvW（MSVC の CRT）の規則で1引数をクォートする。
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (auto it = a.begin();; ++it) {
        std::size_t bs = 0;
        while (it != a.end() && *it == L'\\') {
            ++it;
            ++bs;
        }
        if (it == a.end()) {
            r.append(bs * 2, L'\\');    // 閉じクォートの直前のバックスラッシュは倍にする
            break;
        }
        if (*it == L'"') {
            r.append(bs * 2 + 1, L'\\');
            r.push_back(L'"');
        } else {
            r.append(bs, L'\\');
            r.push_back(*it);
        }
    }
    r.push_back(L'"');
    return r;
}

int run_process(const std::vector<std::string>& args, const fs::path& log) {
    std::wstring cmdline;
    for (const std::string& a : args) {
        if (!cmdline.empty()) cmdline.push_back(L' ');
        cmdline += quote_arg(widen(a));
    }

    // ハンドルは継承不可で開き、CreateProcess の間だけ継承可にする。別スレッドで同時に
    // 起動したとき、互いのログファイルのハンドルが子プロセスに漏れないようにするため
    // （漏れると相手の子が終わるまで一時ディレクトリを消せなくなる）。
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in  = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        throw std::runtime_error("ログファイルを開けません");
    }

    STARTUPINFOW si {};
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = in;
    si.hStdOutput = out;
    si.hStdError  = out;
    PROCESS_INFORMATION pi {};

    BOOL  ok       = FALSE;
    DWORD last_err = 0;
    {
        std::lock_guard<std::mutex> lock(g_proc_mutex);
        if (!g_shutdown) {
            SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(in, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            // CREATE_NO_WINDOW: 子のコンソールウィンドウを出さない。
            // CREATE_SUSPENDED: Job に入れてから走らせる（先に孫を起動されると Job から漏れる）。
            ok       = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
            last_err = GetLastError();
            SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0);
            if (ok) {
                if (HANDLE job = job_object()) AssignProcessToJobObject(job, pi.hProcess);
                ResumeThread(pi.hThread);
            }
        }
    }
    CloseHandle(out);
    CloseHandle(in);
    if (!ok) {
        if (last_err == 0) throw std::runtime_error("アプリの終了中のため起動しませんでした");
        throw std::runtime_error("Python を起動できません（エラー " + std::to_string(last_err) + "）");
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

#else

std::vector<pid_t> g_children;    // 実行中の子（それぞれが自分のプロセスグループの長）

int run_process(const std::vector<std::string>& args, const fs::path& log) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    // 子を新しいプロセスグループにして、終了時に孫（MFA）ごと止められるようにする。
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    pid_t pid = 0;
    int   rc  = 0;
    {
        std::lock_guard<std::mutex> lock(g_proc_mutex);
        if (g_shutdown) {
            rc = -1;
        } else {
            rc = posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), environ);
            if (rc == 0) g_children.push_back(pid);
        }
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc < 0) throw std::runtime_error("アプリの終了中のため起動しませんでした");
    if (rc != 0) throw std::runtime_error("Python を起動できません（errno " + std::to_string(rc) + "）");

    int status = 0;
    int w      = 0;
    while ((w = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {}
    {
        std::lock_guard<std::mutex> lock(g_proc_mutex);
        g_children.erase(std::remove(g_children.begin(), g_children.end(), pid), g_children.end());
    }
    if (w < 0) throw std::runtime_error("子プロセスの待機に失敗しました");
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

// 環境変数を UTF-8 で読む（未設定なら空）。Windows の getenv は ANSI で返すので
// ワイド版の API で読んで変換する。
std::string env_utf8(const char* name) {
#ifdef _WIN32
    const std::wstring wname = widen(name);
    const DWORD        n     = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring w(n, L'\0');
    const DWORD  len = GetEnvironmentVariableW(wname.c_str(), w.data(), n);
    w.resize(len);
    return fs::path(w).u8string();
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

// 要求 JSON を渡して speech_tools.py を実行し、応答 JSON を返す。
// 失敗時（起動できない・応答が無い・ok=false）は err に理由を入れて null を返す。
json call_tool(const json& request, std::string& err) {
    err.clear();
    const std::string python = find_python();
    const std::string script = find_speech_script();
    if (python.empty()) {
        err = "Python が見つかりません（./.env に環境を置くか、環境変数 MORPHALIGNER_PYTHON で指定してください）";
        return nullptr;
    }
    if (script.empty()) {
        err = "python/speech_tools.py が見つかりません";
        return nullptr;
    }

    fs::path work;
    try {
        work = make_work_dir();
        const fs::path req = work / "request.json";
        const fs::path res = work / "response.json";
        const fs::path log = work / "log.txt";
        {
            // スクリプト側の作業ファイルもこのディレクトリの中に作らせる（途中で子を
            // 止めても、下の remove_all でまとめて消える）。
            json req_body        = request;
            req_body["work_dir"] = work.u8string();
            std::ofstream os(req, std::ios::binary);
            os << req_body.dump();    // dump は UTF-8 のまま書く
        }

        const int code = run_process({ python, script, req.u8string(), res.u8string() }, log);

        // 読み終えたらすぐ閉じる（Windows では開いたままのファイルを remove_all で消せず、
        // 作業ディレクトリが一時フォルダに残ってしまう）。
        json response;
        {
            std::ifstream is(res, std::ios::binary);
            if (is) {
                try {
                    is >> response;
                } catch (const std::exception&) {
                    response = nullptr;
                }
            }
        }
        if (!response.is_object()) {
            err = "Python の実行に失敗しました（終了コード " + std::to_string(code) + "）" + log_tail(log, 5);
            response = nullptr;
        } else if (!response.value("ok", false)) {
            // 想定内の失敗はスクリプトが理由を error に詰めている（ログの末尾は添えない）。
            err      = response.value("error", std::string { "不明なエラー" });
            response = nullptr;
        }
        std::error_code ec;
        fs::remove_all(work, ec);
        return response;
    } catch (const std::exception& e) {
        err = e.what();
        if (!work.empty()) {
            std::error_code ec;
            fs::remove_all(work, ec);
        }
        return nullptr;
    }
}

}    // namespace

void terminate_speech_tools() {
    std::lock_guard<std::mutex> lock(g_proc_mutex);
    g_shutdown = true;
#ifdef _WIN32
    if (g_job) TerminateJobObject(g_job, 1);
#else
    for (pid_t pid : g_children) kill(-pid, SIGKILL);    // グループごと（孫の MFA も）
#endif
}

std::string find_python() {
    if (const std::string env = env_utf8("MORPHALIGNER_PYTHON"); !env.empty()) {
        std::error_code ec;
        if (fs::is_regular_file(u8path(env), ec)) return env;
    }
#ifdef _WIN32
    return first_existing({ ".env/python.exe", "../.env/python.exe" });
#else
    return first_existing({ ".env/bin/python", "../.env/bin/python" });
#endif
}

std::string find_speech_script() {
    return first_existing({ "python/speech_tools.py", "../python/speech_tools.py" });
}

Formants run_formants(const std::string& wav, const FormantParams& params, std::string& err) {
    const json res = call_tool({ { "command", "formants" },
                                 { "wav", wav },
                                 { "max_formant_hz", params.max_formant_hz },
                                 { "num_formants", params.num_formants },
                                 { "num_tracks", params.num_tracks } },
                               err);
    Formants out;
    if (res.is_null()) return out;
    try {
        const auto& times  = res.at("times");
        const auto& tracks = res.at("tracks");
        for (const json& jt : tracks) {
            FormantTrack tr;
            for (std::size_t i = 0; i < jt.size() && i < times.size(); ++i) {
                if (jt[i].is_null()) continue;    // 未定義のフレームは除く
                const double hz = jt[i].get<double>();
                if (!std::isfinite(hz) || hz <= 0.0) continue;
                tr.t.push_back(times[i].get<double>());
                tr.hz.push_back(hz);
                tr.erb.push_back(freqscale::hz_to_erb(hz));
            }
            out.tracks.push_back(std::move(tr));
        }
    } catch (const std::exception& e) {
        err = std::string { "応答の形式が不正です: " } + e.what();
        return {};
    }
    return out;
}

Segmentation run_alignment(const std::string& wav, const std::string& text, const AlignParams& params,
                           std::string& err) {
    const json res = call_tool({ { "command", "align" },
                                 { "wav", wav },
                                 { "text", text },
                                 { "acoustic_model", params.acoustic_model },
                                 { "dictionary", params.dictionary } },
                               err);
    Segmentation out;
    if (res.is_null()) return out;
    try {
        for (const json& jt : res.at("tiers")) {
            SegTier tier;
            tier.name = jt.at("name").get<std::string>();
            for (const json& ji : jt.at("intervals"))
                tier.intervals.push_back(
                  SegInterval { ji.at(0).get<double>(), ji.at(1).get<double>(), ji.at(2).get<std::string>() });
            out.tiers.push_back(std::move(tier));
        }
    } catch (const std::exception& e) {
        err = std::string { "応答の形式が不正です: " } + e.what();
        return {};
    }
    return out;
}
