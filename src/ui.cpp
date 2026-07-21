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

        // ドラッグ可能な時間アンカー（縦線）と、その線上に乗る周波数アンカー（点）。
        // DragLineX / DragPoint は draw list に焼かず、掴んで動かせる。
        //
        // 同じ x 上に線と点があり、ドラッグ対象が競合するため Ctrl で役割を切り替える:
        //   Ctrl なし … 時間線をドラッグ可 / 周波数点はロック
        //   Ctrl あり … 時間線をロック     / 周波数点をドラッグ可（＋線上で追加）
        // Ctrl 中に線を NoInputs にすると線上でも IsPlotHovered() が true になり、
        // 周波数アンカーの追加検出もできる。
        // 点は Delayed（描画を1フレーム遅延）にする。DragPoint がマウス位置で更新した
        // x を即描画せず、こちらが渡す「線に固定した x」で描画するので、ドラッグ中も
        // 点が線上に留まる（更新された x は捨て、y だけ採用）。
        const ImPlotDragToolFlags line_flags  = io.KeyCtrl ? ImPlotDragToolFlags_NoInputs : ImPlotDragToolFlags_None;
        const ImPlotDragToolFlags point_flags = io.KeyCtrl ? ImPlotDragToolFlags_Delayed : ImPlotDragToolFlags_NoInputs;

        bool any_active     = false;
        int  hovered_anchor = -1;    // ホバー中の時間アンカー index（線）
        int  hover_fi = -1, hover_fj = -1;    // ホバー中の周波数アンカー (anchor, freq) index
        for (std::size_t i = 0; i < app.anchors.size(); ++i) {
            Anchor& a       = app.anchors[i];
            double* xp      = is_base ? &a.base_t : &a.target_t;
            bool    hovered = false, held = false;
            ImPlot::DragLineX(static_cast<int>(i), xp, kAnchorCol, 2.0f, line_flags, nullptr, &hovered, &held);
            if (hovered || held) {
                any_active     = true;
                hovered_anchor = static_cast<int>(i);
            }

            // この時間アンカー線上の周波数アンカー。X は線（アンカー時刻）に固定し、
            // Y（周波数）だけドラッグで動かす。時間アンカーを動かせば一緒に横移動する。
            for (std::size_t j = 0; j < a.freqs.size(); ++j) {
                double    fx     = is_base ? a.base_t : a.target_t;    // 線に固定（毎フレーム再設定）
                double*   fy     = is_base ? &a.freqs[j].base_f : &a.freqs[j].target_f;
                const int fid    = (static_cast<int>(i) + 1) * 4096 + static_cast<int>(j);
                bool      fh = false, fheld = false;
                ImPlot::DragPoint(fid, &fx, fy, kAnchorCol, 5.0f, point_flags, nullptr, &fh, &fheld);
                *fy = std::clamp(*fy, 0.0, sp.fs / 2.0);    // 範囲内に維持（fx は捨てて線上に固定）
                if (fh || fheld) {
                    any_active = true;
                    hover_fi   = static_cast<int>(i);
                    hover_fj   = static_cast<int>(j);
                }

                // 時間アンカー内の並び順（1始まり）を点の脇に表示。base/target で同じ
                // 番号になり対応が分かる。表示範囲外の点はラベルを出さない（端に張り付か
                // せない）ため、現在の表示範囲内にあるときだけ描画する。
                const double lx = is_base ? a.base_t : a.target_t;    // 線に固定した x
                const bool   in_view =
                  lx >= lim.X.Min && lx <= lim.X.Max && *fy >= lim.Y.Min && *fy <= lim.Y.Max;
                if (in_view)
                    ImPlot::Annotation(lx, *fy, kAnchorCol, ImVec2(8, -8), false, "%d", static_cast<int>(j) + 1);
            }
        }

        // Ctrl+右クリック: ホバー中の周波数アンカーを削除。
        if (io.KeyCtrl && hover_fj >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            auto& fs = app.anchors[hover_fi].freqs;
            fs.erase(fs.begin() + hover_fj);
        }
        // 右クリック: ホバー中の時間アンカー（ペア＝両パネル＋周波数アンカー）を削除。
        else if (hovered_anchor >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            app.anchors.erase(app.anchors.begin() + hovered_anchor);
        }
        // Ctrl+左クリック（点以外の線上）: カーソルに最も近い時間アンカー線上に、
        // クリックした周波数で周波数アンカー（ペア）を追加する。点の上（any_active）は
        // ドラッグ移動なので追加しない。
        else if (io.KeyCtrl && !any_active && ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
                const double f = std::clamp(ImPlot::GetPlotMousePos().y, 0.0, sp.fs / 2.0);
                app.anchors[nearest].freqs.push_back(FreqAnchor { f, f });
            }
        }
        // 左クリック（何もない所）: その時刻に新しい時間アンカー（ペア）を追加。
        else if (!io.KeyCtrl && ImPlot::IsPlotHovered() && !any_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
    ImGui::TextDisabled("Ctrl+左クリックで周波数アンカー追加/ドラッグで移動");
    ImGui::TextDisabled("Ctrl+右クリックで周波数アンカー削除");
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
