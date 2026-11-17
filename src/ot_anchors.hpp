#pragma once

/// @file ot_anchors.hpp
/// @brief 1次元の最適輸送（OT）による周波数アンカーの生成（試作）。
///
/// 時間アンカーの時刻ごとに、base/target のスペクトル包絡を周波数上の「質量」の分布とみなし、
/// 同じ累積質量（分位点）に当たる周波数どうしを周波数アンカーとして対応させる。1次元では、
/// 最適輸送の写像は累積分布の合成 T = F_target^-1 ∘ F_base（単調な並べ替え）になり、凸な輸送
/// コストなら何を選んでも同じになる。結果を左右するのは「質量をどう定義するか」で、ここでは
/// パワーを gamma 乗で圧縮したものを、指定した周波数範囲の Hz 軸上の密度として使う。
///
/// 2つのやり方がある（OtMode）。
///   - 全帯域: 範囲全体で1回だけ対応させ、既存の周波数アンカーは捨てる。対応は量で決まるので、
///     フォルマントの強さの比が base と target で違うと山の対応がずれる（DEVLOG の検証を参照）。
///   - 帯ごと: 既存の周波数アンカー（フォルマントの対応など）で周波数軸を帯に区切り、対応する
///     帯の中だけで対応させて、帯の間に周波数アンカーを足す。山どうしの対応はアンカーで保ったまま、
///     その間の形だけを OT で補う。既存の周波数アンカーは残す。
/// 時間アンカーはどちらでもそのまま使う。GUI 非依存。

#include <string>
#include <vector>

#include "analysis.hpp"    // MorphChannel
#include "anchor.hpp"

// 周波数アンカーの作り方。
enum class OtMode {
    Global,    // 範囲全体で対応させ、既存の周波数アンカーを置き換える
    Bands,     // 既存の周波数アンカーの間の帯ごとに対応させ、帯の中に足す
};

// OT による周波数アンカーの設定。
struct OtAnchorParams {
    OtMode mode       = OtMode::Global;
    int    count      = 9;      // 全帯域: 時間アンカー1本あたりの本数（質量を count+1 等分する点）
    int    band_count = 2;      // 帯ごと: 帯1つあたりの本数（帯の質量を band_count+1 等分する点）
    double gamma      = 0.3;    // 質量 = パワー^gamma（小さいほど、弱い高域のフォルマントも効く）
    // 対象の周波数範囲（この外の質量は使わない）。下限は F0 の帯域（成人でおよそ 100〜300 Hz）を
    // 外す。含めると、話者の F0 の違いで低い分位点が半オクターブほど引きずられた（DEVLOG 参照）。
    // 帯ごとのときは、いちばん下といちばん上の帯の端にもなる。
    double min_hz = 300.0;
    double max_hz = 5500.0;

    static constexpr int    kCountMin = 1, kCountMax = 20;
    static constexpr int    kBandCountMin = 1, kBandCountMax = 10;
    static constexpr double kGammaMin = 0.05, kGammaMax = 1.0;
};

// 生成の結果。error が空なら成功（anchors を App::anchors と置き換える）。
struct OtAnchorResult {
    std::vector<Anchor>      anchors;       // 時間アンカーは入力のまま、周波数アンカーを作り直したもの
    int                      n_freq  = 0;   // OT で打った周波数アンカーの数（帯ごとのときは足した数）
    int                      n_edges = 0;   // 帯ごと: 帯の端に使った既存の周波数アンカーの数
    int                      skipped = 0;   // 無音に近く、OT をかけなかった時間アンカーの数
    int                      crossed = 0;   // 帯ごと: 既存の周波数アンカーが交差していて、かけなかった数
    std::vector<std::string> warnings;
    std::string              error;

    bool ok() const { return error.empty(); }
};

// 1フレーム分のパワースペクトル（nbin 点、0 Hz から fs/2 まで等間隔）の、[lo_hz, hi_hz] の
// 質量の分位点 q_j = j/(count+1)（j = 1..count）に当たる周波数 [Hz]（昇順）。hi_hz は fs/2 で
// 切り詰める。範囲が空、または質量が 0 のときは空を返す（単体テスト用に公開）。
std::vector<double> spectral_quantiles(const double* power, int nbin, int fs, double lo_hz, double hi_hz, int count,
                                       double gamma);

// 各時間アンカーの時刻で base/target の包絡の分位点を対応させ、周波数アンカーを作る（p.mode の
// とおり、置き換えるか帯の中に足すか）。無音に近い時刻（その音声で最も大きいフレームより大きく
// 下回る）には OT をかけない（全帯域なら周波数アンカーは空に、帯ごとなら元のまま）。
OtAnchorResult make_ot_freq_anchors(const MorphChannel& base, const MorphChannel& target,
                                    const std::vector<Anchor>& anchors, const OtAnchorParams& p);
