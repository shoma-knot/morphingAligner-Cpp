#pragma once

/// @file formant_smoothing.hpp
/// @brief フォルマントの移動平均（表示とアンカー自動生成に使う。GUI に依存しない）。

#include "speech_tools.hpp"    // Formants

// フォルマントの移動平均（時刻を中心とする幅 window_s [s] の窓で Hz を平均）。
// 推定できなかったフレームで区切れた区間どうしはまたがず、境目には NaN の点を1つ挟む
// （PlotLine で線が途切れる）。描画のたびではなく、結果や窓幅が変わったときに作る。
Formants smooth_formants(const Formants& f, double window_s);
