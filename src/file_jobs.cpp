#include "file_jobs.hpp"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <tinyfiledialogs.h>

#include "analysis.hpp"
#include "app.hpp"
#include "launch_options.hpp"
#include "log.hpp"
#include "morphing.hpp"
#include "session.hpp"

namespace {

// セッションの既定保存パス。実行ファイルのパス取得は OS 固有になるため、移植性を優先
// してカレントディレクトリ（多くは起動ディレクトリ＝バイナリのある場所）を使う。
const std::string& default_session_path() {
    static const std::string path = [] {
        std::error_code ec;
        const auto      dir = std::filesystem::current_path(ec);
        return ec ? std::string { "session.json" } : (dir / "session.json").string();
    }();
    return path;
}

}    // namespace

void launch_load_track_job(App& app, Side side) {
    const std::string name = app.track(side).name;
    app.jobs.ui.try_launch([name, side]() -> JobApply {
        static const char* kAudioFilter[] = { "*.wav", "*.flac", "*.mp3", "*.ogg" };
        const std::string  title          = name + " 音声を選択";
        const char* picked = tinyfd_openFileDialog(title.c_str(), "", 4, kAudioFilter, "音声ファイル", 0);
        if (!picked) return {};    // キャンセル
        const std::string path = picked;
        applog::add(name + " 解析中...: " + path);
        Result<AnalyzedAudio> r = analyze_file(path);
        if (!r.ok()) {
            applog::add(name + " 読み込み失敗: " + r.error);
            return {};
        }
        // std::function はコピー可能な呼び出し体を要求するので shared_ptr で持ち回す。
        auto audio = std::make_shared<AnalyzedAudio>(std::move(r.value));
        return [side, path, audio](App& a) { apply_track(a.track(side), path, std::move(*audio)); };
    });
}

void apply_launch_options(App& app, const LaunchOptions& opts) {
    app.speech.align_on_ready = opts.align;
    if (!opts.has_files()) {
        if (opts.has_transcript) app.speech.transcript = opts.transcript;
        return;
    }

    app.jobs.ui.try_launch([opts]() -> JobApply {
        // セッション（音声のパスを含むものは、その音声の解析まで）。失敗は load_session_data がログに出す。
        std::shared_ptr<SessionLoadData> session;
        if (!opts.session.empty()) {
            applog::add("セッションを読み込み中...: " + opts.session);
            session = std::make_shared<SessionLoadData>(load_session_data(opts.session));
            if (!session->ok) {
                session.reset();
            } else if (!session->anchors_only && (!opts.base.empty() || !opts.target.empty())) {
                // どちらの音声を使うか決められないので、何も読み込まない。
                applog::add("起動時の読み込みを中止: 音声のパスを含むセッションと --base / --target は"
                            "同時に指定できません");
                return {};
            }
        }

        // --base / --target の音声。
        struct Loaded {
            Side          side;
            std::string   path;
            AnalyzedAudio audio;
        };
        auto tracks = std::make_shared<std::vector<Loaded>>();
        for (const auto& [side, path] : { std::pair { Side::Base, opts.base }, std::pair { Side::Target, opts.target } }) {
            if (path.empty()) continue;
            const std::string name = side == Side::Base ? "base" : "target";
            applog::add(name + " 解析中...: " + path);
            Result<AnalyzedAudio> r = analyze_file(path);
            if (!r.ok()) {
                applog::add(name + " 読み込み失敗: " + r.error);
                continue;
            }
            tracks->push_back(Loaded { side, path, std::move(r.value) });
        }

        return [opts, session, tracks](App& a) {
            for (Loaded& t : *tracks) apply_track(a.track(t.side), t.path, std::move(t.audio));
            // tcmorph 形式のアンカーは、読み込み済みの音声に乗せるので音声の後に適用する。
            if (session) apply_session_data(a, std::move(*session));
            // 書き起こしはセッションの中身より --transcript を優先する。
            if (opts.has_transcript) a.speech.transcript = opts.transcript;
        };
    });
}

void launch_save_session_job(App& app) {
    // ダイアログはブロックするのでワーカーで開く（書き出し自体は速いのでメインで）。
    app.jobs.ui.try_launch([def = default_session_path()]() -> JobApply {
        static const char* kJsonFilter[] = { "*.json" };
        const char* p = tinyfd_saveFileDialog("セッションを保存", def.c_str(), 1, kJsonFilter, "JSON");
        if (!p) return {};    // キャンセル
        const std::string path = p;
        return [path](App& a) { save_session(a, path); };
    });
}

void launch_load_session_job(App& app) {
    app.jobs.ui.try_launch([def = default_session_path()]() -> JobApply {
        static const char* kJsonFilter[] = { "*.json" };
        const char* p = tinyfd_openFileDialog("セッションを読み込み", def.c_str(), 1, kJsonFilter, "JSON", 0);
        if (!p) return {};    // キャンセル
        // JSON パース＋両音声の解析までワーカーで行う（GL なし）。
        auto d = std::make_shared<SessionLoadData>(load_session_data(p));
        if (!d->ok) return {};
        return [d](App& a) { apply_session_data(a, std::move(*d)); };
    });
}

void launch_save_wav_job(App& app) {
    // 波形をコピーしてワーカーへ（ダイアログ→書き出しまでワーカーで完結。GL なし）。
    auto      wave = std::make_shared<const std::vector<double>>(app.morph.out.wave);
    const int fs   = app.morph.out.fs;
    app.jobs.ui.try_launch([wave, fs]() -> JobApply {
        static const char* kWavFilter[] = { "*.wav" };
        const char* p = tinyfd_saveFileDialog("モーフィング結果を保存", "morph.wav", 1, kWavFilter, "WAV");
        if (!p) return {};    // キャンセル
        const Status st = write_wav(p, *wave, fs);
        if (st.ok())
            applog::add(std::string { "WAV 保存: " } + p);
        else
            applog::add("WAV 保存失敗: " + st.error);
        return {};
    });
}
