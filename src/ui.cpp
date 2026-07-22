#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <implot.h>
#include <tinyfiledialogs.h>

#include "anchors.hpp"
#include "app.hpp"
#include "log.hpp"
#include "morphing.hpp"
#include "session.hpp"

namespace {

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
}

// 指定ピクセル高さでスペクトル包絡スペクトログラムを1枚描き、アンカーを重ねる。
// is_base はこのプロットがアンカーペアのどちら側を編集するかを選ぶ。
// out_edges には対応線の端点を返す（何も描かなければ空）。
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

    // 右側に dB カラースケール（凡例）の幅を確保する。
    constexpr float kScaleW = 90.0f;
    const float     plot_w  = ImGui::GetContentRegionAvail().x - kScaleW;

    // 右クリックをアンカー削除に使うため解放する: NoMenus で既定のコンテキストメニュー、
    // NoBoxSelect で右ドラッグの範囲ズームを無効化。
    if (ImPlot::BeginPlot("##spec", ImVec2(plot_w, height), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        // 周波数軸を Lock し、プロット領域上のホイールが時間(X)だけをズームするようにする。
        // Y の表示範囲は自前の状態(tr.y_min/max)で駆動し、軸ラベル上ホバー時のみ手動ズーム。
        ImPlot::SetupAxes("時間 [s]", "周波数 [Hz]", ImPlotAxisFlags_None, ImPlotAxisFlags_Lock);
        // X は初期のみ設定（以後ズーム/パン可）、Y は毎フレーム自前の表示範囲に追従。
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, sp.duration, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, tr.y_min, tr.y_max, ImPlotCond_Always);
        // 時間軸を [0, duration] 内に制約（データ範囲外へパン/ズームアウトさせない）。
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, sp.duration);
        // 焼き込み済みテクスチャ: bin * frame 数に依らず1クアッドで描画。
        ImPlot::PlotImage(
          "##env", static_cast<ImTextureID>(tr.tex), ImPlotPoint(0, 0), ImPlotPoint(sp.duration, sp.fs / 2.0));

        handle_freq_axis_input(tr, sp);
        draw_anchors(app, is_base, sp, out_edges);

        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("dB", sp.db_min, sp.db_max, ImVec2(kScaleW, height));

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

    static std::vector<EdgePoint> base_edges, target_edges;
    draw_spectrogram(app, app.base, /*is_base=*/true, each_h, base_edges);
    ImGui::Spacing();
    draw_spectrogram(app, app.target, /*is_base=*/false, each_h, target_edges);

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
