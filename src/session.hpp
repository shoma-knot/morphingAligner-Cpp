#pragma once

/// @file session.hpp
/// @brief アンカーと wave パスの JSON 保存/読み込み。
///
/// 読み込みは2形式に対応する:
///   - セッション形式（本アプリが保存するもの。waves + anchors）
///   - tcmorph のアンカー形式（anchor_io.hpp の objects 配列。音声パスを持たない）
/// 後者は音声を差し替えず、現在の base/target にアンカーだけを乗せる。

#include <string>
#include <vector>

#include "analysis.hpp"

struct Anchor;
struct App;

// 現在の wave パスとアンカーを JSON で `path` に保存する。結果は applog に出力、成功で true。
bool save_session(const App& app, const std::string& path);

// セッション読み込みのワーカー安全な成果物（GL なし）。ok=false なら失敗（applog 出力済み）。
struct SessionLoadData {
    bool ok = false;
    // tcmorph のアンカー形式だった場合は true。音声を含まないので base_path 以下は空で、
    // 適用時は現在の base/target にアンカーだけを乗せる。
    bool                anchors_only = false;
    std::string         base_path, target_path;
    std::string         transcript;    // MFA 用の書き起こし（任意、base/target 共通）
    AnalyzedAudio       base_audio, target_audio;    // 解析済みの音声
    std::vector<Anchor> anchors;
};

// JSON `path` を読み込んで SessionLoadData を作る（ワーカーで実行可）。
// セッション形式なら waves の音声も解析する。音声が見つからない/開けない場合は中止
// （ok=false）。tcmorph のアンカー形式ならアンカーだけを読む。経過は applog に出力。
SessionLoadData load_session_data(const std::string& path);

// load_session_data の結果を App に反映する（テクスチャ生成を含むため必ずメインスレッドで）。
void apply_session_data(App& app, SessionLoadData&& data);
