#pragma once

/// @file cli_common.hpp
/// @brief サブコマンド（cli_*.cpp）が共有する小さな部品。cli_commands.hpp の実装の中だけで使う。

#include <atomic>
#include <cstdio>
#include <string>

#include "result.hpp"
#include "session_io.hpp"

namespace cli {

constexpr int kExitOk          = 0;
constexpr int kExitFailed      = 1;
constexpr int kExitInterrupted = 130;    // シェルの慣習（128 + SIGINT）

// Ctrl+C（SIGINT）で立つ。SigintScope の間だけ有効。
extern std::atomic<bool> g_interrupted;

// 実行中だけ SIGINT を受け取り、終わったら元に戻す。Linux では Python / MFA の子プロセスを
// 別のプロセスグループで起動しているので、端末の Ctrl+C が子に届かない。受け取って自分で
// 止めないと、本体だけが終わって MFA が残る（Windows は Job Object で道連れになる）。
class SigintScope {
public:
    SigintScope();
    ~SigintScope();
    SigintScope(const SigintScope&)            = delete;
    SigintScope& operator=(const SigintScope&) = delete;

private:
    void (*prev_)(int);
};

// 1行出して、すぐ流す（経過が途中で見えるように）。
void say(std::FILE* f, const std::string& s);

// セッション（または tcmorph のアンカー JSON）のファイルを読む。notes は log に出す。
Result<SessionFile> read_session_file(const std::string& path, std::FILE* log);

// 保存先がすでにあり、上書きしてよいと言われていなければ error。
Status check_not_exists(const std::string& path, bool overwrite);

// セッションを保存する（保存先のフォルダは無ければ作る）。
Status save_session_file(const std::string& path, const SessionFile& s);

}    // namespace cli
