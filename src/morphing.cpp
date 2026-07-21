#include "morphing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <vector>

#include <miniaudio_cpp/audio.hpp>

#include "world/cheaptrick.h"
#include "world/d4c.h"
#include "world/harvest.h"
#include "world/synthesis.h"

#include "app.hpp"    // Anchor / FreqAnchor

namespace {

constexpr double kFramePeriod = 5.0;      // ms
constexpr double kFrameSec    = 0.005;    // s

// 1音声の WORLD 解析結果。
struct Analysis {
    int                              fs       = 0;
    int                              fft_size = 0;
    int                              nbin     = 0;    // fft_size/2 + 1
    int                              f0_len   = 0;
    double                           duration = 0;    // s
    std::vector<double>              f0;              // [f0_len]
    std::vector<std::vector<double>> sp;              // [f0_len][nbin] パワースペクトル
    std::vector<std::vector<double>> ap;              // [f0_len][nbin] 非周期性 [0,1]
};

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

// WORLD で F0 / スペクトル包絡 / 非周期性を推定する。
Analysis analyze(const std::string& path) {
    ma::decoder dec { path };

    Analysis        A;
    A.fs                     = static_cast<int>(dec.sample_rate());
    const std::vector<double> x = to_mono(dec);
    const int       x_length = static_cast<int>(x.size());
    if (x_length == 0) throw std::runtime_error("empty signal: " + path);
    A.duration = static_cast<double>(x_length) / A.fs;

    // F0 (Harvest)
    HarvestOption h_opt;
    InitializeHarvestOption(&h_opt);
    h_opt.frame_period = kFramePeriod;
    A.f0_len           = GetSamplesForHarvest(A.fs, x_length, kFramePeriod);
    std::vector<double> t(A.f0_len);
    A.f0.assign(A.f0_len, 0.0);
    Harvest(x.data(), x_length, A.fs, &h_opt, t.data(), A.f0.data());

    // Spectral envelope (CheapTrick)
    CheapTrickOption c_opt;
    InitializeCheapTrickOption(A.fs, &c_opt);
    A.fft_size = c_opt.fft_size;
    A.nbin     = A.fft_size / 2 + 1;

    A.sp.assign(A.f0_len, std::vector<double>(A.nbin));
    A.ap.assign(A.f0_len, std::vector<double>(A.nbin));
    std::vector<double*> sp_ptr(A.f0_len), ap_ptr(A.f0_len);
    for (int i = 0; i < A.f0_len; ++i) {
        sp_ptr[i] = A.sp[i].data();
        ap_ptr[i] = A.ap[i].data();
    }
    CheapTrick(x.data(), x_length, A.fs, t.data(), A.f0.data(), A.f0_len, &c_opt, sp_ptr.data());

    // Aperiodicity (D4C)
    D4COption d_opt;
    InitializeD4COption(&d_opt);
    D4C(x.data(), x_length, A.fs, t.data(), A.f0.data(), A.f0_len, A.fft_size, &d_opt, ap_ptr.data());

    return A;
}

// 時刻 t での F0 サンプル。MATLAB に合わせ、値は log(F0) を時間方向に線形補間、有声度
// (VUV) は 0/1 を線形補間した割合を返す。無声フレーム(F0=0)は log を取らず値に含めない。
struct F0Sample {
    double value;    // F0 [Hz]（両隣無声なら 0）
    double vuv;      // 有声割合 [0,1]
};

F0Sample f0_sample(const Analysis& A, double t_sec) {
    const double ff = t_sec / kFrameSec;
    const int    i0 = std::clamp(static_cast<int>(std::floor(ff)), 0, A.f0_len - 1);
    const int    i1 = std::min(i0 + 1, A.f0_len - 1);
    const double fr = std::clamp(ff - i0, 0.0, 1.0);

    const double a = A.f0[i0], b = A.f0[i1];
    const double v0 = a > 0.0 ? 1.0 : 0.0, v1 = b > 0.0 ? 1.0 : 0.0;

    double value = 0.0;
    if (a > 0.0 && b > 0.0) value = std::exp((1.0 - fr) * std::log(a) + fr * std::log(b));
    else if (a > 0.0) value = a;
    else if (b > 0.0) value = b;

    return { value, (1.0 - fr) * v0 + fr * v1 };
}

// F0 のモーフィング（値は log 補間、有声度は重み和を 0.99 で閾値: MATLAB 準拠）。
double morph_f0(const F0Sample& b, const F0Sample& t, double fo) {
    const double vm = (1.0 - fo) * b.vuv + fo * t.vuv;
    if (vm < 0.99) return 0.0;    // 無声
    if (b.value > 0.0 && t.value > 0.0)
        return std::exp((1.0 - fo) * std::log(b.value) + fo * std::log(t.value));
    return b.value > 0.0 ? b.value : t.value;
}

// スペクトログラム/非周期性を (時刻, 周波数) で bilinear サンプルする。
double sample(const std::vector<std::vector<double>>& S, int f0_len, int nbin, int fft_size, int fs,
              double t_sec, double f_hz) {
    const double ff = t_sec / kFrameSec;
    const int    i0 = std::clamp(static_cast<int>(std::floor(ff)), 0, f0_len - 1);
    const int    i1 = std::min(i0 + 1, f0_len - 1);
    const double fr = std::clamp(ff - i0, 0.0, 1.0);

    const double bb = f_hz * fft_size / fs;
    const int    b0 = std::clamp(static_cast<int>(std::floor(bb)), 0, nbin - 1);
    const int    b1 = std::min(b0 + 1, nbin - 1);
    const double br = std::clamp(bb - b0, 0.0, 1.0);

    const double v0 = S[i0][b0] + (S[i0][b1] - S[i0][b0]) * br;
    const double v1 = S[i1][b0] + (S[i1][b1] - S[i1][b0]) * br;
    return v0 + (v1 - v0) * fr;
}


// 1つの時間アンカーの周波数アンカーから、全ビン分の「モーフ周波数 → base/target 周波数」
// 写像を作る。MATLAB に合わせ log 周波数で処理する: モーフ後のアンカー位置を
// exp((1-fx)log bf + fx log tf)（幾何平均）とし、各ビンの fm を log 周波数で線形逆写像。
// MATLAB の freqAxisOnObj(:,jj) 相当（時間アンカーごとに1本の写像ベクトルを持たせる）。
void build_freq_warp(const std::vector<FreqAnchor>* freqs, double fx, int nbin, int fft_size, int fs,
                     double nyquist, std::vector<double>& outB, std::vector<double>& outT) {
    // log を取るため 0Hz を避ける下限。低域端ノードに使う。
    constexpr double kFloor = 1.0;    // Hz

    std::vector<double> bf { kFloor }, tf { kFloor };
    if (freqs != nullptr) {
        std::vector<std::pair<double, double>> fp;
        for (const FreqAnchor& f : *freqs)
            if (f.base_f > kFloor && f.base_f < nyquist && f.target_f > kFloor && f.target_f < nyquist)
                fp.emplace_back(f.base_f, f.target_f);
        std::sort(fp.begin(), fp.end());
        for (const auto& p : fp) {
            bf.push_back(p.first);
            tf.push_back(p.second);
        }
    }
    bf.push_back(nyquist);
    tf.push_back(nyquist);

    // 折れ線ノードを log 周波数で保持。モーフ位置は log の重み和（＝幾何平均）。
    const std::size_t   n = bf.size();
    std::vector<double> lbf(n), ltf(n), lfam(n);
    for (std::size_t i = 0; i < n; ++i) {
        lbf[i]  = std::log(bf[i]);
        ltf[i]  = std::log(tf[i]);
        lfam[i] = (1.0 - fx) * lbf[i] + fx * ltf[i];
    }

    outB.resize(nbin);
    outT.resize(nbin);
    int j = 0;
    for (int b = 0; b < nbin; ++b) {
        const double fm = static_cast<double>(b) * fs / fft_size;
        if (fm <= kFloor) {    // DC〜下限は恒等（log 不可のため）
            outB[b] = fm;
            outT[b] = fm;
            continue;
        }
        const double lm = std::log(fm);
        while (j < static_cast<int>(n) - 2 && lm > lfam[j + 1]) ++j;
        const double denom = std::max(lfam[j + 1] - lfam[j], 1e-9);
        const double g     = std::clamp((lm - lfam[j]) / denom, 0.0, 1.0);
        outB[b]            = std::exp(lbf[j] + g * (lbf[j + 1] - lbf[j]));
        outT[b]            = std::exp(ltf[j] + g * (ltf[j + 1] - ltf[j]));
    }
}

// 時間アンカー（base_t 昇順、両端に境界を追加済み）。
struct TimeAnchor {
    double                         base_t;
    double                         target_t;
    const std::vector<FreqAnchor>* freqs;    // nullptr = 周波数アンカーなし（境界）
};

}    // namespace

