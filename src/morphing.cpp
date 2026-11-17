#include "morphing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <miniaudio_cpp/audio.hpp>    // ma::write_wav

#include <tcmorph/word_tv2w_morphing.hpp>

#include "world/synthesis.h"

#include "anchor.hpp"
#include "envelope_transport.hpp"

using Eigen::MatrixXd;
using Eigen::VectorXd;

namespace {

// 凹凸を測って戻す周波数範囲 [Hz]（フォルマントの帯域）。全帯域で1つの倍率にすると、フォルマントの
// 無い高域の平らになり方に引っぱられて、フォルマントの帯域を戻しすぎた（hai の組で目標の 1.16 倍）。
constexpr double kContrastLoHz = 300.0;
constexpr double kContrastHiHz = 5500.0;

// 戻す対象にする構造の細かさの下限 [Hz]。周波数方向の周期がこれより短い成分は触らない
// （フォルマントの構造だけを対象にし、包絡に残る F0 由来の細かい波打ちを強めない）。
constexpr double kContrastMinPeriodHz = 300.0;

// 1フレームに掛ける倍率の上限（凹凸がほとんど無いフレームを極端に強めないため）。
constexpr double kContrastMaxGain = 2.0;

// 凹凸を測る範囲と、その内側の点に対する DST-I（正規直交）の基底 k = 2..K。
// 範囲の dB 包絡から両端を結ぶ直線を引いたものを DST で表すと、基底はどれも両端で 0 になる
// ので、係数を変えても範囲の両端の値は変わらず、範囲の外とつながったままになる（DCT のまま
// 範囲を切ると端に段差ができる）。直線は大まかな傾き、k = 1 は範囲全体のゆるい膨らみなので触らない。
struct ContrastBasis {
    Eigen::Index first = 0, count = 0;    // 範囲の最初の bin と点数（両端を含む）
    MatrixXd     dst;                     // (K-1, count-2)

    bool ok() const { return dst.rows() > 0; }
};

ContrastBasis contrast_basis(Eigen::Index nbin, int fs) {
    ContrastBasis b;
    if (nbin < 4 || fs <= 0) return b;
    const double       df    = (fs / 2.0) / static_cast<double>(nbin - 1);
    const Eigen::Index first = static_cast<Eigen::Index>(std::ceil(kContrastLoHz / df));
    const Eigen::Index last  = std::min<Eigen::Index>(nbin - 1, static_cast<Eigen::Index>(std::floor(kContrastHiHz / df)));
    const Eigen::Index n     = last - first - 1;    // 内側の点の数
    // 基底 k の周期は 2 * (範囲の幅) / k [Hz]。
    const int K = std::min<int>(static_cast<int>(n), static_cast<int>(std::floor(2.0 * (last - first) * df / kContrastMinPeriodHz)));
    if (n < 4 || K < 2) return b;
    b.first = first;
    b.count = last - first + 1;
    b.dst.resize(K - 1, n);
    const double scale = std::sqrt(2.0 / static_cast<double>(n + 1));
    for (int k = 2; k <= K; ++k)
        for (Eigen::Index i = 1; i <= n; ++i)
            b.dst(k - 2, i - 1) = scale * std::sin(3.14159265358979323846 * k * static_cast<double>(i) / (n + 1));
    return b;
}

// 線形パワーの1フレームを dB にして、範囲の両端を結ぶ直線を引いた内側の点を返す。
VectorXd detrended_db(const Eigen::Ref<const VectorXd>& power, const ContrastBasis& b) {
    const VectorXd db   = 10.0 * power.segment(b.first, b.count).array().max(1e-300).log10();
    const Eigen::Index m = b.count - 1;
    VectorXd       out(b.count - 2);
    for (Eigen::Index i = 1; i < m; ++i)
        out[i - 1] = db[i] - (db[0] + (db[m] - db[0]) * static_cast<double>(i) / static_cast<double>(m));
    return out;
}

}    // namespace

double envelope_contrast(const Eigen::VectorXd& power, int fs) {
    const ContrastBasis b = contrast_basis(power.size(), fs);
    return b.ok() ? (b.dst * detrended_db(power, b)).norm() : 0.0;
}

