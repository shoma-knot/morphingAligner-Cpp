#include "launch_options.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <utility>

#include "batch_export.hpp"    // BatchExportSettings::kCountMin / kCountMax

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

// オプションを使えるサブコマンド（ビットの組み合わせ）。
constexpr int kGui   = 1;
constexpr int kBatch = 2;

struct OptionSpec {
    const char* name;
    bool        takes_value;
    int         commands;
};
const OptionSpec kOptions[] = {
    { "--base", true, kGui | kBatch },     { "--target", true, kGui | kBatch }, { "--session", true, kGui | kBatch },
    { "--transcript", true, kGui | kBatch }, { "--align", false, kGui },         { "--count", true, kBatch },
    { "--out", true, kBatch },             { "--prefix", true, kBatch },        { "--sweep", true, kBatch },
    { "--fixed", true, kBatch },           { "--no-manifest", false, kBatch },  { "--overwrite", false, kBatch },
    { "--auto-anchors", false, kBatch },   { "--divisions", true, kBatch },     { "--formant-window", true, kBatch },
    { "--max-formant", true, kBatch },     { "--acoustic-model", true, kBatch }, { "--dictionary", true, kBatch },
    { "--save-session", true, kBatch },
};

// --auto-anchors と一緒にだけ使えるオプション。
const char* const kAutoAnchorOptions[] = { "--divisions", "--formant-window", "--max-formant", "--acoustic-model",
                                           "--dictionary" };

Result<LaunchOptions> fail(std::string msg) {
    return Result<LaunchOptions>::failure(std::move(msg));
}

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out;
    std::size_t              from = 0;
    while (true) {
        const std::size_t comma = s.find(',', from);
        out.push_back(s.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        if (comma == std::string::npos) return out;
        from = comma + 1;
    }
}

// 軸の名前（tx など。大文字でもよい）の添字。知らない名前なら -1。
int axis_index(const std::string& name) {
    const std::string n = ascii_lower(name);
    for (int i = 0; i < kMorphAxisCount; ++i)
        if (n == kMorphAxes[i].key) return i;
    return -1;
}

std::string axis_list() {
    std::string s;
    for (int i = 0; i < kMorphAxisCount; ++i) s += (i ? ", " : "") + std::string { kMorphAxes[i].key };
    return s;
}

