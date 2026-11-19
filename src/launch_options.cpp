#include "launch_options.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
constexpr int kGui     = 1;
constexpr int kAuto    = 2;
constexpr int kConvert = 4;
constexpr int kExport  = 8;

struct OptionSpec {
    const char* name;
    bool        takes_value;
    int         commands;
};
const OptionSpec kOptions[] = {
    { "--base", true, kGui | kAuto | kConvert },
    { "--target", true, kGui | kAuto | kConvert },
    { "--session", true, kGui | kAuto },
    { "--transcript", true, kGui | kAuto },
    { "--align", false, kGui },
    { "--out", true, kAuto | kConvert | kExport },
    { "--overwrite", false, kAuto | kConvert | kExport },
    { "--divisions", true, kAuto },
    { "--formant-window", true, kAuto },
    { "--max-formant", true, kAuto },
    { "--acoustic-model", true, kAuto },
    { "--dictionary", true, kAuto },
    { "--count", true, kExport },
    { "--prefix", true, kExport },
    { "--sweep", true, kExport },
    { "--fixed", true, kExport },
    { "--no-manifest", false, kExport },
    { "--envelope-mix", true, kExport },
    { "--ot-gamma", true, kExport },
    { "--ot-range", true, kExport },
};

// サブコマンドの名前とビット。
struct CommandSpec {
    LaunchCommand command;
    const char*   name;
    int           bit;
};
const CommandSpec kCommands[] = {
    { LaunchCommand::Gui, "gui", kGui },
    { LaunchCommand::AutoAnchors, "auto-anchors", kAuto },
    { LaunchCommand::Convert, "convert", kConvert },
    { LaunchCommand::Export, "export", kExport },
};

const CommandSpec& command_spec(LaunchCommand c) {
    for (const CommandSpec& cs : kCommands)
        if (cs.command == c) return cs;
    return kCommands[0];
}

Result<LaunchOptions> fail(std::string msg) {
    return Result<LaunchOptions>::failure(std::move(msg));
}