double restore_envelope_contrast(MatrixXd& mix, const MatrixXd& ref, const MatrixXd& tgt, double sl, int fs) {
    const Eigen::Index n = mix.rows(), nf = mix.cols();
    if (ref.rows() != n || tgt.rows() != n || ref.cols() != nf || tgt.cols() != nf) return 1.0;
    const ContrastBasis b = contrast_basis(n, fs);
    if (!b.ok()) return 1.0;

    double gain_sum = 0.0;
    int    n_gain   = 0;
    for (Eigen::Index j = 0; j < nf; ++j) {
        const VectorXd y_mix = detrended_db(mix.col(j), b);
        const VectorXd c_mix = b.dst * y_mix;
        const double   have  = c_mix.norm();
        if (have < 1e-9) continue;    // 平坦なフレーム（tcmorph が欠落させたフレームなど）
        // 凹凸の強さ（dB の係数の大きさ）は、両端の値を sl で線形に補間したものを目標にする。
        const double want = (1.0 - sl) * (b.dst * detrended_db(ref.col(j), b)).norm()
                          + sl * (b.dst * detrended_db(tgt.col(j), b)).norm();
        const double g = std::clamp(want / have, 1.0, kContrastMaxGain);    // 弱めることはしない
        gain_sum += g;
        ++n_gain;
        if (g == 1.0) continue;

        // 範囲の内側の dB に、強めた分（係数の (g-1) 倍）を足す。両端と範囲の外はそのまま。
        const VectorXd delta = b.dst.transpose() * (c_mix * (g - 1.0));
        const double   before = mix.col(j).sum();
        for (Eigen::Index i = 0; i < delta.size(); ++i) mix(b.first + 1 + i, j) *= std::pow(10.0, delta[i] / 10.0);
        mix.col(j) *= before / mix.col(j).sum();    // フレームのパワーの和は元のまま
    }
    return n_gain > 0 ? gain_sum / n_gain : 1.0;
}

// ── アンカーの整形 ───────────────────────────────────────────
AnchorMatrices build_anchor_matrices(const std::vector<Anchor>& anchors, double end_ref, double end_tgt,
                                     double nyquist) {
    // 区間長 0 を避けるための最小間隔 [s]。手で打つアンカーの重なり防止用なので小さく取る
    // （自動生成は auto_anchors.cpp の kEps = 1e-4 s でこれより広く空けている）。
    constexpr double kEps   = 1e-6;
    constexpr double kMinHz = 1.0;     // log を取るため 0Hz 付近は使えない

    AnchorMatrices m;

    std::vector<const Anchor*> sorted;
    sorted.reserve(anchors.size());
    for (const Anchor& a : anchors) sorted.push_back(&a);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Anchor* a, const Anchor* b) { return a->base_t < b->base_t; });

    // 両側とも解析範囲の内側にあり、base_t が直前と十分離れているものだけに絞る。
    // 範囲外・重複のアンカーは区間長 0 を作ってエンジンを壊すので必ず落とす。
    std::vector<const Anchor*> cand;
    double                     last_b = 0.0;
    for (const Anchor* a : sorted) {
        const bool in_range = a->base_t > kEps && a->base_t < end_ref - kEps && a->target_t > kEps
                           && a->target_t < end_tgt - kEps;
        if (!in_range || !(a->base_t > last_b + kEps)) {
            ++m.dropped_time;
            continue;
        }
        cand.push_back(a);
        last_b = a->base_t;
    }

    // target_t も狭義単調増加でなければならない（そうでないとモーフ後のタイムラインが
    // 後戻りし、どの区間にも書かれないフレーム＝広帯域ノイズになる）。base 側の線が
    // 交差していると逆順になり得るので、ここで「最も多く残せる部分列」を選ぶ。
    // 先頭から貪欲に落とすと、最初の1本が変なだけで以降が全滅するため DP で選ぶ。
    // アンカーは高々数十本なので O(n^2) で十分。
    const int                  nc = static_cast<int>(cand.size());
    std::vector<int>           len(static_cast<std::size_t>(nc), 1), prev(static_cast<std::size_t>(nc), -1);
    int                        best = -1;
    for (int i = 0; i < nc; ++i) {
        for (int j = 0; j < i; ++j)
            if (cand[j]->target_t + kEps < cand[i]->target_t && len[j] + 1 > len[i]) {
                len[i]  = len[j] + 1;
                prev[i] = j;
            }
        if (best < 0 || len[i] > len[best]) best = i;
    }
    std::vector<const Anchor*> kept;
    for (int i = best; i >= 0; i = prev[i]) kept.push_back(cand[i]);
    std::reverse(kept.begin(), kept.end());
    m.dropped_time += nc - static_cast<int>(kept.size());

    const int n = static_cast<int>(kept.size());
    m.t_ref.resize(n);
    m.t_tgt.resize(n);

    // 周波数アンカーは base_f 昇順に詰める（本数は時間アンカーごとに違ってよい）。
    // UI ではクリック順に並ぶだけで順序に意味がないので、ここで並べ替えて ref 側の
    // 補間格子を単調に保つ。tcmorph は逆転も「折り返すワープ」として通すが、それを
    // 期待しているのは MATLAB 由来のデータだけなので本アプリでは採らない。
    std::vector<std::vector<std::pair<double, double>>> fp(static_cast<std::size_t>(n));
    int max_nf = 0;
    for (int i = 0; i < n; ++i) {
        m.t_ref[i] = kept[i]->base_t;
        m.t_tgt[i] = kept[i]->target_t;
        auto& v    = fp[static_cast<std::size_t>(i)];
        for (const FreqAnchor& f : kept[i]->freqs) {
            if (f.base_f > kMinHz && f.base_f < nyquist && f.target_f > kMinHz && f.target_f < nyquist)
                v.emplace_back(f.base_f, f.target_f);
            else
                ++m.dropped_freq;
        }
        std::sort(v.begin(), v.end());
        max_nf = std::max(max_nf, static_cast<int>(v.size()));
    }

    m.tf_ref = MatrixXd::Zero(max_nf, n);
    m.tf_tgt = MatrixXd::Zero(max_nf, n);
    for (int i = 0; i < n; ++i) {
        const auto& v = fp[static_cast<std::size_t>(i)];
        for (int k = 0; k < static_cast<int>(v.size()); ++k) {
            m.tf_ref(k, i) = v[static_cast<std::size_t>(k)].first;
            m.tf_tgt(k, i) = v[static_cast<std::size_t>(k)].second;
        }
    }
    return m;
}

