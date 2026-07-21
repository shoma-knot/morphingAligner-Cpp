#pragma once

/// @file morphing.hpp
/// @brief WORLD ベースの音声モーフィング。
///
/// Kawahara の generalizedTCmorphing.m を参考に、base/target を WORLD で解析し
/// （Harvest=F0, CheapTrick=スペクトル包絡, D4C=非周期性）、アンカーによる時間軸・
/// 周波数軸ワープと率 rate の重み補間でモーフィングして合成する。

#include <string>
#include <vector>

struct Anchor;

// モーフィング結果。error が空なら成功。
struct MorphResult {
    std::vector<double> wave;    // 合成音声（mono, double, [-1,1] 付近）
    int                 fs = 0;
    std::string         error;

    bool ok() const { return error.empty(); }
};

// base/target を解析し、anchors による時間/周波数ワープと率 rate（0=base, 1=target）の
// 補間でモーフィングした音声を合成する。失敗時は結果の error にメッセージを入れる。
MorphResult morphing(const std::string& base_path, const std::string& target_path,
                     const std::vector<Anchor>& anchors, double rate);

// wave を 16bit PCM モノラル WAV として path に書き出す。成功で true。
bool write_wav(const std::string& path, const std::vector<double>& wave, int fs, std::string& err);
