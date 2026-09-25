#include "anchors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "app.hpp"
#include "freqscale.hpp"

namespace {

// アンカーとその対応線の色。
constexpr ImVec4 kAnchorCol { 1.0f, 0.35f, 0.2f, 1.0f };

constexpr float kLineWidth = 1.0f;    // 時間アンカー線の太さ [px]
constexpr float kPointSize = 3.5f;    // 周波数アンカー点の大きさ [px]
constexpr float kPickPx    = 15.0f;   // 番号を出す対象とみなすカーソルとの距離 [px]
constexpr float kAddPx     = 8.0f;    // 周波数アンカー追加時に線とみなす距離 [px]

// カーソルが近い1本だけを目立たせる。以前「1本を強調して残り全部を減光」したときは、
// カーソルを動かすたびに 20 本以上が一斉に明滅してうるさかったので、既定値は固定して
// 変化を該当の線だけに閉じ込める。
//
// 手段は線の種類で使い分ける。プロット内の縦線は不透明のまま太さを変える（明るさを
// 変えると背景のスペクトログラムに対して見え方が大きく動く）。パネル間の対応線は
// 本数が多く帯になりやすいので普段から薄くしておき、太さは変えずに不透明度で示す。
constexpr float kLineWidthActive = 2.0f;     // カーソルが近い時間アンカー線の太さ [px]
constexpr float kConnectorWidth  = 1.5f;     // 対応線の太さ（常に一定）[px]
constexpr float kConnectorAlpha  = 0.35f;    // 対応線の既定の不透明度（該当の1本は不透明）

// 時間アンカー i の線の太さ。
float anchor_width(std::size_t i, int active) {
    return static_cast<int>(i) == active ? kLineWidthActive : kLineWidth;
}

// 対応線 i の色。
ImVec4 connector_col(std::size_t i, int active) {
    ImVec4 c = kAnchorCol;
    if (static_cast<int>(i) != active) c.w = kConnectorAlpha;
    return c;
}

// 番号ラベルの体裁（ImPlot::Annotation と同じ見た目を自前で描く。理由は draw_freq_labels）。
constexpr float kLabelOffsetX = 8.0f;    // 点からラベル左端までの距離 [px]
constexpr float kLabelOffsetY = 8.0f;    // 点からラベル中心までの距離（上向き）[px]
constexpr float kLabelGap     = 2.0f;    // ラベル同士の最小すき間 [px]

// カーソルに最も近い時間アンカー線を返す（max_px 以内。無ければ -1）。
int nearest_anchor(const App& app, Side side, const ImGuiIO& io, float max_px) {
    int   nearest = -1;
    float best    = max_px;
    for (std::size_t i = 0; i < app.anchors.size(); ++i) {
        const double at = app.anchors[i].time(side);
        const float  d  = std::fabs(ImPlot::PlotToPixels(at, 0.0).x - io.MousePos.x);
        if (d < best) {
            best    = d;
            nearest = static_cast<int>(i);
        }
    }
    return nearest;
}

// ── 時間アンカー（縦線） ─────────────────────────────────────
// 各時間アンカーを DragLineX で描く。ホバー/ドラッグ中のものを any_active/hovered に返す。
void draw_time_lines(App& app, Side side, ImPlotDragToolFlags flags, int active, bool& any_active,
                     int& hovered) {
    for (std::size_t i = 0; i < app.anchors.size(); ++i) {
        double* xp = &app.anchors[i].time(side);
        bool    h = false, held = false;
        ImPlot::DragLineX(static_cast<int>(i), xp, kAnchorCol, anchor_width(i, active), flags, nullptr,
                          &h, &held);
        if (h || held) {
            any_active = true;
            hovered    = static_cast<int>(i);
        }
    }
}

// ── 周波数アンカーの番号ラベル ───────────────────────────────
// 時間アンカー内の並び順（1始まり）を点の脇に出す。base/target で同番号＝対応。
//
// 同じ線上の点は 2-5kHz 帯で ERB 上も詰まるため、点ごとに固定オフセットで描くと必ず
// 重なる。ここでは画面 Y でソートしてから最小すき間だけ押し広げ、点から離れたものには
// 引き出し線を引く。ImPlot::Annotation はオフセットの符号で箱の向きが変わり位置を厳密に
// 決められないので、同じ見た目（下地＋反転色の数字）を自前で描いている。
void draw_freq_labels(const Anchor& a, Side side, const ImPlotRect& lim) {
    struct Label {
        int    j;
        ImVec2 point;    // 点の画面座標
        float  y;        // ラベル中心の画面 Y（重なり解消後）
    };

    const double lx = a.time(side);
    if (lx < lim.X.Min || lx > lim.X.Max) return;    // 線ごと表示範囲外

    std::vector<Label> labels;
    labels.reserve(a.freqs.size());
    for (std::size_t j = 0; j < a.freqs.size(); ++j) {
        const double eyl = freqscale::hz_to_erb(a.freqs[j].freq(side));
        if (eyl < lim.Y.Min || eyl > lim.Y.Max) continue;
        const ImVec2 p = ImPlot::PlotToPixels(lx, eyl);
        labels.push_back(Label { static_cast<int>(j), p, p.y - kLabelOffsetY });
    }
    if (labels.empty()) return;

    const float  h         = ImGui::GetTextLineHeight() + 4.0f;    // ラベル1個の高さ（下地込み）
    const float  step      = h + kLabelGap;
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const float  top       = plot_pos.y + h * 0.5f;
    const float  bottom    = plot_pos.y + plot_size.y - h * 0.5f;

    // 上から順に、上端と最小すき間を守りながら下へ押し下げる。上端の制限を最初に
    // 織り込むのが要点で、最後にまとめてクランプするとせっかく空けたすき間が潰れる。
    std::sort(labels.begin(), labels.end(), [](const Label& x, const Label& y) { return x.y < y.y; });
    labels[0].y = std::max(labels[0].y, top);
    for (std::size_t k = 1; k < labels.size(); ++k)
        labels[k].y = std::max(labels[k].y, labels[k - 1].y + step);

    // 下へはみ出したら、今度は下端から上へ詰め直す。
    if (labels.back().y > bottom) {
        labels.back().y = bottom;
        for (int k = static_cast<int>(labels.size()) - 2; k >= 0; --k)
            labels[k].y = std::min(labels[k].y, labels[k + 1].y - step);
        // 本数が多すぎてプロット高さに収まらない場合だけ、上端で重なることを許す。
        for (Label& l : labels) l.y = std::max(l.y, top);
    }

    // ImPlot::Annotation と同じ規則で、下地に対して読める文字色を選ぶ。
    const float luma     = kAnchorCol.x * 0.299f + kAnchorCol.y * 0.587f + kAnchorCol.z * 0.114f;
    const ImU32 text_col = luma > 0.5f ? IM_COL32_BLACK : IM_COL32_WHITE;
    const ImU32 bg_col = ImGui::ColorConvertFloat4ToU32(kAnchorCol);

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    for (const Label& l : labels) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", l.j + 1);
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 tl(l.point.x + kLabelOffsetX, l.y - (ts.y + 4.0f) * 0.5f);
        const ImVec2 br(tl.x + ts.x + 8.0f, tl.y + ts.y + 4.0f);
        // どの点のラベルかを読むための線なので、距離によらず必ず引き、薄くもしない
        // （押し出された分だけ長くなる）。
        dl->AddLine(l.point, ImVec2(tl.x, l.y), bg_col, 1.0f);
        dl->AddRectFilled(tl, br, bg_col, 2.0f);
        dl->AddText(ImVec2(tl.x + 4.0f, tl.y + 2.0f), text_col, buf);
    }
    ImPlot::PopPlotClipRect();
}

