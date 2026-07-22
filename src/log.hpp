#pragma once

/// @file log.hpp
/// @brief 画面下部のログ領域に出す動作ログ（グローバル・単一スレッド用）。

#include <string>
#include <vector>

namespace applog {

// タイムスタンプ付きで1行追記する。
void add(std::string msg);

// これまでのログ行（古い順）。
const std::vector<std::string>& lines();

// ログを消去する。
void clear();

}    // namespace applog
