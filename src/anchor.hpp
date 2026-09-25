#pragma once

/// @file anchor.hpp
/// @brief base/target の時間・周波数の対応（アンカー）のデータ型。
///
/// GUI に依存しない型なので、モーフィング・セッションの読み書き・アンカー自動生成
/// （いずれもコアのライブラリ側）から、App を経由せずに使う。

#include <vector>

// base / target のどちら側か。値は配列の添字に使う（side_index）。
enum class Side { Base = 0, Target = 1 };

// 両側を順に回すための列（for (Side s : kSides)）。
constexpr Side kSides[] = { Side::Base, Side::Target };

constexpr int side_index(Side s) {
    return static_cast<int>(s);
}

// 時間アンカー上に打つ周波数の対応。作成時は base/target 同じ周波数で、
// あとから各パネルの線上でドラッグして周波数対応を編集する。
// 表示番号は所属する時間アンカー内の並び順（インデックス+1）で、base/target 両方の
// 点に同じ番号を出して対応を示す。
struct FreqAnchor {
    double base_f;      // [Hz]
    double target_f;    // [Hz]

    double&       freq(Side s) { return s == Side::Base ? base_f : target_f; }
    const double& freq(Side s) const { return s == Side::Base ? base_f : target_f; }
};

// 時間軸の対応。base/target のスペクトログラムに縦線を1本ずつ立てる。
// 作成時は両者同じ時刻で、あとから各線をドラッグして対応を定義する。
// freqs はこの時間アンカー線上に乗る周波数アンカー（Ctrl+左クリックで追加）。
struct Anchor {
    double                  base_t;      // [s]
    double                  target_t;    // [s]
    std::vector<FreqAnchor> freqs;

    double&       time(Side s) { return s == Side::Base ? base_t : target_t; }
    const double& time(Side s) const { return s == Side::Base ? base_t : target_t; }
};
