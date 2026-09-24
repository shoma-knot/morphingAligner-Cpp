#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <GLFW/glfw3.h>

#include "freqscale.hpp"
#include "log.hpp"


namespace {

// FIXME: Windowsでビルドが通らないので応急処置
#define GL_CLAMP_TO_EDGE 0x812F

// Bake the dB spectrogram into an RGBA OpenGL texture once. Drawing it then
// costs a single textured quad per frame instead of one CPU-rebuilt cell per
// (bin * frame), which is what made the ImPlot heatmap crawl on large inputs.
// カラーマップから 256 段の色 LUT を作る。
void build_lut(unsigned char lut[256][4]) {
    for (int i = 0; i < 256; ++i) {
        const ImVec4 c = ImPlot::SampleColormap(i / 255.0f, kColormap);
        lut[i][0]      = static_cast<unsigned char>(c.x * 255.0f);
        lut[i][1]      = static_cast<unsigned char>(c.y * 255.0f);
        lut[i][2]      = static_cast<unsigned char>(c.z * 255.0f);
        lut[i][3]      = 255;
    }
}

// RGBA ピクセル列を GL テクスチャにアップロードする（幅 W・高さ H、GL_LINEAR）。
unsigned int upload_texture(const std::vector<unsigned char>& pixels, int W, int H) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// data(bin, frame)（線形周波数ビン。tcmorph と同じ列優先の向き）を ERB 等間隔の行で
// テクスチャ化する（行0=最高周波数）。
// as_db=true は 10*log10(値) を [vmin,vmax] で正規化、false は値をそのまま [vmin,vmax] で正規化。
unsigned int make_heatmap_texture(const Eigen::MatrixXd& data, int fs, bool as_db, double vmin,
                                  double vmax) {
    unsigned char lut[256][4];
    build_lut(lut);

    const int H = static_cast<int>(data.rows());    // 周波数ビン数
    const int W = static_cast<int>(data.cols());    // フレーム数
    if (H < 2 || W < 1) return 0;

    const double range   = vmax > vmin ? vmax - vmin : 1.0;
    const double nyquist = fs / 2.0;
    const double erb_max = freqscale::hz_to_erb(nyquist);

    // 線形ビン b・フレーム t の値（as_db なら dB に変換してから補間する）。
    const auto value_at = [&](int b, int t) -> double {
        b              = std::clamp(b, 0, H - 1);
        const double v = data(b, t);
        return as_db ? 10.0 * std::log10(std::max(v, 1e-12)) : v;
    };

    std::vector<unsigned char> pixels(static_cast<std::size_t>(H) * W * 4);
    for (int r = 0; r < H; ++r) {
        const double erb  = erb_max * (1.0 - static_cast<double>(r) / (H - 1));    // 行0=最大ERB
        const double hz   = freqscale::erb_to_hz(erb);
        const double binf = hz / nyquist * (H - 1);
        const int    b0   = std::clamp(static_cast<int>(std::floor(binf)), 0, H - 1);
        const int    b1   = std::min(b0 + 1, H - 1);
        const double fr   = std::clamp(binf - b0, 0.0, 1.0);
        for (int t = 0; t < W; ++t) {
            const double v  = value_at(b0, t) + (value_at(b1, t) - value_at(b0, t)) * fr;
            const double u  = std::clamp((v - vmin) / range, 0.0, 1.0);
            const int    li = static_cast<int>(u * 255.0);
            unsigned char* px = &pixels[(static_cast<std::size_t>(r) * W + t) * 4];
            px[0]             = lut[li][0];
            px[1]             = lut[li][1];
            px[2]             = lut[li][2];
            px[3]             = lut[li][3];
        }
    }
    return upload_texture(pixels, W, H);
}

unsigned int make_spectrogram_texture(const Spectrogram& sp) {
    unsigned char lut[256][4];
    build_lut(lut);

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

    return upload_texture(pixels, W, H);
}

}    // namespace

Track::~Track() {
    if (tex) glDeleteTextures(1, &tex);
}

