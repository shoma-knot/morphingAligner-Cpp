#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "app.hpp"

namespace {

// Colour of the time-axis anchors and their connectors.
constexpr ImVec4 kAnchorCol { 1.0f, 0.35f, 0.2f, 1.0f };

// Screen-space position of an anchor on the edge facing the other panel,
// used to draw the cross-panel connectors.
struct EdgePoint {
    ImVec2 pos;
    bool   visible;    // false when the anchor time is outside the zoomed view
};

// Load / play controls plus info for a single track, in the left panel.
void draw_track_controls(App& app, Track& tr, const char* label) {
    ImGui::PushID(&tr);
    ImGui::TextUnformatted(label);

    if (ImGui::Button("読み込む", ImVec2(-1, 0))) load_track(tr);

    ImGui::BeginDisabled(!tr.loaded());
    if (ImGui::Button("再生", ImVec2(-1, 0))) {
        try {
            app.engine.play_oneshot(tr.path);
        } catch (const std::exception& e) {
            tr.status = std::string { "再生失敗: " } + e.what();
        }
    }
    ImGui::EndDisabled();

    if (tr.loaded()) {
        ImGui::TextWrapped("%s", tr.path.c_str());
        ImGui::Text("%d Hz, %.2f s", tr.spec.fs, tr.spec.duration);
    } else {
        ImGui::TextDisabled("未読み込み");
    }
    if (!tr.status.empty()) ImGui::TextWrapped("%s", tr.status.c_str());
    ImGui::PopID();
}

// One spectral-envelope spectrogram of the given pixel height, with the
// draggable time anchors overlaid. `is_base` selects which side of each
// anchor pair this plot edits. `out_edges` receives, per anchor, the pixel
// position on the edge facing the other panel (empty if nothing was drawn).
void draw_spectrogram(App& app, Track& tr, bool is_base, float height, std::vector<EdgePoint>& out_edges) {
    out_edges.clear();

    ImGui::PushID(&tr);
    ImGui::TextUnformatted(tr.name.c_str());

    if (!tr.loaded()) {
        ImGui::TextDisabled("音声を読み込むとスペクトル包絡を表示します");
        ImGui::PopID();
        return;
    }

    const Spectrogram& sp = tr.spec;
    ImPlot::PushColormap(kColormap);

    // Reserve room on the right for the dB colour scale (legend).
    constexpr float kScaleW = 90.0f;
    const float     plot_w  = ImGui::GetContentRegionAvail().x - kScaleW;

    // Free the right mouse button for deleting anchors: NoMenus disables the
    // default context menu, NoBoxSelect the right-drag zoom selection.
    if (ImPlot::BeginPlot("##spec", ImVec2(plot_w, height), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        // Lock the frequency axis so the scroll wheel over the plot area only
        // zooms time (X). The Y range is driven by our own state (`tr.y_min/max`)
        // so we can still zoom it manually when the axis itself is hovered.
        ImPlot::SetupAxes("時間 [s]", "周波数 [Hz]", ImPlotAxisFlags_None, ImPlotAxisFlags_Lock);
        // X once (then free to zoom/pan); Y always follows our stored view.
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, sp.duration, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, tr.y_min, tr.y_max, ImPlotCond_Always);
        // Keep the time axis within [0, duration]: no panning/zooming past the
        // data range (stops zoom-out at the full waveform).
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, sp.duration);
        // Pre-baked texture: one quad regardless of the bin * frame count.
        ImPlot::PlotImage(
          "##env", static_cast<ImTextureID>(tr.tex), ImPlotPoint(0, 0), ImPlotPoint(sp.duration, sp.fs / 2.0));

        const ImPlotRect lim = ImPlot::GetPlotLimits();

        // ────────────────────────────────────────────────────────────────
        // FIXME(freq-axis-input): 周波数軸のズーム/パンを手組みしている箇所。
        //
        // ホイールを X 専用ズームにするため Y 軸を Lock しており、ImPlot が Y を
        // 一切駆動しなくなる。そのため Y の表示範囲を tr.y_min/tr.y_max に自前で
        // 保持し、毎フレーム SetupAxisLimits(Always) で再適用している。この二重
        // 管理は壊れやすい:
        //   - IsPlotHovered() ゲート: base/target の2段プロットをまたぐ中ドラッグ
        //     でホバーが切り替わり、もう片方のプロットがパンし始めることがある。
        //   - Y 側の新しい操作（fit・links・2本目のY軸）や Lock の解除で、ImPlot
        //     の軸状態と自前状態が静かにデシンクする。
        //   - ズーム/パンの計算は Y が線形・非反転・[0, fs/2] である前提。
        // 拡張するなら、この手組みを伸ばすより ImPlot に軸を任せる方向（軸ごとの
        // 入力フラグ / リンクされた limits 等）を優先すること。
        // ────────────────────────────────────────────────────────────────

        // 周波数軸ラベル上でのホイールは、カーソル位置を中心に Y をズームする。
        // （プロット領域上のホイールは Y が Lock されているので X しか動かない）
        ImGuiIO& io = ImGui::GetIO();
        if (ImPlot::IsAxisHovered(ImAxis_Y1) && io.MouseWheel != 0.0f) {
            const double yc     = ImPlot::GetPlotMousePos().y;
            const double factor = std::pow(1.0 - ImPlot::GetInputMap().ZoomRate, static_cast<double>(io.MouseWheel));
            double       lo     = std::max(0.0, yc + (tr.y_min - yc) * factor);
            double       hi     = std::min(sp.fs / 2.0, yc + (tr.y_max - yc) * factor);
            if (hi - lo > 1.0) {    // keep at least a 1 Hz span
                tr.y_min = lo;
                tr.y_max = hi;
            }
        }

        // 中ドラッグで周波数軸もパンする（X は ImPlot がネイティブにパンする。Y は
        // Lock しているので、縦方向のドラッグ量ぶん自前の表示範囲をずらす。カーソル
        // 下の点がカーソルに追従する grab 方式）。
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const double h     = ImPlot::GetPlotSize().y;
            double       delta = io.MouseDelta.y / h * (tr.y_max - tr.y_min);
            delta              = std::clamp(delta, -tr.y_min, sp.fs / 2.0 - tr.y_max);
            tr.y_min += delta;
            tr.y_max += delta;
        }
        // ─── FIXME(freq-axis-input) ここまで ─────────────────────────────

