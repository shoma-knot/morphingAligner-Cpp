#pragma once

/// @file log.hpp
/// @brief 画面下部のログ領域に出す動作ログ（スレッドセーフ。ワーカーからも add 可）。
///
/// 長く使っても重くならないよう、保持するのは新しい方から kMaxLines 行まで（古い行から捨てる）。
/// 表示側は見えている範囲だけを lines(first, count) で取り出す。

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace applog {

// 保持する最大行数。
constexpr std::size_t kMaxLines = 5000;

// タイムスタンプ付きで1行追記する（スレッドセーフ）。
void add(std::string msg);

// 保持している行数。
std::size_t size();

// first 行目（0 = 保持している中で最も古い行）から最大 count 行のコピー（古い順）。
std::vector<std::string> lines(std::size_t first, std::size_t count);

// ログを消去する。
void clear();

// t0 からの経過秒数（処理時間をログに出す用）。
double seconds_since(std::chrono::steady_clock::time_point t0);

}    // namespace applog
