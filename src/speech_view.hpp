#pragma once

/// @file speech_view.hpp
/// @brief Python ツールの結果（フォルマント・音素セグメンテーション）の描画。

#include <imgui.h>    // ImVec4

struct Track;
struct Formants;

// フォルマント k 本目（0 = F1）の表示色。左パネルの凡例と共有する。
ImVec4 formant_color(int k);

// アクティブな ImPlot プロット（Y軸 = ERB レート）にフォルマントを点で重ねる。
void draw_formants(const Formants& f);

// 音素セグメンテーションのプロットの高さ [px]（単語・音素の2段）。
float segmentation_plot_height();

// 音素セグメンテーションを独立したプロットに描く。時間軸はスペクトログラムと
// tr.view_x0/x1 でリンクする（同じ ImPlot::BeginAlignedPlots の中で呼ぶと、
// プロット領域の左右端もスペクトログラムと揃う）。
void draw_segmentation(Track& tr, float width, float height);
