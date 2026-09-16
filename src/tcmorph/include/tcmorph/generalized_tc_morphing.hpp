// tcmorph/generalized_tc_morphing.hpp
//
// Hideki Kawahara 氏の worldGUItools/src/generalizedTCmorphing.m (Apache-2.0) の
// C++17 移植版（Eigen 3.4 + OpenMP）。
//
// "TC" = Temporally Constant（時間的に一定な重み）。N 個の音声素材を
//     時間軸 (tx) / 周波数軸 (fx) / 基本周波数 (fo) /
//     スペクトル包絡レベル (sl) / 非周期性指標 (ap)
// の 5 系統の独立した重みでモーフィングする。
//
// ビルド例:
//   g++ -std=c++17 -O3 -march=native -fopenmp -I/usr/include/eigen3 ...
//
// ---------------------------------------------------------------------------
// 行列の向きについて
// ---------------------------------------------------------------------------
// スペクトログラムは MatrixXd(n_fbin, n_frame)、すなわち MATLAB 版と同じ
// (周波数ビン, フレーム) の向き。Eigen の既定が列優先なので、この向きだと
// 「1 フレーム分のスペクトル = 1 列 = 連続メモリ」になる。
//
// 本実装の主要な処理は
//     * 時間方向の整列  -> 隣接 2 列の線形結合
//     * 周波数方向のワープ -> 1 列の中で完結
// といずれも列の中で閉じるため、列優先 × この向きが最適な組み合わせになる。
//
// 依存なし版 (generalized_tc_morphing.hpp) が [フレーム][ビン] だったのは
// std::vector<std::vector<double>> が行優先相当だったためで、
// 「1 フレームを連続に置く」という意図自体は同じ。
//
// ---------------------------------------------------------------------------
// MATLAB 版からの変更点
// ---------------------------------------------------------------------------
//  1. fs / nFbin / nFreqVector をループ変数の残留値ではなく素材 0 から明示取得し、
//     全素材の整合性を検証して不一致なら例外を送出する
//  2. 最終フレームの取りこぼし (.m 234 行の厳密不等号) を修正
//  3. VUV 閾値の食い違い (.m 104 行 0.99 / 271 行 0.995) をオプションで明示分離

#ifndef TCMORPH_GENERALIZED_TC_MORPHING_HPP_
#define TCMORPH_GENERALIZED_TC_MORPHING_HPP_

#include <Eigen/Core>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace tcmorph {

using Eigen::ArrayXd;
using Eigen::ArrayXi;
using Eigen::Index;
using Eigen::MatrixXd;
using Eigen::VectorXd;

// ===========================================================================
// データ構造
// ===========================================================================

/// WORLD の音源パラメータ
struct SourceParameter {
  VectorXd temporal_positions;  ///< フレーム時刻 [s]
  VectorXd f0;                  ///< 基本周波数 [Hz]、無声は 0
  VectorXd vuv;                 ///< 1: 有声 / 0: 無声
  MatrixXd aperiodicity;        ///< (n_fbin, n_frame) 非周期性指標 (0, 1]
};

/// WORLD のスペクトルパラメータ
struct SpectrumParameter {
  VectorXd temporal_positions;  ///< フレーム時刻 [s]
  MatrixXd spectrogram;         ///< (n_fbin, n_frame) パワースペクトル包絡
  double fs = 0.0;              ///< 標本化周波数 [Hz]
};

/// WORLD 分析結果一式
struct WorldParameter {
  double sampling_frequency = 0.0;
  std::size_t span_length = 0;  ///< 分析対象波形のサンプル数
  SourceParameter source_parameter;
  SpectrumParameter spectrum_parameter;

  /// 編集前の F0。wordTV2WmorphingEngineRev はこちらを使う
  /// （generalizedTCmorphing は source_parameter.f0 を使う）。
  /// 空なら source_parameter.f0 で代用する
  VectorXd f0_original;

  /// 総時間長 [s]（.m の 60 行 endTime に対応）
  double duration() const {
    return static_cast<double>(span_length) / sampling_frequency;
  }
};

