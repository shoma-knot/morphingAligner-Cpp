#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <tinyfiledialogs.h>

#include "freqscale.hpp"
#include "log.hpp"

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

    // 各行を ERB レートで等間隔にサンプルし直す（表示の周波数軸を ERB 尺度にする）。
    // 行 0 = 最高周波数（ERB 最大）。線形ビンの dB を Hz→ビンで補間して取得する。
    const int    H       = sp.num_bins;          // テクスチャ高さ（行数）は据え置き
    const int    W       = sp.num_frames;
    const double range   = sp.db_max > sp.db_min ? sp.db_max - sp.db_min : 1.0;
    const double nyquist = sp.fs / 2.0;
    const double erb_max = freqscale::hz_to_erb(nyquist);

    // 線形ビン b・フレーム t の dB。values は [行=num_bins-1-b][frame]（行0=最高周波数）。
    auto db_at = [&](int b, int t) -> double {
        b = std::clamp(b, 0, sp.num_bins - 1);
        return sp.values[static_cast<std::size_t>(sp.num_bins - 1 - b) * W + t];
    };

    std::vector<unsigned char> pixels(static_cast<std::size_t>(H) * W * 4);
    for (int r = 0; r < H; ++r) {
        const double erb  = erb_max * (1.0 - static_cast<double>(r) / (H - 1));    // 行0=最大ERB
        const double hz   = freqscale::erb_to_hz(erb);
        const double binf = hz / nyquist * (sp.num_bins - 1);
        const int    b0   = std::clamp(static_cast<int>(std::floor(binf)), 0, sp.num_bins - 1);
        const int    b1   = std::min(b0 + 1, sp.num_bins - 1);
        const double fr   = std::clamp(binf - b0, 0.0, 1.0);
        for (int t = 0; t < W; ++t) {
            const double db = db_at(b0, t) + (db_at(b1, t) - db_at(b0, t)) * fr;
            const double u  = std::clamp((db - sp.db_min) / range, 0.0, 1.0);
            const int    li = static_cast<int>(u * 255.0);
            unsigned char* px = &pixels[(static_cast<std::size_t>(r) * W + t) * 4];
            px[0]             = lut[li][0];
            px[1]             = lut[li][1];
            px[2]             = lut[li][2];
            px[3]             = lut[li][3];
        }
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
        tr.spec  = analyze_file(path);
        tr.tex   = make_spectrogram_texture(tr.spec);
        tr.y_min = 0.0;                  // reset the frequency-axis view (ERB レート)
        tr.y_max = freqscale::hz_to_erb(tr.spec.fs / 2.0);
        // ミニマップの枠を全体表示で初期化。
        tr.view_x0 = 0.0;
        tr.view_x1 = tr.spec.duration;
        tr.view_y0 = 0.0;
        tr.view_y1 = tr.y_max;
        tr.path    = path;
        applog::add(tr.name + " 読み込み完了: " + path);
        return true;
    } catch (const std::exception& e) {
        tr.spec = {};
        tr.path.clear();
        applog::add(tr.name + " 読み込み失敗: " + e.what());
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
