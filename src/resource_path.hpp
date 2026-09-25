#pragma once

/// @file resource_path.hpp
/// @brief 同梱の資材（font/・licenses/・python/・.env/）の探索。

#include <string>

// rel（配布物のルートからの相対パス、UTF-8）を探す。配布物（またはリポジトリ）のルートから
// 起動した場合と bin/ から起動した場合の両方で見つかるよう、rel → ../rel の順に試し、
// 最初に存在したファイルのパスを返す（相対パスのまま）。見つからなければ空。
std::string find_resource(const std::string& rel);
