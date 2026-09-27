#include "morph_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "app.hpp"
#include "batch_export.hpp"
#include "log.hpp"
#include "morphing.hpp"

// 解析は読み込み時に済んでいる（Track::channel）ので、ここでは取り込んでテクスチャを作るだけ。
void ensure_morph_channels(App& app) {
    MorphState& m       = app.morph;
    bool        changed = false;
    for (Side s : kSides) {
        const auto& ch = app.track(s).channel;
        if (m.ch[side_index(s)] == ch) continue;
        m.ch[side_index(s)] = ch;
        changed             = true;
    }
    if (!changed) return;
    ++m.epoch;     // 実行中モーフの結果は古い base/target のものなので破棄対象に
    m.out = {};    // 元が変わったので以前の morphed は無効
    rebuild_morph_bt_textures(m);
}

// ワーカーは shared_ptr 経由の immutable なチャンネルと、コピーしたアンカー/率だけを使う。
void request_morph(App& app) {
    MorphState& m = app.morph;
    if (!m.channel(Side::Base) || !m.channel(Side::Target)) return;
    if (m.job_running) {
        // 実行中なら畳む。再生予約は play_request に残したままにして、
        // この再要求から始まるジョブ（最新の率）の方で再生されるようにする。
        m.job_pending = true;
        return;
    }
    m.job_running  = true;
    m.job_pending  = false;
    m.job_play     = m.play_request;    // このジョブが再生を担当する
    m.play_request = false;
    m.job_epoch    = m.epoch;
    m.job_t0       = std::chrono::steady_clock::now();

    const auto base    = m.ch[side_index(Side::Base)];
    const auto target  = m.ch[side_index(Side::Target)];
    const auto anchors = app.anchors;    // コピー（ジョブ中の編集と分離）
    const auto rates   = m.rates;
    m.job              = std::async(std::launch::async, [base, target, anchors, rates] {
        return morphing_channels(*base, *target, anchors, rates);
    });
}

void play_wave(App& app, const std::vector<double>& wave, int fs) {
    std::vector<float> pcm(wave.size());
    for (std::size_t i = 0; i < wave.size(); ++i) pcm[i] = static_cast<float>(std::clamp(wave[i], -1.0, 1.0));
    try {
        app.engine.play_pcm(pcm.data(), pcm.size(), 1, static_cast<unsigned>(fs));
    } catch (const std::exception& e) {
        applog::add(std::string { "再生失敗: " } + e.what());
    }
}

