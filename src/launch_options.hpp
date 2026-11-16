#pragma once

/// @file launch_options.hpp
/// @brief コマンドライン引数。最初にサブコマンドを書く。
///
///   morphingAlignerCpp gui   [--base <音声>] [--target <音声>] [--session <JSON>]
///                            [--transcript <書き起こし>] [--align]
///   morphingAlignerCpp batch --count <本数> --out <フォルダ> （--session <JSON> | --base <音声> --target <音声>）
///                            [--prefix <接頭辞>] [--sweep <軸,...>] [--fixed <軸=率,...>]
///                            [--no-manifest] [--overwrite]
///   morphingAlignerCpp --help | --version
///
/// gui は起動時に音声・セッションを読み込んで GUI を開く。batch はウィンドウを開かずに連続書き出し
/// （batch_export.hpp）をして終わる。引数が1つも無ければ gui とみなす（デスクトップのアイコンや
/// ダブルクリックでの起動）。引数があるのにサブコマンドが無ければ誤り。
/// 値は「--base a.wav」「--base=a.wav」のどちらでも書ける。パスは UTF-8。

#include <string>
#include <vector>

#include "morphing.hpp"    // kMorphAxisCount
#include "result.hpp"

enum class LaunchCommand {
    None,     // サブコマンドなしの --help / --version
    Gui,      // gui（引数なしの起動も含む）
    Batch,    // batch
};

// batch の設定（gui では使わない）。
struct BatchCliOptions {
    int         count = 0;    // --count: 書き出す本数（必須）
    std::string out;          // --out: 出力先のフォルダ（必須。無ければ作る）
    std::string prefix;       // --prefix: ファイル名の接頭辞（空 = base と target のファイル名から作る）
    // --sweep: 変化させる軸（添字は kMorphAxes）。既定は全軸。
    bool sweep[kMorphAxisCount] = { true, true, true, true, true };
    // --fixed: 変化させない軸の率。指定しなかった軸は 0（base 側）。
    double fixed[kMorphAxisCount] = {};
    bool   manifest  = true;     // --no-manifest で false（率とアンカーの JSON を書かない）
    bool   overwrite = false;    // --overwrite: 既存のファイルを上書きしてよい
};

struct LaunchOptions {
    LaunchCommand command = LaunchCommand::Gui;
    std::string base;          // --base: base の音声（空 = 指定なし）
    std::string target;        // --target: target の音声
    std::string session;       // --session: セッション（または tcmorph のアンカー JSON）
    std::string transcript;    // --transcript: MFA 用の書き起こし（gui のみ。セッションの書き起こしより優先）
    bool has_transcript = false;    // --transcript が指定されたか（空文字列の指定と区別する）
    bool align          = false;    // --align: 読み込み後、音声解析の環境が使えたら音素アライメントを実行（gui のみ）
    bool help           = false;    // --help: 使い方を表示して終了（command のサブコマンドの説明）
    bool version        = false;    // --version: 版を表示して終了
    BatchCliOptions batch;          // batch の設定

    // 起動時に読み込むものがあるか。
    bool has_files() const { return !base.empty() || !target.empty() || !session.empty(); }
};

// 引数（プログラム名を除く）を解釈する。サブコマンドが無い・知らない、そのサブコマンドで使えない
// 引数・値の無いオプション・同じオプションの重複・値の誤り、などは error に理由を入れて返す。
// --help / --version があれば、そのサブコマンドの中のほかの検査はしない。
Result<LaunchOptions> parse_launch_options(const std::vector<std::string>& args);

// 最初の引数のサブコマンド（大文字でもよい）。サブコマンドでなければ None。
LaunchCommand launch_command_of(const std::vector<std::string>& args);

// 相対パスを、起動時のカレントディレクトリを基準にした絶対パス（UTF-8）にする。
// セッションに保存するパスが、あとで別の場所から起動しても指すものが変わらないようにするため。
void make_paths_absolute(LaunchOptions& opts);

// 使い方の文言（--help と引数のエラーで表示する）。None なら全体の説明。
std::string launch_usage(LaunchCommand command = LaunchCommand::None);

// 標準出力・標準エラーに日本語を出せるようにする（Windows ではコンソールを UTF-8 にする）。
void use_utf8_console();

// プログラムに渡された引数を UTF-8 で取り出す（プログラム名を除く）。Windows の argv は ANSI
// （日本語の Windows なら Shift_JIS）なので使わず、GetCommandLineW から取り直す。
std::vector<std::string> utf8_arguments(int argc, char** argv);
