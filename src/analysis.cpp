#include "analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <miniaudio_cpp/audio.hpp>

#include "world/cheaptrick.h"
#include "world/d4c.h"
#include "world/harvest.h"

namespace {

using Eigen::MatrixXd;
using Eigen::VectorXd;

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

}    // namespace

Result<MorphChannel> analyze_channel(const std::string& path) {
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
        h_opt.frame_period = kFramePeriodMs;
        const int f0_len   = GetSamplesForHarvest(fs, x_length, kFramePeriodMs);

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
        c.frame_period = kFramePeriodMs;
        c.duration     = static_cast<double>(x_length) / fs;
        return Result<MorphChannel>::success(std::move(c));
    } catch (const std::exception& e) {
        return Result<MorphChannel>::failure(e.what());
    }
}

Spectrogram summarize_spectrogram(const MorphChannel& c) {
    Spectrogram s;
    if (c.empty()) return s;
    s.num_frames = c.n_frames;
    s.num_bins   = c.nbin;
    s.fs         = c.fs;
    s.duration   = c.duration;
    // sp はパワースペクトルなので 10*log10 で dB。log は単調なので、最小/最大の係数から範囲が決まる。
    constexpr double kFloor = 1e-12;    // log10(0) を避ける
    s.db_min                = 10.0 * std::log10(std::max(c.sp().minCoeff(), kFloor));
    s.db_max                = 10.0 * std::log10(std::max(c.sp().maxCoeff(), kFloor));
    return s;
}

Result<AnalyzedAudio> analyze_file(const std::string& path) {
    Result<MorphChannel> c = analyze_channel(path);
    if (!c.ok()) return Result<AnalyzedAudio>::failure(std::move(c.error));
    AnalyzedAudio a;
    a.spec    = summarize_spectrogram(c.value);
    a.channel = std::make_shared<const MorphChannel>(std::move(c.value));
    return Result<AnalyzedAudio>::success(std::move(a));
}
