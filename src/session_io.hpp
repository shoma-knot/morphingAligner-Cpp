#pragma once

/// @file session_io.hpp
/// @brief セッション JSON の文字列との相互変換（ファイル操作・音声解析・App に依存しない）。
///
/// 読み込みは2形式に対応する:
///   - セッション形式（本アプリが保存するもの。version + waves + transcript + anchors）
///   - tcmorph のアンカー形式（anchor_io.hpp の objects 配列。音声パスを持たない）

#include <string>
#include <vector>

#include "anchor.hpp"

// セッションの中身。
struct SessionFile {
    // tcmorph のアンカー形式だった場合は true。音声パスと書き起こしは空で、アンカーだけを持つ。
    bool                anchors_only = false;
    std::string         base_path, target_path;
    std::string         transcript;    // MFA 用の書き起こし（任意、base/target 共通）
    std::vector<Anchor> anchors;
    // 読み込めたが知らせておくこと（tcmorph 形式の警告や素材名）。呼び出し側がログに出す。
    std::vector<std::string> notes;
};

// セッション形式の JSON 文字列にする（整形済み、末尾に改行）。anchors_only と notes は使わない。
std::string session_to_json(const SessionFile& s);

// JSON 文字列を読む。形式が不正なら std::exception を投げる（what() は理由。例: "anchors が不正: ..."）。
SessionFile parse_session_json(const std::string& text);
