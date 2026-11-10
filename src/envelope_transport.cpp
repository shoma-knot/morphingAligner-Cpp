#include "envelope_transport.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using Eigen::Index;
using Eigen::MatrixXd;
using Eigen::VectorXd;

namespace {

// これより狭い帯は運ばない（平均のまま）。アンカーどうしが近いと、端のすぐ脇だけを動かすことになるため。
constexpr double kMinBandHz = 50.0;

// フレームのパワーが、いちばん大きいフレームよりこれだけ下回れば運ばない（無音の包絡は雑音の形なので）。
constexpr double kSilenceDb = 40.0;

// a と b の幾何補間（w = 0 で a、1 で b）。a, b > 0 のときだけ使う。
double geo_mix(double a, double b, double w) {
    return std::exp((1.0 - w) * std::log(a) + w * std::log(b));
}

// p（n 点）の実数の位置 x での値（隣の点との線形補間）。
double value_at(const double* p, int n, double x) {
    const int    k = std::clamp(static_cast<int>(std::floor(x)), 0, n - 2);
    const double t = x - k;
    return p[k] * (1.0 - t) + p[k + 1] * t;
}

// 帯 [a, b] の格子（両端と、その内側の bin）と、その上の圧縮した密度・累積質量（台形則）。
struct BandCdf {
    std::vector<double> x, dens, cdf;
    double              total() const { return cdf.back(); }
};

BandCdf band_cdf(const double* p, int n, double a, double b, double gamma) {
    BandCdf f;
    f.x.push_back(a);
    for (int i = static_cast<int>(std::floor(a)) + 1; i < b; ++i) f.x.push_back(i);
    f.x.push_back(b);
    f.dens.resize(f.x.size());
    f.cdf.assign(f.x.size(), 0.0);
    for (std::size_t i = 0; i < f.x.size(); ++i) {
        f.dens[i] = std::pow(std::max(value_at(p, n, f.x[i]), 0.0), gamma);
        if (i > 0) f.cdf[i] = f.cdf[i - 1] + 0.5 * (f.dens[i - 1] + f.dens[i]) * (f.x[i] - f.x[i - 1]);
    }
    return f;
}

// 分位関数: 昇順の us（0〜1）に当たる位置（累積質量の逆関数。区間の中は線形に補間）。
std::vector<double> quantiles(const BandCdf& f, const std::vector<double>& us) {
    std::vector<double> q(us.size());
    const double        total = f.total();
    std::size_t         k     = 1;
    for (std::size_t i = 0; i < us.size(); ++i) {
        const double target = us[i] * total;
        while (k < f.cdf.size() - 1 && f.cdf[k] < target) ++k;
        const double c0 = f.cdf[k - 1], c1 = f.cdf[k];
        const double a  = c1 > c0 ? std::clamp((target - c0) / (c1 - c0), 0.0, 1.0) : 0.0;
        q[i]            = f.x[k - 1] + a * (f.x[k] - f.x[k - 1]);
    }
    return q;
}

// 折れ線 (X, Y) の x での値（X は昇順。範囲の外は端の区間を延ばす）。
double polyline_at(const std::vector<double>& X, const std::vector<double>& Y, double x) {
    std::size_t k = 1;
    while (k + 1 < X.size() && X[k] < x) ++k;
    const double t = (x - X[k - 1]) / (X[k] - X[k - 1]);
    return Y[k - 1] + t * (Y[k] - Y[k - 1]);
}

// 中間の軸の周波数アンカーの位置と、上端（1 Hz から fs/2 までの間隔を積み上げた終点）。n+1 個。
// tcmorph は間隔を幾何補間して積み上げるので、上端はちょうど fs/2 にはならない。
std::vector<double> morph_axis_knots(const std::vector<double>& ref_hz, const std::vector<double>& tgt_hz, double fx,
                                     double fs) {
    const std::size_t n = ref_hz.size();
    if (n == 0 || tgt_hz.size() != n || !(fs > 0.0)) return {};
    // 1 Hz と fs/2 を両端に置いた対数周波数（本アプリは声道長比を使わないので、target 側の上端も fs/2）。
    std::vector<double> lr { 0.0 }, lt { 0.0 };
    for (std::size_t k = 0; k < n; ++k) {
        if (!(ref_hz[k] > 0.0 && tgt_hz[k] > 0.0)) return {};
        lr.push_back(std::log(ref_hz[k]));
        lt.push_back(std::log(tgt_hz[k]));
    }
    lr.push_back(std::log(fs / 2.0));
    lt.push_back(std::log(fs / 2.0));

    std::vector<double> out;
    double              acc = 0.0;
    for (std::size_t k = 0; k + 1 < lr.size(); ++k) {
        const double d_ref = lr[k + 1] - lr[k], d_tgt = lt[k + 1] - lt[k];
        if (!(d_ref > 0.0 && d_tgt > 0.0)) return {};    // 逆転（tcmorph では「折り返すワープ」になる）
        acc += geo_mix(d_ref, d_tgt, fx);
        out.push_back(std::exp(acc));
    }
    return out;
}

// 1つの列（始端・時間アンカー・終端）の周波数ワープ。tcmorph は、base 側は (1 Hz, 周波数アンカー, fs/2) を
// 中間の軸の (1 Hz, アンカーの位置, 上端) へ、target 側も同じ位置へ折れ線で写す。周波数アンカーが無い列は
// 恒等写像。
struct Column {
    std::vector<double> ref, tgt;           // 周波数アンカー（base 側 / target 側）[Hz]
    std::vector<double> x_ref, x_tgt, y;    // 写像の折れ線（y は中間の軸）
    bool                valid = true;       // 逆転していて写像を決められなければ false

