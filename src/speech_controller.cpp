#include "speech_controller.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app.hpp"
#include "auto_anchors.hpp"
#include "formant_smoothing.hpp"
#include "log.hpp"
#include "speech_tools.hpp"

namespace {

// 音声解析の環境をセットアップするスクリプト（配布物のルートにある）。
#ifdef _WIN32
constexpr const char* kInstallScript = "install-win.bat";
#else
constexpr const char* kInstallScript = "install.sh";
#endif

// 移動平均の窓幅（SpeechState::formant_ma_ms）を秒で。
double ma_window_s(const App& app) {
    return app.speech.formant_ma_ms / 1000.0;
}

// フォルマント推定を開始する。完了時、音声が差し替わっていたら結果は捨てる。
void launch_formant_job(App& app, Side side) {
    Track& tr       = app.track(side);
    tr.formant_busy = true;
    tr.formant_path = tr.path;    // 失敗しても同じ音声で毎フレーム再試行しないよう先に記録
    const std::string   name = tr.name, path = tr.path;
    const FormantParams params = app.speech.formant_params;
    applog::add(name + " フォルマント推定中...");
    app.jobs.tools.launch(
      [side, name, path, params]() -> JobApply {
          const auto       t0 = std::chrono::steady_clock::now();
          Result<Formants> r  = run_formants(path, params);
          if (!r.ok()) {
              applog::add(name + " フォルマント推定失敗: " + r.error);
          } else {
              char buf[128];
              std::snprintf(buf, sizeof buf, " フォルマント推定完了 (%.2f s)", applog::seconds_since(t0));
              applog::add(name + buf);
          }
          auto f = std::make_shared<Formants>(std::move(r.value));
          return [side, path, f, ok = r.ok()](App& a) {
              Track& t       = a.track(side);
              t.formant_busy = false;
              if (!ok || t.path != path) return;    // 失敗、または実行中に音声が差し替わった
              t.formants    = std::move(*f);
              t.formants_ma = smooth_formants(t.formants, ma_window_s(a));
          };
      },
      /*on_error=*/[side](App& a) { a.track(side).formant_busy = false; });
}

// 音素セグメンテーション（MFA）を開始する。完了時、音声が差し替わっていたら結果は捨てる。
void launch_align_job(App& app, Side side, const std::string& text) {
    Track& tr     = app.track(side);
    tr.align_busy = true;
    const std::string name = tr.name, path = tr.path;
    const AlignParams params = app.speech.align_params;
    applog::add(name + " 音素セグメンテーション中（MFA、数十秒かかります）...");
    app.jobs.tools.launch(
      [side, name, path, text, params]() -> JobApply {
          const auto           t0 = std::chrono::steady_clock::now();
          Result<Segmentation> r  = run_alignment(path, text, params);
          auto                 seg = std::make_shared<Segmentation>(std::move(r.value));
          if (!r.ok()) {
              applog::add(name + " 音素セグメンテーション失敗: " + r.error);
          } else {
              // 音素列もログに出す（辞書にない語は spn になるので、ここで気づけるように）。
              char buf[128];
              std::snprintf(buf, sizeof buf, " 音素セグメンテーション完了 (%.2f s): ", applog::seconds_since(t0));
              applog::add(name + buf + join_labels(spoken_phones(*seg)));
          }
          return [side, path, seg, ok = r.ok()](App& a) {
              Track& t     = a.track(side);
              t.align_busy = false;
              if (!ok || t.path != path) return;    // 失敗、または実行中に音声が差し替わった
              t.segmentation = std::move(*seg);
          };
      },
      /*on_error=*/[side](App& a) { a.track(side).align_busy = false; });
}

}    // namespace

const char* install_script_name() {
    return kInstallScript;
}

