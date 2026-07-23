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

// モーフィング結果（morphed の中間データ＋合成音声）。error が空なら成功。
struct MorphOutput {
    MorphChannel        morphed;    // モーフ後の f0/sp/ap（表示用）
    std::vector<double> wave;       // 合成音声（mono, double, [-1,1] 付近）
    int                 fs = 0;
    std::string         error;

    bool ok() const { return error.empty(); }
};

// 1音源を WORLD で解析して f0/sp/ap のチャンネルを作る（base/target 用）。
// 失敗時は err に理由を入れ、empty() なチャンネルを返す。
MorphChannel analyze_channel(const std::string& path, std::string& err);

// 解析済みの base/target チャンネルから、anchors と軸ごとの率で morphed を合成する
// （base/target の再解析は行わない）。失敗時は結果の error にメッセージを入れる。
MorphOutput morphing_channels(const MorphChannel& base, const MorphChannel& target,
                              const std::vector<Anchor>& anchors, const MorphRates& rates);

// wave を 16bit PCM モノラル WAV として path に書き出す。成功で true。
bool write_wav(const std::string& path, const std::vector<double>& wave, int fs, std::string& err);
