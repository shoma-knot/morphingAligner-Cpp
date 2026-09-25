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
        try {
            // std::function はコピー可能な呼び出し体を要求するので shared_ptr で持ち回す。
            auto audio = std::make_shared<AnalyzedAudio>(analyze_file(path));
            return [side, path, audio](App& a) { apply_track(a.track(side), path, std::move(*audio)); };
        } catch (const std::exception& e) {
            applog::add(name + " 読み込み失敗: " + e.what());
            return {};
        }
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
        std::string werr;
        if (write_wav(p, *wave, fs, werr))
            applog::add(std::string { "WAV 保存: " } + p);
        else
            applog::add("WAV 保存失敗: " + werr);
        return {};
    });
}