/// モーフィング素材 1 個分（MATLAB 版 morphStr(ii) に対応）
struct MorphObject {
  WorldParameter world_parameter;
  VectorXd time_anchor;       ///< 時間アンカー [s]、単調増加
  MatrixXd time_freq_anchor;  ///< (max_n_fanchor, n_tanchor) 周波数アンカー [Hz]
  ///< 列 jj が time_anchor(jj) における周波数アンカー（MATLAB 版と同じ向き）。
  ///< 本数が時刻によって異なる場合は末尾を 0 で詰める
  ///< （0 の個数で本数を判定するため、有効値に 0 Hz は使えない）
};

/// 5 系統の重み。いずれも長さ n_obj で、和が 1 になるようにすること。
/// 対数領域での重み付き和を使うため、和が 1 でないとスケールが狂う。
struct MorphWeights {
  VectorXd tx;  ///< 時間軸
  VectorXd fx;  ///< 周波数軸
  VectorXd fo;  ///< 基本周波数（VUV にも同じ重みが使われる）
  VectorXd sl;  ///< スペクトル包絡レベル
  VectorXd ap;  ///< 非周期性指標
};

struct MorphOptions {
  double frame_period = 0.005;          ///< 出力フレーム周期 [s]
  double vuv_threshold = 0.99;          ///< 有声と判定する混合 VUV の下限
  double vuv_output_threshold = 0.995;  ///< 出力 vuv を 1 にする下限
  double ap_floor = 1e-5;               ///< 非周期性指標の下限クリップ値

  /// モーフィング後の周波数アンカーが逆転したときに、最小限ずらして続行するか。
  /// false にすると例外を投げる。素材ごとのアンカーの逆転自体は
  /// （interp1 の y 側に入るだけなので）ここでは問題にならない
  bool repair_nonmonotonic_frequency = true;
};

struct MorphOutput {
  SourceParameter source_parameter;
  SpectrumParameter spectrum_parameter;
  double elapsed_time = 0.0;  ///< 処理時間 [s]
  std::vector<std::string> warnings;  ///< 計算は続行したが注意が要る点

  // --- 以下はデバッグ・解析用 ---
  MatrixXd morphed_sgram_wo_fmod;  ///< 周波数ワープを適用しない版（比較用）
  VectorXd morphed_tanchor;        ///< モーフ後の時間アンカー [s]
  MatrixXd morphed_tf_anchor;      ///< (max_n_fanchor, n_tanchor) [Hz]
  std::vector<MatrixXd> freq_axis_on_obj;  ///< 素材ごとの周波数写像
};

// ===========================================================================
// 内部ユーティリティ
// ===========================================================================
namespace detail {

inline void Require(bool cond, const std::string& msg) {
  if (!cond) throw std::invalid_argument("tcmorph: " + msg);
}

/// xq が属する区間の左端インデックスを返す。
/// 範囲外は端の区間に丸められ、結果として線形外挿になる
/// （MATLAB の interp1(..., "linear", "extrap") と同じ挙動）。
inline Index LocateSegment(const VectorXd& x, double xq) {
  const Index n = x.size();
  const double* first = x.data();
  const double* it = std::upper_bound(first, first + n, xq);
  const Index idx = static_cast<Index>(it - first);
  if (idx == 0) return 0;
  if (idx >= n) return n - 2;
  return idx - 1;
}

/// スカラー版の線形補間（外挿あり）。x は単調増加であること。
inline double Interp1(const VectorXd& x, const VectorXd& y, double xq) {
  const Index i = LocateSegment(x, xq);
  const double t = (xq - x[i]) / (x[i + 1] - x[i]);
  return (1.0 - t) * y[i] + t * y[i + 1];
}

/// 補間の「インデックス + 重み」だけを保持する構造体。
///
/// 依存なし版では周波数リサンプルのたびに区間探索をやり直していたが、
/// スペクトル包絡と非周期性は同じワープ関数を共有するので、
/// 探索結果を 1 度だけ作って 2 回使えば探索コストが半分になる。
struct GatherMap {
  ArrayXi idx;   ///< 参照する左端ビン
  ArrayXd frac;  ///< 左端からの内分比 [0, 1]

