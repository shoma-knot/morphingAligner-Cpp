// convert サブコマンド（cli_commands.hpp）。

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "analysis.hpp"
#include "cli_commands.hpp"
#include "cli_common.hpp"
#include "tcmorph_manifest.hpp"

using namespace cli;

namespace {

// 同じフォルダの manifest.json から音声を決める。標本化周波数が manifest と違えば、別の音声を
// 指しているおそれがあるので誤りにする。record に manifest の記録を入れる。
Status apply_manifest(const std::string& anchors_path, SessionFile& sf, nlohmann::json& record, std::FILE* log) {
    const Result<TcmorphManifest> m = read_tcmorph_manifest(anchors_path);
    if (!m.ok()) return Status::failure(m.error);
    const Result<AudioInfo> b = read_audio_info(m.value.base_path);
    if (!b.ok()) return Status::failure("base の音声を読めません: " + m.value.base_path + "（" + b.error + "）");
    const Result<AudioInfo> t = read_audio_info(m.value.target_path);
    if (!t.ok()) return Status::failure("target の音声を読めません: " + m.value.target_path + "（" + t.error + "）");
    if (const Status st = check_manifest_sample_rates(m.value, b.value.fs, t.value.fs); !st.ok()) return st;

    sf.base_path   = m.value.base_path;
    sf.target_path = m.value.target_path;
    record         = nlohmann::json::parse(m.value.record);
    say(log, "manifest.json から音声を決めました: " + m.value.path);
    say(log, "  base:   " + sf.base_path);
    say(log, "  target: " + sf.target_path);
    return {};
}

}    // namespace

int run_convert_command(const LaunchOptions& opts, std::FILE* out, std::FILE* log) {
    Result<SessionFile> in = read_session_file(opts.input, log);
    if (!in.ok()) {
        say(log, "エラー: 入力を" + in.error);
        return kExitFailed;
    }
    if (!in.value.anchors_only) {
        say(log, "エラー: convert は tcmorph のアンカー JSON（objects を持つもの）を変換します。セッションなら、そのまま"
                 "export に渡せます: " + opts.input);
        return kExitFailed;
    }
    if (const Status st = check_not_exists(opts.out, opts.overwrite); !st.ok()) {
        say(log, "エラー: " + st.error);
        return kExitFailed;
    }

    SessionFile sf;
    sf.anchors = std::move(in.value.anchors);
    // 元の anchors.json（と manifest.json の情報）を "tcmorph" に残す。
    nlohmann::json record = nlohmann::json::object();
    if (opts.base.empty()) {
        if (const Status st = apply_manifest(opts.input, sf, record, log); !st.ok()) {
            say(log, "エラー: " + st.error);
            return kExitFailed;
        }
    } else {
        // --base / --target があれば manifest.json は読まない（場所が変わった音声を指すときの逃げ道）。
        for (const std::string* p : { &opts.base, &opts.target }) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(std::filesystem::u8path(*p), ec)) {
                say(log, "エラー: 音声が見つかりません: " + *p);
                return kExitFailed;
            }
        }
        sf.base_path   = opts.base;
        sf.target_path = opts.target;
        record["anchors"] = opts.input;
    }
    sf.extras = nlohmann::json { { "tcmorph", record } }.dump();

    if (const Status st = save_session_file(opts.out, sf); !st.ok()) {
        say(log, "エラー: セッションを保存できません: " + st.error);
        return kExitFailed;
    }
    say(out, opts.out);
    say(log, "セッションを保存しました: " + opts.out + "（アンカー " + std::to_string(sf.anchors.size()) + " 個）");
    return kExitOk;
}
