#pragma once

/// @file app_icon.hpp
/// @brief ウィンドウ（タスクバー）アイコンの設定。

struct GLFWwindow;

// 埋め込みアイコンをウィンドウに設定する。意匠と画素は tools/gen_icon.py が生成し、
// src/app_icon_data.inc に入っている。
void set_window_icon(GLFWwindow* window);
