#pragma once

/// @file ui.hpp
/// @brief 2つのパネルの毎フレーム描画。

struct App;

// 左パネル: トラックごとの読み込み/再生操作とアンカー一覧。
void draw_left_panel(App& app);

// 右パネル: base/target のスペクトル包絡スペクトログラムを縦に並べ、ドラッグ可能な
// 時間アンカーとパネル間の対応線を重ねて描く。
void draw_right_panel(App& app);
