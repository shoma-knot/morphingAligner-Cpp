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


// Windows の GL ヘッダ（gl.h）は OpenGL 1.1 までしか定義しておらず、1.2 で入った
// GL_CLAMP_TO_EDGE が無い。値は仕様で決まっているので、無いときだけ自前で定義する
// （拡張の読み込み（glad など）を入れるほどではないため）。
#ifndef GL_CLAMP_TO_EDGE
    #define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {

// スペクトログラムは dB 値を RGBA のテクスチャに一度だけ焼き込み、毎フレームは1枚の四角形として
// 描く。ImPlot のヒートマップ（ビン×フレームのセルを毎フレーム CPU で組み立てる）だと、長い
// 音声で描画が極端に遅くなったため。

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
GlTexture upload_texture(const std::vector<unsigned char>& pixels, int W, int H) {
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
    return GlTexture { tex };
}

// data(bin, frame)（線形周波数ビン。tcmorph と同じ列優先の向き）を ERB 等間隔の行で
// テクスチャ化する（行0=最高周波数）。
// as_db=true は 10*log10(値) を [vmin,vmax] で正規化、false は値をそのまま [vmin,vmax] で正規化。
GlTexture make_heatmap_texture(const Eigen::MatrixXd& data, int fs, bool as_db, double vmin,
                                  double vmax) {
    unsigned char lut[256][4];
    build_lut(lut);

    const int H = static_cast<int>(data.rows());    // 周波数ビン数
    const int W = static_cast<int>(data.cols());    // フレーム数
    if (H < 2 || W < 1) return {};

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

}    // namespace

void rebuild_morphed_texture(MorphState& m) {
    m.tex_sp[1].reset();
    m.tex_ap[1].reset();

    const MorphChannel& c = m.out.morphed;
    if (!m.out.ok() || c.empty()) return;
    m.tex_sp[1] =
      make_heatmap_texture(c.sp(), c.fs, /*as_db=*/true, m.db_min, m.db_max);
    m.tex_ap[1] = make_heatmap_texture(c.ap(), c.fs, /*as_db=*/false, 0.0, 1.0);
}

void rebuild_morph_bt_textures(MorphState& m) {
    for (GlTexture& t : m.tex_sp) t.reset();
    for (GlTexture& t : m.tex_ap) t.reset();

    // sp の共通 dB レンジを base/target から算出（morphed は両者の log 補間なのでレンジ内）。
    double dmin = 1e30, dmax = -1e30;
    bool   any  = false;
    for (Side s : kSides) {
        const MorphChannel* ch = m.channel(s);
        if (ch == nullptr || ch->empty()) continue;
        any = true;
        // log は単調なので、最小/最大の係数から dB レンジが決まる。
        const Eigen::MatrixXd& sp = ch->sp();
        dmin = std::min(dmin, 10.0 * std::log10(std::max(sp.minCoeff(), 1e-12)));
        dmax = std::max(dmax, 10.0 * std::log10(std::max(sp.maxCoeff(), 1e-12)));
    }
    if (!any) {
        m.db_min = m.db_max = 0.0;
        rebuild_morphed_texture(m);
        return;
    }
    m.db_min = dmin;
    m.db_max = dmax;

    for (Side s : kSides) {
        const MorphChannel* ch = m.channel(s);
        if (ch == nullptr || ch->empty()) continue;
        const int col         = s == Side::Base ? 0 : 2;    // テクスチャの列（1 は morphed）
        m.tex_sp[col] = make_heatmap_texture(ch->sp(), ch->fs, /*as_db=*/true, dmin, dmax);
        m.tex_ap[col] = make_heatmap_texture(ch->ap(), ch->fs, /*as_db=*/false, 0.0, 1.0);
    }

    // レンジが変わったので morphed 側も作り直す。
    rebuild_morphed_texture(m);
}

void apply_track(Track& tr, const std::string& path, AnalyzedAudio&& audio) {
    tr.channel = std::move(audio.channel);
    tr.spec    = audio.spec;
    tr.tex     = tr.channel ? make_heatmap_texture(tr.channel->sp(), tr.spec.fs, /*as_db=*/true, tr.spec.db_min,
                                                   tr.spec.db_max)
                            : GlTexture {};
    tr.y_min = 0.0;    // 周波数軸の表示範囲をリセット（ERB レート）
    tr.y_max = freqscale::hz_to_erb(tr.spec.fs / 2.0);
    // ミニマップの枠を全体表示で初期化。
    tr.view_x0 = 0.0;
    tr.view_x1 = tr.spec.duration;
    tr.view_y0 = 0.0;
    tr.view_y1 = tr.y_max;
    // フォルマントと音素セグメンテーションは前の音声のものなので捨てる。フォルマントは
    // パスが変わったことを speech_controller.cpp の ensure_formants が見て、自動で推定し直す。
    tr.formants     = {};
    tr.formants_ma  = {};
    tr.formant_path.clear();    // 同じファイルを読み直した場合も推定し直させる
    tr.segmentation = {};
    tr.path         = path;
    applog::add(tr.name + " 読み込み完了: " + path);
}