// 文字列の全体が整数なら値を返す。
bool parse_int(const std::string& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// 文字列の全体が有限の実数なら値を返す。
bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    char*        end = nullptr;
    const double v   = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// 1つのオプションを LaunchOptions に反映する。誤りがあれば理由を返す（空 = 成功）。
std::string apply_option(LaunchOptions& o, bool fixed_set[], const std::string& name, const std::string& value) {
    BatchCliOptions& b = o.batch;
    if (name == "--align") {
        o.align = true;
    } else if (name == "--no-manifest") {
        b.manifest = false;
    } else if (name == "--overwrite") {
        b.overwrite = true;
    } else if (name == "--auto-anchors") {
        b.auto_anchors = true;
    } else if (name == "--transcript") {
        o.transcript     = value;
        o.has_transcript = true;
    } else if (value.empty()) {
        return name + " の値が空です";
    } else if (name == "--base") {
        o.base = value;
    } else if (name == "--target") {
        o.target = value;
    } else if (name == "--session") {
        o.session = value;
    } else if (name == "--out") {
        b.out = value;
    } else if (name == "--prefix") {
        b.prefix = value;
    } else if (name == "--save-session") {
        b.save_session = value;
    } else if (name == "--acoustic-model") {
        b.auto_settings.align.acoustic_model = value;
    } else if (name == "--dictionary") {
        b.auto_settings.align.dictionary = value;
    } else if (name == "--divisions") {
        int& v = b.auto_settings.divisions;
        if (!parse_int(value, v) || v < AutoAnchorSettings::kDivisionsMin || v > AutoAnchorSettings::kDivisionsMax)
            return "--divisions は " + std::to_string(AutoAnchorSettings::kDivisionsMin) + "〜"
                 + std::to_string(AutoAnchorSettings::kDivisionsMax) + " の整数で指定してください: " + value;
    } else if (name == "--formant-window") {
        int& v = b.auto_settings.formant_window_ms;
        if (!parse_int(value, v) || v < AutoAnchorSettings::kWindowMsMin || v > AutoAnchorSettings::kWindowMsMax)
            return "--formant-window は " + std::to_string(AutoAnchorSettings::kWindowMsMin) + "〜"
                 + std::to_string(AutoAnchorSettings::kWindowMsMax) + " の整数（ms）で指定してください: " + value;
    } else if (name == "--max-formant") {
        double& v = b.auto_settings.formant.max_formant_hz;
        if (!parse_double(value, v) || v < AutoAnchorSettings::kMaxFormantMin || v > AutoAnchorSettings::kMaxFormantMax)
            return "--max-formant は " + std::to_string(static_cast<int>(AutoAnchorSettings::kMaxFormantMin)) + "〜"
                 + std::to_string(static_cast<int>(AutoAnchorSettings::kMaxFormantMax)) + " の数（Hz）で指定してください: "
                 + value;
    } else if (name == "--count") {
        if (!parse_int(value, b.count) || b.count < BatchExportSettings::kCountMin || b.count > BatchExportSettings::kCountMax)
            return "--count は " + std::to_string(BatchExportSettings::kCountMin) + "〜"
                 + std::to_string(BatchExportSettings::kCountMax) + " の整数で指定してください: " + value;
    } else if (name == "--sweep") {
        if (ascii_lower(value) == "all") {
            for (bool& s : b.sweep) s = true;
            return {};
        }
        for (bool& s : b.sweep) s = false;
        for (const std::string& t : split_commas(value)) {
            const int i = axis_index(t);
            if (i < 0) return "--sweep に知らない軸があります: " + t + "（" + axis_list() + "、または all）";
            b.sweep[i] = true;
        }
    } else if (name == "--fixed") {
        for (const std::string& t : split_commas(value)) {
            const std::size_t eq = t.find('=');
            const int         i  = eq == std::string::npos ? -1 : axis_index(t.substr(0, eq));
            if (i < 0) return "--fixed は「軸=率」をコンマで区切って書いてください（軸は " + axis_list() + "）: " + t;
            double v = 0.0;
            if (!parse_double(t.substr(eq + 1), v) || v < 0.0 || v > 1.0)
                return "--fixed の率は 0〜1 の数で指定してください: " + t;
            if (fixed_set[i]) return std::string { "--fixed で " } + kMorphAxes[i].key + " が2回指定されています";
            fixed_set[i] = true;
            b.fixed[i]   = v;
        }
    }
    return {};
}

}    // namespace

LaunchCommand launch_command_of(const std::vector<std::string>& args) {
    if (args.empty()) return LaunchCommand::None;
    const std::string first = ascii_lower(args.front());
    if (first == "gui") return LaunchCommand::Gui;
    if (first == "batch") return LaunchCommand::Batch;
    return LaunchCommand::None;
}

