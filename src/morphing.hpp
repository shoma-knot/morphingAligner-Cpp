#pragma once

/// @file morphing.hpp
/// @brief tcmorph（Kawahara の wordTV2WmorphingEngineRev.m の移植）による音声モーフィング。
///
/// WORLD で解析済みの base/target（analysis.hpp の MorphChannel）と、
/// アンカーと率を tcmorph::aligner::WordTV2WMorphing に渡してモーフィングし、WORLD で
/// 合成する。エンジンのオプションは既定値＝MATLAB 版の挙動を再現する側のまま使う。
///
/// tcmorph には N 素材の GeneralizedTCMorphing もあるが、本アプリは morphingAligner
/// 相当（参照/目標の2素材）なので aligner 側を使う。両者は別物で同じアンカーでも音が違う。

#include <string>
#include <vector>

#include "analysis.hpp"    // MorphChannel

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

// モーフィング結果（morphed の中間データ＋合成音声）。error が空なら成功。
struct MorphOutput {
    MorphChannel        morphed;    // モーフ後の f0/sp/ap（表示用）
    std::vector<double> wave;       // 合成音声（mono, double, [-1,1] 付近）
    int                 fs = 0;
    std::string         error;

    // 計算は続行したが注意が要る点（エンジンの警告＋読み飛ばしたアンカー）。
    std::vector<std::string> warnings;

    bool ok() const { return error.empty(); }
};

// 解析済みの base/target チャンネルから、anchors と軸ごとの率で morphed を合成する
// （base/target の再解析は行わない）。失敗時は結果の error にメッセージを入れる。
MorphOutput morphing_channels(const MorphChannel& base, const MorphChannel& target,
                              const std::vector<Anchor>& anchors, const MorphRates& rates);

// tcmorph が要求する形に整えたアンカー（時間アンカーは両側とも狭義単調増加、周波数
// アンカーは (本数, 時間アンカー数) の 0 詰め行列）。両端の境界アンカーはエンジン側が
// 付けるので含まない。
struct AnchorMatrices {
    Eigen::VectorXd t_ref, t_tgt;      // [n_anch]
    Eigen::MatrixXd tf_ref, tf_tgt;    // (n_fanchor, n_anch)、余りは 0 詰め
    int             dropped_time = 0;  // 範囲外/順序が逆で読み飛ばした時間アンカー数
    int             dropped_freq = 0;  // 同・周波数アンカー数
};

// UI のアンカー列を AnchorMatrices に変換する（morphing_channels が内部で使う。単体テスト用に公開）。
//   - 解析範囲 (0, end_ref) / (0, end_tgt) の外と、base_t が直前と重なるものは落とす。
//   - target_t も狭義単調増加になるよう、残せる本数が最大の部分列を選ぶ（交差した線を落とす）。
//   - 周波数アンカーは (1 Hz, nyquist) の外を落とし、base_f の昇順に並べる。
AnchorMatrices build_anchor_matrices(const std::vector<Anchor>& anchors, double end_ref, double end_tgt,
                                     double nyquist);

// wave を WAV（32bit float モノラル）として path に書き出す。
Status write_wav(const std::string& path, const std::vector<double>& wave, int fs);
