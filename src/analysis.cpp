#include "analysis.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <miniaudio_cpp/audio.hpp>

#include "world/cheaptrick.h"
#include "world/harvest.h"

namespace {

// Frame shift for the analysis [ms]. 5 ms is WORLD's usual default.
constexpr double kFramePeriod = 5.0;

// Downmix interleaved float PCM to a mono double signal, as WORLD expects.
std::vector<double> to_mono_double(const ma::decoder& dec) {
    const float* samples = dec.data();
    const auto frames = static_cast<std::size_t>(dec.frame_count());
    const auto channels = static_cast<std::size_t>(dec.channels());

    std::vector<double> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        double acc = 0.0;
        for (std::size_t c = 0; c < channels; ++c)
            acc += samples[i * channels + c];
        mono[i] = acc / static_cast<double>(channels);
    }
    return mono;
}

}  // namespace

Spectrogram analyze_file(const std::string& path) {
    ma::decoder dec{path};

    const int fs = static_cast<int>(dec.sample_rate());
    const std::vector<double> x = to_mono_double(dec);
    const int x_length = static_cast<int>(x.size());
    if (x_length == 0)
        throw std::runtime_error("analyze_file: empty signal");

    // ── F0 estimation (Harvest) ─────────────────────────────────
    HarvestOption h_opt;
    InitializeHarvestOption(&h_opt);
    h_opt.frame_period = kFramePeriod;

    const int f0_length = GetSamplesForHarvest(fs, x_length, kFramePeriod);
    std::vector<double> temporal_positions(f0_length);
    std::vector<double> f0(f0_length);
    Harvest(x.data(), x_length, fs, &h_opt, temporal_positions.data(),
            f0.data());

    // ── Spectral envelope (CheapTrick) ──────────────────────────
    CheapTrickOption c_opt;
    InitializeCheapTrickOption(fs, &c_opt);
    const int fft_size = c_opt.fft_size;
    const int num_bins = fft_size / 2 + 1;

    // WORLD wants an array of row pointers, one per analysis frame.
    std::vector<std::vector<double>> storage(f0_length,
                                             std::vector<double>(num_bins));
    std::vector<double*> spectrogram(f0_length);
    for (int t = 0; t < f0_length; ++t)
        spectrogram[t] = storage[t].data();

    CheapTrick(x.data(), x_length, fs, temporal_positions.data(), f0.data(),
               f0_length, &c_opt, spectrogram.data());

    // ── Pack into a heatmap-ready dB image ──────────────────────
    Spectrogram out;
    out.num_frames = f0_length;
    out.num_bins = num_bins;
    out.fs = fs;
    out.duration = static_cast<double>(x_length) / fs;
    out.values.resize(static_cast<std::size_t>(num_bins) * f0_length);

    constexpr double kFloor = 1e-12;  // avoids log10(0)
    double db_min = 1e30, db_max = -1e30;
    for (int t = 0; t < f0_length; ++t) {
        for (int b = 0; b < num_bins; ++b) {
            // CheapTrick returns a power spectrum -> 10*log10 for dB.
            const double db = 10.0 * std::log10(std::max(storage[t][b], kFloor));
            // Row 0 = highest frequency bin so low freqs sit at the bottom.
            const int row = num_bins - 1 - b;
            out.values[static_cast<std::size_t>(row) * f0_length + t] =
                static_cast<float>(db);
            db_min = std::min(db_min, db);
            db_max = std::max(db_max, db);
        }
    }
    out.db_min = db_min;
    out.db_max = db_max;
    return out;
}