Result<LaunchOptions> parse_launch_options(const std::vector<std::string>& args) {
    LaunchOptions o;
    if (args.empty()) return Result<LaunchOptions>::success(std::move(o));    // 引数なしの起動は gui

    o.command = launch_command_of(args);
    if (o.command == LaunchCommand::None) {
        // サブコマンドなしの --help / --version は全体の説明・版を出す（--version は CI でも使う）。
        for (const std::string& a : args) {
            if (a == "--help" || a == "-h") o.help = true;
            if (a == "--version") o.version = true;
        }
        if (o.help || o.version) return Result<LaunchOptions>::success(std::move(o));
        if (!args.front().empty() && args.front()[0] == '-')
            return fail("サブコマンドがありません。最初に gui か batch を書いてください（例: morphingAlignerCpp gui "
                        + args.front() + " ...）");
        return fail("知らないサブコマンドです: " + args.front() + "（gui か batch）");
    }
    const int         cmd_bit  = o.command == LaunchCommand::Gui ? kGui : kBatch;
    const std::string cmd_name = o.command == LaunchCommand::Gui ? "gui" : "batch";

    // --help / --version は、ほかの引数に誤りがあっても優先する。
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") o.help = true;
        if (args[i] == "--version") o.version = true;
    }
    if (o.help || o.version) return Result<LaunchOptions>::success(std::move(o));

    std::vector<std::string> seen;
    bool                     fixed_set[kMorphAxisCount] = {};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];

        // 「--name 値」と「--name=値」。値を取らないオプションに「=」は付けられない。
        const OptionSpec* opt = nullptr;
        std::string       value;
        for (const OptionSpec& spec : kOptions) {
            const std::string name = spec.name;
            if (a == name) {
                opt = &spec;
                if (spec.takes_value) {
                    if (i + 1 >= args.size()) return fail(name + " に値がありません");
                    value = args[++i];
                }
            } else if (spec.takes_value && a.rfind(name + "=", 0) == 0) {
                opt   = &spec;
                value = a.substr(name.size() + 1);
            }
            if (opt) break;
        }
        if (!opt) return fail("知らない引数です: " + a);
        if (!(opt->commands & cmd_bit)) return fail(std::string { opt->name } + " は " + cmd_name + " では使えません");
        if (std::find(seen.begin(), seen.end(), opt->name) != seen.end())
            return fail(std::string { opt->name } + " が2回指定されています");
        seen.emplace_back(opt->name);
        if (const std::string e = apply_option(o, fixed_set, opt->name, value); !e.empty()) return fail(e);
    }

    if (o.command == LaunchCommand::Gui) {
        // --align は音声が要る。書き起こしは --transcript か、セッションの中のもの（読み込むまで
        // 分からないので、ここでは --session があれば通し、無ければ実行時に知らせる）。
        if (o.align && o.base.empty() && o.target.empty() && o.session.empty())
            return fail("--align には --base / --target / --session のどれかが要ります");
        if (o.align && !o.has_transcript && o.session.empty())
            return fail("--align には --transcript（または書き起こしを含む --session）が要ります");
        return Result<LaunchOptions>::success(std::move(o));
    }

    // batch: 書き出すなら本数と出力先の両方が要る。書き出さないのは、--save-session でセッションを
    // 保存するだけのとき（--auto-anchors で作ったアンカーや、tcmorph のアンカー JSON を変換したもの）。
    // 音声は、音声のパスを含むセッションか、--base と --target の両方（tcmorph のアンカー JSON を
    // --session で乗せてもよい。--base / --target が無ければ同じフォルダの manifest.json から決める）。
    // セッションの中身は読むまで分からないので、--session と --base / --target の組み合わせの可否は
    // 実行時に確かめる。
    const BatchCliOptions& b    = o.batch;
    const auto             seen_opt = [&](const char* n) { return std::find(seen.begin(), seen.end(), n) != seen.end(); };
    if (b.count == 0 && (!b.out.empty() || b.save_session.empty()))
        return fail("batch には --count（書き出す本数）が要ります（セッションを保存するだけなら --save-session）");
    if (b.count > 0 && b.out.empty()) return fail("batch には --out（出力先のフォルダ）が要ります");
    for (const char* n : kAutoAnchorOptions)
        if (seen_opt(n) && !b.auto_anchors) return fail(std::string { n } + " は --auto-anchors と一緒に使います");
    if (o.has_transcript && !b.auto_anchors) return fail("batch の --transcript は --auto-anchors と一緒に使います");
    if (b.auto_anchors && o.session.empty() && (!o.has_transcript || o.transcript.empty()))
        return fail("--auto-anchors には --transcript（または書き起こしを含む --session）が要ります");
    if (o.base.empty() != o.target.empty()) return fail("--base と --target は両方指定してください");
    if (o.base.empty() && o.session.empty())
        return fail("batch には --session か、--base と --target の両方が要ります");
    for (int i = 0; i < kMorphAxisCount; ++i)
        if (fixed_set[i] && b.sweep[i])
            return fail(std::string { "--fixed の " } + kMorphAxes[i].key
                        + " は変化させる軸です（--sweep で外した軸だけ固定できます）");
    if (std::none_of(std::begin(b.sweep), std::end(b.sweep), [](bool s) { return s; }))
        return fail("--sweep で変化させる軸がありません");
    return Result<LaunchOptions>::success(std::move(o));
}

void make_paths_absolute(LaunchOptions& opts) {
    for (std::string* p : { &opts.base, &opts.target, &opts.session, &opts.batch.out, &opts.batch.save_session }) {
        if (p->empty()) continue;
        std::error_code             ec;
        const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::u8path(*p), ec);
        if (!ec) *p = abs.lexically_normal().u8string();
    }
}

