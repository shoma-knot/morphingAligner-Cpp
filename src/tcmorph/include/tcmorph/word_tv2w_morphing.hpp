// tcmorph/word_tv2w_morphing.hpp
//
// worldGUItools/src/wordTV2WmorphingEngineRev.m (Apache-2.0) の C++17 移植版。
// morphingAligner.mlapp / morphingSoundGenerator.mlapp が使うエンジンで、
// morphingStr（edit.mat）を保存する GUI はこちらを呼ぶ。
//
// generalizedTCmorphing.m とは別物であることに注意。主な違い:
//
//   項目               generalizedTCmorphing   本エンジン
//   F0 の混合          Hz 領域の算術平均        幾何平均
//   周波数区間長の混合  log 差の算術平均         log 差の幾何平均
//   F0 の取得元        source_parameter.f0     f0_original
//   VUV                閾値で二値化             全フレーム有声に固定
//   非周期性           そのまま                 低域を最大 -60 dB 減衰
//   発話長             length(span)/fs         tFrameRef(end)
//   Nyquist ビン       そのまま                 直前のビンで上書き
//
// ---------------------------------------------------------------------------
// 移植方針
// ---------------------------------------------------------------------------
// MATLAB 版との数値比較が目的なので、既知の不具合も既定では「そのまま再現」する。
// Options のフラグで個別に修正版へ切り替えられる。既定値はすべて再現側。
//
//   fix_segment_end_index      130 行の off-by-one
//   emulate_catch_dropout      147-162 行の空 catch によるフレーム欠落
//   apply_aperiodicity_shaping 169-181 行の低域減衰
//   force_all_voiced           45 行の vuv = vuv*0 + 1
//
// ---------------------------------------------------------------------------
// 複素対数について
// ---------------------------------------------------------------------------
// 20 行と 77 行の real(exp((1-w)*log(a) + w*log(b))) は、a や b が負のとき
// MATLAB では複素対数になる。log(-|a|) = log|a| + i*pi なので、実部を取ると
//
//     exp((1-w)log|a| + w log|b|) * cos(pi * ((a<0)(1-w) + (b<0)w))
//
// となり、片方だけ負なら cos(pi/2) = 0 でちょうど 0 になる。
// アンカーが逆転していても MATLAB 版が破綻しないのはこのため
// （逆転した帯域が幅ゼロに潰れ、135 行の safeguard で分離される）。
// 本移植もこの挙動をそのまま再現する。

#ifndef TCMORPH_WORD_TV2W_MORPHING_HPP_
#define TCMORPH_WORD_TV2W_MORPHING_HPP_

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <tcmorph/generalized_tc_morphing.hpp>

namespace tcmorph {
namespace aligner {

/// モーフィング率。0 が参照 (ref)、1 が目標 (tgt)
struct MorphRate {
  double tx = 0.5;  ///< 時間軸
  double fx = 0.5;  ///< 周波数軸
  double fo = 0.5;  ///< 基本周波数
  double sl = 0.5;  ///< スペクトルレベル
  double ap = 0.5;  ///< 非周期性

  static MorphRate Uniform(double r) { return MorphRate{r, r, r, r, r}; }
};

struct Options {
  double vtl_ratio = 1.0;  ///< 声道長比。GUI の VTLSlider の値

  // --- 以下は既定で MATLAB の挙動を再現する。true にすると修正版になる ---
  bool fix_segment_end_index = false;   ///< 130 行の off-by-one を直す
  bool emulate_catch_dropout = true;    ///< 空 catch によるフレーム欠落を再現
  bool apply_aperiodicity_shaping = true;  ///< 低域 -60 dB 減衰を掛ける
  bool force_all_voiced = true;         ///< 45 行の vuv = vuv*0 + 1 を再現
};

struct Output {
  double fs = 0.0;
  VectorXd temporal_positions;
  VectorXd f0;            ///< f0_original .* vuv
  VectorXd f0_original;   ///< vuv を掛ける前
  VectorXd vuv;
  MatrixXd spectrogram;   ///< (n_fbin, n_frame) 線形パワー
  MatrixXd aperiodicity;  ///< (n_fbin, n_frame)
  MatrixXd aperiodicity_unshaped;  ///< 低域減衰を掛ける前（比較用）

