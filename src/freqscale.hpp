#pragma once

/// @file freqscale.hpp
/// @brief スペクトログラム表示の周波数軸尺度（ERB レート）。
///
/// 表示・周波数アンカー・Y軸ズームはこの尺度（ERB レート）を軸座標として扱う。
/// 実周波数(Hz)との相互変換をここに集約する。モーフィング処理は Hz のままで無関係。

#include <cmath>

namespace freqscale {

// ERB レート（Glasberg & Moore 1990）。hz→ERB-rate。
inline double hz_to_erb(double hz) {
    return 21.4 * std::log10(1.0 + 0.00437 * hz);
}

// ERB レート→hz（hz_to_erb の逆関数）。
inline double erb_to_hz(double erb) {
    return (std::pow(10.0, erb / 21.4) - 1.0) / 0.00437;
}

}    // namespace freqscale