    double map_ref(double f) const { return ref.empty() ? f : polyline_at(x_ref, y, f); }
    double map_tgt(double f) const { return tgt.empty() ? f : polyline_at(x_tgt, y, f); }
};

Column make_column(std::vector<double> ref, std::vector<double> tgt, double fx, double fs) {
    Column c;
    if (ref.empty()) return c;
    const std::vector<double> knots = morph_axis_knots(ref, tgt, fx, fs);
    if (knots.empty()) {
        c.valid = false;
        return c;
    }
    c.x_ref = { 1.0 };
    c.x_tgt = { 1.0 };
    c.y     = { 1.0 };
    for (std::size_t k = 0; k < ref.size(); ++k) {
        c.x_ref.push_back(ref[k]);
        c.x_tgt.push_back(tgt[k]);
    }
    c.x_ref.push_back(fs / 2.0);
    c.x_tgt.push_back(fs / 2.0);
    c.y.insert(c.y.end(), knots.begin(), knots.end());
    c.ref = std::move(ref);
    c.tgt = std::move(tgt);
    return c;
}

}    // namespace

void transport_band(const double* ref, const double* tgt, double* out, int n, double a_ref, double b_ref, double a_tgt,
                    double b_tgt, double s, double gamma) {
    const auto in_range = [n](double a, double b) { return b > a && a >= 0.0 && b <= n - 1; };
    if (n < 2 || !in_range(a_ref, b_ref) || !in_range(a_tgt, b_tgt) || !(gamma > 0.0)) return;

    const BandCdf fr = band_cdf(ref, n, a_ref, b_ref, gamma), ft = band_cdf(tgt, n, a_tgt, b_tgt, gamma);
    const double  mr = fr.total(), mt = ft.total();
    if (!(mr > 0.0 && mt > 0.0) || !std::isfinite(mr) || !std::isfinite(mt)) return;

    // 変位補間: 分位関数どうしを率で線形に補間する（Q_s = (1-s) Q_ref + s Q_tgt）。分位関数を引く点は、
    // 両側の累積質量の折れ点を合わせたもの。どちらの分位関数もその間では線形なので、Q_s も正確に
    // 折れ線で表せる（等間隔に引くと、質量の小さい所で1歩が何十 bin にもなって密度がならされる）。
    std::vector<double> us;
    us.reserve(fr.cdf.size() + ft.cdf.size());
    for (double c : fr.cdf) us.push_back(c / mr);
    for (double c : ft.cdf) us.push_back(c / mt);
    std::sort(us.begin(), us.end());
    us.erase(std::unique(us.begin(), us.end()), us.end());
    const std::vector<double> qr = quantiles(fr, us), qt = quantiles(ft, us);
    const double              a_s = (1.0 - s) * a_ref + s * a_tgt, b_s = (1.0 - s) * b_ref + s * b_tgt;
    std::vector<double>       qs(us.size());
    for (std::size_t i = 0; i < us.size(); ++i) qs[i] = (1.0 - s) * qr[i] + s * qt[i];
    qs.front() = a_s;
    qs.back()  = b_s;
    if (qs.size() < 2) return;

    // 補間後の帯の内側の bin i の幅 [i-0.5, i+0.5]（帯の端で切る）の境目。昇順に並ぶ。
    const int first = static_cast<int>(std::floor(a_s)) + 1;
    const int last  = static_cast<int>(std::ceil(b_s)) - 1;
    if (last < first) return;
    std::vector<double> edges;
    edges.push_back(std::max(a_s, first - 0.5));
    for (int i = first; i <= last; ++i) edges.push_back(std::min(b_s, i + 0.5));

    // 補間後の累積 F_s（Q_s の逆関数）を境目で求める。どちらも昇順なので続きから探す。
    std::vector<double> F(edges.size());
    std::size_t         j = 0;
    for (std::size_t e = 0; e < edges.size(); ++e) {
        const double x = edges[e];
        while (j + 1 < qs.size() - 1 && qs[j + 1] < x) ++j;
        const double q0 = qs[j], q1 = qs[j + 1];
        const double t  = q1 > q0 ? std::clamp((x - q0) / (q1 - q0), 0.0, 1.0) : 1.0;
        F[e]            = us[j] + t * (us[j + 1] - us[j]);
    }

    // 帯の質量の合計は率で幾何補間する。圧縮した密度を戻して（1/gamma 乗）パワーにする。
    const double ms       = geo_mix(mr, mt, s);
    const auto   to_power = [&](double normalized_density) {
        return std::pow(std::max(ms * normalized_density, 0.0), 1.0 / gamma);
    };

    // 帯の両端の値を、端に当たる base 側と target 側の値の幾何補間に合わせる（dB で直線の補正を足す）。
    // 端での補間後の密度は、分位関数の傾きの逆数 1 / ((1-s)/ρ_ref + s/ρ_tgt)（ρ は質量で割った密度）。
    const auto edge_db_shift = [&](double x_ref, double x_tgt, double d_ref, double d_tgt) {
        const double rr = d_ref / mr, rt = d_tgt / mt;
        const double want = geo_mix(std::max(value_at(ref, n, x_ref), 1e-300), std::max(value_at(tgt, n, x_tgt), 1e-300), s);
        if (!(rr > 0.0 && rt > 0.0)) return 0.0;
        const double p = to_power(1.0 / ((1.0 - s) / rr + s / rt));
        return p > 0.0 ? 10.0 * std::log10(want / p) : 0.0;
    };
    const double shift_a = edge_db_shift(a_ref, a_tgt, fr.dens.front(), ft.dens.front());
    const double shift_b = edge_db_shift(b_ref, b_tgt, fr.dens.back(), ft.dens.back());

    for (int i = first; i <= last; ++i) {
        const std::size_t e     = static_cast<std::size_t>(i - first);
        const double      width = edges[e + 1] - edges[e];
        if (!(width > 0.0)) continue;
        const double p = to_power((F[e + 1] - F[e]) / width);
        if (!(p > 0.0) || !std::isfinite(p)) continue;    // 質量の無い所は元のまま
        const double shift = shift_a + (shift_b - shift_a) * (i - a_s) / (b_s - a_s);
        out[i]             = p * std::pow(10.0, shift / 10.0);
    }
}

