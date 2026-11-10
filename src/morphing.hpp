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

// 軸の一覧（添字で回すとき用）。key はファイル名や JSON に使う短い名前。
// 率は rates.*kMorphAxes[i].rate で読み書きする。
struct MorphAxis {
    const char* key;
    double MorphRates::*rate;
};
inline constexpr MorphAxis kMorphAxes[] = {
    { "tx", &MorphRates::tx }, { "fx", &MorphRates::fx }, { "fo", &MorphRates::fo },
    { "sl", &MorphRates::sl }, { "ap", &MorphRates::ap },
};
inline constexpr int kMorphAxisCount = static_cast<int>(sizeof kMorphAxes / sizeof kMorphAxes[0]);

// スペクトル包絡の混ぜ方（試作）。tcmorph はアンカーで揃えた base/target の包絡を dB で線形に平均
// するので、山の位置や形が少しでも違うと平らになる（sl = 0.5 付近で凹凸が約 2 割減った。DEVLOG 参照）。
// どれも sl = 0 / 1 では何もせず（元の音のまま）、フレームの音量は平均のときと同じにする。
enum class EnvelopeMix {
    // tcmorph の平均のまま。
    Average,
    // 平均の後で凹凸を戻す（restore_envelope_contrast）。フォルマントの帯域（300〜5500 Hz）の凹凸を、
    // その強さ（envelope_contrast）が「揃えた base/target の凹凸を sl で補間した値」になるように定数倍する。
    AverageRestore,
    // 範囲（MorphPostOptions::transport_lo_hz〜hi_hz）の中だけ、平均の代わりに、揃えた base/target の
    // 間で質量を運んで補間する（envelope_transport.hpp）。範囲の外は平均のまま。周波数アンカーの構造ごとの
    // 帯の中だけで運ぶ。ずれた山は、平らな2つの山ではなく中間の位置の1つの山になる（揃えた後も山の数や
    // 形がまったく違うフレームでは、平均より良くはならない）。
    Transport,
};

// tcmorph の出力を WORLD で合成する前に掛ける仕上げの処理（試作）。
struct MorphPostOptions {
    EnvelopeMix envelope        = EnvelopeMix::Average;
    double      transport_gamma = 0.3;    // OT で補間するときの質量 = パワー^gamma
    // OT で補間する範囲 [Hz]。既定は 5 kHz より上。5 kHz 以下は平均のままが聴感上よく（ユーザーの確認）、
    // 一方 5 kHz より上には周波数アンカーが無く（tcmorph は一様に引き伸ばすだけ）、base/target のエネルギーの
    // 山が帯域をまたいでずれていた。上限を 12 kHz で止めるのは、その上はほぼ雑音で、録音ごとの高域の
    // 落ち方の違いに引っぱられて、下の構造の対応までずれるため（上限はナイキスト周波数で切り詰める）。
    double transport_lo_hz = 5000.0;
    double transport_hi_hz = 12000.0;

    static constexpr double kGammaMin = 0.05, kGammaMax = 1.0;
    static constexpr double kRangeMinHz = 300.0, kRangeMaxHz = 24000.0;
};

// モーフィング結果（morphed の中間データ＋合成音声）。error が空なら成功。
struct MorphOutput {
    MorphChannel        morphed;    // モーフ後の f0/sp/ap（表示用）
    std::vector<double> wave;       // 合成音声（mono, double, [-1,1] 付近）
    int                 fs = 0;
    std::string         error;

    // 計算は続行したが注意が要る点（エンジンの警告＋読み飛ばしたアンカー）。
    std::vector<std::string> warnings;

    // 凹凸を戻したとき、フレームに掛けた倍率の平均（戻さなかったら 0）。ログ用。
    double contrast_gain = 0.0;
    // OT で補間したとき: 補間したフレーム数、そのフレームの帯の数の平均、周波数アンカーの逆転で
    // 平均のままにしたフレーム数（補間しなかったらすべて 0）。ログ用。
    int    transport_frames  = 0;
    double transport_bands   = 0.0;
    int    transport_skipped = 0;

    bool ok() const { return error.empty(); }
};

// 解析済みの base/target チャンネルから、anchors と軸ごとの率で morphed を合成する
// （base/target の再解析は行わない）。失敗時は結果の error にメッセージを入れる。
MorphOutput morphing_channels(const MorphChannel& base, const MorphChannel& target,
                              const std::vector<Anchor>& anchors, const MorphRates& rates,
                              const MorphPostOptions& post = {});

// 包絡の凹凸の強さ（線形パワーの1フレーム）。300〜5500 Hz の dB 包絡から両端を結ぶ直線（大まかな
// 傾き）を引き、周期 300 Hz 以上の構造を DST で表したときの係数の大きさ（範囲全体のゆるい膨らみ
// である最初の係数は除く）。単位は dB。restore_envelope_contrast が目標にする量で、評価にも使う。
double envelope_contrast(const Eigen::VectorXd& power, int fs);

// 包絡の凹凸を戻す（EnvelopeMix::AverageRestore の中身。単体テスト用に公開）。
// mix は tcmorph が混ぜた包絡、ref / tgt は同じ格子に揃えた base / target の包絡（sl = 0 / 1 で
// 合成したもの）。いずれも (nbin, n_frames) の線形パワー。各フレームの線形パワーの和は変えない
// （強めると山が持ち上がって音量が上がり、聴き比べが音量に引きずられるため。そのための一律の
// 倍率はフレーム全体にかかる）。
// 戻り値は掛けた倍率の平均（1 = 何もしなかった）。
double restore_envelope_contrast(Eigen::MatrixXd& mix, const Eigen::MatrixXd& ref, const Eigen::MatrixXd& tgt,
                                 double sl, int fs);

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
