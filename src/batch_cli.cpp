#include "batch_cli.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
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

#include <nlohmann/json.hpp>

#include "analysis.hpp"
#include "batch_export.hpp"
#include "session_io.hpp"

namespace {

// Ctrl+C（SIGINT）で立てる。シグナルハンドラから触れるのは lock-free の atomic だけにする。
std::atomic<bool> g_interrupted { false };

extern "C" void on_sigint(int) {
    g_interrupted.store(true);
}

// 実行中だけ SIGINT を受け取り、終わったら元に戻す。Linux では Python / MFA の子プロセスを
// 別のプロセスグループで起動しているので、端末の Ctrl+C が子に届かない。受け取って自分で
// 止めないと、本体だけが終わって MFA が残る（Windows は Job Object で道連れになる）。
class SigintScope {
public:
    SigintScope() {
        g_interrupted.store(false);
        prev_ = std::signal(SIGINT, on_sigint);
    }
    ~SigintScope() { std::signal(SIGINT, prev_ == SIG_ERR ? SIG_DFL : prev_); }
    SigintScope(const SigintScope&)            = delete;
    SigintScope& operator=(const SigintScope&) = delete;

private:
    void (*prev_)(int) = SIG_DFL;
};

constexpr int kExitFailed      = 1;
constexpr int kExitInterrupted = 130;    // シェルの慣習（128 + SIGINT）

void say(std::FILE* f, const std::string& s) {
    std::fputs((s + "\n").c_str(), f);
    std::fflush(f);
}

// 音声とアンカー（セッションから決めたもの）。
struct BatchInput {
    std::string         base_path, target_path, transcript;
    std::vector<Anchor> anchors;
};

// --session / --base / --target / --transcript から、使う音声・アンカー・書き起こしを決める。
Result<BatchInput> resolve_input(const LaunchOptions& o, std::FILE* log) {
    const bool auto_anchors = o.batch.auto_anchors;
    BatchInput in;
    in.base_path   = o.base;
    in.target_path = o.target;
    if (!o.session.empty()) {
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
            if (auto_anchors)
                return Result<BatchInput>::failure("--auto-anchors と tcmorph のアンカー JSON は同時に使えません"
                                                   "（アンカーの指定が2通りになるため）");
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
        if (auto_anchors && !s.anchors.empty())
            say(log, "注意: セッションのアンカー " + std::to_string(s.anchors.size()) + " 個は使わず、作り直します");
        else
            in.anchors = std::move(s.anchors);
    }
    if (o.has_transcript) in.transcript = o.transcript;    // --transcript はセッションの書き起こしより優先
    if (auto_anchors && in.transcript.empty())
        return Result<BatchInput>::failure("書き起こしがありません（--transcript か、書き起こしを含むセッションを指定してください）");
    return Result<BatchInput>::success(std::move(in));
}

// 既定の接頭辞（GUI の連続書き出しと同じく、base と target のファイル名をつなぐ）。
std::string default_prefix(const BatchInput& in) {
    return std::filesystem::u8path(in.base_path).stem().u8string() + "-"
         + std::filesystem::u8path(in.target_path).stem().u8string();
}

// 作ったアンカーをセッション形式で保存する（"auto_anchors" に生成の設定と音素列を足す）。
Status save_session_file(const std::string& path, const BatchInput& in, const std::vector<Anchor>& anchors,
                         const std::string& record) {
    try {
        SessionFile sf;
        sf.base_path     = in.base_path;
        sf.target_path   = in.target_path;
        sf.transcript    = in.transcript;
        sf.anchors       = anchors;
        nlohmann::json j = nlohmann::json::parse(session_to_json(sf));
        j["auto_anchors"] = nlohmann::json::parse(record);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
        std::ofstream os(std::filesystem::u8path(path), std::ios::binary);
        if (!os) return Status::failure("書き込めません: " + path);
        os << j.dump(2) << '\n';
        return os ? Status {} : Status::failure("書き込めません: " + path);
    } catch (const std::exception& e) {
        return Status::failure(e.what());
    }
}

}    // namespace

