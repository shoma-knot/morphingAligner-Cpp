#include "batch_cli.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#include "analysis.hpp"
#include "batch_export.hpp"
#include "session_io.hpp"

namespace {

void say(std::FILE* f, const std::string& s) {
    std::fputs((s + "\n").c_str(), f);
    std::fflush(f);
}

// 音声とアンカー（セッションから決めたもの）。
struct BatchInput {
    std::string         base_path, target_path, transcript;
    std::vector<Anchor> anchors;
};

// --session / --base / --target から、使う音声とアンカーを決める。
Result<BatchInput> resolve_input(const LaunchOptions& o, std::FILE* log) {
    BatchInput in;
    in.base_path   = o.base;
    in.target_path = o.target;
    if (o.session.empty()) return Result<BatchInput>::success(std::move(in));    // アンカーなし

    std::ifstream is(std::filesystem::u8path(o.session), std::ios::binary);
    if (!is) return Result<BatchInput>::failure("セッションを開けません: " + o.session);
    std::ostringstream ss;
    ss << is.rdbuf();
    Result<SessionFile> parsed = parse_session_json(ss.str());
    if (!parsed.ok()) return Result<BatchInput>::failure("セッションを読めません: " + parsed.error);
    SessionFile& s = parsed.value;
    for (const std::string& n : s.notes) say(log, n);

    if (s.anchors_only) {
        // tcmorph のアンカー JSON は音声を持たないので、--base / --target の音声に乗せる。
        if (o.base.empty())
            return Result<BatchInput>::failure("tcmorph のアンカー JSON には --base と --target が要ります: " + o.session);
    } else {
        // GUI の起動時の読み込みと同じく、どちらの音声を使うか決められないので誤りにする。
        if (!o.base.empty())
            return Result<BatchInput>::failure("音声のパスを含むセッションと --base / --target は同時に指定できません");
        in.base_path   = std::move(s.base_path);
        in.target_path = std::move(s.target_path);
        in.transcript  = std::move(s.transcript);
    }
    in.anchors = std::move(s.anchors);
    return Result<BatchInput>::success(std::move(in));
}

// 既定の接頭辞（GUI の連続書き出しと同じく、base と target のファイル名をつなぐ）。
std::string default_prefix(const BatchInput& in) {
    return std::filesystem::u8path(in.base_path).stem().u8string() + "-"
         + std::filesystem::u8path(in.target_path).stem().u8string();
}

}    // namespace

int run_batch_command(const LaunchOptions& opts, std::FILE* out, std::FILE* log) {
    const BatchCliOptions& b = opts.batch;

    const Result<BatchInput> in = resolve_input(opts, log);
    if (!in.ok()) {
        say(log, "エラー: " + in.error);
        return 1;
    }

    // 書き出しの設定（GUI のダイアログの項目と同じ）。
    BatchExportSettings s;
    s.count          = b.count;
    s.dir            = b.out;
    s.prefix         = b.prefix.empty() ? default_prefix(in.value) : b.prefix;
    s.write_manifest = b.manifest;
    for (int i = 0; i < kMorphAxisCount; ++i) {
        s.sweep[i]                  = b.sweep[i];
        s.fixed.*kMorphAxes[i].rate = b.fixed[i];
    }

    // 出力先は無ければ作る（設定の検査が出力先の存在を確かめるので、その前に）。
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(s.dir), ec);
    if (const Status v = validate_batch_settings(s); !v.ok()) {
        say(log, "エラー: " + v.error);
        return 1;
    }
    if (const int n = count_existing_batch_files(s); n > 0 && !b.overwrite) {
        say(log, "エラー: 出力先に同じ名前のファイルが " + std::to_string(n)
                   + " 個あります（上書きするなら --overwrite を付けてください）: " + s.dir);
        return 1;
    }

    // 解析（GUI で音声を開いたときと同じ WORLD の解析）。
    BatchExportRequest req;
    for (const auto& [name, path, dst] : { std::tuple { "base", in.value.base_path, &req.base },
                                           std::tuple { "target", in.value.target_path, &req.target } }) {
        say(log, std::string { name } + " を解析中: " + path);
        Result<MorphChannel> c = analyze_channel(path);
        if (!c.ok()) {
            say(log, std::string { "エラー: " } + name + " を解析できません: " + c.error);
            return 1;
        }
        *dst = std::make_shared<const MorphChannel>(std::move(c.value));
    }
    req.anchors     = in.value.anchors;
    req.base_path   = in.value.base_path;
    req.target_path = in.value.target_path;
    req.transcript  = in.value.transcript;
    req.settings    = s;
    if (req.anchors.empty()) say(log, "注意: アンカーがありません（始点と終点だけで対応づけます）");

    // 書き出しは別スレッドで走らせ、1本書けるごとに経過を出す。
    const std::vector<double> rates = batch_rates(s.count);
    BatchExportProgress       progress;
    BatchExportResult         result;
    std::atomic<bool>         finished { false };
    std::thread               worker([&]() {
        try {
            result = run_batch_export(req, progress);
        } catch (const std::exception& e) {
            result.error = std::string { "書き出し中の例外: " } + e.what();
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
        show_done();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    worker.join();
    show_done();

    for (const std::string& w : result.warnings) say(log, "警告: " + w);
    if (!result.ok()) {
        say(log, "エラー: " + result.error + "（" + std::to_string(result.written) + " 本まで書き出しました）");
        return 1;
    }

    // 書き出したファイルの一覧（スクリプトから使いやすいよう、標準出力に1行1ファイル）。
    for (int k = 0; k < result.written; ++k)
        say(out, (std::filesystem::u8path(s.dir) / std::filesystem::u8path(batch_wav_name(s, rates[static_cast<std::size_t>(k)])))
                   .u8string());
    if (!result.manifest.empty()) say(out, result.manifest);
    say(log, "完了: " + std::to_string(result.written) + " 本を書き出しました: " + s.dir);
    return 0;
}
