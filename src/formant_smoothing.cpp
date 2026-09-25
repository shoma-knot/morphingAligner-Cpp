#include "formant_smoothing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

#include "freqscale.hpp"

Formants smooth_formants(const Formants& f, double window_s) {
    const double half = window_s * 0.5;
    const double nan  = std::numeric_limits<double>::quiet_NaN();

    Formants out;
    for (const FormantTrack& tr : f.tracks) {
        FormantTrack s;
        const std::size_t n = tr.t.size();

        // 連続区間の判定: 最小のフレーム間隔の 2.5 倍より空いていたら、推定できなかった
        // フレームを挟んでいるとみなす（その前後をまたいで平均をとらない）。
        double step = std::numeric_limits<double>::infinity();
        for (std::size_t i = 1; i < n; ++i)
            if (tr.t[i] > tr.t[i - 1]) step = std::min(step, tr.t[i] - tr.t[i - 1]);
        const double gap = std::isfinite(step) ? step * 2.5 : 0.0;

        for (std::size_t i = 0; i < n;) {
            std::size_t e = i + 1;    // [i, e) が連続区間
            while (e < n && tr.t[e] - tr.t[e - 1] <= gap) ++e;

            // 区間の境目に NaN を1点挟むと、PlotLine が線を途切れさせる。
            if (!s.t.empty()) {
                s.t.push_back((s.t.back() + tr.t[i]) * 0.5);
                s.hz.push_back(nan);
                s.erb.push_back(nan);
            }

            // 時刻 t[k] を中心とする ±half の窓を尺取りで動かす（区間の端では窓が欠ける分
            // だけ点数が減る）。平均は Hz で取り、表示用に ERB へ変換する。
            std::size_t lo = i, hi = i;
            double      sum = 0.0;
            for (std::size_t k = i; k < e; ++k) {
                while (hi < e && tr.t[hi] <= tr.t[k] + half) sum += tr.hz[hi++];
                while (tr.t[lo] < tr.t[k] - half) sum -= tr.hz[lo++];
                const double m = sum / static_cast<double>(hi - lo);
                s.t.push_back(tr.t[k]);
                s.hz.push_back(m);
                s.erb.push_back(freqscale::hz_to_erb(m));
            }
            i = e;
        }
        out.tracks.push_back(std::move(s));
    }
    return out;
}
