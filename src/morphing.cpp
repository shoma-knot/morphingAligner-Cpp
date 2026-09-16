#include "morphing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <miniaudio_cpp/audio.hpp>

#include <tcmorph/word_tv2w_morphing.hpp>

#include "world/cheaptrick.h"
#include "world/d4c.h"
#include "world/harvest.h"
#include "world/synthesis.h"

#include "app.hpp"    // Anchor / FreqAnchor

namespace {

using Eigen::MatrixXd;
using Eigen::VectorXd;

constexpr double kFramePeriod = 5.0;    // ms（WORLD の既定。tcmorph はフレーム間隔を入力から拾う）

// interleaved float PCM をモノラル double へ。
std::vector<double> to_mono(const ma::decoder& dec) {
    const float* s        = dec.data();
    const auto   frames   = static_cast<std::size_t>(dec.frame_count());
    const auto   channels = static_cast<std::size_t>(dec.channels());
    std::vector<double> x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        double acc = 0.0;
        for (std::size_t c = 0; c < channels; ++c) acc += s[i * channels + c];
        x[i] = acc / static_cast<double>(channels);
    }
    return x;
}

// ── アンカーの整形 ───────────────────────────────────────────
// tcmorph が要求する形（時間アンカーは狭義単調増加、周波数アンカーは (本数, 時間
// アンカー数) の 0 詰め行列）へ変換する。両端の境界アンカーはエンジン側が付けるので
// ここでは入れない。
struct AnchorMatrices {
    VectorXd t_ref, t_tgt;      // [n_anch]
    MatrixXd tf_ref, tf_tgt;    // (n_fanchor, n_anch)、余りは 0 詰め
    int      dropped_time = 0;  // 範囲外/順序が逆で読み飛ばした時間アンカー数
    int      dropped_freq = 0;  // 同・周波数アンカー数
};

AnchorMatrices build_anchors(const std::vector<Anchor>& anchors, double end_ref, double end_tgt,
                             double nyquist) {
    constexpr double kEps   = 1e-6;    // 区間長 0 を避けるための最小間隔 [s]
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

}    // namespace

MorphChannel analyze_channel(const std::string& path, std::string& err) {
    err.clear();
    try {
        ma::decoder dec { path };

        const int                 fs       = static_cast<int>(dec.sample_rate());
        const std::vector<double> x        = to_mono(dec);
        const int                 x_length = static_cast<int>(x.size());
        if (x_length == 0) throw std::runtime_error("empty signal: " + path);

        MorphChannel             c;
        tcmorph::WorldParameter& w = c.world;
        w.sampling_frequency       = fs;
        w.span_length              = static_cast<std::size_t>(x_length);

        // ── F0 (Harvest) ──
        HarvestOption h_opt;
        InitializeHarvestOption(&h_opt);
        h_opt.frame_period = kFramePeriod;
        const int f0_len   = GetSamplesForHarvest(fs, x_length, kFramePeriod);

        VectorXd& t  = w.source_parameter.temporal_positions;
        VectorXd& f0 = w.source_parameter.f0;
        t.resize(f0_len);
        f0.resize(f0_len);
        Harvest(x.data(), x_length, fs, &h_opt, t.data(), f0.data());

        // WORLD は無声フレームの F0 を 0 にするので、そこから VUV を作る。
        w.source_parameter.vuv = (f0.array() > 0.0).cast<double>().matrix();

        // ── スペクトル包絡 (CheapTrick) / 非周期性 (D4C) ──
        CheapTrickOption c_opt;
        InitializeCheapTrickOption(fs, &c_opt);
        D4COption d_opt;
        InitializeD4COption(&d_opt);
        const int fft_size = c_opt.fft_size;
        const int nbin     = fft_size / 2 + 1;

        // tcmorph は (nbin, n_frames) の列優先なので、各列の先頭ポインタをそのまま
        // WORLD に渡せる（フレームごとのコピーが不要）。
        MatrixXd& sp = w.spectrum_parameter.spectrogram;
        MatrixXd& ap = w.source_parameter.aperiodicity;
        sp.resize(nbin, f0_len);
        ap.resize(nbin, f0_len);
        std::vector<double*> sp_ptr(f0_len), ap_ptr(f0_len);
        for (int i = 0; i < f0_len; ++i) {
            sp_ptr[i] = sp.col(i).data();
            ap_ptr[i] = ap.col(i).data();
        }
        CheapTrick(x.data(), x_length, fs, t.data(), f0.data(), f0_len, &c_opt, sp_ptr.data());
        D4C(x.data(), x_length, fs, t.data(), f0.data(), f0_len, fft_size, &d_opt, ap_ptr.data());

        w.spectrum_parameter.temporal_positions = t;
        w.spectrum_parameter.fs                 = fs;
        // f0_original は空のまま（tcmorph が source_parameter.f0 で代用する）。
        // MATLAB 版の f0_original は GUI で編集する前の F0 で、本アプリには編集機能がない。

        c.fs           = fs;
        c.fft_size     = fft_size;
        c.nbin         = nbin;
        c.n_frames     = f0_len;
        c.frame_period = kFramePeriod;
        c.duration     = static_cast<double>(x_length) / fs;
        return c;
    } catch (const std::exception& e) {
        err = e.what();
        return {};
    }
}

MorphOutput morphing_channels(const MorphChannel& B, const MorphChannel& T,
                              const std::vector<Anchor>& anchors, const MorphRates& rates) {
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
          build_anchors(anchors, tb[tb.size() - 1], tt[tt.size() - 1], nyquist);

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

        R.warnings = std::move(o.warnings);
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
        c.frame_period  = kFramePeriod;
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
        Synthesis(c.f0().data(), M, spp.data(), app.data(), fft_size, kFramePeriod, fs, y_length,
                  y.data());

        R.wave = std::move(y);
        R.fs   = fs;
    } catch (const std::exception& e) {
        R.error = std::string { "モーフィング失敗: " } + e.what();
    }
    return R;
}

bool write_wav(const std::string& path, const std::vector<double>& wave, int fs, std::string& err) {
    try {
        std::vector<float> f(wave.size());
        for (std::size_t i = 0; i < wave.size(); ++i)
            f[i] = static_cast<float>(std::clamp(wave[i], -1.0, 1.0));
        ma::write_wav(path, f.data(), static_cast<std::uint64_t>(f.size()), /*channels=*/1,
                      static_cast<std::uint32_t>(fs));
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}