std::string launch_usage(LaunchCommand command) {
    static const char* kGuiUsage =
      "使い方: morphingAlignerCpp gui [オプション]\n"
      "  音声・セッションを読み込んで GUI を開く。\n"
      "\n"
      "  --base <音声>          base の音声を読み込む\n"
      "  --target <音声>        target の音声を読み込む\n"
      "  --session <JSON>       セッションを読み込む。tcmorph のアンカー JSON なら、\n"
      "                         --base / --target の音声にアンカーだけを乗せる\n"
      "  --transcript <文>      音素アライメント（MFA）用の書き起こし（セッションのものより優先）\n"
      "  --align                読み込み後、音声解析の環境が使えたら音素アライメントを実行する\n"
      "  -h, --help             この説明を表示して終了する\n"
      "\n"
      "音声のパスを含むセッションと --base / --target は同時には指定できません。\n";
    static const char* kBatchUsage =
      "使い方: morphingAlignerCpp batch --count <本数> --out <フォルダ> [オプション]\n"
      "        morphingAlignerCpp batch --save-session <JSON> [オプション]\n"
      "  ウィンドウを開かずに、率を 0（base）から 1（target）まで等間隔に変えた WAV を書き出す\n"
      "  （GUI の「連続書き出し」と同じ処理）。アンカーはセッションのものを使うか、--auto-anchors で\n"
      "  音素アライメントから作る。--count / --out を省けば、セッションを保存するだけで止める。\n"
      "\n"
      "  --session <JSON>       セッション（音声のパスとアンカー）。tcmorph のアンカー JSON なら、\n"
      "                         音声は --base / --target か、同じフォルダの manifest.json から決める\n"
      "  --base <音声>          base の音声（--target と一緒に指定する）\n"
      "  --target <音声>        target の音声\n"
      "  --count <本数>         書き出す本数（両端を含む。2〜101）\n"
      "  --out <フォルダ>       出力先（無ければ作る）\n"
      "  --prefix <接頭辞>      ファイル名の接頭辞（既定: base と target のファイル名をつないだもの）\n"
      "  --sweep <軸,...>       変化させる軸（tx, fx, fo, sl, ap をコンマで区切る。既定: all）\n"
      "  --fixed <軸=率,...>    変化させない軸の率（既定: 0）。例: --sweep fo --fixed tx=0.5\n"
      "  --no-manifest          率とアンカーを残す JSON（<接頭辞>_batch.json）を書かない\n"
      "  --overwrite            同じ名前のファイルがあれば上書きする（無ければ、あるときは中止する）\n"
      "  --save-session <JSON>  使うアンカーと音声をセッションとして保存する（GUI で開いて確かめられる）\n"
      "\n"
      "  --auto-anchors         音素アライメント（MFA）とフォルマントから、アンカーを作り直して使う\n"
      "                         （音声解析の環境 .env が要る。配布物のルートで実行する）\n"
      "  --transcript <文>      書き起こし（セッションのものより優先）\n"
      "  --divisions <n>        各音素の区間を何等分するか（1〜10。既定: 2）\n"
      "  --formant-window <ms>  フォルマントの移動平均の窓幅（5〜500。既定: 50）\n"
      "  --max-formant <Hz>     最大フォルマント（base / target 共通。2000〜10000。既定: 5500）\n"
      "  --acoustic-model <名前> MFA の音響モデル（既定: japanese_mfa）\n"
      "  --dictionary <名前>    MFA の辞書（既定: japanese_mfa）\n"
      "  -h, --help             この説明を表示して終了する\n"
      "\n"
      "ファイル名は <接頭辞>_r050.wav（軸を選んだときは <接頭辞>_fo_r050.wav）。\n"
      "終了コード: 0 = 成功、1 = 処理の失敗、2 = 引数の誤り、130 = Ctrl+C で中止。\n";
    static const char* kCommon =
      "\n"
      "値は「--base a.wav」「--base=a.wav」のどちらでも書けます。相対パスは起動したときの\n"
      "カレントディレクトリが基準です。\n";

    switch (command) {
    case LaunchCommand::Gui: return std::string { kGuiUsage } + kCommon;
    case LaunchCommand::Batch: return std::string { kBatchUsage } + kCommon;
    case LaunchCommand::None: break;
    }
    return "使い方: morphingAlignerCpp <サブコマンド> [オプション]\n"
           "\n"
           "  gui      音声・セッションを読み込んで GUI を開く\n"
           "  batch    ウィンドウを開かずに連続書き出しをする\n"
           "\n"
           "  --version              版を表示して終了する\n"
           "  -h, --help             この説明を表示して終了する\n"
           "\n"
           "引数なしで起動すると、何も読み込まずに GUI を開きます。\n"
           "サブコマンドごとのオプションは「morphingAlignerCpp gui --help」「morphingAlignerCpp batch --help」\n"
           "で表示します。\n";
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