        // Draggable time anchors. DragLineX stays interactive (movable) rather
        // than being baked into the draw list.
        bool any_active     = false;
        int  hovered_anchor = -1;
        for (std::size_t i = 0; i < app.anchors.size(); ++i) {
            double* xp      = is_base ? &app.anchors[i].base_t : &app.anchors[i].target_t;
            bool    hovered = false, held = false;
            ImPlot::DragLineX(
              static_cast<int>(i), xp, kAnchorCol, 2.0f, ImPlotDragToolFlags_None, nullptr, &hovered, &held);
            if (hovered || held) {
                any_active     = true;
                hovered_anchor = static_cast<int>(i);
            }
        }

        // Right-click on a hovered anchor deletes the whole pair (both panels).
        if (hovered_anchor >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            app.anchors.erase(app.anchors.begin() + hovered_anchor);
        }
        // Left-click on empty plot area adds a new anchor pair at that time
        // (same time on both tracks initially).
        else if (ImPlot::IsPlotHovered() && !any_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const double t = std::clamp(ImPlot::GetPlotMousePos().x, 0.0, sp.duration);
            app.anchors.push_back(Anchor { t, t });
        }

        // Capture each anchor's pixel position for the connectors. The vertical
        // position is pinned to the plot's pixel edge facing the other panel
        // (base: bottom, target: top) so frequency-axis zoom never moves it;
        // only the horizontal position follows the (zoomable) time axis.
        const ImVec2 plot_pos  = ImPlot::GetPlotPos();
        const ImVec2 plot_size = ImPlot::GetPlotSize();
        const float  edge_py   = is_base ? plot_pos.y + plot_size.y : plot_pos.y;
        out_edges.resize(app.anchors.size());
        for (std::size_t i = 0; i < app.anchors.size(); ++i) {
            const double x       = is_base ? app.anchors[i].base_t : app.anchors[i].target_t;
            out_edges[i].pos     = ImVec2(ImPlot::PlotToPixels(x, 0.0).x, edge_py);
            out_edges[i].visible = x >= lim.X.Min && x <= lim.X.Max;
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("dB", sp.db_min, sp.db_max, ImVec2(kScaleW, height));

    ImPlot::PopColormap();
    ImGui::PopID();
}

}    // namespace

void draw_left_panel(App& app) {
    ImGui::TextUnformatted("操作");
    ImGui::Separator();

    draw_track_controls(app, app.base, "Base");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_track_controls(app, app.target, "Target");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("アンカー: %d", static_cast<int>(app.anchors.size()));
    ImGui::TextDisabled("左クリックで追加 / 右クリックで削除");
    ImGui::BeginDisabled(app.anchors.empty());
    if (ImGui::Button("アンカーを全消去", ImVec2(-1, 0))) app.anchors.clear();
    ImGui::EndDisabled();
}

void draw_right_panel(App& app) {
    const float labels_h = 2.0f * ImGui::GetTextLineHeightWithSpacing();
    const float avail_h  = ImGui::GetContentRegionAvail().y;
    const float each_h   = std::max(140.0f, (avail_h - labels_h - 12.0f) / 2.0f);

    static std::vector<EdgePoint> base_edges, target_edges;
    draw_spectrogram(app, app.base, /*is_base=*/true, each_h, base_edges);
    ImGui::Spacing();
    draw_spectrogram(app, app.target, /*is_base=*/false, each_h, target_edges);

    // Connect corresponding base/target anchors across the gap between panels.
    if (base_edges.size() == target_edges.size()) {
        ImDrawList* dl  = ImGui::GetForegroundDrawList();
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(kAnchorCol);
        for (std::size_t i = 0; i < base_edges.size(); ++i) {
            if (base_edges[i].visible && target_edges[i].visible)
                dl->AddLine(base_edges[i].pos, target_edges[i].pos, col, 1.5f);
        }
    }
}
