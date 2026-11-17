#pragma once

/// @file envelope_transport.hpp
/// @brief スペクトル包絡を 1次元の最適輸送（変位補間）で混ぜる（試作。MorphPostOptions の OT で補間）。
///
/// tcmorph は、アンカーで同じ周波数軸に揃えた base/target の包絡を dB で線形に平均するので、山の位置
/// が少しでも残ってずれていると、山が低く広くなる。ここでは平均の代わりに、揃えた2つの包絡の間で
/// 質量を「運ぶ」補間（displacement interpolation）をする。ずれた山は、平らな2つの山ではなく、中間の
/// 位置の1つの山になる。
///
/// - 質量はパワーを gamma 乗で圧縮したもの。運ぶのは、base 側の帯と target 側の対応する帯の間だけ。
///   帯は周波数アンカーの構造ごとに1つで、端は隣り合う構造の中間（谷のあたり）。各構造が揃えた後の
///   包絡のどこにあるかは tcmorph と同じ計算で求める。時間アンカーの時刻では base/target で同じ位置に
///   重なるが、その間のフレームでは tcmorph が両側のワープを別々に線形補間するので、同じ構造でも
///   base 側と target 側で位置がずれる。そのため帯の端は側ごとに持ち、補間後の帯はその間（率で線形
///   補間した位置）になる。フォルマントの山は1つの塊のまま、対応する山へ運ばれる（帯をまたいで質量が
///   流れない）。
/// - 帯の中の質量の合計は、base/target の値を率で幾何補間する。帯の両端の値は、端に当たる base 側と
///   target 側の値の幾何補間に合わせる（dB で直線の補正を足す）。隣の帯とは同じ端を共有するので境目で
///   段差ができず、範囲の両端では tcmorph の平均と一致するので範囲の外ともつながる。
/// - 範囲（lo_hz〜hi_hz）の外の形と、無音に近いフレームは tcmorph の平均のまま。
///
/// 周波数軸の目盛りは tcmorph の中間の軸に合わせる（bin b が b * fs / (2 * nbin) Hz。WORLD の
/// b * fs / fft_size とは 1 bin の幅がわずかに違う）。GUI 非依存。

#include <vector>

#include <Eigen/Core>

#include "morphing.hpp"    // AnchorMatrices / MorphRates

// 1つの帯の変位補間。ref / tgt は同じ格子に揃えた1フレームの包絡（線形パワー、n 点）、[a_ref, b_ref] と
// [a_tgt, b_tgt] は対応する帯（bin の単位の実数。0 <= a < b <= n-1）。補間後の帯
// [(1-s) a_ref + s a_tgt, (1-s) b_ref + s b_tgt] の内側の bin の out を書き換える（外側と、ちょうど帯の端に
// 当たる bin は触らない）。s は率（0 = ref, 1 = tgt）。質量が 0 の帯は何もしない（単体テスト用に公開）。
void transport_band(const double* ref, const double* tgt, double* out, int n, double a_ref, double b_ref, double a_tgt,
                    double b_tgt, double s, double gamma);

// tcmorph が1つの時間アンカーで、中間の周波数軸に置く周波数アンカーの位置 [Hz]（昇順）。ref_hz / tgt_hz は
// base / target 側の周波数アンカー（同じ本数、base 側の昇順）。アンカーの間隔（対数周波数）を fx の率で
// 幾何補間して積み上げる（tcmorph の word_tv2w_morphing.hpp と同じ式）。間隔が正でない（target 側が
// 逆転している）ときは空を返す（単体テスト用に公開）。
std::vector<double> morph_axis_freq_anchors(const std::vector<double>& ref_hz, const std::vector<double>& tgt_hz,
                                            double fx, double fs);

struct TransportStats {
    int    frames  = 0;      // OT で補間したフレーム数
    double bands   = 0.0;    // そのフレームの帯の数の平均
    int    skipped = 0;      // 周波数アンカーの逆転などで帯を決められず、平均のままにしたフレーム数
};

// 包絡を OT で混ぜる（MorphPostOptions の OT で補間の中身）。mix は tcmorph が混ぜた包絡で、これを
// 書き換える。ref / tgt は同じ格子に揃えた base / target の包絡（sl = 0 / 1 で合成したもの）。
// am はモーフィングに使ったアンカー、end_ref / end_tgt は base / target の最終フレーム時刻 [s]、
// frame_s はフレーム周期 [s]（いずれも tcmorph がモーフ後の時間軸を作るのに使う値）。
// 各フレームのパワーの和は mix のまま（聴き比べが音量に引きずられないように）。
// lo_hz〜hi_hz は運ぶ範囲（hi_hz はナイキスト周波数で切り詰める）。範囲の外は形を変えない（フレームの
// パワーを戻すための一律の倍率だけ）。
TransportStats transport_envelopes(Eigen::MatrixXd& mix, const Eigen::MatrixXd& ref, const Eigen::MatrixXd& tgt,
                                   const AnchorMatrices& am, double end_ref, double end_tgt, double frame_s,
                                   const MorphRates& rate, int fs, double gamma, double lo_hz, double hi_hz);
