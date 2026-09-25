#pragma once

/// @file log.hpp
/// @brief 画面下部のログ領域に出す動作ログ（スレッドセーフ。ワーカーからも add 可）。

#include <chrono>
#include <string>
#include <vector>

namespace applog {

// タイムスタンプ付きで1行追記する（スレッドセーフ）。
void add(std::string msg);

// これまでのログ行のスナップショット（古い順）。
std::vector<std::string> lines();

// ログを消去する。
void clear();

// t0 からの経過秒数（処理時間をログに出す用）。
double seconds_since(std::chrono::steady_clock::time_point t0);

}    // namespace applog