MorphResult morphing(const std::string& base_path, const std::string& target_path,
                     const std::vector<Anchor>& anchors, const MorphRates& rates) {
    MorphResult R;
    try {
        const Analysis B = analyze(base_path);
        const Analysis T = analyze(target_path);
        if (B.fs != T.fs) {
            R.error = "サンプリング周波数が異なります（リサンプリング未対応）";
            return R;
        }
        if (B.fft_size != T.fft_size) {
            R.error = "FFT サイズが一致しません";
            return R;
        }

        const int    fs       = B.fs;
        const int    fft_size = B.fft_size;
        const int    nbin     = B.nbin;
        const double nyquist  = fs / 2.0;

        // 軸ごとの率（それぞれ [0,1] にクランプ）。
        const double tx = std::clamp(rates.tx, 0.0, 1.0);    // 時間軸
        const double fx = std::clamp(rates.fx, 0.0, 1.0);    // 周波数軸
        const double fo = std::clamp(rates.fo, 0.0, 1.0);    // F0
        const double sl = std::clamp(rates.sl, 0.0, 1.0);    // スペクトルレベル
        const double ap = std::clamp(rates.ap, 0.0, 1.0);    // 非周期性

        // ── 時間アンカーを base_t 昇順に整列し、両端に境界 (0,0),(dur,dur) を足す ──
        std::vector<const Anchor*> sorted;
        sorted.reserve(anchors.size());
        for (const Anchor& a : anchors) sorted.push_back(&a);
        std::sort(sorted.begin(), sorted.end(),
                  [](const Anchor* a, const Anchor* b) { return a->base_t < b->base_t; });

        std::vector<TimeAnchor> ta;
        ta.push_back({ 0.0, 0.0, nullptr });
        for (const Anchor* a : sorted) {
            if (a->base_t > 1e-6 && a->base_t < B.duration - 1e-6 && a->target_t > 1e-6
                && a->target_t < T.duration - 1e-6)
                ta.push_back({ a->base_t, a->target_t, &a->freqs });
        }
        ta.push_back({ B.duration, T.duration, nullptr });
        const int K = static_cast<int>(ta.size());

        // ── モーフ後のタイムライン（セグメント長を log 補間） ──
        std::vector<double> tm(K, 0.0);
        for (int k = 0; k < K - 1; ++k) {
            const double db = std::max(ta[k + 1].base_t - ta[k].base_t, 1e-6);
            const double dt = std::max(ta[k + 1].target_t - ta[k].target_t, 1e-6);
            tm[k + 1]       = tm[k] + std::exp((1.0 - tx) * std::log(db) + tx * std::log(dt));
        }
        const double total = tm[K - 1];
        const int    M     = std::max(1, static_cast<int>(std::floor(total / kFrameSec)) + 1);

        // ── 出力バッファ ──
        std::vector<double>              f0o(M, 0.0);
        std::vector<std::vector<double>> spo(M, std::vector<double>(nbin));
        std::vector<std::vector<double>> apo(M, std::vector<double>(nbin));

        // ── 各時間アンカーの周波数写像を前計算（全ビン分の morph→base/target 周波数） ──
        // これを区間内でビンごとに補間することで、周波数ワープが時間方向に連続になる
        // （MATLAB: currentFreqAxOnObj = (1-lambda)*last + lambda*next）。
        std::vector<std::vector<double>> warpB(K), warpT(K);
        for (int k = 0; k < K; ++k)
            build_freq_warp(ta[k].freqs, fx, nbin, fft_size, fs, nyquist, warpB[k], warpT[k]);

        int seg = 0;
        for (int m = 0; m < M; ++m) {
            const double tau = m * kFrameSec;
            while (seg < K - 2 && tau > tm[seg + 1]) ++seg;
            const double seg_dur = std::max(tm[seg + 1] - tm[seg], 1e-9);
            const double s       = std::clamp((tau - tm[seg]) / seg_dur, 0.0, 1.0);

            // モーフ時刻 → 各元の時刻（区間内は線形）。
            const double taub = ta[seg].base_t + s * (ta[seg + 1].base_t - ta[seg].base_t);
            const double taut = ta[seg].target_t + s * (ta[seg + 1].target_t - ta[seg].target_t);

            f0o[m] = morph_f0(f0_sample(B, taub), f0_sample(T, taut), fo);

            // 周波数写像は区間の左右アンカーをビンごとに s 補間（時間方向に連続）。
            const std::vector<double>& wbL = warpB[seg];
            const std::vector<double>& wbR = warpB[seg + 1];
            const std::vector<double>& wtL = warpT[seg];
            const std::vector<double>& wtR = warpT[seg + 1];
            for (int b = 0; b < nbin; ++b) {
                const double fb = (1.0 - s) * wbL[b] + s * wbR[b];
                const double ft = (1.0 - s) * wtL[b] + s * wtR[b];

                const double sb = sample(B.sp, B.f0_len, nbin, fft_size, fs, taub, fb);
                const double st = sample(T.sp, T.f0_len, nbin, fft_size, fs, taut, ft);
                spo[m][b]       = std::exp((1.0 - sl) * std::log(std::max(sb, 1e-20))
                                     + sl * std::log(std::max(st, 1e-20)));

                const double ab = sample(B.ap, B.f0_len, nbin, fft_size, fs, taub, fb);
                const double at = sample(T.ap, T.f0_len, nbin, fft_size, fs, taut, ft);
                apo[m][b]       = std::clamp((1.0 - ap) * ab + ap * at, 0.0, 1.0);
            }
        }

        // ── WORLD 合成 ──
        const int           y_length = static_cast<int>(total * fs) + 1;
        std::vector<double> y(y_length, 0.0);
        std::vector<const double*> spp(M), app(M);
        for (int m = 0; m < M; ++m) {
            spp[m] = spo[m].data();
            app[m] = apo[m].data();
        }
        Synthesis(f0o.data(), M, spp.data(), app.data(), fft_size, kFramePeriod, fs, y_length, y.data());

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
