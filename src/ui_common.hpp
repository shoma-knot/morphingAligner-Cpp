#pragma once

/// @file ui_common.hpp
/// @brief 画面の各部（タブ）で共有する小さな描画の部品。

// Y軸（ERB レート）に 1-2-5 系列（…100,200,500,1000,…）の目盛りと Hz 表示を設定する。
// ImPlot::BeginPlot と SetupAxes のあと、プロットの Setup 中に呼ぶ。nyq はナイキスト周波数 [Hz]。
void setup_erb_yaxis_ticks(double nyq);

// 直前の項目と同じ行に "(?)" を出し、ホバーで説明を表示する。
void help_marker(const char* text);

// 直前の項目と同じ行の右端にボタンを置く（幅はラベルに合わせる）。押されたら true。
bool right_aligned_button(const char* label);
