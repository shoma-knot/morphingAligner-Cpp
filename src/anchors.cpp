#include "anchors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "app.hpp"
#include "freqscale.hpp"

namespace {

// アンカーとその対応線の色。
constexpr ImVec4 kAnchorCol { 1.0f, 0.35f, 0.2f, 1.0f };

// ── 時間アンカー（縦線） ─────────────────────────────────────
// 各時間アンカーを DragLineX で描く。ホバー/ドラッグ中のものを any_active/hovered に返す。
void draw_time_lines(App& app, bool is_base, ImPlotDragToolFlags flags, bool& any_active, int& hovered) {
    for (std::size_t i = 0; i < app.anchors.size(); ++i) {
        double* xp = is_base ? &app.anchors[i].base_t : &app.anchors[i].target_t;
        bool    h = false, held = false;
        ImPlot::DragLineX(static_cast<int>(i), xp, kAnchorCol, 2.0f, flags, nullptr, &h, &held);
        if (h || held) {
            any_active = true;
            hovered    = static_cast<int>(i);
        }
    }
}

// ── 周波数アンカー（点＋番号） ───────────────────────────────
// 各時間アンカー線上の周波数アンカーを DragPoint で描く。X は線に固定し Y のみ移動。
// ホバー/ドラッグ中の点 (anchor, freq) index を hover_fi/hover_fj に返す。
void draw_freq_points(App& app, bool is_base, const Spectrogram& sp, const ImPlotRect& lim,
                      ImPlotDragToolFlags flags, bool& any_active, int& hover_fi, int& hover_fj) {
    for (std::size_t i = 0; i < app.anchors.size(); ++i) {
        Anchor& a = app.anchors[i];
        for (std::size_t j = 0; j < a.freqs.size(); ++j) {
            double    fx  = is_base ? a.base_t : a.target_t;    // 線に固定（毎フレーム再設定）
            double*   fy  = is_base ? &a.freqs[j].base_f : &a.freqs[j].target_f;
            const int fid = (static_cast<int>(i) + 1) * 4096 + static_cast<int>(j);
            // Y軸は ERB レートなので、保持している Hz を ERB にして DragPoint に渡し、
            // ドラッグ結果（ERB）を Hz に戻す。
            double ey = freqscale::hz_to_erb(*fy);
            bool   h = false, held = false;
            ImPlot::DragPoint(fid, &fx, &ey, kAnchorCol, 5.0f, flags, nullptr, &h, &held);
            *fy = std::clamp(freqscale::erb_to_hz(ey), 0.0, sp.fs / 2.0);    // 範囲内に維持（fx は捨てて線上固定）
            if (h || held) {
                any_active = true;
                hover_fi   = static_cast<int>(i);
                hover_fj   = static_cast<int>(j);
            }

            // 時間アンカー内の並び順（1始まり）を点の脇に表示。base/target 同番号＝対応。
            // 表示範囲外の点はラベルを出さない。位置は ERB 座標。
            const double lx  = is_base ? a.base_t : a.target_t;
            const double eyl = freqscale::hz_to_erb(*fy);
            const bool   in_view =
              lx >= lim.X.Min && lx <= lim.X.Max && eyl >= lim.Y.Min && eyl <= lim.Y.Max;
            if (in_view)
                ImPlot::Annotation(lx, eyl, kAnchorCol, ImVec2(8, -8), false, "%d", static_cast<int>(j) + 1);
        }
    }
}

// ── 時間アンカーの追加/削除（Ctrl なし） ─────────────────────
void handle_time_input(App& app, const Spectrogram& sp, ImGuiIO& io, bool any_active, int hovered) {
    if (io.KeyCtrl) return;
    // 右クリック: ホバー中の時間アンカー（配下の周波数アンカーごと）を削除。
    if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        app.anchors.erase(app.anchors.begin() + hovered);
    }
    // 左クリック（何もない所）: その時刻に新しい時間アンカーを追加。
    else if (ImPlot::IsPlotHovered() && !any_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const double t = std::clamp(ImPlot::GetPlotMousePos().x, 0.0, sp.duration);
        app.anchors.push_back(Anchor { t, t });
    }
}

