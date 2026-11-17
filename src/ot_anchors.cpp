#include "ot_anchors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <Eigen/Core>

namespace {

// 時刻 t を中心に平均する幅の半分 [ms]。時間アンカーは音素境界（遷移の途中）に打たれることが
// 多く、1フレームだけだとその瞬間の形に引きずられるので、前後を少し平均して形を安定させる。
constexpr double kAvgHalfMs = 10.0;

// 無音とみなす、その音声で最も大きいフレームからの差 [dB]。無音の包絡は雑音の形なので、
// 分位点を対応させても意味のある対応にならない。
constexpr double kSilenceDb = 40.0;

// 帯ごとに足すときの、帯の幅の下限 [Hz]（base/target の両側）。これより狭い帯には足さない
// （端のアンカーのすぐ脇に点が増えるだけで、急なワープを作りかねないため）。
constexpr double kMinBandHz = 50.0;

double power_db(double p) {
    return 10.0 * std::log10(std::max(p, 1e-300));
}

// 時刻 t の包絡（前後 kAvgHalfMs のフレームをパワーで平均したもの）。
Eigen::VectorXd envelope_at(const MorphChannel& c, double t) {
    const double fp     = c.frame_period > 0.0 ? c.frame_period : kFramePeriodMs;
    const int    last   = c.n_frames - 1;
    const int    center = std::clamp(static_cast<int>(std::lround(t * 1000.0 / fp)), 0, last);
    const int    half   = std::max(0, static_cast<int>(std::lround(kAvgHalfMs / fp)));
    const int    lo = std::max(0, center - half), hi = std::min(last, center + half);
    return c.sp().middleCols(lo, hi - lo + 1).rowwise().mean();
}

// その音声で最も大きいフレームの総パワー [dB]（無音の判定の基準）。
double loudest_frame_db(const MorphChannel& c) {
    return power_db(c.sp().colwise().sum().maxCoeff());
}

}    // namespace

std::vector<double> spectral_quantiles(const double* power, int nbin, int fs, double lo_hz, double hi_hz, int count,
                                       double gamma) {
    std::vector<double> out;
    if (nbin < 2 || fs <= 0 || count < 1) return out;
    const double df = (fs / 2.0) / (nbin - 1);    // bin の間隔 [Hz]
    const double lo = std::max(0.0, lo_hz);
    const double hi = std::min(hi_hz, fs / 2.0);
    if (!(hi > lo)) return out;

    // パワーを周波数で線形補間して読み、gamma 乗で圧縮したものを質量の密度にする。
    const auto density = [&](double f) {
        const double x = f / df;
        const int    k = std::clamp(static_cast<int>(std::floor(x)), 0, nbin - 2);
        const double a = x - k;
        return std::pow(std::max(power[k] * (1.0 - a) + power[k + 1] * a, 0.0), gamma);
    };

    // 格子は範囲の両端と、その内側の bin（両端を bin の間に置けるようにする）。
    std::vector<double> xs { lo };
    for (int k = static_cast<int>(std::ceil(lo / df)); k < nbin; ++k) {
        const double f = k * df;
        if (f <= lo) continue;
        if (f >= hi) break;
        xs.push_back(f);
    }
    xs.push_back(hi);

    // 累積質量（台形則）。
    std::vector<double> cdf(xs.size(), 0.0);
    double              prev = density(xs[0]);
    for (std::size_t i = 1; i < xs.size(); ++i) {
        const double d = density(xs[i]);
        cdf[i]         = cdf[i - 1] + 0.5 * (prev + d) * (xs[i] - xs[i - 1]);
        prev           = d;
    }
    const double total = cdf.back();
    if (!(total > 0.0) || !std::isfinite(total)) return out;

    // 分位点は累積質量の逆関数で求める（区間の中は線形に補間）。分位点は昇順なので、
    // 区間の探索は前回の続きから進めればよい。
    out.reserve(static_cast<std::size_t>(count));
    std::size_t k = 1;
    for (int j = 1; j <= count; ++j) {
        const double target = total * j / (count + 1);
        while (k < cdf.size() - 1 && cdf[k] < target) ++k;
        const double c0 = cdf[k - 1], c1 = cdf[k];
        const double a  = c1 > c0 ? (target - c0) / (c1 - c0) : 0.0;
        out.push_back(xs[k - 1] + a * (xs[k] - xs[k - 1]));
    }
    return out;
}