int run_batch_command(const LaunchOptions& opts, std::FILE* out, std::FILE* log, const AutoAnchorTools* tools) {
    const BatchCliOptions& b = opts.batch;
    SigintScope            sigint;

    const Result<BatchInput> in = resolve_input(opts, log);
    if (!in.ok()) {
        say(log, "エラー: " + in.error);
        return kExitFailed;
    }

    // 時間のかかる処理（MFA・書き出し）の前に、書き出せるかを確かめておく。
    BatchExportSettings s;
    if (b.exports()) {
        // 書き出しの設定（GUI のダイアログの項目と同じ）。
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
            return kExitFailed;
        }
        if (const int n = count_existing_batch_files(s); n > 0 && !b.overwrite) {
            say(log, "エラー: 出力先に同じ名前のファイルが " + std::to_string(n)
                       + " 個あります（上書きするなら --overwrite を付けてください）: " + s.dir);
            return kExitFailed;
        }
    }
    if (!b.save_session.empty() && !b.overwrite) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::u8path(b.save_session), ec)) {
            say(log, "エラー: 保存先のセッションがすでにあります（上書きするなら --overwrite を付けてください）: "
                       + b.save_session);
            return kExitFailed;
        }
    }

    // 解析（GUI で音声を開いたときと同じ WORLD の解析）。
    BatchExportRequest req;
    for (const auto& [name, path, dst] : { std::tuple { "base", in.value.base_path, &req.base },
                                           std::tuple { "target", in.value.target_path, &req.target } }) {
        say(log, std::string { name } + " を解析中: " + path);
        Result<MorphChannel> c = analyze_channel(path);
        if (!c.ok()) {
            say(log, std::string { "エラー: " } + name + " を解析できません: " + c.error);
            return kExitFailed;
        }
        *dst = std::make_shared<const MorphChannel>(std::move(c.value));
    }
    req.anchors     = in.value.anchors;
    req.base_path   = in.value.base_path;
    req.target_path = in.value.target_path;
    req.transcript  = in.value.transcript;
    req.settings    = s;

    // --auto-anchors: 音素アライメントとフォルマントからアンカーを作る。
    if (b.auto_anchors) {
        AutoAnchorSettings as = b.auto_settings;
        as.transcript         = in.value.transcript;
        const AutoAnchorPipelineResult r =
          build_auto_anchors(AutoAnchorSource { req.base_path, req.base->duration },
                             AutoAnchorSource { req.target_path, req.target->duration }, as,
                             tools ? *tools : AutoAnchorTools::real(), [&](const std::string& m) { say(log, m); },
                             &g_interrupted);
        for (const std::string& w : r.warnings) say(log, "警告: " + w);
        if (r.canceled) {
            say(log, "中止しました");
            return kExitInterrupted;
        }
        if (!r.ok()) {
            say(log, "エラー: " + r.error);
            return kExitFailed;
        }
        req.anchors              = r.anchors;
        const std::string record = auto_anchor_record_json(as, r);
        req.manifest_extra       = nlohmann::json { { "auto_anchors", nlohmann::json::parse(record) } }.dump();

        if (!b.save_session.empty()) {
            if (const Status st = save_session_file(b.save_session, in.value, r.anchors, record); !st.ok()) {
                say(log, "エラー: セッションを保存できません: " + st.error);
                return kExitFailed;
            }
            say(out, b.save_session);
            say(log, "セッションを保存しました: " + b.save_session);
        }
    }
    if (!b.exports()) return 0;    // アンカーを保存するだけ

    if (req.anchors.empty()) say(log, "注意: アンカーがありません（始点と終点だけで対応づけます）");

    // 書き出しは別スレッドで走らせ、1本書けるごとに経過を出す。Ctrl+C なら次の1本の前で止める。
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
    return 0;
}
