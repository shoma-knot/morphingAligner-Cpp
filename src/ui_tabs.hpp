#pragma once

/// @file ui_tabs.hpp
/// @brief 各タブの描画（ui.cpp の draw_root から呼ぶ。ui.cpp 以外からは使わない）。

struct App;

// 「アライメント」タブ（ui_align.cpp）: 左=操作パネル（1）、右=スペクトログラム/アンカー編集（4）。
void draw_align_tab(App& app);

// 「モーフィング」タブ（ui_morph.cpp）: 上=率のスライダー、中=3×3 プロット、下=出力。
void draw_morph_tab(App& app);

// 「ライセンス表示」タブ（ui_license.cpp）: 左=同梱物のリスト、右=選択した条文。
void draw_license_tab(App& app);
