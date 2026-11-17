// auto-anchors サブコマンド（cli_commands.hpp）。

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "analysis.hpp"
#include "cli_commands.hpp"
#include "cli_common.hpp"

using namespace cli;

int run_auto_anchors_command(const LaunchOptions& opts, std::FILE* out, std::FILE* log, const AutoAnchorTools* tools) {
    SigintScope sigint;

    // 音声と書き起こし（--session か --base / --target）。
    SessionFile sf;
    sf.base_path   = opts.base;
    sf.target_path = opts.target;
    if (!opts.session.empty()) {
        Result<SessionFile> in = read_session_file(opts.session, log);
        if (!in.ok()) {
            say(log, "エラー: セッションを" + in.error);
            return kExitFailed;
        }
        if (in.value.anchors_only) {
            say(log, "エラー: tcmorph のアンカー JSON は音声を持たないので使えません。音声を --base / --target で"
                     "指定してください: " + opts.session);
            return kExitFailed;
        }
        // GUI の起動時の読み込みと同じく、どちらの音声を使うか決められないので誤りにする。
        if (!opts.base.empty()) {
            say(log, "エラー: 音声のパスを含むセッションと --base / --target は同時に指定できません");
            return kExitFailed;
        }
        sf.base_path   = std::move(in.value.base_path);
        sf.target_path = std::move(in.value.target_path);
        sf.transcript  = std::move(in.value.transcript);
        if (!in.value.anchors.empty())
            say(log, "注意: セッションのアンカー " + std::to_string(in.value.anchors.size()) + " 個は使わず、作り直します");
        // 元のセッションの記録（tcmorph・batch など）は、作り直したアンカーには当てはまらないので引き継がない。
    }
    if (opts.has_transcript) sf.transcript = opts.transcript;    // --transcript はセッションの書き起こしより優先
    if (sf.transcript.empty()) {
        say(log, "エラー: 書き起こしがありません（--transcript か、書き起こしを含むセッションを指定してください）");
        return kExitFailed;
    }
    // 時間のかかる MFA の前に、保存できるかを確かめておく。
    if (const Status st = check_not_exists(opts.out, opts.overwrite); !st.ok()) {
        say(log, "エラー: " + st.error);
        return kExitFailed;
    }

    // 音声の長さ（GUI では解析した MorphChannel::duration。同じ値をデコードだけで求める）。
    AutoAnchorSource src[2] = { { sf.base_path, 0.0 }, { sf.target_path, 0.0 } };
    for (AutoAnchorSource& s : src) {
        const Result<AudioInfo> info = read_audio_info(s.path);
        if (!info.ok()) {
            say(log, "エラー: 音声を読めません: " + s.path + "（" + info.error + "）");
            return kExitFailed;
        }
        s.duration = info.value.duration;
    }

    AutoAnchorSettings as = opts.auto_settings;
    as.transcript         = sf.transcript;
    const AutoAnchorPipelineResult r =
      build_auto_anchors(src[0], src[1], as, tools ? *tools : AutoAnchorTools::real(),
                         [&](const std::string& m) { say(log, m); }, &g_interrupted);
    for (const std::string& w : r.warnings) say(log, "警告: " + w);
    if (r.canceled) {
        say(log, "中止しました");
        return kExitInterrupted;
    }
    if (!r.ok()) {
        say(log, "エラー: " + r.error);
        return kExitFailed;
    }

    // 生成の設定と音素列を "auto_anchors" に残す（export が書き出しの JSON に引き継ぐ）。
    sf.anchors = r.anchors;
    sf.extras  = nlohmann::json { { "auto_anchors", nlohmann::json::parse(auto_anchor_record_json(as, r)) } }.dump();
    if (const Status st = save_session_file(opts.out, sf); !st.ok()) {
        say(log, "エラー: セッションを保存できません: " + st.error);
        return kExitFailed;
    }
    say(out, opts.out);
    say(log, "セッションを保存しました: " + opts.out);
    return kExitOk;
}