// ── 周波数アンカーの追加/削除（Ctrl あり） ───────────────────
void handle_freq_input(App& app, bool is_base, const Spectrogram& sp, ImGuiIO& io,
                       bool any_active, int hover_fi, int hover_fj) {
    if (!io.KeyCtrl) return;
    // Ctrl+右クリック: ホバー中の周波数アンカーを削除。
    if (hover_fj >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto& fs = app.anchors[hover_fi].freqs;
        fs.erase(fs.begin() + hover_fj);
    }
    // Ctrl+左クリック（点以外の線上）: カーソルに最も近い時間アンカー線上に、クリックした
    // 周波数で周波数アンカー（ペア）を追加。点の上（any_active）はドラッグ移動なので追加しない。
    else if (!any_active && ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int   nearest = -1;
        float best_px = 8.0f;    // 許容ピクセル距離
        for (std::size_t i = 0; i < app.anchors.size(); ++i) {
            const double at = is_base ? app.anchors[i].base_t : app.anchors[i].target_t;
            const float  d  = std::fabs(ImPlot::PlotToPixels(at, 0.0).x - io.MousePos.x);
            if (d < best_px) {
                best_px = d;
                nearest = static_cast<int>(i);
            }
        }
        if (nearest >= 0) {
            // クリック位置(Y=ERB)を Hz に変換して追加。
            const double f = std::clamp(freqscale::erb_to_hz(ImPlot::GetPlotMousePos().y), 0.0, sp.fs / 2.0);
            app.anchors[nearest].freqs.push_back(FreqAnchor { f, f });
        }
    }
}

// ── 対応線用の端点取得 ───────────────────────────────────────
// 縦位置はプロット矩形のピクセル端に固定（base=下端 / target=上端）。周波数ズームで
// 動かないよう、横位置のみ時間軸に追従させる。
void capture_edges(App& app, bool is_base, const ImPlotRect& lim, std::vector<EdgePoint>& out) {
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const float  edge_py   = is_base ? plot_pos.y + plot_size.y : plot_pos.y;
    out.resize(app.anchors.size());
    for (std::size_t i = 0; i < app.anchors.size(); ++i) {
        const double x = is_base ? app.anchors[i].base_t : app.anchors[i].target_t;
        out[i].pos     = ImVec2(ImPlot::PlotToPixels(x, 0.0).x, edge_py);
        out[i].visible = x >= lim.X.Min && x <= lim.X.Max;
    }
}

}    // namespace

void draw_anchors(App& app, bool is_base, const Spectrogram& sp, std::vector<EdgePoint>& out_edges) {
    ImGuiIO&         io  = ImGui::GetIO();
    const ImPlotRect lim = ImPlot::GetPlotLimits();

    // 同じ x 上に線と点があり、ドラッグ対象が競合するため Ctrl で役割を切り替える:
    //   Ctrl なし … 時間線ドラッグ可 / 周波数点ロック（NoInputs）
    //   Ctrl あり … 時間線ロック（NoInputs）/ 周波数点ドラッグ可
    // 点は Delayed（描画1フレーム遅延）でドラッグ中も渡した x（＝線上）に描画される。
    const ImPlotDragToolFlags line_flags  = io.KeyCtrl ? ImPlotDragToolFlags_NoInputs : ImPlotDragToolFlags_None;
    const ImPlotDragToolFlags point_flags = io.KeyCtrl ? ImPlotDragToolFlags_Delayed : ImPlotDragToolFlags_NoInputs;

    bool any_active = false;
    int  hovered    = -1;            // ホバー中の時間アンカー（線）
    int  hover_fi = -1, hover_fj = -1;    // ホバー中の周波数アンカー (anchor, freq)
    draw_time_lines(app, is_base, line_flags, any_active, hovered);
    draw_freq_points(app, is_base, sp, lim, point_flags, any_active, hover_fi, hover_fj);

    // 入力処理は Ctrl の有無で排他（どちらか一方だけが作用する）。
    handle_freq_input(app, is_base, sp, io, any_active, hover_fi, hover_fj);
    handle_time_input(app, sp, io, any_active, hovered);

    capture_edges(app, is_base, lim, out_edges);
}

void draw_anchor_connectors(const std::vector<EdgePoint>& base_edges, const std::vector<EdgePoint>& target_edges) {
    if (base_edges.size() != target_edges.size()) return;
    ImDrawList* dl  = ImGui::GetForegroundDrawList();
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(kAnchorCol);
    for (std::size_t i = 0; i < base_edges.size(); ++i) {
        if (base_edges[i].visible && target_edges[i].visible)
            dl->AddLine(base_edges[i].pos, target_edges[i].pos, col, 1.5f);
    }
}