App::~App() {
    for (unsigned int& t : morph_tex_sp)
        if (t) glDeleteTextures(1, &t);
    for (unsigned int& t : morph_tex_ap)
        if (t) glDeleteTextures(1, &t);
}

namespace {

// テクスチャを解放して 0 にする。
void delete_tex(unsigned int& t) {
    if (t) {
        glDeleteTextures(1, &t);
        t = 0;
    }
}

}    // namespace

void rebuild_morphed_texture(App& app) {
    delete_tex(app.morph_tex_sp[1]);
    delete_tex(app.morph_tex_ap[1]);

    const MorphChannel& c = app.morph_out.morphed;
    if (!app.morph_out.ok() || c.empty()) return;
    app.morph_tex_sp[1] =
      make_heatmap_texture(c.sp(), c.fs, /*as_db=*/true, app.morph_db_min, app.morph_db_max);
    app.morph_tex_ap[1] = make_heatmap_texture(c.ap(), c.fs, /*as_db=*/false, 0.0, 1.0);
}

void rebuild_morph_bt_textures(App& app) {
    delete_tex(app.morph_tex_sp[0]);
    delete_tex(app.morph_tex_ap[0]);
    delete_tex(app.morph_tex_sp[2]);
    delete_tex(app.morph_tex_ap[2]);

    // sp の共通 dB レンジを base/target から算出（morphed は両者の log 補間なのでレンジ内）。
    double dmin = 1e30, dmax = -1e30;
    bool   any  = false;
    for (const MorphChannel* ch : { app.morph_base.get(), app.morph_target.get() }) {
        if (ch == nullptr || ch->empty()) continue;
        any = true;
        // log は単調なので、最小/最大の係数から dB レンジが決まる。
        const Eigen::MatrixXd& s = ch->sp();
        dmin = std::min(dmin, 10.0 * std::log10(std::max(s.minCoeff(), 1e-12)));
        dmax = std::max(dmax, 10.0 * std::log10(std::max(s.maxCoeff(), 1e-12)));
    }
    if (!any) {
        app.morph_db_min = app.morph_db_max = 0.0;
        rebuild_morphed_texture(app);
        return;
    }
    app.morph_db_min = dmin;
    app.morph_db_max = dmax;

    const MorphChannel* chs[2] = { app.morph_base.get(), app.morph_target.get() };
    const int           idx[2] = { 0, 2 };
    for (int i = 0; i < 2; ++i) {
        if (chs[i] == nullptr || chs[i]->empty()) continue;
        const MorphChannel& c    = *chs[i];
        app.morph_tex_sp[idx[i]] = make_heatmap_texture(c.sp(), c.fs, /*as_db=*/true, dmin, dmax);
        app.morph_tex_ap[idx[i]] = make_heatmap_texture(c.ap(), c.fs, /*as_db=*/false, 0.0, 1.0);
    }

    // レンジが変わったので morphed 側も作り直す。
    rebuild_morphed_texture(app);
}

void apply_track(Track& tr, const std::string& path, Spectrogram&& spec) {
    if (tr.tex) {
        glDeleteTextures(1, &tr.tex);
        tr.tex = 0;
    }
    tr.spec  = std::move(spec);
    tr.tex   = make_spectrogram_texture(tr.spec);
    tr.y_min = 0.0;    // 周波数軸の表示範囲をリセット（ERB レート）
    tr.y_max = freqscale::hz_to_erb(tr.spec.fs / 2.0);
    // ミニマップの枠を全体表示で初期化。
    tr.view_x0 = 0.0;
    tr.view_x1 = tr.spec.duration;
    tr.view_y0 = 0.0;
    tr.view_y1 = tr.y_max;
    // フォルマントと音素セグメンテーションは前の音声のものなので捨てる。フォルマントは
    // パスが変わったことを ui.cpp の ensure_formants が見て、自動で推定し直す。
    tr.formants     = {};
    tr.segmentation = {};
    tr.path         = path;
    applog::add(tr.name + " 読み込み完了: " + path);
}
