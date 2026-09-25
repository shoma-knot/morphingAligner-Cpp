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

// 反映時に共通 dB レンジとテクスチャを作り直して morphed を無効化する。
void ensure_morph_channels(App& app) {
    if (app.jobs.ui.busy()) return;    // 進行中の UI ジョブと直列化

    // 古くなっている方（base 優先）を1つだけ処理する。両方古い場合は次のフレームで続き。
    MorphState& m     = app.morph;
    const auto  stale = [&](Side s) {
        const Track& tr = app.track(s);
        return tr.loaded() && m.ch_path[side_index(s)] != tr.path;
    };
    Side side;
    if (stale(Side::Base)) side = Side::Base;
    else if (stale(Side::Target)) side = Side::Target;
    else return;

    const std::string name = app.track(side).name;
    const std::string path = app.track(side).path;
    // 先に試行済みパスを記録して、失敗時に毎フレーム再解析されるのを防ぐ。
    m.ch_path[side_index(side)] = path;

    app.jobs.ui.try_launch([name, path, side]() -> JobApply {
        applog::add(name + " をモーフィング用に解析中...");
        const auto  t0 = std::chrono::steady_clock::now();
        std::string err;
        MorphChannel c = analyze_channel(path, err);

        std::shared_ptr<const MorphChannel> ch;    // 失敗時は nullptr のまま反映
        if (!err.empty()) {
            applog::add(name + " 解析失敗: " + err);
        } else {
            ch = std::make_shared<const MorphChannel>(std::move(c));
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s 解析完了 (%.2f ms)", name.c_str(), applog::seconds_since(t0) * 1000.0);
            applog::add(buf);
        }
        return [side, ch](App& a) {
            a.morph.ch[side_index(side)] = ch;
            ++a.morph.epoch;     // 実行中モーフの結果は古い base/target のものなので破棄対象に
            a.morph.out = {};    // 元が変わったので以前の morphed は無効
            rebuild_morph_bt_textures(a.morph);
        };
    });
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
