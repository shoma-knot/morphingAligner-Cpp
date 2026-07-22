#pragma once

/// @file session.hpp
/// @brief アンカーと wave パスの JSON 保存/読み込み。

#include <string>

struct App;

// 現在の wave パスとアンカーを JSON で `path` に保存する。結果は applog に出力、成功で true。
bool save_session(const App& app, const std::string& path);

// JSON `path` を読み込み、waves の音声を再解析してアンカーを復元する。音声が見つからない/
// 開けない場合は中止する。結果は applog に出力、成功で true。
bool load_session(App& app, const std::string& path);
