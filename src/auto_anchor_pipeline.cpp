#include "auto_anchor_pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <future>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "formant_smoothing.hpp"

AutoAnchorTools AutoAnchorTools::real() {
    AutoAnchorTools t;
    t.check     = [](const AlignParams& p) { return run_check(p); };
    t.formants  = [](const std::string& w, const FormantParams& p) { return run_formants(w, p); };
    t.align     = [](const std::string& w, const std::string& s, const AlignParams& p) { return run_alignment(w, s, p); };
    t.terminate = []() { terminate_speech_tools(); };
    return t;
}

namespace {

// 例外もエラーとして返す（偽物のツールや、子プロセスの起動まわりの想定外の例外に備える）。
template <class T, class F>
Result<T> guarded(F&& f) {
    try {
        return f();
    } catch (const std::exception& e) {
        return Result<T>::failure(std::string { "例外: " } + e.what());
    }
}

}    // namespace

AutoAnchorPipelineResult build_auto_anchors(const AutoAnchorSource& base, const AutoAnchorSource& target,
                                            const AutoAnchorSettings& settings, const AutoAnchorTools& tools,
                                            const std::function<void(const std::string&)>& log,
                                            const std::atomic<bool>* cancel) {
    AutoAnchorPipelineResult R;
    const auto canceled = [&]() { return cancel && cancel->load(); };

    if (settings.transcript.empty()) {
        R.error = "書き起こしがありません";
        return R;
    }

    // ① 環境の確認（Python・MFA・モデル）。
    log("音声解析の環境を確認中...");
    const SpeechEnvStatus env = tools.check(settings.align);
    for (const std::string& w : env.warnings) R.warnings.push_back(w);
    if (!env.ready) {
        R.error = "音声解析の環境が使えません";
        for (const std::string& p : env.problems) R.error += "\n  - " + p;
        return R;
    }
    if (!env.summary.empty()) log("  " + env.summary);

    // ② フォルマント推定と ④ 音素アライメントを、base / target で並行に走らせる（GUI と同じ）。
    log("フォルマント推定と音素アライメント（MFA、数十秒かかります）を実行中...");
    const auto t0     = std::chrono::steady_clock::now();
    auto       run_fm = [&](const std::string& path) {
        return std::async(std::launch::async,
                          [&tools, path, p = settings.formant]() { return guarded<Formants>([&]() { return tools.formants(path, p); }); });
    };
    auto run_al = [&](const std::string& path) {
        return std::async(std::launch::async, [&tools, path, text = settings.transcript, p = settings.align]() {
            return guarded<Segmentation>([&]() { return tools.align(path, text, p); });
        });
    };
    std::future<Result<Formants>>     fb = run_fm(base.path), ft = run_fm(target.path);
    std::future<Result<Segmentation>> sb = run_al(base.path), st = run_al(target.path);

    // 待つ間も中止を見る。中止なら子プロセスを止め、すぐに返ってくる結果を捨てる。
    const auto all_ready = [&]() {
        constexpr auto kZero = std::chrono::seconds(0);
        return fb.wait_for(kZero) == std::future_status::ready && ft.wait_for(kZero) == std::future_status::ready
            && sb.wait_for(kZero) == std::future_status::ready && st.wait_for(kZero) == std::future_status::ready;
    };
    while (!all_ready()) {
        if (canceled()) {
            if (tools.terminate) tools.terminate();
            fb.wait(), ft.wait(), sb.wait(), st.wait();
            R.canceled = true;
            R.error    = "中止しました";
            return R;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Result<Formants>     rfb = fb.get(), rft = ft.get();
    Result<Segmentation> rsb = sb.get(), rst = st.get();
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "  完了（%.1f s）",
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        log(buf);
    }
    for (const auto& [name, r] : { std::pair { "base", &rfb }, std::pair { "target", &rft } })
        if (!r->ok()) {
            R.error = std::string { name } + " のフォルマント推定に失敗しました: " + r->error;
            return R;
        }
    for (const auto& [name, r] : { std::pair { "base", &rsb }, std::pair { "target", &rst } })
        if (!r->ok()) {
            R.error = std::string { name } + " の音素アライメントに失敗しました: " + r->error;
            return R;
        }
    R.base_phones   = join_labels(spoken_phones(rsb.value));
    R.target_phones = join_labels(spoken_phones(rst.value));
    log("  base の音素:   " + R.base_phones);
    log("  target の音素: " + R.target_phones);

    // ③ 移動平均と ⑤ アンカー自動生成。
    const double   window = settings.formant_window_ms / 1000.0;
    const Formants mb = smooth_formants(rfb.value, window), mt = smooth_formants(rft.value, window);
    AutoAnchorResult a = generate_auto_anchors(AutoAnchorInput { rsb.value, mb, base.duration },
                                               AutoAnchorInput { rst.value, mt, target.duration }, settings.divisions);
    for (std::string& w : a.warnings) R.warnings.push_back(std::move(w));
    if (!a.ok()) {
        R.error = "アンカー自動生成に失敗しました: " + a.error;
        return R;
    }
    R.anchors = std::move(a.anchors);
    R.n_freq  = a.n_freq;
    log("アンカー自動生成: 時間アンカー " + std::to_string(R.anchors.size()) + " 個、周波数アンカー "
        + std::to_string(R.n_freq) + " 個");
    return R;
}

std::string auto_anchor_record_json(const AutoAnchorSettings& s, const AutoAnchorPipelineResult& r) {
    const nlohmann::json j = {
        { "divisions", s.divisions },
        { "formant_window_ms", s.formant_window_ms },
        { "max_formant_hz", s.formant.max_formant_hz },
        { "acoustic_model", s.align.acoustic_model },
        { "dictionary", s.align.dictionary },
        { "base_phones", r.base_phones },
        { "target_phones", r.target_phones },
        { "time_anchors", r.anchors.size() },
        { "freq_anchors", r.n_freq },
    };
    return j.dump();
}
