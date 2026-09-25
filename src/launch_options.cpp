#include "launch_options.hpp"

#include <filesystem>
#include <system_error>
#include <utility>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <shellapi.h>    // CommandLineToArgvW
#endif

namespace {

// 値を取るオプション。
struct ValueOption {
    const char*  name;
    std::string LaunchOptions::*field;
};
const ValueOption kValueOptions[] = {
    { "--base", &LaunchOptions::base },
    { "--target", &LaunchOptions::target },
    { "--session", &LaunchOptions::session },
    { "--transcript", &LaunchOptions::transcript },
};

Result<LaunchOptions> fail(std::string msg) {
    return Result<LaunchOptions>::failure(std::move(msg));
}

}    // namespace

Result<LaunchOptions> parse_launch_options(const std::vector<std::string>& args) {
    LaunchOptions o;

    // --help / --version は、ほかの引数に誤りがあっても優先する。
    for (const std::string& a : args) {
        if (a == "--help" || a == "-h") o.help = true;
        if (a == "--version") o.version = true;
    }
    if (o.help || o.version) return Result<LaunchOptions>::success(std::move(o));

    bool seen_align = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--align") {
            if (seen_align) return fail("--align が2回指定されています");
            seen_align = o.align = true;
            continue;
        }

        // 「--name 値」と「--name=値」
        const ValueOption* opt = nullptr;
        std::string        value;
        bool               has_value = false;
        for (const ValueOption& v : kValueOptions) {
            const std::string name = v.name;
            if (a == name) {
                opt = &v;
                if (i + 1 >= args.size()) return fail(name + " に値がありません");
                value     = args[++i];
                has_value = true;
            } else if (a.rfind(name + "=", 0) == 0) {
                opt       = &v;
                value     = a.substr(name.size() + 1);
                has_value = true;
            }
            if (opt) break;
        }
        if (!opt) return fail("知らない引数です: " + a);

        std::string& field  = o.*(opt->field);
        const bool   is_trn = opt->field == &LaunchOptions::transcript;
        if (is_trn ? o.has_transcript : !field.empty())
            return fail(std::string { opt->name } + " が2回指定されています");
        if (!is_trn && value.empty()) return fail(std::string { opt->name } + " の値（パス）が空です");
        field = std::move(value);
        if (is_trn) o.has_transcript = has_value;
    }

    // --align は音声が要る。書き起こしは --transcript か、セッションの中のもの（読み込むまで
    // 分からないので、ここでは --session があれば通し、無ければ実行時に知らせる）。
    if (o.align && o.base.empty() && o.target.empty() && o.session.empty())
        return fail("--align には --base / --target / --session のどれかが要ります");
    if (o.align && !o.has_transcript && o.session.empty())
        return fail("--align には --transcript（または書き起こしを含む --session）が要ります");
    return Result<LaunchOptions>::success(std::move(o));
}

void make_paths_absolute(LaunchOptions& opts) {
    for (std::string* p : { &opts.base, &opts.target, &opts.session }) {
        if (p->empty()) continue;
        std::error_code             ec;
        const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::u8path(*p), ec);
        if (!ec) *p = abs.lexically_normal().u8string();
    }
}

std::string launch_usage() {
    return "使い方: morphingAlignerCpp [オプション]\n"
           "\n"
           "  --base <音声>          base の音声を読み込む\n"
           "  --target <音声>        target の音声を読み込む\n"
           "  --session <JSON>       セッションを読み込む。tcmorph のアンカー JSON なら、\n"
           "                         --base / --target の音声にアンカーだけを乗せる\n"
           "  --transcript <文>      音素アライメント（MFA）用の書き起こし（セッションのものより優先）\n"
           "  --align                読み込み後、音声解析の環境が使えたら音素アライメントを実行する\n"
           "  --version              版を表示して終了する\n"
           "  -h, --help             この説明を表示して終了する\n"
           "\n"
           "値は「--base a.wav」「--base=a.wav」のどちらでも書けます。相対パスは起動したときの\n"
           "カレントディレクトリが基準です。音声のパスを含むセッションと --base / --target は\n"
           "同時には指定できません。\n";
}

void use_utf8_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

std::vector<std::string> utf8_arguments(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int       n  = 0;
    LPWSTR*   wv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wv == nullptr) return out;
    for (int i = 1; i < n; ++i) out.push_back(std::filesystem::path(wv[i]).u8string());
    LocalFree(wv);
#else
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
#endif
    return out;
}
