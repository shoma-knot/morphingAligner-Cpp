#pragma once

/// @file batch_cli.hpp
/// @brief batch サブコマンド（ウィンドウを開かずに連続書き出しをする）。
///
/// セッション（または音声＋tcmorph のアンカー JSON）を読み、base / target を解析して、GUI の
/// 「連続書き出し」と同じ run_batch_export を呼ぶ。--auto-anchors なら、その前に音素アライメントから
/// アンカーを作る（build_auto_anchors）。GUI に依存しない（GLFW を初期化する前に呼ぶ）。

#include <cstdio>

#include "auto_anchor_pipeline.hpp"
#include "launch_options.hpp"

// batch を実行する。経過と誤りは log に、書き出したファイル（と保存したセッション）の一覧は out に出す。
// 戻り値は終了コード（0 = 成功、1 = 失敗、130 = Ctrl+C で中止）。opts のパスは絶対パスにしてから渡すこと。
// tools は --auto-anchors で使う Python のツール（nullptr なら本物。テストで偽物に差し替える）。
int run_batch_command(const LaunchOptions& opts, std::FILE* out = stdout, std::FILE* log = stderr,
                      const AutoAnchorTools* tools = nullptr);
