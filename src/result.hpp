#pragma once

/// @file result.hpp
/// @brief 失敗しうる処理の戻り値（値＋エラーの理由）。
///
/// コア（GUI 非依存）の関数の失敗は、次のどれかで返す（引数の err や例外では返さない）:
///   - Result<T> … 値を返す処理（analyze_file・run_formants・parse_session_json など）
///   - Status    … 値を返さない処理（write_wav など）
///   - 独自の結果構造体 … 値とエラーのほかに警告なども返すもの（MorphOutput・AutoAnchorResult・
///                        SpeechEnvStatus）。いずれも error が空なら成功で、ok() を持つ。
/// エラーの理由は利用者向けの日本語で、呼び出し側がそのままログに出せる形にする。

#include <string>
#include <utility>

// 値を返さない処理の結果。error が空なら成功。
struct Status {
    std::string error;

    bool ok() const { return error.empty(); }

    static Status failure(std::string e) { return Status { std::move(e) }; }
};

// 値を返す処理の結果。成功なら value が有効で error は空。失敗なら error に理由が入る
// （value は既定値のまま）。
template <class T>
struct Result {
    T           value {};
    std::string error;

    bool ok() const { return error.empty(); }

    static Result success(T v) {
        Result r;
        r.value = std::move(v);
        return r;
    }
    static Result failure(std::string e) {
        Result r;
        r.error = std::move(e);
        return r;
    }
};