// 範囲の値を、誤りの文言に入れる短い形にする（0.05、1、300、24000）。
std::string format_number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
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
    ExportCliOptions&   b  = o.exp;
    AutoAnchorSettings& as = o.auto_settings;
    if (name == "--align") {
        o.align = true;
    } else if (name == "--envelope-mix") {
        const std::string v = ascii_lower(value);
        if (v == "average") b.post.envelope = EnvelopeMix::Average;
        else if (v == "ot") b.post.envelope = EnvelopeMix::Transport;
        else return "--envelope-mix は average か ot で指定してください: " + value;
    } else if (name == "--ot-gamma") {
        double& v = b.post.transport_gamma;
        if (!parse_double(value, v) || v < MorphPostOptions::kGammaMin || v > MorphPostOptions::kGammaMax)
            return "--ot-gamma は " + format_number(MorphPostOptions::kGammaMin) + "〜"
                 + format_number(MorphPostOptions::kGammaMax) + " の数で指定してください: " + value;
    } else if (name == "--ot-range") {
        // 「下限-上限」（Hz）。GUI と同じく、下限は上限より 100 Hz 以上低くする。
        const std::size_t dash = value.find('-', 1);
        double            lo = 0.0, hi = 0.0;
        if (dash == std::string::npos || !parse_double(value.substr(0, dash), lo) || !parse_double(value.substr(dash + 1), hi))
            return "--ot-range は「下限-上限」（Hz）で指定してください（例: 5000-12000）: " + value;
        if (lo < MorphPostOptions::kRangeMinHz || hi > MorphPostOptions::kRangeMaxHz || lo > hi - 100.0)
            return "--ot-range は " + format_number(MorphPostOptions::kRangeMinHz) + "〜"
                 + format_number(MorphPostOptions::kRangeMaxHz) + " Hz の中で、下限を上限より 100 Hz 以上低くしてください: "
                 + value;
        b.post.transport_lo_hz = lo;
        b.post.transport_hi_hz = hi;
    } else if (name == "--no-manifest") {
        b.manifest = false;
    } else if (name == "--overwrite") {
        o.overwrite = true;
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
        o.out = value;
    } else if (name == "--prefix") {
        b.prefix = value;
    } else if (name == "--acoustic-model") {
        as.align.acoustic_model = value;
    } else if (name == "--dictionary") {
        as.align.dictionary = value;
    } else if (name == "--divisions") {
        int& v = as.divisions;
        if (!parse_int(value, v) || v < AutoAnchorSettings::kDivisionsMin || v > AutoAnchorSettings::kDivisionsMax)
            return "--divisions は " + std::to_string(AutoAnchorSettings::kDivisionsMin) + "〜"
                 + std::to_string(AutoAnchorSettings::kDivisionsMax) + " の整数で指定してください: " + value;
    } else if (name == "--formant-window") {
        int& v = as.formant_window_ms;
        if (!parse_int(value, v) || v < AutoAnchorSettings::kWindowMsMin || v > AutoAnchorSettings::kWindowMsMax)
            return "--formant-window は " + std::to_string(AutoAnchorSettings::kWindowMsMin) + "〜"
                 + std::to_string(AutoAnchorSettings::kWindowMsMax) + " の整数（ms）で指定してください: " + value;
    } else if (name == "--max-formant") {
        double& v = as.formant.max_formant_hz;
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
    for (const CommandSpec& cs : kCommands)
        if (first == cs.name) return cs.command;
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
            return fail("サブコマンドがありません。最初に gui・auto-anchors・convert・export のどれかを書いてください"
                        "（例: morphingAlignerCpp gui " + args.front() + " ...）");
        // batch は auto-anchors / convert / export に分けてなくした（2026-11-17）。古いスクリプトに分かるように知らせる。
        if (ascii_lower(args.front()) == "batch")
            return fail("batch サブコマンドはなくなりました。アンカーの自動生成は auto-anchors、tcmorph のアンカー JSON の"
                        "変換は convert、連続書き出しは export を使ってください");
        return fail("知らないサブコマンドです: " + args.front() + "（gui・auto-anchors・convert・export）");
    }
    const int         cmd_bit  = command_spec(o.command).bit;
    const std::string cmd_name = command_spec(o.command).name;
    const bool        takes_input = o.command == LaunchCommand::Convert || o.command == LaunchCommand::Export;

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
        if (!opt) {
            // convert / export は、入力の JSON をオプション名なしで1つ受け取る。
            if (takes_input && !a.empty() && a[0] != '-') {
                if (!o.input.empty()) return fail("入力の JSON は1つだけ指定してください: " + a);
                o.input = a;
                continue;
            }
            return fail("知らない引数です: " + a);
        }
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

    if (o.base.empty() != o.target.empty()) return fail("--base と --target は両方指定してください");

    if (o.command == LaunchCommand::AutoAnchors) {
        // 音声は --base / --target か、音声のパスを含むセッション。書き起こしは --transcript か、
        // セッションの中のもの（読むまで分からないので、--session があれば実行時に確かめる）。
        if (o.base.empty() && o.session.empty())
            return fail("auto-anchors には --base と --target、または --session が要ります");
        if (o.session.empty() && (!o.has_transcript || o.transcript.empty()))
            return fail("auto-anchors には --transcript（または書き起こしを含む --session）が要ります");
        if (o.out.empty()) return fail("auto-anchors には --out（保存するセッション）が要ります");
        return Result<LaunchOptions>::success(std::move(o));
    }

    if (o.input.empty()) return fail(cmd_name + " には入力の JSON が要ります（例: morphingAlignerCpp " + cmd_name + " work.json ...）");
    if (o.command == LaunchCommand::Convert) {
        if (o.out.empty()) return fail("convert には --out（保存するセッション）が要ります");
        return Result<LaunchOptions>::success(std::move(o));
    }

    // export
    const ExportCliOptions& b = o.exp;
    if (b.count == 0) return fail("export には --count（書き出す本数）が要ります");
    if (o.out.empty()) return fail("export には --out（出力先のフォルダ）が要ります");
    // 黙って無視すると、OT が効いていると思い込んだまま書き出してしまうので誤りにする（ユーザーの指示）。
    if (b.post.envelope != EnvelopeMix::Transport)
        for (const char* n : { "--ot-gamma", "--ot-range" })
            if (std::find(seen.begin(), seen.end(), n) != seen.end())
                return fail(std::string { n } + " は --envelope-mix ot と一緒に使います");
    for (int i = 0; i < kMorphAxisCount; ++i)
        if (fixed_set[i] && b.sweep[i])
            return fail(std::string { "--fixed の " } + kMorphAxes[i].key
                        + " は変化させる軸です（--sweep で外した軸だけ固定できます）");
    if (std::none_of(std::begin(b.sweep), std::end(b.sweep), [](bool s) { return s; }))
        return fail("--sweep で変化させる軸がありません");
    return Result<LaunchOptions>::success(std::move(o));
}

