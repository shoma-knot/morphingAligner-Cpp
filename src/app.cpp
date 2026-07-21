#include "app.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>

namespace {

// Bake the dB spectrogram into an RGBA OpenGL texture once. Drawing it then
// costs a single textured quad per frame instead of one CPU-rebuilt cell per
// (bin * frame), which is what made the ImPlot heatmap crawl on large inputs.
unsigned int make_spectrogram_texture(const Spectrogram& sp) {
    // 256-entry colour lookup table sampled from the colormap.
    unsigned char lut[256][4];
    for (int i = 0; i < 256; ++i) {
        const ImVec4 c = ImPlot::SampleColormap(i / 255.0f, kColormap);
        lut[i][0]      = static_cast<unsigned char>(c.x * 255.0f);
        lut[i][1]      = static_cast<unsigned char>(c.y * 255.0f);
        lut[i][2]      = static_cast<unsigned char>(c.z * 255.0f);
        lut[i][3]      = 255;
    }

    const double               range = sp.db_max > sp.db_min ? sp.db_max - sp.db_min : 1.0;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(sp.num_bins) * sp.num_frames * 4);
    for (std::size_t k = 0; k < sp.values.size(); ++k) {
        const double   t  = std::clamp((sp.values[k] - sp.db_min) / range, 0.0, 1.0);
        const int      li = static_cast<int>(t * 255.0);
        unsigned char* px = &pixels[k * 4];
        px[0]             = lut[li][0];
        px[1]             = lut[li][1];
        px[2]             = lut[li][2];
        px[3]             = lut[li][3];
    }

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Texture width = frames (time / x), height = bins (frequency / y).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sp.num_frames, sp.num_bins, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

}    // namespace

Track::~Track() {
    if (tex) glDeleteTextures(1, &tex);
}

bool load_track_from_path(Track& tr, const std::string& path) {
    if (tr.tex) {
        glDeleteTextures(1, &tr.tex);
        tr.tex = 0;
    }
    try {
        tr.spec   = analyze_file(path);
        tr.tex    = make_spectrogram_texture(tr.spec);
        tr.y_min  = 0.0;                  // reset the frequency-axis view
        tr.y_max  = tr.spec.fs / 2.0;
        tr.path   = path;
        tr.status = "読み込み完了";
        return true;
    } catch (const std::exception& e) {
        tr.spec = {};
        tr.path.clear();
        tr.status = std::string { "読み込み失敗: " } + e.what();
        return false;
    }
}

void load_track(Track& tr) {
    static const char* filters[] = { "*.wav", "*.flac", "*.mp3", "*.ogg" };
    const std::string  title     = tr.name + " 音声を選択";
    const char*        picked    = tinyfd_openFileDialog(title.c_str(), "", 4, filters, "音声ファイル", 0);
    if (!picked) return;    // cancelled
    load_track_from_path(tr, picked);
}
