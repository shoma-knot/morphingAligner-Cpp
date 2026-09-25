#pragma once

/// @file anchors.hpp
/// @brief スペクトログラム上の時間/周波数アンカーの描画・操作。

#include <vector>

#include <imgui.h>    // ImVec2

#include "anchor.hpp"    // Side

struct App;
struct Spectrogram;

// 相手パネルへ結ぶ端点（スクリーン座標）。
struct EdgePoint {
    ImVec2 pos;
    bool   visible;    // 表示範囲外なら false（対応線を引かない）
};

// アクティブな ImPlot プロット内で、時間アンカー（縦線）と周波数アンカー（点）を
// 描画・操作する。out_edges に相手パネルへ結ぶ端点を返す。
void draw_anchors(App& app, Side side, const Spectrogram& sp, std::vector<EdgePoint>& out_edges);

// base/target の対応する時間アンカーを結ぶ線を前面に描く（本数が多いと帯になるので薄く、
// active（カーソルが近いアンカー）の1本だけ不透明にする）。
void draw_anchor_connectors(const std::vector<EdgePoint>& base_edges,
                            const std::vector<EdgePoint>& target_edges, int active);
