#pragma once

/// @file analysis.hpp
/// @brief Offline spectral-envelope analysis of an audio file using WORLD.

#include <string>
#include <vector>

/// Spectral-envelope spectrogram, laid out ready for ImPlot::PlotHeatmap.
///
/// `values` is row-major with `num_bins` rows and `num_frames` columns.
/// Row 0 corresponds to the HIGHEST frequency bin so that, drawn top-to-bottom,
/// low frequencies end up at the bottom of the plot. Cells hold the envelope
/// magnitude in decibels.
struct Spectrogram {
    int num_frames = 0;   ///< time axis length
    int num_bins = 0;     ///< frequency axis length (fft_size/2 + 1)
    int fs = 0;           ///< sampling frequency [Hz]
    double duration = 0;  ///< signal duration [s]
    double db_min = 0;    ///< minimum magnitude in `values` [dB]
    double db_max = 0;    ///< maximum magnitude in `values` [dB]
    std::vector<float> values;  ///< num_bins * num_frames, row-major, [dB]

    bool empty() const { return num_frames == 0 || num_bins == 0; }
};

/// Decode `path`, estimate F0 (Harvest) and the spectral envelope (CheapTrick),
/// and return a heatmap-ready spectrogram.
///
/// @throws std::runtime_error (incl. ma::error) on decode/analysis failure.
Spectrogram analyze_file(const std::string& path);