  void resize(Index n) {
    idx.resize(n);
    frac.resize(n);
  }
};

/// 補間の「インデックス + 重み」を構築する。O(n + m)（単調な問い合わせ列の場合）。
///
/// 問い合わせ列 xq は単調でなくてもよい。周波数アンカーが逆転していると
/// ワープ関数が折り返して非単調になるが、MATLAB の interp1 は各点を独立に
/// 探索するのでそれを許容する。ここでも走査ポインタを前進だけでなく後退も
/// させることで同じ挙動にしてある（前進のみだと黙って誤った値を返す）。
/// x のほうは単調増加であること。
inline void BuildGatherMap(const VectorXd& x, const VectorXd& xq, GatherMap* g) {
  const Index n = x.size();
  const Index m = xq.size();
  g->resize(m);
  Index i = 0;
  for (Index k = 0; k < m; ++k) {
    const double q = xq[k];
    while (i + 2 < n && q >= x[i + 1]) ++i;   // 前進
    while (i > 0 && q < x[i]) --i;            // 後退（非単調な xq のため）
    g->idx[k] = static_cast<int>(i);
    g->frac[k] = (q - x[i]) / (x[i + 1] - x[i]);
  }
}

/// 構築済みマップを使って補間を適用する。
/// Eigen 3.4 のインデックス付きビューでギャザーを表現している。
inline void ApplyGather(const GatherMap& g, const VectorXd& y, VectorXd* out) {
  *out = ((1.0 - g.frac) * y(g.idx).array() + g.frac * y(g.idx + 1).array()).matrix();
}

/// log(0) = -inf を許容しつつ対数を取る
inline double SafeLog(double v) {
  return v > 0.0 ? std::log(v) : -std::numeric_limits<double>::infinity();
}

}  // namespace detail

// ===========================================================================
// ステップ 1: 時間軸のモーフィング (.m の 52-81 行)
// ===========================================================================
//
// アンカー間の区間長を対数領域で重み付き平均する。
// 区間長の「重み付き幾何平均」になる。話速比は乗法的に効くので、
// 線形平均ではなく対数領域で混ぜるのが要点。
inline void MorphTimeAxis(const std::vector<MorphObject>& objs, const VectorXd& w_tx,
                          VectorXd* morphed_tanchor, MatrixXd* extended_tanchor) {
  const Index n_obj = static_cast<Index>(objs.size());
  const Index n_tanchor = objs[0].time_anchor.size();

  // (区間, 素材) の区間長。区間数が n_tanchor+1 なのは、
  // 最終アンカーから発話末尾までの区間を含むため
  MatrixXd seg_length(n_tanchor + 1, n_obj);
  for (Index ii = 0; ii < n_obj; ++ii) {
    const VectorXd& a = objs[ii].time_anchor;
    seg_length(0, ii) = a[0];
    for (Index jj = 1; jj < n_tanchor; ++jj) seg_length(jj, ii) = a[jj] - a[jj - 1];
    seg_length(n_tanchor, ii) =
        objs[ii].world_parameter.duration() - a[n_tanchor - 1];

    detail::Require((seg_length.col(ii).array() > 0.0).all(),
                    "素材 " + std::to_string(ii) +
                        " の時間アンカー区間長が 0 以下です。"
                        "単調増加か、発話長を超えていないか確認してください");
  }

  // 対数区間長 → 重み付き和 → 指数 → 累積 (.m の 68-71 行)
  const VectorXd morphed_slope =
      (seg_length.array().log().matrix() * w_tx).array().exp();

  morphed_tanchor->resize(n_tanchor + 1);
  double acc = 0.0;
  for (Index jj = 0; jj <= n_tanchor; ++jj) {
    acc += morphed_slope[jj];
    (*morphed_tanchor)[jj] = acc;
  }

  // 各素材の累積アンカー時刻
  extended_tanchor->resize(n_tanchor + 1, n_obj);
  for (Index ii = 0; ii < n_obj; ++ii) {
    double a = 0.0;
    for (Index jj = 0; jj <= n_tanchor; ++jj) {
      a += seg_length(jj, ii);
      (*extended_tanchor)(jj, ii) = a;
    }
  }
}