// ── 周波数アンカー（点＋番号） ───────────────────────────────
// 各時間アンカー線上の周波数アンカーを DragPoint で描く。X は線に固定し Y のみ移動。
// ホバー/ドラッグ中の点 (anchor, freq) index を hover_fi/hover_fj に返す。
void draw_freq_points(App& app, Side side, const Spectrogram& sp, const ImPlotRect& lim,
                      ImPlotDragToolFlags flags, int active, bool& any_active, int& hover_fi,
                      int& hover_fj) {
    for (std::size_t i = 0; i < app.anchors.size(); ++i) {
        Anchor&    a  = app.anchors[i];
        const bool on = static_cast<int>(i) == active;
        for (std::size_t j = 0; j < a.freqs.size(); ++j) {
            double    fx  = a.time(side);    // 線に固定（毎フレーム再設定）
            double*   fy  = &a.freqs[j].freq(side);
            const int fid = (static_cast<int>(i) + 1) * 4096 + static_cast<int>(j);
            // Y軸は ERB レートなので、保持している Hz を ERB にして DragPoint に渡し、
            // ドラッグ結果（ERB）を Hz に戻す。
            double ey = freqscale::hz_to_erb(*fy);
            bool   h = false, held = false;
            ImPlot::DragPoint(fid, &fx, &ey, kAnchorCol, kPointSize, flags, nullptr, &h, &held);
            *fy = std::clamp(freqscale::erb_to_hz(ey), 0.0, sp.fs / 2.0);    // 範囲内に維持（fx は捨てて線上固定）
            if (h || held) {
                any_active = true;
                hover_fi   = static_cast<int>(i);
                hover_fj   = static_cast<int>(j);
            }

        }
        // 番号ラベルは点をすべて動かしたあとに、まとめて重なりを解いてから描く。
        if (on) draw_freq_labels(a, side, lim);
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
        app.anchors.push_back(Anchor { t, t, {} });
    }
}

