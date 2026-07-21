#pragma once

/// @file app.hpp
/// @brief Application state model (tracks, anchors) and loading logic.

#include <string>
#include <utility>
#include <vector>

#include <implot.h>
#include <miniaudio_cpp/audio.hpp>

#include "analysis.hpp"

// Viridis, shared by the pre-baked spectrogram texture and the on-screen legend.
constexpr ImPlotColormap kColormap = ImPlotColormap_Viridis;

// One loaded audio source (base or target) with its analysis and GPU texture.
// Owns a GL texture, so it is non-copyable.
struct Track {
    std::string  name;       // "base" / "target", shown in dialog + labels
    std::string  path;       // loaded file ("" = none)
    std::string  status;     // last message for this track
    Spectrogram  spec;       // spectral envelope
    unsigned int tex = 0;    // baked GPU texture

    explicit Track(std::string n) : name(std::move(n)) {}
    ~Track();    // frees the GL texture (defined in app.cpp)
    Track(const Track&)            = delete;
    Track& operator=(const Track&) = delete;

    bool loaded() const { return !path.empty(); }
};

// A time-axis correspondence: one vertical anchor on each spectrogram.
// Created with both times equal; dragged apart later to define the mapping.
struct Anchor {
    double base_t;
    double target_t;
};

// Application state shared across the frame.
struct App {
    ma::engine          engine;    // audio output device (shared by both tracks)
    Track               base { "base" };
    Track               target { "target" };
    std::vector<Anchor> anchors;    // base<->target time correspondences
};

// Open a file dialog for `tr`, analyse it, and (re)build its GPU texture.
// On failure the track is reset and `tr.status` describes the error.
void load_track(Track& tr);
