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

// 軸ごとのモーフィング率（各 0=base, 1=target）。UI から一律にしたい場合は全て同値にする。
struct MorphRates {
    double tx = 0.5;    // 時間軸
    double fx = 0.5;    // 周波数軸
    double fo = 0.5;    // F0
    double sl = 0.5;    // スペクトルレベル
    double ap = 0.5;    // 非周期性

    // 全軸を同じ率にした MorphRates を作る。
    static MorphRates uniform(double r) { return { r, r, r, r, r }; }
};

// 1音源（base/target/morphed）の表示用データ（f0 と、スペクトル/非周期性）。
struct MorphChannel {
    int    fs = 0, fft_size = 0, nbin = 0, n_frames = 0;
    double frame_period = 0;    // ms
    double duration     = 0;    // s
    std::vector<double>              f0;    // [n_frames]
    std::vector<std::vector<double>> sp;    // [n_frames][nbin] パワースペクトル
    std::vector<std::vector<double>> ap;    // [n_frames][nbin] 非周期性 [0,1]

    bool empty() const { return n_frames == 0; }
};

// base/target/morphed の中間データ＋合成音声（モーフィングタブの表示用）。
struct MorphOutput {
    MorphChannel        base, target, morphed;
    std::vector<double> wave;
    int                 fs = 0;
    std::string         error;

    bool ok() const { return error.empty(); }
};

// base/target を解析し、anchors による時間/周波数ワープと軸ごとの率でモーフィングした
// 音声を合成する。失敗時は結果の error にメッセージを入れる。
MorphResult morphing(const std::string& base_path, const std::string& target_path,
                     const std::vector<Anchor>& anchors, const MorphRates& rates);

// morphing() と同じ処理で、base/target/morphed の f0/sp/ap も返す（表示用）。
MorphOutput morphing_full(const std::string& base_path, const std::string& target_path,
                          const std::vector<Anchor>& anchors, const MorphRates& rates);

// wave を 16bit PCM モノラル WAV として path に書き出す。成功で true。
bool write_wav(const std::string& path, const std::vector<double>& wave, int fs, std::string& err);
