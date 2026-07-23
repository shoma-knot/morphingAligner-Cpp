#pragma once

/// @file log.hpp
/// @brief 画面下部のログ領域に出す動作ログ（スレッドセーフ。ワーカーからも add 可）。

#include <string>
#include <vector>

namespace applog {

// タイムスタンプ付きで1行追記する（スレッドセーフ）。
void add(std::string msg);

// これまでのログ行のスナップショット（古い順）。
std::vector<std::string> lines();

// ログを消去する。
void clear();

}    // namespace applog
