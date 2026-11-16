#pragma once

/// @file batch_cli.hpp
/// @brief batch サブコマンド（ウィンドウを開かずに連続書き出しをする）。
///
/// セッション（または音声＋tcmorph のアンカー JSON）を読み、base / target を解析して、GUI の
/// 「連続書き出し」と同じ run_batch_export を呼ぶ。GUI に依存しない（GLFW を初期化する前に呼ぶ）。

#include <cstdio>

#include "launch_options.hpp"

// batch を実行する。経過と誤りは log に、書き出したファイルの一覧は out に出す。
// 戻り値は終了コード（0 = 成功、1 = 失敗）。opts のパスは絶対パスにしてから渡すこと。
int run_batch_command(const LaunchOptions& opts, std::FILE* out = stdout, std::FILE* log = stderr);