void make_paths_absolute(LaunchOptions& opts) {
    for (std::string* p : { &opts.base, &opts.target, &opts.session, &opts.input, &opts.out }) {
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
    static const char* kAutoUsage =
      "使い方: morphingAlignerCpp auto-anchors --base <音声> --target <音声> --transcript <文> --out <JSON> [オプション]\n"
      "        morphingAlignerCpp auto-anchors --session <JSON> --out <JSON> [オプション]\n"
      "  音素アライメント（MFA）とフォルマントからアンカーを作り（GUI の「アンカー自動生成」と同じ方法）、\n"
      "  セッションとして保存する。音声解析の環境 .env が要る（配布物のルートで実行する）。\n"
      "\n"
      "  --base <音声>          base の音声（--target と一緒に指定する）\n"
      "  --target <音声>        target の音声\n"
      "  --session <JSON>       音声のパス（と書き起こし）を含むセッション。中のアンカーは使わず作り直す\n"
      "  --transcript <文>      書き起こし（セッションのものより優先）\n"
      "  --out <JSON>           保存するセッション\n"
      "  --divisions <n>        各音素の区間を何等分するか（1〜10。既定: 2）\n"
      "  --formant-window <ms>  フォルマントの移動平均の窓幅（5〜500。既定: 50）\n"
      "  --max-formant <Hz>     最大フォルマント（base / target 共通。2000〜10000。既定: 5500）\n"
      "  --acoustic-model <名前> MFA の音響モデル（既定: japanese_mfa）\n"
      "  --dictionary <名前>    MFA の辞書（既定: japanese_mfa）\n"
      "  --overwrite            保存先があれば上書きする（付けなければ、あるときは中止する）\n"
      "  -h, --help             この説明を表示して終了する\n";
    static const char* kConvertUsage =
      "使い方: morphingAlignerCpp convert <tcmorph のアンカー JSON> --out <JSON> [オプション]\n"
      "  tcmorph のアンカー JSON（mat2json.py の anchors.json）をセッションにする。音声は --base / --target か、\n"
      "  同じフォルダの manifest.json（fileFullPath と fileName）から決める。\n"
      "\n"
      "  --out <JSON>           保存するセッション\n"
      "  --base <音声>          base の音声（--target と一緒に指定する。manifest.json は読まない）\n"
      "  --target <音声>        target の音声\n"
      "  --overwrite            保存先があれば上書きする（付けなければ、あるときは中止する）\n"
      "  -h, --help             この説明を表示して終了する\n";
    static const char* kExportUsage =
      "使い方: morphingAlignerCpp export <セッション> --count <本数> --out <フォルダ> [オプション]\n"
      "  率を 0（base）から 1（target）まで等間隔に変えた WAV を書き出す（GUI の「連続書き出し」と同じ処理）。\n"
      "  セッションは、音声のパスとアンカーを含むもの（GUI で保存したもの、auto-anchors や convert の出力）。\n"
      "\n"
      "  --count <本数>         書き出す本数（両端を含む。2〜101）\n"
      "  --out <フォルダ>       出力先（無ければ作る）\n"
      "  --prefix <接頭辞>      ファイル名の接頭辞（既定: base と target のファイル名をつないだもの）\n"
      "  --sweep <軸,...>       変化させる軸（tx, fx, fo, sl, ap をコンマで区切る。既定: all）\n"
      "  --fixed <軸=率,...>    変化させない軸の率（既定: 0）。例: --sweep fo --fixed tx=0.5\n"
      "  --no-manifest          率とアンカーを残す JSON（<接頭辞>_batch.json）を書かない\n"
      "  --overwrite            同じ名前のファイルがあれば上書きする（付けなければ、あるときは中止する）\n"
      "  --envelope-mix <方式>  包絡の混ぜ方。average は平均のまま、ot は範囲の中だけ OT で補間する\n"
      "                         （GUI の「OT で補間」。1本ごとの合成に 2〜3 倍の時間がかかる。既定: average）\n"
      "  --ot-gamma <γ>         OT の質量 = パワー^γ（0.05〜1。既定: 0.3）\n"
      "  --ot-range <下限-上限> OT で補間する範囲 [Hz]（300〜24000。既定: 5000-12000）\n"
      "  -h, --help             この説明を表示して終了する\n"
      "\n"
      "ファイル名は <接頭辞>_r050.wav（軸を選んだときは <接頭辞>_fo_r050.wav、OT のときは\n"
      "<接頭辞>_ot030_5000-12000_r050.wav）。--ot-gamma と --ot-range は --envelope-mix ot と一緒に使う。\n";
    static const char* kExitCodes =
      "\n"
      "作ったファイルの一覧は標準出力に、経過は標準エラーに出す。\n"
      "終了コード: 0 = 成功、1 = 処理の失敗、2 = 引数の誤り、130 = Ctrl+C で中止。\n";
    static const char* kCommon =
      "\n"
      "値は「--base a.wav」「--base=a.wav」のどちらでも書けます。相対パスは起動したときの\n"
      "カレントディレクトリが基準です。\n";

    switch (command) {
    case LaunchCommand::Gui: return std::string { kGuiUsage } + kCommon;
    case LaunchCommand::AutoAnchors: return std::string { kAutoUsage } + kExitCodes + kCommon;
    case LaunchCommand::Convert: return std::string { kConvertUsage } + kExitCodes + kCommon;
    case LaunchCommand::Export: return std::string { kExportUsage } + kExitCodes + kCommon;
    case LaunchCommand::None: break;
    }
    return "使い方: morphingAlignerCpp <サブコマンド> [オプション]\n"
           "\n"
           "  gui           音声・セッションを読み込んで GUI を開く\n"
           "  auto-anchors  音素アライメントからアンカーを作ってセッションに保存する\n"
           "  convert       tcmorph のアンカー JSON をセッションにする\n"
           "  export        セッションから、率を変えた WAV を連続で書き出す\n"
           "\n"
           "  --version              版を表示して終了する\n"
           "  -h, --help             この説明を表示して終了する\n"
           "\n"
           "引数なしで起動すると、何も読み込まずに GUI を開きます。\n"
           "サブコマンドごとのオプションは「morphingAlignerCpp <サブコマンド> --help」で表示します。\n";
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