// 結果が出るまではフォルマント推定・音素アライメントを走らせない（環境が無いときに、音声を
// 読み込むたびに失敗がログに積もるのを防ぐ）。
void ensure_speech_env(App& app) {
    if (app.speech.env != SpeechEnv::Unknown) return;
    app.speech.env = SpeechEnv::Checking;
    const AlignParams params = app.speech.align_params;
    app.jobs.tools.launch(
      [params]() -> JobApply {
          auto st = std::make_shared<SpeechEnvStatus>(run_check(params));
          if (st->ready) {
              applog::add("音声解析の環境: 使えます（" + st->summary + "）");
          } else {
              applog::add(std::string { "音声解析の環境: 使えません。" } + kInstallScript
                          + " を実行してください（フォルマント表示と音素アライメントに必要）");
              for (const std::string& p : st->problems) applog::add("  理由: " + p);
          }
          for (const std::string& w : st->warnings) applog::add("警告: " + w);
          return [st](App& a) {
              a.speech.env          = st->ready ? SpeechEnv::Ready : SpeechEnv::Unavailable;
              a.speech.env_problems = st->problems;
          };
      },
      /*on_error=*/[](App& a) {
          a.speech.env          = SpeechEnv::Unavailable;
          a.speech.env_problems = { "環境の確認中に内部エラーが起きました（ログを参照）" };
      });
}

void ensure_formants(App& app) {
    if (app.speech.env != SpeechEnv::Ready) return;
    for (Side s : kSides) {
        const Track& tr = app.track(s);
        if (tr.loaded() && !tr.formant_busy && tr.formant_path != tr.path) launch_formant_job(app, s);
    }
}

void invalidate_formants(App& app) {
    for (Side s : kSides) app.track(s).formant_path.clear();
}

void update_formant_ma(App& app) {
    // 計算は軽い（点数に比例）ので、値が変わるたびに作り直してよい。
    for (Side s : kSides) {
        Track& tr      = app.track(s);
        tr.formants_ma = smooth_formants(tr.formants, ma_window_s(app));
    }
}

void launch_alignment(App& app) {
    for (Side s : kSides)
        if (app.track(s).loaded()) launch_align_job(app, s, app.speech.transcript);
}

void ensure_pending_alignment(App& app) {
    SpeechState& sp = app.speech;
    if (!sp.align_on_ready || app.jobs.ui.busy()) return;
    if (sp.env == SpeechEnv::Unknown || sp.env == SpeechEnv::Checking) return;
    sp.align_on_ready = false;

    if (sp.env != SpeechEnv::Ready) {
        applog::add("--align: 音声解析の環境が使えないため、音素アライメントを実行しません");
    } else if (!app.base.loaded() && !app.target.loaded()) {
        applog::add("--align: 読み込まれた音声が無いため、音素アライメントを実行しません");
    } else if (sp.transcript.empty()) {
        applog::add("--align: 書き起こしが無いため、音素アライメントを実行しません"
                    "（--transcript か、書き起こしを含むセッションを指定してください）");
    } else {
        applog::add("--align: 音素アライメントを実行します");
        launch_alignment(app);
    }
}

bool auto_anchors_ready(const App& app) {
    const auto ready = [](const Track& t) {
        return t.loaded() && !t.segmentation.empty() && !t.formants.empty() && !t.align_busy && !t.formant_busy;
    };
    return ready(app.base) && ready(app.target);
}

// 生成の中身は auto_anchors.cpp。
void run_auto_anchors(App& app) {
    const auto input = [](const Track& t) {
        return AutoAnchorInput { t.segmentation, t.formants_ma, t.spec.duration };
    };
    AutoAnchorResult r =
      generate_auto_anchors(input(app.base), input(app.target), app.speech.auto_anchor_divisions);
    for (const std::string& w : r.warnings) applog::add("警告: " + w);
    if (!r.ok()) {
        applog::add("アンカー自動生成失敗: " + r.error);
        return;
    }
    app.anchors = std::move(r.anchors);
    char buf[160];
    std::snprintf(buf, sizeof buf, "アンカー自動生成: 時間アンカー %d 個、周波数アンカー %d 個（分割数 %d、窓幅 %d ms）",
                  static_cast<int>(app.anchors.size()), r.n_freq, app.speech.auto_anchor_divisions,
                  app.speech.formant_ma_ms);
    applog::add(buf);
}
