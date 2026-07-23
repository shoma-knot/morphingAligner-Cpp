#pragma once

/// @file app.hpp
/// @brief Application state model (tracks, anchors) and loading logic.

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <implot.h>
#include <miniaudio_cpp/audio.hpp>

#include "analysis.hpp"
#include "morphing.hpp"    // MorphRates / MorphOutput

// Viridis, shared by the pre-baked spectrogram texture and the on-screen legend.
constexpr ImPlotColormap kColormap = ImPlotColormap_Viridis;

// One loaded audio source (base or target) with its analysis and GPU texture.
// Owns a GL texture, so it is non-copyable.
struct Track {
    std::string  name;       // "base" / "target", shown in dialog + labels
    std::string  path;       // loaded file ("" = none)
    Spectrogram  spec;       // spectral envelope
    unsigned int tex = 0;    // baked GPU texture
    double       y_min = 0;  // frequency-axis view range [ERB レート]; zoomable over
    double       y_max = 0;  // the axis, reset to [0, ERB(fs/2)] on load

    // メインプロットの現在の表示範囲（ミニマップの枠に使う）。X=秒, Y=ERB レート。
    double view_x0 = 0, view_x1 = 0, view_y0 = 0, view_y1 = 0;

    explicit Track(std::string n) : name(std::move(n)) {}
    ~Track();    // frees the GL texture (defined in app.cpp)
    Track(const Track&)            = delete;
    Track& operator=(const Track&) = delete;

    bool loaded() const { return !path.empty(); }
};

// 時間アンカー上に打つ周波数の対応。作成時は base/target 同じ周波数で、
// あとから各パネルの線上でドラッグして周波数対応を編集する。
// 表示番号は所属する時間アンカー内の並び順（インデックス+1）で、base/target 両方の
// 点に同じ番号を出して対応を示す。
struct FreqAnchor {
    double base_f;
    double target_f;
};

// 時間軸の対応。base/target のスペクトログラムに縦線を1本ずつ立てる。
// 作成時は両者同じ時刻で、あとから各線をドラッグして対応を定義する。
// freqs はこの時間アンカー線上に乗る周波数アンカー（Ctrl+左クリックで追加）。
struct Anchor {
    double                  base_t;
    double                  target_t;
    std::vector<FreqAnchor> freqs;
};

// Application state shared across the frame.
struct App {
    ma::engine          engine;    // audio output device (shared by both tracks)
    Track               base { "base" };
    Track               target { "target" };
    std::vector<Anchor> anchors;    // base<->target time correspondences
    bool                show_minimap = false;    // スペクトログラムのミニマップ表示
    bool                log_open     = true;     // 下部ログ領域の展開状態

    // モーフィング（モーフィングタブ）。
    MorphRates morph_rates;               // 軸ごとの率（0=base, 1=target）
    bool       morph_link     = true;     // 全軸を一括操作するか
    bool       morph_realtime = true;     // スライダー操作中も逐次再合成するか（OFF=離した時のみ）

    // base/target の解析チャンネル（タブ表示時に解析、パス変更で再解析）。
    // 非同期ジョブと安全に共有するため immutable な shared_ptr で保持する（失敗時は nullptr）。
    std::shared_ptr<const MorphChannel> morph_base, morph_target;
    std::string                         morph_base_path, morph_target_path;    // 解析済みチャンネルの元パス
    MorphOutput                         morph_out;    // 再合成の結果（morphed の f0/sp/ap＋wave）

    // 非同期モーフィングジョブ（ワーカーで morphing_channels を実行。GL への反映は
    // 完了回収時にメインスレッドで行う）。実行中の再要求は pending に畳んで最新条件で1回だけ再実行。
    std::future<MorphOutput>              morph_job;
    bool                                  morph_job_running    = false;
    bool                                  morph_job_pending    = false;    // 実行中に来た再要求
    bool                                  morph_play_when_done = false;    // 完了後に再生する（生成して再生）
    int                                   morph_epoch = 0, morph_job_epoch = 0;    // base/target の世代
    std::chrono::steady_clock::time_point morph_job_t0;    // 計測用

    // 汎用 UI ジョブ: ファイルダイアログや解析などブロックする処理をワーカーで実行し、
    // 完了時に「メインスレッドで適用する処理」を受け取って実行する（同時に1本のみ）。
    std::future<std::function<void(App&)>> ui_job;
    bool                                   ui_job_running = false;

    // モーフィングタブの sp/ap ヒートマップ用テクスチャ（0=base, 1=morphed, 2=target）。
    unsigned int morph_tex_sp[3] = { 0, 0, 0 };
    unsigned int morph_tex_ap[3] = { 0, 0, 0 };
    double       morph_db_min = 0, morph_db_max = 0;    // sp 共通の dB レンジ

    ~App();    // モーフィング用テクスチャを解放（app.cpp で定義）
};

// base/target の sp/ap テクスチャと共通 dB レンジを作り直す（morphed テクスチャも
// 新レンジで作り直す）。morphed は base/target の log 補間なので必ずレンジ内に収まる。
void rebuild_morph_bt_textures(App& app);

// morphed の sp/ap テクスチャだけを作り直す（レンジは計算済みのものを使用）。
void rebuild_morphed_texture(App& app);

// 解析済みの Spectrogram を Track に反映する（テクスチャ生成・表示状態リセット・ログ）。
// GL を使うため必ずメインスレッドで呼ぶこと（解析はワーカーで analyze_file を使う）。
void apply_track(Track& tr, const std::string& path, Spectrogram&& spec);