std::vector<double> morph_axis_freq_anchors(const std::vector<double>& ref_hz, const std::vector<double>& tgt_hz,
                                            double fx, double fs) {
    std::vector<double> knots = morph_axis_knots(ref_hz, tgt_hz, fx, fs);
    if (!knots.empty()) knots.pop_back();    // 上端は周波数アンカーではない
    return knots;
}

TransportStats transport_envelopes(MatrixXd& mix, const MatrixXd& ref, const MatrixXd& tgt, const AnchorMatrices& am,
                                   double end_ref, double end_tgt, double frame_s, const MorphRates& rate, int fs,
                                   double gamma, double range_lo_hz, double range_hi_hz) {
    TransportStats st;
    const Index    n = mix.rows(), nf = mix.cols();
    if (ref.rows() != n || tgt.rows() != n || ref.cols() != nf || tgt.cols() != nf || n < 4 || !(frame_s > 0.0))
        return st;
    const Index na = am.t_ref.size();

    // モーフ後の時間軸での列の時刻（tcmorph と同じ: 区間の長さを tx の率で幾何補間して積み上げる）。
    // 列 0 は始端、1..na は時間アンカー、na+1 は終端。
    std::vector<double> ta(static_cast<std::size_t>(na) + 2, 0.0);
    double              prev_r = 0.0, prev_t = 0.0;
    for (Index k = 0; k <= na; ++k) {
        const double tr = k < na ? am.t_ref[k] : end_ref;
        const double tt = k < na ? am.t_tgt[k] : end_tgt;
        if (!(tr > prev_r && tt > prev_t)) return st;
        ta[static_cast<std::size_t>(k) + 1] = ta[static_cast<std::size_t>(k)] + geo_mix(tr - prev_r, tt - prev_t, rate.tx);
        prev_r                               = tr;
        prev_t                               = tt;
    }

    // 各列の周波数ワープ（始端・終端は恒等写像）。
    std::vector<Column> cols(static_cast<std::size_t>(na) + 2);
    for (Index ii = 0; ii < na; ++ii) {
        std::vector<double> r, t;
        for (Index k = 0; k < am.tf_ref.rows() && am.tf_ref(k, ii) > 0.0; ++k) {
            r.push_back(am.tf_ref(k, ii));
            t.push_back(am.tf_tgt(k, ii));
        }
        cols[static_cast<std::size_t>(ii) + 1] = make_column(std::move(r), std::move(t), rate.fx, fs);
    }

    const double hz_per_bin = fs / (2.0 * static_cast<double>(n));    // tcmorph の中間の軸の目盛り
    const double lo_hz      = std::max(range_lo_hz, hz_per_bin);
    const double hi_hz      = std::min(range_hi_hz, static_cast<double>(n - 1) * hz_per_bin);
    const double top        = mix.colwise().sum().maxCoeff();
    if (!(top > 0.0) || !(hi_hz > lo_hz)) return st;

    double band_sum = 0.0;
    for (Index j = 0; j < nf; ++j) {
        const double before = mix.col(j).sum();
        if (!(before > 0.0) || 10.0 * std::log10(before / top) < -kSilenceDb) continue;

        // このフレームが入る列の区間と、その中での位置。tcmorph はこの間の周波数ワープを、base 側・
        // target 側それぞれ両端の列の写像の線形補間にする。
        const double t  = static_cast<double>(j) * frame_s;
        std::size_t  ii = 0;
        while (ii + 2 < ta.size() && ta[ii + 1] <= t) ++ii;
        const double  frac = std::clamp((t - ta[ii]) / (ta[ii + 1] - ta[ii]), 0.0, 1.0);
        const Column& c0   = cols[ii];
        const Column& c1   = cols[ii + 1];
        if (!c0.valid || !c1.valid) {
            ++st.skipped;
            continue;
        }
        const auto cur_ref = [&](double f) { return (1.0 - frac) * c0.map_ref(f) + frac * c1.map_ref(f); };
        const auto cur_tgt = [&](double f) { return (1.0 - frac) * c0.map_tgt(f) + frac * c1.map_tgt(f); };

        // 帯の端: 列の周波数アンカーの構造が、このフレームの揃えた包絡のどこにあるか（側ごと）。
        // 両隣の列にアンカーがあって本数が同じなら、両方から求めた位置を線形に補間する（フレームごとに
        // 帯が飛ばないように）。本数が違えば近い方の列、片方にしか無ければその列のものを使う。
        const auto landmarks = [&](const Column& c) {
            std::vector<std::pair<double, double>> l;
            for (std::size_t k = 0; k < c.ref.size(); ++k) l.emplace_back(cur_ref(c.ref[k]), cur_tgt(c.tgt[k]));
            return l;
        };
        std::vector<std::pair<double, double>> lm;
        const auto l0 = landmarks(c0), l1 = landmarks(c1);
        if (!l0.empty() && l0.size() == l1.size()) {
            for (std::size_t k = 0; k < l0.size(); ++k)
                lm.emplace_back((1.0 - frac) * l0[k].first + frac * l1[k].first,
                                (1.0 - frac) * l0[k].second + frac * l1[k].second);
        } else if (!l0.empty() && !l1.empty()) {
            lm = frac < 0.5 ? l0 : l1;
        } else {
            lm = l0.empty() ? l1 : l0;
        }

        // 帯の端は、隣り合うアンカーの構造の中間（谷のあたり）に置く。アンカーはフォルマントの山に
        // 打たれるので、アンカーの位置を端にすると山が2つの帯に半分ずつ分かれ、しかも端（山の頂上）の値が
        // 両側の幾何補間＝平均と同じ値に固定されて、平均で下がった山の高さがそのまま残ってしまう
        // （実データで凹凸が平均よりかえって弱くなった）。中間を端にすれば、山は1つの塊のまま運ばれる。
        std::vector<double> pr_in, pt_in;
        for (const auto& [pr, pt] : lm)
            if (pr > lo_hz && pr < hi_hz && pt > lo_hz && pt < hi_hz) {
                pr_in.push_back(pr);
                pt_in.push_back(pt);
            }
        bool monotone = true;
        for (std::size_t k = 1; k < pr_in.size(); ++k) monotone &= pr_in[k] > pr_in[k - 1] && pt_in[k] > pt_in[k - 1];
        if (!monotone) {
            ++st.skipped;
            continue;
        }
        std::vector<double> er { lo_hz }, et { lo_hz };
        for (std::size_t k = 1; k < pr_in.size(); ++k) {
            er.push_back(0.5 * (pr_in[k - 1] + pr_in[k]));
            et.push_back(0.5 * (pt_in[k - 1] + pt_in[k]));
        }
        er.push_back(hi_hz);
        et.push_back(hi_hz);

        int nb = 0;
        for (std::size_t k = 0; k + 1 < er.size(); ++k) {
            const double w_ref = er[k + 1] - er[k], w_tgt = et[k + 1] - et[k];
            if (w_ref < kMinBandHz || w_tgt < kMinBandHz || (1.0 - rate.sl) * w_ref + rate.sl * w_tgt < kMinBandHz)
                continue;
            transport_band(ref.col(j).data(), tgt.col(j).data(), mix.col(j).data(), static_cast<int>(n),
                           er[k] / hz_per_bin, er[k + 1] / hz_per_bin, et[k] / hz_per_bin, et[k + 1] / hz_per_bin, rate.sl,
                           gamma);
            ++nb;
        }
        if (nb == 0) continue;
        mix.col(j) *= before / mix.col(j).sum();    // フレームのパワーの和は平均のまま
        ++st.frames;
        band_sum += nb;
    }
    st.bands = st.frames > 0 ? band_sum / st.frames : 0.0;
    return st;
}
