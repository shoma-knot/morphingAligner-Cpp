#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>
#include <tinyfiledialogs.h>

#include "anchors.hpp"
#include "app.hpp"
#include "freqscale.hpp"
#include "log.hpp"
#include "morphing.hpp"
#include "session.hpp"

namespace {

// Y軸（ERB レート）の目盛りを実周波数 [Hz] で表示するフォーマッタ。
int erb_hz_formatter(double erb, char* buff, int size, void*) {
    return std::snprintf(buff, size, "%.0f", freqscale::erb_to_hz(erb));
}

// セッションの既定保存パス。実行ファイルのパス取得は OS 固有になるため、移植性を優先
// してカレントディレクトリ（多くは起動ディレクトリ＝バイナリのある場所）を使う。
std::string default_session_path() {
    std::error_code ec;
    const auto      dir = std::filesystem::current_path(ec);
    return ec ? "session.json" : (dir / "session.json").string();
}

// 左パネル内の、トラック1つ分の読み込み/再生操作と情報表示。
void draw_track_controls(App& app, Track& tr, const char* label) {
    ImGui::PushID(&tr);
    ImGui::TextUnformatted(label);

    if (ImGui::Button("読み込む", ImVec2(-1, 0))) load_track(tr);

    ImGui::BeginDisabled(!tr.loaded());
    if (ImGui::Button("再生", ImVec2(-1, 0))) {
        try {
            app.engine.play_oneshot(tr.path);
        } catch (const std::exception& e) {
            applog::add(tr.name + " 再生失敗: " + e.what());
        }
    }
    ImGui::EndDisabled();

    if (tr.loaded()) {
        ImGui::TextWrapped("%s", tr.path.c_str());
        ImGui::Text("%d Hz, %.2f s", tr.spec.fs, tr.spec.duration);
    } else {
        ImGui::TextDisabled("未読み込み");
    }
    ImGui::PopID();
}

// 周波数軸のホイールズーム/中ドラッグパンを手組みする。Y 軸を Lock しているため
// ImPlot が Y を駆動せず、tr.y_min/tr.y_max を自前で更新している。
// 詳細と注意は FIXME(freq-axis-input) を参照。
void handle_freq_axis_input(Track& tr, const Spectrogram& sp) {
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
    //   - ズーム/パンの計算は Y が線形・非反転・[0, ERB(fs/2)] である前提
    //     （Y軸座標は ERB レート。tr.y_min/max も ERB 単位）。
    // 拡張するなら、この手組みを伸ばすより ImPlot に軸を任せる方向（軸ごとの
    // 入力フラグ / リンクされた limits 等）を優先すること。
    // ────────────────────────────────────────────────────────────────

    const double erb_max = freqscale::hz_to_erb(sp.fs / 2.0);    // Y軸の上限（ERB）

    // 周波数軸ラベル上でのホイールは、カーソル位置を中心に Y をズームする。
    // （プロット領域上のホイールは Y が Lock されているので X しか動かない）
    ImGuiIO& io = ImGui::GetIO();
    if (ImPlot::IsAxisHovered(ImAxis_Y1) && io.MouseWheel != 0.0f) {
        const double yc     = ImPlot::GetPlotMousePos().y;
        const double factor = std::pow(1.0 - ImPlot::GetInputMap().ZoomRate, static_cast<double>(io.MouseWheel));
        double       lo     = std::max(0.0, yc + (tr.y_min - yc) * factor);
        double       hi     = std::min(erb_max, yc + (tr.y_max - yc) * factor);
        if (hi - lo > 0.05) {    // 最小スパン（ERB 単位）
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
        delta              = std::clamp(delta, -tr.y_min, erb_max - tr.y_max);
        tr.y_min += delta;
        tr.y_max += delta;
    }
    // ─── FIXME(freq-axis-input) ここまで ─────────────────────────────
}

// メインプロットの上に置く VSCode 風ミニマップ。全体を表示し、現在の表示範囲
// (tr.view_*, 前フレームの値)を白の半透明ボックスで重ねる。非インタラクティブ。
void draw_minimap(Track& tr, float width, float height) {
    const Spectrogram& sp      = tr.spec;
    const double       erb_max = freqscale::hz_to_erb(sp.fs / 2.0);
    ImGui::PushID("minimap");
    if (ImPlot::BeginPlot("##minimap", ImVec2(width, height), ImPlotFlags_CanvasOnly | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, sp.duration, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, erb_max, ImPlotCond_Always);
        ImPlot::PlotImage("##mini", static_cast<ImTextureID>(tr.tex), ImPlotPoint(0, 0),
                          ImPlotPoint(sp.duration, erb_max));
        // 現在の表示範囲を白の半透明ボックスで重ねる（Y は上が y_max）。
        const ImVec2 pmin = ImPlot::PlotToPixels(tr.view_x0, tr.view_y1);
        const ImVec2 pmax = ImPlot::PlotToPixels(tr.view_x1, tr.view_y0);
        ImDrawList*  dl   = ImPlot::GetPlotDrawList();
        dl->AddRectFilled(pmin, pmax, IM_COL32(255, 255, 255, 50));
        dl->AddRect(pmin, pmax, IM_COL32(255, 255, 255, 200));
        ImPlot::EndPlot();
    }
    ImGui::PopID();
}

// ミニマップの配置。
enum class Minimap { None, Above, Below };

// 指定ピクセル高さでスペクトル包絡スペクトログラムを1枚描き、アンカーを重ねる。
// is_base はこのプロットがアンカーペアのどちら側を編集するかを選ぶ。
// out_edges には対応線の端点を返す（何も描かなければ空）。minimap で上/下にミニマップ。
void draw_spectrogram(App& app, Track& tr, bool is_base, float height, std::vector<EdgePoint>& out_edges,
                      Minimap minimap) {
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

    // 右側に dB カラースケール（凡例）の幅を確保する。
    constexpr float kScaleW = 90.0f;
    const float     plot_w  = ImGui::GetContentRegionAvail().x - kScaleW;

    // ミニマップの分だけ本体を低くする（上/下は下で描き分ける）。
    float       spec_h = height;
    const float mini_h = height * 0.22f;
    if (minimap != Minimap::None)
        spec_h = std::max(80.0f, height - mini_h - ImGui::GetStyle().ItemSpacing.y);
    if (minimap == Minimap::Above) draw_minimap(tr, plot_w, mini_h);

    // 右クリックをアンカー削除に使うため解放する: NoMenus で既定のコンテキストメニュー、
    // NoBoxSelect で右ドラッグの範囲ズームを無効化。
    if (ImPlot::BeginPlot("##spec", ImVec2(plot_w, spec_h), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        // 周波数軸を Lock し、プロット領域上のホイールが時間(X)だけをズームするようにする。
        // Y の表示範囲は自前の状態(tr.y_min/max)で駆動し、軸ラベル上ホバー時のみ手動ズーム。
        ImPlot::SetupAxes("時間 [s]", "周波数 [Hz]", ImPlotAxisFlags_None, ImPlotAxisFlags_Lock);
        // Y軸は ERB レートを座標にし、目盛りは Hz で表示（テクスチャも ERB 等間隔）。
        ImPlot::SetupAxisFormat(ImAxis_Y1, erb_hz_formatter);
        // 目盛りは 1-2-5 系列（…100,200,500,1000,2000,5000,…）に。ERB 軸では高域が
        // 圧縮されるので、値が大きいほど間隔を空けることでほぼ均等に並ぶ。ラベルは上の
        // formatter が Hz 表示（位置は ERB 座標）。
        const double        nyq = sp.fs / 2.0;
        std::vector<double> yticks { freqscale::hz_to_erb(0.0) };    // 0Hz
        for (double dec = 10.0; dec <= nyq; dec *= 10.0)
            for (double m : { 1.0, 2.0, 5.0 }) {
                const double hz = dec * m;
                if (hz >= 100.0 && hz <= nyq) yticks.push_back(freqscale::hz_to_erb(hz));
            }
        ImPlot::SetupAxisTicks(ImAxis_Y1, yticks.data(), static_cast<int>(yticks.size()), nullptr, false);
        // X は初期のみ設定（以後ズーム/パン可）、Y は毎フレーム自前の表示範囲に追従。
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, sp.duration, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, tr.y_min, tr.y_max, ImPlotCond_Always);
        // 時間軸を [0, duration] 内に制約（データ範囲外へパン/ズームアウトさせない）。
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, sp.duration);
        // 焼き込み済みテクスチャ: bin * frame 数に依らず1クアッドで描画。Y は ERB レート範囲。
        ImPlot::PlotImage("##env", static_cast<ImTextureID>(tr.tex), ImPlotPoint(0, 0),
                          ImPlotPoint(sp.duration, freqscale::hz_to_erb(sp.fs / 2.0)));

        handle_freq_axis_input(tr, sp);
        draw_anchors(app, is_base, sp, out_edges);

        // 現在の表示範囲をミニマップ用に保存（X=秒, Y=ERB レート）。
        const ImPlotRect vlim = ImPlot::GetPlotLimits();
        tr.view_x0            = vlim.X.Min;
        tr.view_x1            = vlim.X.Max;
        tr.view_y0            = vlim.Y.Min;
        tr.view_y1            = vlim.Y.Max;

        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("dB", sp.db_min, sp.db_max, ImVec2(kScaleW, spec_h));

    if (minimap == Minimap::Below) draw_minimap(tr, plot_w, mini_h);

    ImPlot::PopColormap();
    ImGui::PopID();
}

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

    // ── 表示設定 ─────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Checkbox("ミニマップを表示", &app.show_minimap);

    // ── セッション（アンカー）の保存/読み込み ─────────────────
    ImGui::Spacing();
    ImGui::Separator();
    static const char* kJsonFilter[] = { "*.json" };
    // 既定の保存先はカレントディレクトリ。読み込みも同じ場所から開始。
    static const std::string kDefaultPath = default_session_path();

    // 保存は base/target が両方読み込まれているとき（waves パスが有効）だけ許可。
    ImGui::BeginDisabled(!(app.base.loaded() && app.target.loaded()));
    if (ImGui::Button("セッション保存", ImVec2(-1, 0))) {
        if (const char* p = tinyfd_saveFileDialog("セッションを保存", kDefaultPath.c_str(), 1, kJsonFilter, "JSON"))
            save_session(app, p);
    }
    ImGui::EndDisabled();
    if (ImGui::Button("セッション読み込み", ImVec2(-1, 0))) {
        if (const char* p = tinyfd_openFileDialog("セッションを読み込み", kDefaultPath.c_str(), 1, kJsonFilter, "JSON", 0))
            load_session(app, p);
    }

    // ── モーフィング ─────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("モーフィング");
    static const char* kWavFilter[] = { "*.wav" };
    ImGui::SliderFloat("率(0=base,1=target)", &app.morph_rate, 0.0f, 1.0f, "%.2f");
    ImGui::BeginDisabled(!(app.base.loaded() && app.target.loaded()));
    // 注: morphing() は同期実行（Harvest 等で数秒かかり UI が一瞬固まる）。
    if (ImGui::Button("生成して再生", ImVec2(-1, 0))) {
        // UI は一律操作: 全軸に同じ率を渡す。
        applog::add("モーフィング生成中... (rate=" + std::to_string(app.morph_rate) + ")");
        const MorphResult mr =
          morphing(app.base.path, app.target.path, app.anchors, MorphRates::uniform(app.morph_rate));
        if (!mr.ok()) {
            applog::add(mr.error);
        } else {
            app.morph_wave = mr.wave;    // 保存用に保持
            app.morph_fs   = mr.fs;
            // ファイルを書かずメモリから直接再生（double→float）。
            std::vector<float> pcm(mr.wave.size());
            for (std::size_t i = 0; i < mr.wave.size(); ++i)
                pcm[i] = static_cast<float>(std::clamp(mr.wave[i], -1.0, 1.0));
            try {
                app.engine.play_pcm(pcm.data(), pcm.size(), 1, static_cast<unsigned>(mr.fs));
                applog::add("モーフィング生成・再生: " + std::to_string(mr.wave.size()) + " samples");
            } catch (const std::exception& e) {
                applog::add(std::string { "再生失敗: " } + e.what());
            }
        }
    }
    ImGui::EndDisabled();

    // 直近の結果を WAV で保存（メモリ再生とは分離）。
    ImGui::BeginDisabled(app.morph_wave.empty());
    if (ImGui::Button("結果を WAV 保存", ImVec2(-1, 0))) {
        if (const char* p = tinyfd_saveFileDialog("モーフィング結果を保存", "morph.wav", 1, kWavFilter, "WAV")) {
            std::string werr;
            if (write_wav(p, app.morph_wave, app.morph_fs, werr))
                applog::add(std::string { "WAV 保存: " } + p);
            else
                applog::add("WAV 保存失敗: " + werr);
        }
    }
    ImGui::EndDisabled();
}

void draw_right_panel(App& app) {
    const float labels_h = 2.0f * ImGui::GetTextLineHeightWithSpacing();
    const float avail_h  = ImGui::GetContentRegionAvail().y;
    const float each_h   = std::max(140.0f, (avail_h - labels_h - 12.0f) / 2.0f);

    const Minimap base_mm   = app.show_minimap ? Minimap::Above : Minimap::None;
    const Minimap target_mm = app.show_minimap ? Minimap::Below : Minimap::None;

    static std::vector<EdgePoint> base_edges, target_edges;
    draw_spectrogram(app, app.base, /*is_base=*/true, each_h, base_edges, base_mm);
    ImGui::Spacing();
    draw_spectrogram(app, app.target, /*is_base=*/false, each_h, target_edges, target_mm);

    draw_anchor_connectors(base_edges, target_edges);
}

// 画面下部の動作ログ領域。applog の内容を古い順に表示し、最下部にいるときは自動追従。
void draw_log_panel() {
    ImGui::TextUnformatted("ログ");
    ImGui::Separator();

    ImGui::BeginChild("log_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : applog::lines()) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

}    // namespace

void draw_root(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                   | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoCollapse);

    // 縦 8:2 分割: 上=操作/表示（左1:右4）、下=動作ログ。
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float log_h   = std::max(100.0f, avail_h * 0.2f);
    const float top_h   = avail_h - log_h - ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("top", ImVec2(0, top_h), false);
    {
        const float avail  = ImGui::GetContentRegionAvail().x;
        const float left_w = avail * (1.0f / 5.0f);
        ImGui::BeginChild("left", ImVec2(left_w, 0), true);
        draw_left_panel(app);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, 0), true);
        draw_right_panel(app);
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::BeginChild("log", ImVec2(0, 0), true);
    draw_log_panel();
    ImGui::EndChild();

    ImGui::End();
}
