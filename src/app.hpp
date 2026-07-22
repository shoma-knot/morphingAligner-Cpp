#pragma once

/// @file app.hpp
/// @brief Application state model (tracks, anchors) and loading logic.

#include <string>
#include <utility>
#include <vector>

#include <implot.h>
#include <miniaudio_cpp/audio.hpp>

#include "analysis.hpp"

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
    float               morph_rate   = 0.5f;    // モーフィング率（0=base, 1=target）
    bool                show_minimap = false;    // スペクトログラムのミニマップ表示

    // 直近のモーフィング結果（メモリ再生＋WAV保存用に保持）。
    std::vector<double> morph_wave;
    int                 morph_fs = 0;
};

// ファイルダイアログで `tr` を選び、解析してテクスチャを (再)生成する。
void load_track(Track& tr);

// 指定パスを解析してテクスチャを (再)生成する。成功で true、失敗時は tr を初期化する。
// 結果は applog に出力。セッション読み込みからも使う。
bool load_track_from_path(Track& tr, const std::string& path);