MorphOutput morphing_channels(const MorphChannel& B, const MorphChannel& T,
                              const std::vector<Anchor>& anchors, const MorphRates& rates,
                              const MorphPostOptions& post) {
    MorphOutput R;
    try {
        if (B.empty() || T.empty()) {
            R.error = "モーフィング失敗: base/target の解析データがありません";
            return R;
        }
        if (B.fs != T.fs) {
            R.error = "サンプリング周波数が異なります（リサンプリング未対応）";
            return R;
        }
        if (B.nbin != T.nbin) {
            R.error = "FFT サイズが一致しません";
            return R;
        }

        const int    fs       = B.fs;
        const int    fft_size = B.fft_size;
        const double nyquist  = fs / 2.0;

        // tcmorph（MATLAB 版）は発話の終端を length(span)/fs ではなく最終フレーム時刻で
        // 扱う。アンカーの範囲検査もそれに合わせる。
        const VectorXd& tb = B.world.source_parameter.temporal_positions;
        const VectorXd& tt = T.world.source_parameter.temporal_positions;
        const AnchorMatrices am =
          build_anchor_matrices(anchors, tb[tb.size() - 1], tt[tt.size() - 1], nyquist);

        tcmorph::aligner::MorphRate rate;
        rate.tx = std::clamp(rates.tx, 0.0, 1.0);
        rate.fx = std::clamp(rates.fx, 0.0, 1.0);
        rate.fo = std::clamp(rates.fo, 0.0, 1.0);
        rate.sl = std::clamp(rates.sl, 0.0, 1.0);
        rate.ap = std::clamp(rates.ap, 0.0, 1.0);

        // 既定のまま使う（MATLAB 版の挙動を再現する側。声道長比は本アプリでは 1 固定）。
        const tcmorph::aligner::Options opt;

        tcmorph::aligner::Output o = tcmorph::aligner::WordTV2WMorphing(
          B.world, T.world, am.t_ref, am.tf_ref, am.t_tgt, am.tf_tgt, rate, opt);

        // 包絡の混ぜ方を変える。sl = 0 / 1 で合成し直すと、混ぜる前の（同じ時間・周波数の格子に揃えた）
        // base / target の包絡がそのまま出る（ワープは tx / fx だけで決まり、sl は混ぜる重みにしか
        // 使われないため）。凹凸を戻すときはそれを目標に、OT で補間するときはそれを運ぶ元と先に使う。
        if (post.envelope != EnvelopeMix::Average && rate.sl > 0.0 && rate.sl < 1.0) {
            tcmorph::aligner::MorphRate r0 = rate, r1 = rate;
            r0.sl = 0.0;
            r1.sl = 1.0;
            const tcmorph::aligner::Output o0 =
              tcmorph::aligner::WordTV2WMorphing(B.world, T.world, am.t_ref, am.tf_ref, am.t_tgt, am.tf_tgt, r0, opt);
            const tcmorph::aligner::Output o1 =
              tcmorph::aligner::WordTV2WMorphing(B.world, T.world, am.t_ref, am.tf_ref, am.t_tgt, am.tf_tgt, r1, opt);
            if (o0.spectrogram.cols() != o.spectrogram.cols() || o1.spectrogram.cols() != o.spectrogram.cols()) {
                R.warnings.push_back("包絡の混ぜ方を変えられませんでした（base/target の格子のフレーム数が一致しません）");
            } else if (post.envelope == EnvelopeMix::AverageRestore) {
                R.contrast_gain = restore_envelope_contrast(o.spectrogram, o0.spectrogram, o1.spectrogram, rate.sl, fs);
            } else {
                // フレーム周期は tcmorph と同じく base の最初の2フレームの間隔。
                const double         frame_s = tb.size() >= 2 ? tb[1] - tb[0] : kFramePeriodMs / 1000.0;
                const TransportStats ts      = transport_envelopes(o.spectrogram, o0.spectrogram, o1.spectrogram, am,
                                                                   tb[tb.size() - 1], tt[tt.size() - 1], frame_s,
                                                                   MorphRates { rate.tx, rate.fx, rate.fo, rate.sl, rate.ap },
                                                                   fs, post.transport_gamma, post.transport_lo_hz,
                                                                   post.transport_hi_hz);
                R.transport_frames  = ts.frames;
                R.transport_bands   = ts.bands;
                R.transport_skipped = ts.skipped;
            }
        }

        // tcmorph の警告を先に並べる。代入で置き換えると、上で足した包絡の混ぜ方の警告が消える
        // （2026-11-17 に直した）。
        R.warnings.insert(R.warnings.begin(), std::make_move_iterator(o.warnings.begin()),
                          std::make_move_iterator(o.warnings.end()));
        if (am.dropped_time > 0)
            R.warnings.push_back("範囲外または順序が逆の時間アンカーを "
                                 + std::to_string(am.dropped_time) + " 個読み飛ばしました");
        if (am.dropped_freq > 0)
            R.warnings.push_back("範囲外の周波数アンカーを " + std::to_string(am.dropped_freq)
                                 + " 個読み飛ばしました");

        // ── morphed（表示用＋合成入力）──
        const int     M = static_cast<int>(o.f0.size());
        MorphChannel& c = R.morphed;
        c.fs            = fs;
        c.fft_size      = fft_size;
        c.nbin          = B.nbin;
        c.n_frames      = M;
        c.frame_period  = kFramePeriodMs;
        c.duration      = o.temporal_positions[M - 1];

        c.world.sampling_frequency                   = fs;
        c.world.source_parameter.temporal_positions  = o.temporal_positions;
        c.world.source_parameter.f0                  = std::move(o.f0);
        c.world.source_parameter.vuv                 = std::move(o.vuv);
        c.world.source_parameter.aperiodicity        = std::move(o.aperiodicity);
        c.world.spectrum_parameter.temporal_positions = std::move(o.temporal_positions);
        c.world.spectrum_parameter.spectrogram       = std::move(o.spectrogram);
        c.world.spectrum_parameter.fs                = fs;

        // ── WORLD 合成 ──
        // 末尾は最終フレーム時刻ちょうどで切る（MATLAB の Synthesis と同じ長さ。
        // フレーム数×フレーム周期にすると常に1フレーム分だけ長くなる）。
        const int y_length      = std::max(1, static_cast<int>(c.duration * fs));
        c.world.span_length     = static_cast<std::size_t>(y_length);
        std::vector<double> y(y_length, 0.0);

        std::vector<const double*> spp(M), app(M);
        for (int i = 0; i < M; ++i) {
            spp[i] = c.sp().col(i).data();
            app[i] = c.ap().col(i).data();
        }
        Synthesis(c.f0().data(), M, spp.data(), app.data(), fft_size, kFramePeriodMs, fs, y_length,
                  y.data());

        R.wave = std::move(y);
        R.fs   = fs;
    } catch (const std::exception& e) {
        R.error = std::string { "モーフィング失敗: " } + e.what();
    }
    return R;
}

Status write_wav(const std::string& path, const std::vector<double>& wave, int fs) {
    try {
        std::vector<float> f(wave.size());
        for (std::size_t i = 0; i < wave.size(); ++i)
            f[i] = static_cast<float>(std::clamp(wave[i], -1.0, 1.0));
        ma::write_wav(path, f.data(), static_cast<std::uint64_t>(f.size()), /*channels=*/1,
                      static_cast<std::uint32_t>(fs));
        return {};
    } catch (const std::exception& e) {
        return Status::failure(e.what());
    }
}