// ===========================================================================
// ステップ 3: 周波数軸のモーフィング (.m の 121-190 行)
// ===========================================================================
//
// 時間アンカーごとに周波数ワープ関数を作る。ここで参照するのは
// 周波数アンカーの「位置」だけで、スペクトルの「値」は一切見ない。
// 画像モーフィングでいうワープ場の設計に相当する。
inline void MorphFrequencyAxis(const std::vector<MorphObject>& objs,
                               const VectorXd& w_fx, double fs, Index n_fbin,
                               Index n_tanchor, VectorXd* freq_axis_on_morph,
                               std::vector<MatrixXd>* freq_axis_on_obj,
                               MatrixXd* morphed_tf_anchor,
                               bool repair = true,
                               std::vector<std::string>* warnings = nullptr) {
  const Index n_obj = static_cast<Index>(objs.size());
  const double nyquist_log = std::log(fs / 2.0);

  // モーフ後側の周波数軸。第 0 ビンは log を取るため 0 Hz を避けて半分にずらす
  *freq_axis_on_morph =
      VectorXd::LinSpaced(n_fbin, 0.0, static_cast<double>(n_fbin - 1)) / n_fbin *
      (fs / 2.0);
  (*freq_axis_on_morph)[0] = (*freq_axis_on_morph)[1] / 2.0;
  const VectorXd log_fx_axis = freq_axis_on_morph->array().log();

  // 各時間アンカーでの周波数アンカー本数。ゼロ詰めの個数から判定 (.m の 140 行)。
  // MATLAB 版は最後の素材の値が残留して使われていたため、
  // ここでは素材 0 を基準にし、他の素材との一致を検証する
  std::vector<Index> n_freq_vec(static_cast<std::size_t>(n_tanchor), 0);
  auto count_anchors = [](const MatrixXd& m, Index jj) {
    return (m.col(jj).array() != 0.0).count();
  };
  for (Index jj = 0; jj < n_tanchor; ++jj)
    n_freq_vec[jj] = count_anchors(objs[0].time_freq_anchor, jj);
  for (Index ii = 1; ii < n_obj; ++ii)
    for (Index jj = 0; jj < n_tanchor; ++jj)
      detail::Require(count_anchors(objs[ii].time_freq_anchor, jj) == n_freq_vec[jj],
                      "素材 " + std::to_string(ii) +
                          " の周波数アンカー本数が素材 0 と一致しません。"
                          "本実装は全素材で本数が揃っていることを前提とします");

  Index max_n_fanchor = 0;
  for (Index jj = 0; jj < n_tanchor; ++jj)
    max_n_fanchor = std::max(max_n_fanchor, n_freq_vec[jj]);

  freq_axis_on_obj->assign(static_cast<std::size_t>(n_obj),
                           MatrixXd::Zero(n_fbin, n_tanchor));
  *morphed_tf_anchor = MatrixXd::Zero(std::max<Index>(max_n_fanchor, 1), n_tanchor);

  detail::GatherMap gmap;
  VectorXd tmp;
  for (Index jj = 0; jj < n_tanchor; ++jj) {
    const Index n_freq = n_freq_vec[jj];

    if (n_freq == 0) {
      // この時刻にはアンカーが無いので周波数ワープは恒等写像
      for (Index ii = 0; ii < n_obj; ++ii)
        (*freq_axis_on_obj)[ii].col(jj) = *freq_axis_on_morph;
      continue;
    }

    // 対数周波数軸を [0, a1, ..., log(fs/2)] に分割した区間長を
    // 重み付けして素材間で足し合わせ、累積してモーフ後のアンカー位置にする。
    // 先頭の基準 0 は log(1 Hz) に相当する
    VectorXd slope = VectorXd::Zero(n_freq + 1);
    for (Index ii = 0; ii < n_obj; ++ii) {
      double last = 0.0;
      for (Index kk = 0; kk < n_freq; ++kk) {
        const double lf = std::log(objs[ii].time_freq_anchor(kk, jj));
        slope[kk] += (lf - last) * w_fx[ii];
        last = lf;
      }
      slope[n_freq] += (nyquist_log - last) * w_fx[ii];
    }

    VectorXd morphed_log_fanchor(n_freq + 1);
    double acc = 0.0;
    for (Index kk = 0; kk <= n_freq; ++kk) {
      acc += slope[kk];
      morphed_log_fanchor[kk] = acc;
    }
    morphed_log_fanchor[n_freq] = nyquist_log;  // 最高周波数は常に固定端

    // ここが本当に単調性を要求する箇所。素材ごとのアンカーは後で interp1 の
    // y 側に入るので逆転していても構わないが、モーフィング後のアンカーは
    // x 側になるため、逆転すると補間そのものが定義できなくなる。
    // 素材間の重み付き和なので、片方の逆転は打ち消されることが多い。
    // 打ち消されずに残った場合は、止めるよりも最小限ずらして続行するほうが
    // 実用的なので、前向き・後ろ向きの 2 パスで押し広げて警告を出す
    {
      constexpr double kEps = 1e-6;   // 対数周波数での最小間隔
      bool repaired = false;
      double worst_lo = 0.0, worst_hi = 0.0;
      for (Index kk = 1; kk <= n_freq; ++kk)
        if (!(morphed_log_fanchor[kk] > morphed_log_fanchor[kk - 1])) {
          if (!repaired) {
            worst_lo = morphed_log_fanchor[kk - 1];
            worst_hi = morphed_log_fanchor[kk];
          }
          repaired = true;
        }

      if (repaired) {
        detail::Require(repair,
            "時間アンカー " + std::to_string(jj) +
            " におけるモーフィング後の周波数アンカーが単調増加になりません（" +
            std::to_string(std::exp(worst_lo)) + " Hz -> " +
            std::to_string(std::exp(worst_hi)) + " Hz）");

        // 前向きに最小間隔を確保してから、最高周波数の固定端を超えないよう
        // 後ろ向きに押し戻す
        for (Index kk = 1; kk < n_freq; ++kk)
          if (morphed_log_fanchor[kk] < morphed_log_fanchor[kk - 1] + kEps)
            morphed_log_fanchor[kk] = morphed_log_fanchor[kk - 1] + kEps;
        for (Index kk = n_freq - 1; kk >= 0; --kk)
          if (morphed_log_fanchor[kk] > morphed_log_fanchor[kk + 1] - kEps)
            morphed_log_fanchor[kk] = morphed_log_fanchor[kk + 1] - kEps;

        detail::Require(morphed_log_fanchor[0] > 0.0,
            "時間アンカー " + std::to_string(jj) +
            " の周波数アンカーが補正しきれません。アンカーの本数に対して"
            "帯域が狭すぎる可能性があります");

        if (warnings)
          warnings->push_back(
              "時間アンカー " + std::to_string(jj) +
              " でモーフィング後の周波数アンカーが逆転したため補正しました（" +
              std::to_string(std::exp(worst_lo)) + " Hz -> " +
              std::to_string(std::exp(worst_hi)) +
              " Hz）。この時刻の結果は MATLAB 版と違う可能性があります");
      }
    }

    for (Index kk = 0; kk < n_freq; ++kk)
      (*morphed_tf_anchor)(kk, jj) = std::exp(morphed_log_fanchor[kk]);

    // 各素材への逆写像を構築。
    // x = モーフ後のアンカー位置、y = 素材側のアンカー位置 とした折れ線。
    // 向きは「モーフ後のビン -> 素材側の周波数」(backward mapping)
    VectorXd x_break(n_freq + 2);
    x_break[0] = 0.0;
    x_break.tail(n_freq + 1) = morphed_log_fanchor;
    detail::BuildGatherMap(x_break, log_fx_axis, &gmap);

    VectorXd y_break(n_freq + 2);
    for (Index ii = 0; ii < n_obj; ++ii) {
      y_break[0] = 0.0;
      for (Index kk = 0; kk < n_freq; ++kk)
        y_break[kk + 1] = std::log(objs[ii].time_freq_anchor(kk, jj));
      y_break[n_freq + 1] = nyquist_log;

      // x_break は素材によらず共通なのでマップを使い回せる
      detail::ApplyGather(gmap, y_break, &tmp);
      (*freq_axis_on_obj)[ii].col(jj) = tmp.array().exp();
    }
  }
}

