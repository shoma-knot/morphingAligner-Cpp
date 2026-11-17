// export サブコマンド（cli_commands.hpp）。

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "analysis.hpp"
#include "batch_export.hpp"
#include "cli_commands.hpp"
#include "cli_common.hpp"

using namespace cli;

int run_export_command(const LaunchOptions& opts, std::FILE* out, std::FILE* log) {
    SigintScope sigint;

    Result<SessionFile> in = read_session_file(opts.input, log);
    if (!in.ok()) {
        say(log, "エラー: セッションを" + in.error);
        return kExitFailed;
    }
    if (in.value.anchors_only) {
        say(log, "エラー: tcmorph のアンカー JSON は音声を持たないので、先に convert でセッションにしてください: "
                 + opts.input);
        return kExitFailed;
    }
    SessionFile& sf = in.value;

    // 書き出しの設定（GUI のダイアログの項目と同じ）。
    const ExportCliOptions& e = opts.exp;
    BatchExportSettings     s;
    s.count          = e.count;
    s.dir            = opts.out;
    s.prefix         = e.prefix.empty() ? std::filesystem::u8path(sf.base_path).stem().u8string() + "-"
                                            + std::filesystem::u8path(sf.target_path).stem().u8string()
                                        : e.prefix;    // 既定は GUI の連続書き出しと同じ
    s.write_manifest = e.manifest;
    for (int i = 0; i < kMorphAxisCount; ++i) {
        s.sweep[i]                  = e.sweep[i];
        s.fixed.*kMorphAxes[i].rate = e.fixed[i];
    }
    // 出力先は無ければ作る（設定の検査が出力先の存在を確かめるので、その前に）。時間のかかる解析の前に、
    // 書き出せるかを確かめておく。
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(s.dir), ec);
    if (const Status v = validate_batch_settings(s); !v.ok()) {
        say(log, "エラー: " + v.error);
        return kExitFailed;
    }
    if (const int n = count_existing_batch_files(s); n > 0 && !opts.overwrite) {
        say(log, "エラー: 出力先に同じ名前のファイルが " + std::to_string(n)
                   + " 個あります（上書きするなら --overwrite を付けてください）: " + s.dir);
        return kExitFailed;
    }

    // 解析（GUI で音声を開いたときと同じ WORLD の解析）。
    BatchExportRequest req;
    for (const auto& [name, path, dst] : { std::tuple { "base", sf.base_path, &req.base },
                                           std::tuple { "target", sf.target_path, &req.target } }) {
        say(log, std::string { name } + " を解析中: " + path);
        Result<MorphChannel> c = analyze_channel(path);
        if (!c.ok()) {
            say(log, std::string { "エラー: " } + name + " を解析できません: " + c.error);
            return kExitFailed;
        }
        *dst = std::make_shared<const MorphChannel>(std::move(c.value));
    }
    req.anchors     = sf.anchors;
    req.base_path   = sf.base_path;
    req.target_path = sf.target_path;
    req.transcript  = sf.transcript;
    req.settings    = s;
    // セッションの記録（auto_anchors・tcmorph など）を、書き出しの JSON に引き継ぐ。
    req.manifest_extra = sf.extras;
    if (req.anchors.empty()) say(log, "注意: アンカーがありません（始点と終点だけで対応づけます）");

    // 書き出しは別スレッドで走らせ、1本書けるごとに経過を出す。Ctrl+C なら次の1本の前で止める。
    const std::vector<double> rates = batch_rates(s.count);
    BatchExportProgress       progress;
    BatchExportResult         result;
    std::atomic<bool>         finished { false };
    std::thread               worker([&]() {
        try {
            result = run_batch_export(req, progress);
        } catch (const std::exception& ex) {
            result.error = std::string { "書き出し中の例外: " } + ex.what();
        }
        finished.store(true);
    });
    int        shown     = 0;
    const auto show_done = [&]() {
        for (const int done = progress.done.load(); shown < done && shown < static_cast<int>(rates.size()); ++shown)
            say(log, "[" + std::to_string(shown + 1) + "/" + std::to_string(s.count) + "] "
                       + batch_wav_name(s, rates[static_cast<std::size_t>(shown)]));
    };
    while (!finished.load()) {
        if (g_interrupted.load()) progress.cancel.store(true);
        show_done();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    worker.join();
    show_done();

    for (const std::string& w : result.warnings) say(log, "警告: " + w);
    if (!result.ok()) {
        say(log, "エラー: " + result.error + "（" + std::to_string(result.written) + " 本まで書き出しました）");
        return kExitFailed;
    }

    // 書き出したファイルの一覧（スクリプトから使いやすいよう、標準出力に1行1ファイル）。
    for (int k = 0; k < result.written; ++k)
        say(out, (std::filesystem::u8path(s.dir) / std::filesystem::u8path(batch_wav_name(s, rates[static_cast<std::size_t>(k)])))
                   .u8string());
    if (!result.manifest.empty()) say(out, result.manifest);
    if (result.canceled) {
        say(log, "中止しました（" + std::to_string(result.written) + " 本まで書き出しました）");
        return kExitInterrupted;
    }
    say(log, "完了: " + std::to_string(result.written) + " 本を書き出しました: " + s.dir);
    return kExitOk;
}