// ── 周波数アンカーの追加/削除（Ctrl あり） ───────────────────
void handle_freq_input(App& app, Side side, const Spectrogram& sp, ImGuiIO& io,
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
        const int nearest = nearest_anchor(app, side, io, kAddPx);
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
void capture_edges(App& app, Side side, const ImPlotRect& lim, std::vector<EdgePoint>& out) {
    const ImVec2 plot_pos  = ImPlot::GetPlotPos();
    const ImVec2 plot_size = ImPlot::GetPlotSize();
    const float  edge_py   = side == Side::Base ? plot_pos.y + plot_size.y : plot_pos.y;
    out.resize(app.anchors.size());
    for (std::size_t i = 0; i < app.anchors.size(); ++i) {
        const double x = app.anchors[i].time(side);
        out[i].pos     = ImVec2(ImPlot::PlotToPixels(x, 0.0).x, edge_py);
        out[i].visible = x >= lim.X.Min && x <= lim.X.Max;
    }
}

}    // namespace

void draw_anchors(App& app, Side side, const Spectrogram& sp, std::vector<EdgePoint>& out_edges) {
    ImGuiIO&         io  = ImGui::GetIO();
    const ImPlotRect lim = ImPlot::GetPlotLimits();

    // 同じ x 上に線と点があり、ドラッグ対象が競合するため Ctrl で役割を切り替える:
    //   Ctrl なし … 時間線ドラッグ可 / 周波数点ロック（NoInputs）
    //   Ctrl あり … 時間線ロック（NoInputs）/ 周波数点ドラッグ可
    // 点は Delayed（描画1フレーム遅延）でドラッグ中も渡した x（＝線上）に描画される。
    const ImPlotDragToolFlags line_flags  = io.KeyCtrl ? ImPlotDragToolFlags_NoInputs : ImPlotDragToolFlags_None;
    const ImPlotDragToolFlags point_flags = io.KeyCtrl ? ImPlotDragToolFlags_Delayed : ImPlotDragToolFlags_NoInputs;

    // 番号を出す対象は前フレームに決めたもの（App のコメント参照）。アンカーが削除されて
    // 範囲外になっていることがあるので検査する。
    int active = app.active_anchor;
    if (active >= static_cast<int>(app.anchors.size())) active = -1;

    bool any_active = false;
    int  hovered    = -1;            // ホバー中の時間アンカー（線）
    int  hover_fi = -1, hover_fj = -1;    // ホバー中の周波数アンカー (anchor, freq)
    draw_time_lines(app, side, line_flags, active, any_active, hovered);
    draw_freq_points(app, side, sp, lim, point_flags, active, any_active, hover_fi, hover_fj);

    // 入力処理は Ctrl の有無で排他（どちらか一方だけが作用する）。
    handle_freq_input(app, side, sp, io, any_active, hover_fi, hover_fj);
    handle_time_input(app, sp, io, any_active, hovered);

    // 次フレームの対象を集める。ドラッグ中の線を最優先（カーソルが線から離れても番号を
    // 出したままにするため）、次に周波数点、最後にカーソルに最も近い線。距離判定を併用
    // するのは Ctrl の有無で線か点の一方が NoInputs になりホバーを返さなくなるため。
    if (hovered >= 0) {
        app.hover_anchor = hovered;
    } else if (hover_fi >= 0) {
        app.hover_anchor = hover_fi;
    } else if (ImPlot::IsPlotHovered()) {
        const int pick = nearest_anchor(app, side, io, kPickPx);
        if (pick >= 0) app.hover_anchor = pick;
    }

    capture_edges(app, side, lim, out_edges);
}

void draw_anchor_connectors(const std::vector<EdgePoint>& base_edges,
                            const std::vector<EdgePoint>& target_edges, int active) {
    if (base_edges.size() != target_edges.size()) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (std::size_t i = 0; i < base_edges.size(); ++i)
        if (base_edges[i].visible && target_edges[i].visible)
            dl->AddLine(base_edges[i].pos, target_edges[i].pos,
                        ImGui::ColorConvertFloat4ToU32(connector_col(i, active)), kConnectorWidth);
}