// ===========================================================================
// 本体
// ===========================================================================
inline MorphOutput GeneralizedTCMorphing(const std::vector<MorphObject>& objs,
                                         const MorphWeights& weights,
                                         const MorphOptions& opt = {}) {
  const auto start = std::chrono::steady_clock::now();
  const Index n_obj = static_cast<Index>(objs.size());
  detail::Require(n_obj > 0, "素材が 1 つもありません");

  // ---- 重みの検証 ----
  for (const auto& [name, w] : std::vector<std::pair<const char*, const VectorXd*>>{
           {"tx", &weights.tx}, {"fx", &weights.fx}, {"fo", &weights.fo},
           {"sl", &weights.sl}, {"ap", &weights.ap}}) {
    detail::Require(w->size() == n_obj,
                    std::string("weights.") + name + " の長さが素材数と一致しません");
    // 対数領域で混ぜるため、和が 1 でないとスケールが狂う
    detail::Require(std::abs(w->sum() - 1.0) < 1e-6,
                    std::string("weights.") + name + " の和が 1 ではありません");
  }

  // ---- 素材間の整合性を検証（MATLAB 版では暗黙の前提だった部分） ----
  const Index n_tanchor = objs[0].time_anchor.size();
  const double fs = objs[0].world_parameter.sampling_frequency;
  const Index n_fbin = objs[0].world_parameter.spectrum_parameter.spectrogram.rows();
  for (Index ii = 1; ii < n_obj; ++ii) {
    const auto& wp = objs[ii].world_parameter;
    detail::Require(objs[ii].time_anchor.size() == n_tanchor,
                    "素材 " + std::to_string(ii) + " の時間アンカー数が異なります");
    detail::Require(wp.sampling_frequency == fs,
                    "素材 " + std::to_string(ii) + " の標本化周波数が異なります");
    detail::Require(wp.spectrum_parameter.spectrogram.rows() == n_fbin,
                    "素材 " + std::to_string(ii) + " の周波数ビン数が異なります");
  }

  // ---- ステップ 1: 時間軸 ----
  VectorXd morphed_tanchor;
  MatrixXd extended_tanchor;
  MorphTimeAxis(objs, weights.tx, &morphed_tanchor, &extended_tanchor);

  // モーフ後の時間軸。浮動小数点誤差で末尾を取りこぼさないよう個数を先に決める
  const Index n_frame = static_cast<Index>(std::floor(
                            morphed_tanchor[n_tanchor] / opt.frame_period + 1e-6)) + 1;
  const VectorXd morphed_t =
      VectorXd::LinSpaced(n_frame, 0.0, static_cast<double>(n_frame - 1)) *
      opt.frame_period;

  // ---- ステップ 2: F0 / VUV (.m の 83-104 行) ----
  // 「モーフ後の時刻 -> 素材 ii 上の時刻」の写像。アンカー同士の対応を
  // 折れ線で結んだ区分線形の時間伸縮関数。後段でも再利用する
  MatrixXd time_map(n_frame, n_obj);
  VectorXd morphed_fo = VectorXd::Zero(n_frame);
  VectorXd morphed_vuv = VectorXd::Zero(n_frame);

  for (Index ii = 0; ii < n_obj; ++ii) {
    const auto& sp = objs[ii].world_parameter.source_parameter;
    const VectorXd ext = extended_tanchor.col(ii);
    for (Index n = 0; n < n_frame; ++n) {
      const double tm = detail::Interp1(morphed_tanchor, ext, morphed_t[n]);
      time_map(n, ii) = tm;

      // F0 は「時間方向の補間は対数領域、素材間の混合は線形 Hz 領域」。
      // exp を取ってから重み付き和を取る順序に注意 (.m の 96-100 行)
      const Index k = detail::LocateSegment(sp.temporal_positions, tm);
      const double t = (tm - sp.temporal_positions[k]) /
                       (sp.temporal_positions[k + 1] - sp.temporal_positions[k]);
      double v = std::exp((1.0 - t) * detail::SafeLog(sp.f0[k]) +
                          t * detail::SafeLog(sp.f0[k + 1]));
      // 無声区間 (f0 = 0) の log は -inf なので、隣接補間で NaN が生じうる
      if (!std::isfinite(v)) v = 0.0;

      morphed_fo[n] += v * weights.fo[ii];
      // VUV にも fo の重みを流用（原典通り）
      morphed_vuv[n] +=
          ((1.0 - t) * sp.vuv[k] + t * sp.vuv[k + 1]) * weights.fo[ii];
    }
  }
  // どれか 1 つでも無声寄りなら無声にする（閾値がほぼ 1 のため実質 AND 条件）
  for (Index n = 0; n < n_frame; ++n)
    if (morphed_vuv[n] < opt.vuv_threshold) morphed_fo[n] = 0.0;

  // ---- ステップ 3: 周波数軸（座標系の生成） ----
  VectorXd freq_axis_on_morph;
  std::vector<MatrixXd> freq_axis_on_obj;
  MatrixXd morphed_tf_anchor;
  std::vector<std::string> warnings;
  MorphFrequencyAxis(objs, weights.fx, fs, n_fbin, n_tanchor, &freq_axis_on_morph,
                     &freq_axis_on_obj, &morphed_tf_anchor,
                     opt.repair_nonmonotonic_frequency, &warnings);

  // ---- ステップ 4 の下準備 ----
  //
  // 対数は素材ごとに 1 回だけ取って使い回す。Eigen の配列演算なので
  // -O3 -march=native ならベクトル化された exp/log が使われる
  std::vector<MatrixXd> log_sgram(static_cast<std::size_t>(n_obj));
  std::vector<MatrixXd> log_ap(static_cast<std::size_t>(n_obj));
  std::vector<ArrayXi> t_idx(static_cast<std::size_t>(n_obj));
  std::vector<ArrayXd> t_frac(static_cast<std::size_t>(n_obj));

  for (Index ii = 0; ii < n_obj; ++ii) {
    const auto& wp = objs[ii].world_parameter;
    log_sgram[ii] = wp.spectrum_parameter.spectrogram.array().max(1e-300).log();
    log_ap[ii] =
        wp.source_parameter.aperiodicity.array().max(opt.ap_floor).min(1.0).log();

    // 時間整列に使う「左隣のフレーム番号と内分比」を先に求めておく
    const VectorXd& tp = wp.spectrum_parameter.temporal_positions;
    t_idx[ii].resize(n_frame);
    t_frac[ii].resize(n_frame);
    for (Index n = 0; n < n_frame; ++n) {
      const Index k = detail::LocateSegment(tp, time_map(n, ii));
      t_idx[ii][n] = static_cast<int>(k);
      t_frac[ii][n] = (time_map(n, ii) - tp[k]) / (tp[k + 1] - tp[k]);
    }
  }

  // 周波数ワープの区間番号と内分比はフレームだけで決まり、素材に依存しない
  ArrayXi seg_idx(n_frame);
  ArrayXd seg_lambda(n_frame);
  for (Index n = 0; n < n_frame; ++n) {
    Index jj = 0;
    while (jj < n_tanchor && morphed_t[n] >= morphed_tanchor[jj]) ++jj;
    const double last_time = (jj == 0) ? 0.0 : morphed_tanchor[jj - 1];
    seg_idx[n] = static_cast<int>(jj);
    seg_lambda[n] = (morphed_t[n] - last_time) / (morphed_tanchor[jj] - last_time);
  }

  // ---- ステップ 4: スペクトログラム / 非周期性（値の混合） ----
  //
  // 対数振幅での重み付き和 = dB 軸上の線形補間 = 幾何平均。
  // 周波数軸を先に揃えてから混ぜることで、フォルマントのピークが
  // 「平均されて鈍る」のを防いでいる。
  //
  // フレームを外側ループにしてあるので、各フレームの計算は完全に独立になる。
  // 依存なし版（素材が外側）ではこの並列化ができなかった。
  MatrixXd morphed_sgram(n_fbin, n_frame);
  MatrixXd morphed_ap(n_fbin, n_frame);
  MatrixXd morphed_sgram_wo_fmod(n_fbin, n_frame);

#ifdef _OPENMP
#pragma omp parallel
#endif
  {
    // スクラッチ領域はスレッドごとに確保する
    VectorXd aligned_sgram(n_fbin), aligned_ap(n_fbin);
    VectorXd current_axis(n_fbin), tmp_sgram(n_fbin), tmp_ap(n_fbin);
    detail::GatherMap gmap;

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
    for (Index n = 0; n < n_frame; ++n) {
      morphed_sgram.col(n).setZero();
      morphed_ap.col(n).setZero();
      morphed_sgram_wo_fmod.col(n).setZero();

      const Index jj = seg_idx[n];
      const double lambda = seg_lambda[n];

      for (Index ii = 0; ii < n_obj; ++ii) {
        // --- (a) 時間方向の整列: 隣接 2 列の線形結合 ---
        const Index k = t_idx[ii][n];
        const double t = t_frac[ii][n];
        aligned_sgram = (1.0 - t) * log_sgram[ii].col(k) + t * log_sgram[ii].col(k + 1);
        aligned_ap = (1.0 - t) * log_ap[ii].col(k) + t * log_ap[ii].col(k + 1);

        // --- (b) このフレームの周波数ワープ関数を作る (.m の 227-252 行) ---
        //
        // ワープ関数は時間アンカー上にしか定義されていないので、
        // 隣接アンカー間でワープ関数そのものを線形補間して引き伸ばす。
        // 端点の扱い: 最初のアンカーより前と最後のアンカーより後では
        // 恒等写像に向かってフェードする（原典通り）。アンカーを発話の
        // 内側に狭く打つと端で意図しない変形が起きる点に注意
        // 三項演算子で const VectorXd& を受けると型が揃わず一時オブジェクトの
        // コピーが発生するため、生ポインタから Map でゼロコピーに束縛する。
        // 列優先なので .col() の中身は連続しており Map で安全に包める
        const double* last_ptr = (jj == 0) ? freq_axis_on_morph.data()
                                           : freq_axis_on_obj[ii].col(jj - 1).data();
        const double* next_ptr = (jj < n_tanchor) ? freq_axis_on_obj[ii].col(jj).data()
                                                  : freq_axis_on_morph.data();
        const Eigen::Map<const VectorXd> last_axis(last_ptr, n_fbin);
        const Eigen::Map<const VectorXd> next_axis(next_ptr, n_fbin);
        current_axis = (1.0 - lambda) * last_axis + lambda * next_axis;

        // --- (c) 周波数方向のリサンプルと素材間の混合（対数領域） ---
        // 包絡と非周期性は同じワープを共有するので、探索は 1 回で済む
        detail::BuildGatherMap(freq_axis_on_morph, current_axis, &gmap);
        detail::ApplyGather(gmap, aligned_sgram, &tmp_sgram);
        detail::ApplyGather(gmap, aligned_ap, &tmp_ap);

        morphed_sgram.col(n) += weights.sl[ii] * tmp_sgram;
        morphed_ap.col(n) += weights.ap[ii] * tmp_ap;
        morphed_sgram_wo_fmod.col(n) += weights.sl[ii] * aligned_sgram;
      }
    }
  }

  morphed_sgram = morphed_sgram.array().exp();
  morphed_ap = morphed_ap.array().exp();
  morphed_sgram_wo_fmod = morphed_sgram_wo_fmod.array().exp();

  // ---- 出力 VUV の二値化 (.m の 271-272 行) ----
  VectorXd out_vuv =
      (morphed_vuv.array() >= opt.vuv_output_threshold).cast<double>();

  MorphOutput out;
  out.warnings = std::move(warnings);
  out.source_parameter.temporal_positions = morphed_t;
  out.source_parameter.f0 = std::move(morphed_fo);
  out.source_parameter.vuv = std::move(out_vuv);
  out.source_parameter.aperiodicity = std::move(morphed_ap);
  out.spectrum_parameter.temporal_positions = morphed_t;
  out.spectrum_parameter.spectrogram = std::move(morphed_sgram);
  out.spectrum_parameter.fs = fs;
  out.morphed_sgram_wo_fmod = std::move(morphed_sgram_wo_fmod);
  out.morphed_tanchor = std::move(morphed_tanchor);
  out.morphed_tf_anchor = std::move(morphed_tf_anchor);
  out.freq_axis_on_obj = std::move(freq_axis_on_obj);
  out.elapsed_time =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return out;
}

}  // namespace tcmorph

#endif  // TCMORPH_GENERALIZED_TC_MORPHING_HPP_
