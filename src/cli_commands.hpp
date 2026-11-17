#pragma once

/// @file cli_commands.hpp
/// @brief ウィンドウを開かずに動くサブコマンド（auto-anchors・convert・export）。
///
/// どれもセッションの JSON を受け渡す。auto-anchors は音声と書き起こしからアンカーを作ってセッションに保存し
/// （auto_anchor_pipeline.hpp）、convert は tcmorph のアンカー JSON をセッションにし（tcmorph_manifest.hpp）、
/// export はセッションから GUI の「連続書き出し」と同じ run_batch_export で書き出す。GUI に依存しない
/// （GLFW を初期化する前に呼ぶ）。2026-11-17 まではこの3つが batch サブコマンドにまとまっていた。
///
/// 経過と誤りは log に、作ったファイルの一覧は out に1行1ファイルで出す。戻り値は終了コード
/// （0 = 成功、1 = 失敗、130 = Ctrl+C で中止）。opts のパスは絶対パスにしてから渡すこと。

#include <cstdio>

#include "auto_anchor_pipeline.hpp"
#include "launch_options.hpp"

// auto-anchors。tools は Python のツール（nullptr なら本物。テストで偽物に差し替える）。
int run_auto_anchors_command(const LaunchOptions& opts, std::FILE* out = stdout, std::FILE* log = stderr,
                             const AutoAnchorTools* tools = nullptr);

// convert。
int run_convert_command(const LaunchOptions& opts, std::FILE* out = stdout, std::FILE* log = stderr);

// export。
int run_export_command(const LaunchOptions& opts, std::FILE* out = stdout, std::FILE* log = stderr);
