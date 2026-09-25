#include "morph_controller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app.hpp"
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