// GL への反映はここ＝メインスレッドで行う。
void poll_morph_job(App& app) {
    MorphState& m = app.morph;
    if (!m.job_running) return;
    if (m.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    m.job_running = false;

    // morphing_channels は内部で例外を捕まえて error に入れるが、それ以外（メモリ不足など）で
    // 投げられてもアプリを落とさず、失敗として扱う。
    MorphOutput out;
    try {
        out = m.job.get();
    } catch (const std::exception& e) {
        out.error = std::string { "モーフィング失敗（内部エラー）: " } + e.what();
    }

    if (m.job_epoch != m.epoch) {
        // base/target が差し替わった後に完了した古い結果は捨てる。
        m.play_request = false;
    } else {
        const double ms = applog::seconds_since(m.job_t0) * 1000.0;
        m.out           = std::move(out);
        m.last_ms       = ms;    // 連続書き出しの所要時間の見積もりに使う
        // 警告はアンカーが同じなら毎回同じ内容になる。リアルタイム更新でスライダーを
        // 動かすたびに積むとログが埋まるので、内容が変わったときだけ出す。
        if (m.out.warnings != m.last_warnings) {
            m.last_warnings = m.out.warnings;
            for (const std::string& w : m.last_warnings) applog::add("警告: " + w);
        }
        if (!m.out.ok()) {
            applog::add(m.out.error);
        } else {
            char buf[128];
            std::snprintf(buf, sizeof buf, "モーフィング生成: %zu samples (%.2f ms)", m.out.wave.size(), ms);
            applog::add(buf);
        }
        rebuild_morphed_texture(m);
        if (m.job_play && m.out.ok()) play_wave(app, m.out.wave, m.out.fs);
    }
    m.job_play = false;

    if (m.job_pending) request_morph(app);
}

// ── 連続書き出し ────────────────────────────────────────────
std::string batch_default_prefix(const App& app) {
    std::string s;
    for (Side side : kSides) {
        const std::string& p = app.track(side).path;
        if (p.empty()) continue;
        if (!s.empty()) s += '-';
        s += std::filesystem::u8path(p).stem().u8string();
    }
    return s.empty() ? std::string { "morph" } : s;
}

std::string batch_default_dir(const App& app) {
    // 素材の置き場所を既定にする（書き出したものを素材の隣に置くことが多いため）。
    if (!app.base.path.empty()) return std::filesystem::u8path(app.base.path).parent_path().u8string();
    std::error_code ec;
    const auto      d = std::filesystem::current_path(ec);
    return ec ? std::string {} : d.u8string();
}

// ワーカーは App に触れない。解析チャンネルは immutable なので shared_ptr のまま共有し、
// アンカーと設定はコピーを渡す（書き出し中に UI で編集されても結果が揺れないように）。
void start_batch_export(App& app) {
    MorphState& m = app.morph;
    if (m.batch_running()) return;
    if (!m.channel(Side::Base) || !m.channel(Side::Target)) return;

    BatchExportRequest req;
    req.base        = m.ch[side_index(Side::Base)];
    req.target      = m.ch[side_index(Side::Target)];
    req.anchors     = app.anchors;
    req.base_path   = app.base.path;
    req.target_path = app.target.path;
    req.transcript  = app.speech.transcript;
    req.settings    = m.batch;

    auto       progress = std::make_shared<BatchExportProgress>();
    const bool started  = app.jobs.ui.try_launch(
      [req, progress]() -> JobApply {
          // どの軸を掃引し、ほかをどこに固定したかもログに残す。
          std::string axes;
          if (batch_sweeps_all_axes(req.settings)) {
              axes = "全軸";
          } else {
              std::string swept, fixed;
              for (int i = 0; i < kMorphAxisCount; ++i) {
                  if (req.settings.sweep[i]) {
                      swept += (swept.empty() ? "" : "+") + std::string { kMorphAxes[i].key };
                  } else {
                      char buf[32];
                      std::snprintf(buf, sizeof buf, "%s%s=%.2f", fixed.empty() ? "" : ", ", kMorphAxes[i].key,
                                    req.settings.fixed.*kMorphAxes[i].rate);
                      fixed += buf;
                  }
              }
              axes = swept + "、固定: " + fixed;
          }
          applog::add("連続書き出し中: " + std::to_string(req.settings.count) + " 本（" + axes + "）→ "
                      + req.settings.dir);
          const auto        t0  = std::chrono::steady_clock::now();
          BatchExportResult r   = run_batch_export(req, *progress);
          const double      sec = applog::seconds_since(t0);

          for (const std::string& w : r.warnings) applog::add("警告: " + w);
          if (!r.ok()) {
              applog::add("連続書き出し失敗: " + r.error);
          } else {
              char buf[160];
              std::snprintf(buf, sizeof buf, "連続書き出し%s: %d 本 (%.1f 秒)",
                            r.canceled ? "を中止しました" : "完了", r.written, sec);
              applog::add(buf);
              if (!r.manifest.empty()) applog::add("設定を保存しました: " + r.manifest);
          }
          return [](App& a) { a.morph.batch_progress.reset(); };
      },
      /*on_error=*/[](App& a) { a.morph.batch_progress.reset(); });
    if (!started) return;    // ほかのジョブが実行中（ボタン側でも無効にしてある）

    m.batch_progress = progress;
    m.batch_total    = m.batch.count;
}