OtAnchorResult make_ot_freq_anchors(const MorphChannel& base, const MorphChannel& target,
                                    const std::vector<Anchor>& anchors, const OtAnchorParams& p) {
    OtAnchorResult R;
    const bool     bands = p.mode == OtMode::Bands;
    if (base.empty() || target.empty()) {
        R.error = "base/target の解析データがありません";
        return R;
    }
    if (base.fs != target.fs) {
        R.error = "base と target のサンプリング周波数が違います";
        return R;
    }
    if (anchors.empty()) {
        R.error = "時間アンカーがありません（先に時間アンカーを打ってください）";
        return R;
    }
    if (!bands && (p.count < OtAnchorParams::kCountMin || p.count > OtAnchorParams::kCountMax)) {
        R.error = "本数は " + std::to_string(OtAnchorParams::kCountMin) + "〜" + std::to_string(OtAnchorParams::kCountMax)
                + " で指定してください";
        return R;
    }
    if (bands && (p.band_count < OtAnchorParams::kBandCountMin || p.band_count > OtAnchorParams::kBandCountMax)) {
        R.error = "区間あたりの本数は " + std::to_string(OtAnchorParams::kBandCountMin) + "〜"
                + std::to_string(OtAnchorParams::kBandCountMax) + " で指定してください";
        return R;
    }
    if (!(p.gamma >= OtAnchorParams::kGammaMin && p.gamma <= OtAnchorParams::kGammaMax)) {
        R.error = "圧縮（gamma）が範囲外です";
        return R;
    }
    const double nyq = base.fs / 2.0;
    if (!(p.min_hz >= 0.0 && p.max_hz > p.min_hz && p.min_hz < nyq)) {
        R.error = "周波数範囲が不正です（下限 < 上限、下限はナイキスト周波数未満）";
        return R;
    }
    if (p.max_hz > nyq)
        R.warnings.push_back("上限をナイキスト周波数（" + std::to_string(static_cast<int>(nyq)) + " Hz）に切り詰めました");

    const double lo = p.min_hz, hi = std::min(p.max_hz, nyq);
    const int    per_band = bands ? p.band_count : p.count;
    const double top_b = loudest_frame_db(base), top_t = loudest_frame_db(target);

    R.anchors = anchors;
    for (Anchor& a : R.anchors) {
        if (!bands) a.freqs.clear();    // 全帯域は置き換える（帯ごとは既存を残して足す）
        const Eigen::VectorXd eb = envelope_at(base, a.base_t);
        const Eigen::VectorXd et = envelope_at(target, a.target_t);
        if (power_db(eb.sum()) < top_b - kSilenceDb || power_db(et.sum()) < top_t - kSilenceDb) {
            ++R.skipped;
            continue;
        }

        // 帯の端（base 側 / target 側）。範囲の両端と、帯ごとのときは範囲の内側にある既存の
        // 周波数アンカー。範囲の外にあるアンカーは端に使わない（残しはする）。
        std::vector<double> edge_b { lo }, edge_t { lo };
        if (bands) {
            std::vector<FreqAnchor> inner;
            for (const FreqAnchor& f : a.freqs)
                if (f.base_f > lo && f.base_f < hi && f.target_f > lo && f.target_f < hi) inner.push_back(f);
            std::sort(inner.begin(), inner.end(),
                      [](const FreqAnchor& x, const FreqAnchor& y) { return x.base_f < y.base_f; });
            // 交差している（base の順と target の順が食い違う）と帯が対応しないので、この時刻はかけない。
            bool monotone = true;
            for (std::size_t j = 1; j < inner.size(); ++j)
                monotone &= inner[j].base_f > inner[j - 1].base_f && inner[j].target_f > inner[j - 1].target_f;
            if (!monotone) {
                ++R.crossed;
                continue;
            }
            for (const FreqAnchor& f : inner) {
                edge_b.push_back(f.base_f);
                edge_t.push_back(f.target_f);
            }
            R.n_edges += static_cast<int>(inner.size());
        }
        edge_b.push_back(hi);
        edge_t.push_back(hi);

        // 対応する帯どうしで分位点を対応させる。
        std::vector<FreqAnchor> added;
        for (std::size_t j = 0; j + 1 < edge_b.size(); ++j) {
            const double b0 = edge_b[j], b1 = edge_b[j + 1], t0 = edge_t[j], t1 = edge_t[j + 1];
            if (b1 - b0 < kMinBandHz || t1 - t0 < kMinBandHz) continue;
            const std::vector<double> qb =
              spectral_quantiles(eb.data(), static_cast<int>(eb.size()), base.fs, b0, b1, per_band, p.gamma);
            const std::vector<double> qt =
              spectral_quantiles(et.data(), static_cast<int>(et.size()), target.fs, t0, t1, per_band, p.gamma);
            if (qb.empty() || qb.size() != qt.size()) continue;
            for (std::size_t k = 0; k < qb.size(); ++k) added.push_back(FreqAnchor { qb[k], qt[k] });
        }
        R.n_freq += static_cast<int>(added.size());
        a.freqs.insert(a.freqs.end(), added.begin(), added.end());
        std::sort(a.freqs.begin(), a.freqs.end(),
                  [](const FreqAnchor& x, const FreqAnchor& y) { return x.base_f < y.base_f; });
    }
    if (R.skipped > 0)
        R.warnings.push_back("無音に近い時間アンカー " + std::to_string(R.skipped) + " 本には OT をかけませんでした");
    if (R.crossed > 0)
        R.warnings.push_back("周波数アンカーが交差している時間アンカー " + std::to_string(R.crossed)
                             + " 本には OT をかけませんでした（区間が対応しないため）");
    return R;
}
