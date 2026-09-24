#include "speech_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include <implot.h>

#include "app.hpp"
#include "freqscale.hpp"

namespace {

// 音素の区間の色。隣り合う区間を見分けられるよう、偶数/奇数番目で明るさを変える。
constexpr ImU32 kPhoneFill[2] = { IM_COL32(55, 135, 105, 120), IM_COL32(75, 160, 125, 120) };
constexpr ImU32 kUnknownFill  = IM_COL32(200, 120, 40, 150);    // spn（辞書にない語＝発話雑音扱い）
constexpr ImU32 kBoundaryCol  = IM_COL32(230, 230, 230, 150);
constexpr ImU32 kHoverCol     = IM_COL32(255, 255, 255, 230);
constexpr ImU32 kTextCol      = IM_COL32(255, 255, 255, 255);

constexpr float kRawAlphaWithMa = 0.35f;    // 移動平均を重ねるときの元の点の不透明度

}    // namespace

ImVec4 formant_color(int k) {
    // viridis の背景とアンカー（橙）の両方から見分けられる色。F1 から順に白・水色・桃・灰。
    static const ImVec4 kCols[] = {
        { 1.00f, 1.00f, 1.00f, 1.0f },
        { 0.35f, 0.90f, 1.00f, 1.0f },
        { 1.00f, 0.55f, 0.95f, 1.0f },
        { 0.75f, 0.75f, 0.75f, 1.0f },
    };
    constexpr int n = static_cast<int>(sizeof kCols / sizeof kCols[0]);
    return kCols[((k % n) + n) % n];
}

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

void draw_formants(const Formants& f, const Formants* ma) {
    for (int k = 0; k < static_cast<int>(f.tracks.size()); ++k) {
        const FormantTrack& tr = f.tracks[static_cast<std::size_t>(k)];
        if (tr.t.empty()) continue;
        ImVec4 col = formant_color(k);
        if (ma != nullptr) col.w = kRawAlphaWithMa;    // 移動平均を主役にして元の点は薄く
        ImPlotSpec spec;
        spec.Marker          = ImPlotMarker_Circle;
        spec.MarkerSize      = 1.5f;
        spec.MarkerFillColor = col;
        spec.MarkerLineColor = col;
        spec.Flags           = ImPlotItemFlags_NoFit;    // 軸の自動フィットに関与させない
        char label[16];
        std::snprintf(label, sizeof label, "##F%d", k + 1);
        ImPlot::PlotScatter(label, tr.t.data(), tr.erb.data(), static_cast<int>(tr.t.size()), spec);
    }

    if (ma == nullptr) return;
    for (int k = 0; k < static_cast<int>(ma->tracks.size()); ++k) {
        const FormantTrack& tr = ma->tracks[static_cast<std::size_t>(k)];
        if (tr.t.empty()) continue;
        ImPlotSpec spec;
        spec.LineColor  = formant_color(k);
        spec.LineWeight = 1.5f;
        spec.Flags      = ImPlotItemFlags_NoFit;    // NaN は線の切れ目として描かれる（SkipNaN なし）
        char label[16];
        std::snprintf(label, sizeof label, "##F%dma", k + 1);
        ImPlot::PlotLine(label, tr.t.data(), tr.erb.data(), static_cast<int>(tr.t.size()), spec);
    }
}

float segmentation_plot_height() {
    return ImGui::GetTextLineHeight() + 8.0f + 2.0f * ImPlot::GetStyle().PlotPadding.y + 2.0f;
}

void draw_segmentation(Track& tr, float width, float height) {
    const SegTier* tier = tr.segmentation.phones();
    const double   dur  = tr.spec.duration;

    ImGui::PushID("segmentation");
    // 軸の装飾・メニュー・範囲選択は不要。時間軸のズーム/パン（ホイール・中ドラッグ）は
    // リンクしているのでスペクトログラムと連動する。
    if (!ImPlot::BeginPlot("##seg", ImVec2(width, height), ImPlotFlags_CanvasOnly)) {
        ImGui::PopID();
        return;
    }
    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations,
                      ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks);
    ImPlot::SetupAxisLinks(ImAxis_X1, &tr.view_x0, &tr.view_x1);
    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, dur);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 1, ImPlotCond_Always);
    // Y 目盛りに「音素」を出す。結果が無くても出しておき、実行前後でプロット領域の幅
    // （＝スペクトログラムとの揃え）が変わらないようにする。
    const double      tick_pos[1] = { 0.5 };
    const char* const tick_lab[1] = { "音素" };
    ImPlot::SetupAxisTicks(ImAxis_Y1, tick_pos, 1, tick_lab, false);

    const ImPlotRect lim = ImPlot::GetPlotLimits();
    ImDrawList*      dl  = ImPlot::GetPlotDrawList();

    if (tier == nullptr) {
        const char*  msg = tr.align_busy ? "音素アライメントを実行中..." : "音素アライメント未実行";
        const ImVec2 ts  = ImGui::CalcTextSize(msg);
        const ImVec2 p   = ImPlot::GetPlotPos();
        const ImVec2 s   = ImPlot::GetPlotSize();
        dl->AddText(ImVec2(p.x + (s.x - ts.x) * 0.5f, p.y + (s.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
        ImPlot::EndPlot();
        ImGui::PopID();
        return;
    }

    // カーソル下の区間（ツールチップと枠の強調用）。
    const bool         hovered = ImPlot::IsPlotHovered();
    const ImPlotPoint  mouse   = ImPlot::GetPlotMousePos();
    const SegInterval* hover   = nullptr;

    ImPlot::PushPlotClipRect();
    for (std::size_t i = 0; i < tier->intervals.size(); ++i) {
        const SegInterval& iv = tier->intervals[i];
        if (iv.end < lim.X.Min || iv.start > lim.X.Max) continue;    // 表示範囲外
        const ImVec2 a = ImPlot::PlotToPixels(iv.start, 1.0);
        const ImVec2 b = ImPlot::PlotToPixels(iv.end, 0.0);

        if (!is_silence_label(iv.label))
            dl->AddRectFilled(a, b, iv.label == "spn" ? kUnknownFill : kPhoneFill[i % 2]);
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, b.y), kBoundaryCol);
        dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, b.y), kBoundaryCol);

        // ラベルは区間に収まるときだけ出す（収まらないものはホバーで読む）。
        if (!is_silence_label(iv.label)) {
            const ImVec2 ts = ImGui::CalcTextSize(iv.label.c_str());
            if (ts.x + 4.0f <= b.x - a.x)
                dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), kTextCol,
                            iv.label.c_str());
        }

        if (hovered && mouse.x >= iv.start && mouse.x < iv.end) hover = &iv;
    }
    if (hover != nullptr)
        dl->AddRect(ImPlot::PlotToPixels(hover->start, 1.0), ImPlot::PlotToPixels(hover->end, 0.0), kHoverCol,
                    0.0f, 0, 2.0f);
    ImPlot::PopPlotClipRect();

    if (hover != nullptr) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(is_silence_label(hover->label) ? "（無音）" : hover->label.c_str());
        ImGui::Text("%.3f - %.3f s（%.0f ms）", hover->start, hover->end, (hover->end - hover->start) * 1000.0);
        ImGui::EndTooltip();
    }

    ImPlot::EndPlot();
    ImGui::PopID();
}
