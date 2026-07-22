#pragma once

/// @file ui.hpp
/// @brief 画面全体（フルビューポート）の毎フレーム描画。

struct App;

// ルートウィンドウを縦 8:2 に分割し、上に操作パネル（左1:右4）、下に動作ログを描く。
void draw_root(App& app);
