#include "auto_anchors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>


namespace {

constexpr double kEps = 1e-4;    // 同じ時刻とみなす差 [s]（アンカーの重複・ポーズの有無の判定）

// 無音を除いた音素の区間。
std::vector<const SegInterval*> spoken_phones(const Segmentation& seg) {
    std::vector<const SegInterval*> out;
    if (const SegTier* tier = seg.phones())
        for (const SegInterval& iv : tier->intervals)
            if (!is_silence_label(iv.label)) out.push_back(&iv);
    return out;
}

// 音素列を空白区切りの文字列にする（ログ用）。
std::string join_labels(const std::vector<const SegInterval*>& ph) {
    std::string s;
    for (const SegInterval* iv : ph) {
        if (!s.empty()) s += ' ';
        s += iv->label;
    }
    return s;
}

// 移動平均フォルマントの時刻 t での値 [Hz]（前後の点を線形補間）。範囲外、または前後の
// どちらかが NaN（推定できなかった区間の切れ目）なら NaN。
double value_at(const FormantTrack& tr, double t) {
    const auto it = std::lower_bound(tr.t.begin(), tr.t.end(), t);
    if (it == tr.t.end()) return NAN;
    const std::size_t i = static_cast<std::size_t>(it - tr.t.begin());
    if (tr.t[i] == t) return tr.hz[i];
    if (i == 0) return NAN;
    const double t0 = tr.t[i - 1], t1 = tr.t[i];
    const double h0 = tr.hz[i - 1], h1 = tr.hz[i];
    if (std::isnan(h0) || std::isnan(h1)) return NAN;
    return h0 + (h1 - h0) * (t - t0) / (t1 - t0);
}

}    // namespace

AutoAnchorResult generate_auto_anchors(const AutoAnchorInput& base, const AutoAnchorInput& target, int divisions) {
    AutoAnchorResult r;
    divisions = std::max(1, divisions);

    const auto pb = spoken_phones(base.segmentation);
    const auto pt = spoken_phones(target.segmentation);
    if (pb.empty() || pt.empty()) {
        r.error = "音素アライメントの結果がありません";
        return r;
    }
    if (pb.size() != pt.size()) {
        r.error = "base と target で音素の数が違うため対応がとれません（base " + std::to_string(pb.size())
                + " 個: " + join_labels(pb) + " / target " + std::to_string(pt.size())
                + " 個: " + join_labels(pt) + "）";
        return r;
    }
    int mismatched = 0;
    for (std::size_t i = 0; i < pb.size(); ++i)
        if (pb[i]->label != pt[i]->label) ++mismatched;
    if (mismatched > 0)
        r.warnings.push_back("音素のラベルが base と target で違う箇所が " + std::to_string(mismatched)
                             + " 個あります（順番どおりに対応させました）: base " + join_labels(pb)
                             + " / target " + join_labels(pt));

    // ── 時間アンカー ──
    // 同じ時刻に重ねて打たないよう、直前に打った時刻より（両側とも）先のときだけ追加する。
    std::vector<std::pair<double, double>> times;    // (base_t, target_t)
    const auto push = [&](double tb, double tt) {
        if (tb <= kEps || tt <= kEps || tb >= base.duration - kEps || tt >= target.duration - kEps)
            return;    // 音声の範囲外（両端）は打たない
        if (!times.empty() && (tb <= times.back().first + kEps || tt <= times.back().second + kEps)) return;
        times.emplace_back(tb, tt);
    };
    const std::size_t n = pb.size();
    for (std::size_t i = 0; i < n; ++i) {
        const SegInterval& b = *pb[i];
        const SegInterval& t = *pt[i];
        push(b.start, t.start);    // 音素の始まり（＝前の音素との境界）
        for (int k = 1; k < divisions; ++k) {    // 音素の区間を等分する位置
            const double f = static_cast<double>(k) / divisions;
            push(b.start + (b.end - b.start) * f, t.start + (t.end - t.start) * f);
        }
        // 音素の終わり: 最後の音素か、次の音素との間に両側ともポーズ（無音区間）がある場合。
        // 片側だけにポーズがあると、ポーズの無い側では次の音素の始まりと同じ時刻になり
        // 時間アンカーが重なるので打たない。
        const bool last  = i + 1 == n;
        const bool pause = !last && pb[i + 1]->start > b.end + kEps && pt[i + 1]->start > t.end + kEps;
        if (last || pause) push(b.end, t.end);
    }

    // ── 周波数アンカー ──
    // 各時間アンカーの時刻で移動平均フォルマント F_k を読み、両側で値が取れたものを対にする。
    const std::size_t n_tracks = std::min(base.formants_ma.tracks.size(), target.formants_ma.tracks.size());
    for (const auto& [tb, tt] : times) {
        Anchor a { tb, tt, {} };
        for (std::size_t k = 0; k < n_tracks; ++k) {
            const double fb = value_at(base.formants_ma.tracks[k], tb);
            const double ft = value_at(target.formants_ma.tracks[k], tt);
            if (std::isnan(fb) || std::isnan(ft)) continue;
            a.freqs.push_back(FreqAnchor { fb, ft });
        }
        r.n_freq += static_cast<int>(a.freqs.size());
        r.anchors.push_back(std::move(a));
    }
    if (r.anchors.empty()) r.error = "アンカーを打てる位置がありませんでした";
    return r;
}