  double elapsed_time = 0.0;
  std::vector<std::string> warnings;
  int n_dropped_frames = 0;   ///< 空 catch 相当で欠落したフレーム数
  int n_unsorted_interp = 0;  ///< 補間格子が非単調だった回数
};

namespace detail_tv {

constexpr double kPi = 3.14159265358979323846;

inline void Fail(const std::string& m) {
  throw std::invalid_argument("tcmorph::aligner: " + m);
}

/// real(exp((1-w)*log(a) + w*log(b))) を MATLAB の複素対数規則で評価する。
/// a や b が 0 のときは log(0) = -inf。w が 0 または 1 だと 0*(-inf) = NaN に
/// なるが、これも MATLAB と同じ挙動なのでそのまま返す。
inline double RealGeoMix(double a, double b, double w) {
  const double la = std::log(std::abs(a));   // a == 0 なら -inf
  const double lb = std::log(std::abs(b));
  const double r = (1.0 - w) * la + w * lb;  // 0 * (-inf) は NaN
  const double im = kPi * ((a < 0.0 ? (1.0 - w) : 0.0) + (b < 0.0 ? w : 0.0));
  return std::exp(r) * std::cos(im);
}

/// MATLAB の interp1(x, y, xq, "linear", "extrap") 相当。
///
/// MATLAB の interp1 は x が昇順でない場合 x と y をまとめてソートしてから
/// 補間する。アンカーが逆転していると x が非単調になるので、その挙動を
/// 再現する。重複点は微小量ずらして分離する（safeguard と同じ考え方）。
inline void Interp1(const VectorXd& x_in, const VectorXd& y_in, const VectorXd& xq,
                    VectorXd* out, bool* was_unsorted = nullptr) {
  const Index n = x_in.size();
  if (n < 2) Fail("interp1: 格子点は 2 点以上必要です");

  bool sorted = true;
  for (Index i = 1; i < n; ++i)
    if (x_in[i] < x_in[i - 1]) { sorted = false; break; }
  if (was_unsorted) *was_unsorted = !sorted;

  VectorXd x = x_in, y = y_in;
  if (!sorted) {
    std::vector<Index> idx(static_cast<std::size_t>(n));
    std::iota(idx.begin(), idx.end(), Index(0));
    std::stable_sort(idx.begin(), idx.end(),
                     [&](Index a, Index b) { return x_in[a] < x_in[b]; });
    for (Index i = 0; i < n; ++i) {
      x[i] = x_in[idx[static_cast<std::size_t>(i)]];
      y[i] = y_in[idx[static_cast<std::size_t>(i)]];
    }
  }

  // 重複点があると区間幅 0 で割ることになるので、最小限ずらして分離する
  for (Index i = 1; i < n; ++i) {
    const double gap = std::max(std::abs(x[i]), 1.0) * 1e-12;
    if (!(x[i] > x[i - 1])) x[i] = x[i - 1] + gap;
  }

  tcmorph::detail::GatherMap g;
  tcmorph::detail::BuildGatherMap(x, xq, &g);
  tcmorph::detail::ApplyGather(g, y, out);
}

/// 行列の各行を時間方向に補間する（(n_fbin, n_frame) の向き）
inline MatrixXd InterpTime(const VectorXd& t_src, const MatrixXd& m,
                           const VectorXd& t_dst) {
  const Index n_fbin = m.rows();
  MatrixXd out(n_fbin, t_dst.size());
  tcmorph::detail::GatherMap g;
  tcmorph::detail::BuildGatherMap(t_src, t_dst, &g);
  for (Index k = 0; k < t_dst.size(); ++k) {
    const Index i = g.idx[k];
    const double f = g.frac[k];
    out.col(k) = (1.0 - f) * m.col(i) + f * m.col(i + 1);
  }
  return out;
}

inline VectorXd Diff(const VectorXd& v) {
  return v.tail(v.size() - 1) - v.head(v.size() - 1);
}

}  // namespace detail_tv

// ===========================================================================
// 本体
// ===========================================================================

/// wordTV2WmorphingEngineRev.m と同じ処理を行う。
///
/// anchor_ref / anchor_tgt は (n_fanchor, n_tanchor) の周波数アンカーと
/// 長さ n_tanchor の時間アンカー。0 詰めは MATLAB 版と同じく
/// 「正の値の個数」で本数を判定する。
inline Output WordTV2WMorphing(const WorldParameter& ref, const WorldParameter& tgt,
                               const VectorXd& t_anchor_ref,
                               const MatrixXd& tf_anchor_ref,
                               const VectorXd& t_anchor_tgt,
                               const MatrixXd& tf_anchor_tgt,
                               const MorphRate& m, const Options& opt = {}) {
  using detail_tv::Interp1;
  using detail_tv::InterpTime;
  using detail_tv::Diff;
  using detail_tv::RealGeoMix;

  const auto start = std::chrono::steady_clock::now();
  Output out;

  const VectorXd& t_frame_ref = ref.source_parameter.temporal_positions;
  const VectorXd& t_frame_tgt = tgt.source_parameter.temporal_positions;
  const Index n_anch = t_anchor_ref.size();
  if (t_anchor_tgt.size() != n_anch)
    detail_tv::Fail("参照と目標で時間アンカー数が違います");
  if (t_frame_ref.size() < 2)
    detail_tv::Fail("参照のフレーム数が足りません");

  // ---- 時間軸 (.m 15-24 行) ----
  // 終端は length(span)/fs ではなく最終フレーム時刻を使う点に注意
  VectorXd ta_ref(n_anch + 2), ta_tgt(n_anch + 2);
  ta_ref[0] = 0.0;
  ta_tgt[0] = 0.0;
  ta_ref.segment(1, n_anch) = t_anchor_ref;
  ta_tgt.segment(1, n_anch) = t_anchor_tgt;
  ta_ref[n_anch + 1] = t_frame_ref[t_frame_ref.size() - 1];
  ta_tgt[n_anch + 1] = t_frame_tgt[t_frame_tgt.size() - 1];

  const VectorXd d_ref = Diff(ta_ref), d_tgt = Diff(ta_tgt);
  VectorXd ta_mrph(n_anch + 2);
  ta_mrph[0] = 0.0;
  double acc = 0.0;
  for (Index k = 0; k < n_anch + 1; ++k) {
    acc += RealGeoMix(d_ref[k], d_tgt[k], m.tx);
    ta_mrph[k + 1] = acc;
  }
  if (!(ta_mrph[n_anch + 1] > 0.0))
    detail_tv::Fail("モーフィング後の総時間長が正になりません");

  const double delta_t = t_frame_ref[1] - t_frame_ref[0];
  const Index n_frame =
      static_cast<Index>(std::floor(ta_mrph[n_anch + 1] / delta_t + 1e-9)) + 1;
  VectorXd t_morph(n_frame);
  for (Index k = 0; k < n_frame; ++k) t_morph[k] = static_cast<double>(k) * delta_t;

  VectorXd t_on_ref, t_on_tgt;
  Interp1(ta_mrph, ta_ref, t_morph, &t_on_ref);
  Interp1(ta_mrph, ta_tgt, t_morph, &t_on_tgt);

  // ---- F0 (.m 33-37 行) ----
  // f0_original を使う。線形補間したあと幾何平均で混ぜる
  const VectorXd& f0_ref =
      ref.f0_original.size() ? ref.f0_original : ref.source_parameter.f0;
  const VectorXd& f0_tgt =
      tgt.f0_original.size() ? tgt.f0_original : tgt.source_parameter.f0;

  VectorXd f0_ref_on, f0_tgt_on;
  Interp1(t_frame_ref, f0_ref, t_on_ref, &f0_ref_on);
  Interp1(t_frame_tgt, f0_tgt, t_on_tgt, &f0_tgt_on);

  VectorXd f0(n_frame);
  for (Index k = 0; k < n_frame; ++k)
    f0[k] = std::exp((1.0 - m.fo) * std::log(f0_ref_on[k]) +
                     m.fo * std::log(f0_tgt_on[k]));

  // ---- VUV (.m 39-45 行) ----
  VectorXd vuv_ref_on, vuv_tgt_on;
  Interp1(t_frame_ref, ref.source_parameter.vuv, t_on_ref, &vuv_ref_on);
  Interp1(t_frame_tgt, tgt.source_parameter.vuv, t_on_tgt, &vuv_tgt_on);

  VectorXd vuv(n_frame);
  for (Index k = 0; k < n_frame; ++k) {
    const double v = (1.0 - m.fo) * vuv_ref_on[k] + m.fo * vuv_tgt_on[k];
    vuv[k] = (v > 0.3) ? 1.0 : 0.0;
  }
  if (opt.force_all_voiced) vuv.setOnes();  // .m 45 行

  // ---- スペクトル (.m 57-63 行) ----
  const double fs = ref.spectrum_parameter.fs > 0 ? ref.spectrum_parameter.fs
                                                  : ref.sampling_frequency;
  const Index n_fbin = ref.spectrum_parameter.spectrogram.rows();
  if (tgt.spectrum_parameter.spectrogram.rows() != n_fbin)
    detail_tv::Fail("参照と目標で周波数ビン数が違います");

  // 第 0 ビンは 0 Hz のまま。generalizedTCmorphing のような半分ずらしは無い
  VectorXd fx_ref(n_fbin);
  for (Index b = 0; b < n_fbin; ++b)
    fx_ref[b] = static_cast<double>(b) / static_cast<double>(n_fbin) * fs / 2.0;

  const double vtl = opt.vtl_ratio;
  const VectorXd fx_morph_r = fx_ref / ((1.0 - m.fx) + m.fx * vtl);
  const VectorXd fx_morph_t = fx_ref / ((1.0 - m.fx) + m.fx / vtl);

  // ---- 周波数写像 (.m 65-92 行) ----
  MatrixXd map_ref(n_fbin, n_anch + 2), map_tgt(n_fbin, n_anch + 2);
  map_ref.col(0) = fx_morph_r;
  map_ref.col(n_anch + 1) = fx_morph_r;
  map_tgt.col(0) = fx_morph_t;
  map_tgt.col(n_anch + 1) = fx_morph_t;

  for (Index ii = 0; ii < n_anch; ++ii) {
    // 本数は参照側だけで決める（MATLAB 版と同じ。目標側は同数と仮定）
    Index nfc = 0;
    for (Index kk = 0; kk < tf_anchor_ref.rows(); ++kk)
      if (tf_anchor_ref(kk, ii) > 0.0) ++nfc;

    if (nfc == 0) {
      map_ref.col(ii + 1) = fx_morph_r;
      map_tgt.col(ii + 1) = fx_morph_t;
      continue;
    }

    VectorXd lr(nfc + 2), lt(nfc + 2);
    lr[0] = 0.0;  // log(1)
    lt[0] = 0.0;
    for (Index kk = 0; kk < nfc; ++kk) {
      lr[kk + 1] = std::log(tf_anchor_ref(kk, ii));
      lt[kk + 1] = std::log(tf_anchor_tgt(kk, ii));
    }
    lr[nfc + 1] = std::log(fs / 2.0);
    lt[nfc + 1] = std::log(fs / 2.0 / vtl);

    // log 区間長の「幾何平均」。逆転していると複素対数になり、
    // 実部を取ると 0 に潰れる（RealGeoMix のコメント参照）
    const VectorXd dlr = Diff(lr), dlt = Diff(lt);
    VectorXd f_anc(nfc + 2);
    double a = 0.0;
    f_anc[0] = 1.0;  // exp(0)
    for (Index kk = 0; kk < nfc + 1; ++kk) {
      a += RealGeoMix(dlr[kk], dlt[kk], m.fx);
      f_anc[kk + 1] = std::exp(a);
    }

    VectorXd xr = lr.array().exp(), xt = lt.array().exp();
    VectorXd tmp;
    bool unsorted = false;
    Interp1(xr, f_anc, fx_ref, &tmp, &unsorted);
    if (unsorted) ++out.n_unsorted_interp;
    map_ref.col(ii + 1) = tmp;
    Interp1(xt, f_anc, fx_ref, &tmp, &unsorted);
    if (unsorted) ++out.n_unsorted_interp;
    map_tgt.col(ii + 1) = tmp;
  }

  if (out.n_unsorted_interp > 0)
    out.warnings.push_back(
        "周波数アンカーの逆転により補間格子が非単調になった箇所が " +
        std::to_string(out.n_unsorted_interp) +
        " 件あります。MATLAB の interp1 は格子をソートして処理するため"
        "本移植も同じ扱いにしていますが、MATLAB の版によっては例外になる"
        "可能性があります");

  // ---- 時間方向の整列 (.m 116-124 行) ----
  MatrixXd sg_ref = ref.spectrum_parameter.spectrogram.array().abs().max(1e-300);
  MatrixXd sg_tgt = tgt.spectrum_parameter.spectrogram.array().abs().max(1e-300);
  sg_ref = 10.0 * sg_ref.array().log10();   // dB 領域
  sg_tgt = 10.0 * sg_tgt.array().log10();
  // 最終ビン（Nyquist）を直前のビンで上書きする
  sg_ref.row(n_fbin - 1) = sg_ref.row(n_fbin - 2);
  sg_tgt.row(n_fbin - 1) = sg_tgt.row(n_fbin - 2);

  MatrixXd ap_ref =
      ref.source_parameter.aperiodicity.array().abs().max(1e-300).log();
  MatrixXd ap_tgt =
      tgt.source_parameter.aperiodicity.array().abs().max(1e-300).log();

  const MatrixXd sg_ref_on = InterpTime(t_frame_ref, sg_ref, t_on_ref);
  const MatrixXd sg_tgt_on = InterpTime(t_frame_tgt, sg_tgt, t_on_tgt);
  const MatrixXd ap_ref_on = InterpTime(t_frame_ref, ap_ref, t_on_ref);
  const MatrixXd ap_tgt_on = InterpTime(t_frame_tgt, ap_tgt, t_on_tgt);

  // ---- 区間ごとの周波数ワープと混合 (.m 128-164 行) ----
  MatrixXd sg_mix = MatrixXd::Zero(n_fbin, n_frame);
  MatrixXd ap_mix = MatrixXd::Zero(n_fbin, n_frame);

  // safeguard: 0.0001 刻みのランプを x に足して強制的に単調化する (.m 135 行)
  VectorXd safeguard(n_fbin);
  for (Index b = 0; b < n_fbin; ++b) safeguard[b] = 0.0001 * (b + 1);

  std::vector<Index> idx_on_morph(static_cast<std::size_t>(n_anch + 2));
  for (Index k = 0; k < n_anch + 2; ++k)
    idx_on_morph[k] = std::min<Index>(
        n_frame - 1, static_cast<Index>(std::floor(ta_mrph[k] / delta_t)));

  VectorXd cur_ref(n_fbin), cur_tgt(n_fbin), xq(n_fbin), tmp(n_fbin);
  std::vector<char> written(static_cast<std::size_t>(n_frame), 0);

  for (Index ii = 0; ii <= n_anch; ++ii) {
    const Index strt = idx_on_morph[ii];
    // MATLAB の `if ii == nAnch` は off-by-one。既定では再現する
    const bool last_case = opt.fix_segment_end_index ? (ii == n_anch)
                                                     : (ii == n_anch - 1);
    const Index end = last_case ? (n_frame - 1) : idx_on_morph[ii + 1];
    if (end < strt) continue;

    const double denom = t_morph[end] - t_morph[strt];
    for (Index jj = strt; jj <= end; ++jj) {
      const double frac = (denom > 0.0) ? (t_morph[jj] - t_morph[strt]) / denom : 0.0;
      cur_ref = (1.0 - frac) * map_ref.col(ii) + frac * map_ref.col(ii + 1);
      cur_tgt = (1.0 - frac) * map_tgt.col(ii) + frac * map_tgt.col(ii + 1);

      bool bad = false;
      xq = cur_ref + safeguard;
      for (Index b = 1; b < n_fbin; ++b)
        if (!(xq[b] > xq[b - 1])) { bad = true; break; }
      if (!bad) {
        xq = cur_tgt + safeguard;
        for (Index b = 1; b < n_fbin; ++b)
          if (!(xq[b] > xq[b - 1])) { bad = true; break; }
      }
      // MATLAB は interp1 の失敗を空の catch で握りつぶすため、
      // そのフレームは 0 のまま残る（後段で 0 dB 平坦 + 完全非周期になる）
      if (bad && opt.emulate_catch_dropout) {
        ++out.n_dropped_frames;
        continue;
      }

      VectorXd x1 = cur_ref + safeguard;
      Interp1(x1, sg_ref_on.col(jj), fx_ref, &tmp);
      sg_mix.col(jj) = (1.0 - m.sl) * tmp;
      Interp1(x1, ap_ref_on.col(jj), fx_ref, &tmp);
      ap_mix.col(jj) = (1.0 - m.ap) * tmp;

      VectorXd x2 = cur_tgt + safeguard;
      Interp1(x2, sg_tgt_on.col(jj), fx_ref, &tmp);
      sg_mix.col(jj) += m.sl * tmp;
      Interp1(x2, ap_tgt_on.col(jj), fx_ref, &tmp);
      ap_mix.col(jj) += m.ap * tmp;

      written[static_cast<std::size_t>(jj)] = 1;
    }
  }

  int n_unwritten = 0;
  for (Index k = 0; k < n_frame; ++k) if (!written[k]) ++n_unwritten;
  if (n_unwritten > 0)
    out.warnings.push_back(
        "どの区間にも書き込まれなかったフレームが " + std::to_string(n_unwritten) +
        " 件あります（0 dB 平坦・完全非周期になります）。"
        "130 行の off-by-one に由来する可能性があります。"
        "Options::fix_segment_end_index を true にすると挙動が変わります");
  if (out.n_dropped_frames > 0)
    out.warnings.push_back(
        "補間格子が単調化できず欠落したフレームが " +
        std::to_string(out.n_dropped_frames) + " 件あります（MATLAB の空 catch 相当）");

  // ---- 線形領域へ戻す (.m 166-168 行) ----
  MatrixXd spectrogram = (sg_mix.array() / 10.0).unaryExpr(
      [](double v) { return std::pow(10.0, v); });
  MatrixXd aperiodicity = ap_mix.array().exp();
  out.aperiodicity_unshaped = aperiodicity;

  // ---- F0 の穴埋め (.m 171-172 行) ----
  double min_f0 = std::numeric_limits<double>::infinity();
  for (Index k = 0; k < n_frame; ++k)
    if (f0[k] > 30.0) min_f0 = std::min(min_f0, f0[k]);
  if (!std::isfinite(min_f0))
    detail_tv::Fail("30 Hz を超える F0 が 1 つもありません");
  for (Index k = 0; k < n_frame; ++k)
    if (!(f0[k] > 0.0) || std::isnan(f0[k])) f0[k] = min_f0;

  // ---- 非周期性の低域整形 (.m 173-181 行) ----
  if (opt.apply_aperiodicity_shaping) {
    const double avg_f0 = f0.mean();
    Index n_low = 0;
    while (n_low < n_fbin && fx_ref[n_low] < 3.0 * avg_f0) ++n_low;

    VectorXd shaper(n_low);
    for (Index b = 0; b < n_low; ++b) {
      double s = 0.5 * std::cos((fx_ref[b] - avg_f0) / avg_f0 / 2.0 * detail_tv::kPi)
                 + 0.5;
      if (fx_ref[b] <= avg_f0) s = 1.0;
      shaper[b] = std::pow(10.0, -60.0 * s / 10.0);
    }
    for (Index k = 0; k < n_frame; ++k)
      aperiodicity.col(k).head(n_low).array() *= shaper.array();
  }

  // ---- 出力 (.m 183-194 行) ----
  out.fs = ref.sampling_frequency;
  out.temporal_positions = t_morph;
  out.f0_original = f0;
  out.f0 = f0.array() * vuv.array();
  out.vuv = vuv;
  out.spectrogram = std::move(spectrogram);
  out.aperiodicity = std::move(aperiodicity);
  out.elapsed_time =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return out;
}

}  // namespace aligner
}  // namespace tcmorph

#endif  // TCMORPH_WORD_TV2W_MORPHING_HPP_
