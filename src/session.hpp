#pragma once

/// @file session.hpp
/// @brief アンカーと wave パスの JSON 保存/読み込み。

#include <string>
#include <vector>

#include "analysis.hpp"

struct Anchor;
struct App;

// 現在の wave パスとアンカーを JSON で `path` に保存する。結果は applog に出力、成功で true。
bool save_session(const App& app, const std::string& path);

// セッション読み込みのワーカー安全な成果物（GL なし）。ok=false なら失敗（applog 出力済み）。
struct SessionLoadData {
    bool                ok = false;
    std::string         base_path, target_path;
    Spectrogram         base_spec, target_spec;    // 表示用スペクトログラム（解析済み）
    std::vector<Anchor> anchors;
};

// JSON `path` を読み込み、waves の音声を解析して SessionLoadData を作る（ワーカーで実行可）。
// 音声が見つからない/開けない場合は中止（ok=false）。経過は applog に出力。
SessionLoadData load_session_data(const std::string& path);

// load_session_data の結果を App に反映する（テクスチャ生成を含むため必ずメインスレッドで）。
void apply_session_data(App& app, SessionLoadData&& data);
