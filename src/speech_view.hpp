#pragma once

/// @file speech_view.hpp
/// @brief Python ツールの結果（フォルマント・音素セグメンテーション）の描画。

#include <imgui.h>    // ImVec4

#include "speech_tools.hpp"    // Formants

struct Track;

// フォルマント k 本目（0 = F1）の表示色。左パネルの凡例と共有する。
ImVec4 formant_color(int k);

// アクティブな ImPlot プロット（Y軸 = ERB レート）にフォルマントを点で重ねる。
// ma を渡すと移動平均を同じ色の線で重ね、元の点は薄くする。
void draw_formants(const Formants& f, const Formants* ma);

// 音素セグメンテーションのプロットの高さ [px]（音素の1段）。
float segmentation_plot_height();

// 音素セグメンテーション（音素ティアのみ）を独立したプロットに描く。時間軸は
// スペクトログラムと tr.view_x0/x1 でリンクする（同じ ImPlot::BeginAlignedPlots の中で
// 呼ぶと、プロット領域の左右端もスペクトログラムと揃う）。
void draw_segmentation(Track& tr, float width, float height);
