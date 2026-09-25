#pragma once

/// @file launch_options.hpp
/// @brief コマンドライン引数（起動時に音声・セッションを読み込む。開発を楽にするため）。
///
///   morphingAlignerCpp [--base <音声>] [--target <音声>] [--session <JSON>]
///                      [--transcript <書き起こし>] [--align] [--help] [--version]
///
/// 値は「--base a.wav」「--base=a.wav」のどちらでも書ける。パスは UTF-8。

#include <string>
#include <vector>

#include "result.hpp"

struct LaunchOptions {
    std::string base;          // --base: base の音声（空 = 指定なし）
    std::string target;        // --target: target の音声
    std::string session;       // --session: セッション（または tcmorph のアンカー JSON）
    std::string transcript;    // --transcript: MFA 用の書き起こし（セッションの書き起こしより優先）
    bool has_transcript = false;    // --transcript が指定されたか（空文字列の指定と区別する）
    bool align          = false;    // --align: 読み込み後、音声解析の環境が使えたら音素アライメントを実行
    bool help           = false;    // --help: 使い方を表示して終了
    bool version        = false;    // --version: 版を表示して終了

    // 起動時に読み込むものがあるか。
    bool has_files() const { return !base.empty() || !target.empty() || !session.empty(); }
};

// 引数（プログラム名を除く）を解釈する。知らない引数・値の無いオプション・同じオプションの重複・
// 読み込むものが無いのに --align、などは error に理由を入れて返す。--help / --version があれば
// ほかの検査はしない。
Result<LaunchOptions> parse_launch_options(const std::vector<std::string>& args);

// 相対パスを、起動時のカレントディレクトリを基準にした絶対パス（UTF-8）にする。
// セッションに保存するパスが、あとで別の場所から起動しても指すものが変わらないようにするため。
void make_paths_absolute(LaunchOptions& opts);

// 使い方の文言（--help と引数のエラーで表示する）。
std::string launch_usage();

// 標準出力・標準エラーに日本語を出せるようにする（Windows ではコンソールを UTF-8 にする）。
void use_utf8_console();

// プログラムに渡された引数を UTF-8 で取り出す（プログラム名を除く）。Windows の argv は ANSI
// （日本語の Windows なら Shift_JIS）なので使わず、GetCommandLineW から取り直す。
std::vector<std::string> utf8_arguments(int argc, char** argv);
