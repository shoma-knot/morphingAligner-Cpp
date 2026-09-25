#include "ui_tabs.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>    // std::string 版 InputText
#include <implot.h>

#include "anchors.hpp"
#include "app.hpp"
#include "file_jobs.hpp"
#include "freqscale.hpp"
#include "log.hpp"
#include "speech_controller.hpp"
#include "speech_view.hpp"
#include "ui_common.hpp"

namespace {

// 分割数のスピンボックスと「アンカー自動生成」ボタン。音素アライメントとフォルマント推定が
// base/target の両方で終わってから押せる。既存のアンカーがあれば確認してから置き換える。
void draw_auto_anchor_controls(App& app) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("分割数");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputInt("##auto_div", &app.speech.auto_anchor_divisions, 1, 1))
        app.speech.auto_anchor_divisions = std::clamp(app.speech.auto_anchor_divisions, 1, 10);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("各音素の区間を何等分してアンカーを追加するか（1 なら音素境界のみ）。");

    const char* kConfirm = "アンカーの自動生成";
    ImGui::BeginDisabled(!auto_anchors_ready(app));
    if (ImGui::Button("アンカー自動生成", ImVec2(-1, 0))) {
        if (app.anchors.empty()) run_auto_anchors(app);
        else ImGui::OpenPopup(kConfirm);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("音素境界と、各音素を分割数で等分した位置に時間アンカーを打ち、\n"
                          "その時刻の移動平均フォルマント（窓幅は上の設定）に周波数アンカーを打ちます。\n"
                          "既存のアンカーは置き換えます。音素アライメントとフォルマント推定が\n"
                          "base / target の両方で終わると押せます。");

    if (ImGui::BeginPopupModal(kConfirm, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("既存のアンカー %d 個を消して、自動生成したアンカーに置き換えます。",
                    static_cast<int>(app.anchors.size()));
        if (ImGui::Button("置き換える")) {
            run_auto_anchors(app);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// 左パネルの「音声解析（Python）」: 表示切替、共通の書き起こしと音素アライメント、設定。
// フォルマントは読み込み時に自動で推定する（ensure_formants）ので、ここでは表示の切替だけ。
void draw_speech_tools_panel(App& app) {
    if (!ImGui::CollapsingHeader("音声解析（Python）", ImGuiTreeNodeFlags_DefaultOpen)) return;

    // 環境の状態。使えないときはセットアップの案内と「再確認」ボタンを出す。
    if (app.speech.env == SpeechEnv::Checking || app.speech.env == SpeechEnv::Unknown) {
        ImGui::TextDisabled("環境を確認中...");
    } else if (app.speech.env == SpeechEnv::Unavailable) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "未セットアップ");
        if (ImGui::IsItemHovered() && !app.speech.env_problems.empty()) {
            std::string tip = "使えない理由:";
            for (const std::string& p : app.speech.env_problems) tip += "\n- " + p;
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::TextWrapped("配布物の %s を実行してから「再確認」を押してください（手順は README）。",
                           install_script_name());
        if (ImGui::Button("再確認", ImVec2(-1, 0))) app.speech.env = SpeechEnv::Unknown;
    }
    const bool env_ready = app.speech.env == SpeechEnv::Ready;

    ImGui::Checkbox("フォルマント", &app.speech.show_formants);
    // 凡例（スペクトログラム上の点の色）。
    for (int k = 0; k < app.speech.formant_params.num_tracks; ++k) {
        ImGui::SameLine(0.0f, k == 0 ? -1.0f : 4.0f);
        ImGui::TextColored(formant_color(k), "F%d", k + 1);
    }
    // 移動平均（フォルマント表示の下位設定）。窓幅は ms 単位のスピンボックス。
    ImGui::Indent();
    ImGui::BeginDisabled(!app.speech.show_formants);
    ImGui::Checkbox("移動平均", &app.speech.show_formant_ma);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("ms").x
                                              - ImGui::GetStyle().ItemInnerSpacing.x));
    if (ImGui::InputInt("ms##ma_window", &app.speech.formant_ma_ms, 5, 25)) {
        app.speech.formant_ma_ms = std::clamp(app.speech.formant_ma_ms, 5, 500);
        update_formant_ma(app);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("移動平均の窓幅（各時刻を中心とする幅）。5〜500 ms。");
    ImGui::EndDisabled();
    ImGui::Unindent();
    ImGui::Checkbox("音素セグメンテーション", &app.speech.show_segmentation);

    // 書き起こしは base/target 共通（同じ文を読んだ2音声を想定）。ボタンで両方を整列する。
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##transcript", "書き起こし（MFA 用）", &app.speech.transcript);
    const bool any_loaded = app.base.loaded() || app.target.loaded();
    const bool busy       = app.base.align_busy || app.target.align_busy;
    ImGui::BeginDisabled(!env_ready || !any_loaded || busy || app.speech.transcript.empty());
    if (ImGui::Button(busy ? "音素アライメント実行中..." : "音素アライメント", ImVec2(-1, 0))) launch_alignment(app);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Montreal Forced Aligner で書き起こしを base / target の音声に合わせ、\n"
                          "音素の区間を求めます（数十秒かかります）。%s",
                          env_ready ? "" : "\n音声解析の環境がセットアップされていないため使えません。");

    draw_auto_anchor_controls(app);

    if (ImGui::TreeNode("設定##speech")) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputDouble("##maxf", &app.speech.formant_params.max_formant_hz, 250.0, 500.0, "最大フォルマント %.0f Hz");
        app.speech.formant_params.max_formant_hz = std::clamp(app.speech.formant_params.max_formant_hz, 2000.0, 10000.0);
        // 値を確定したら推定し直す（入力中の1文字ごとには走らせない）。
        if (ImGui::IsItemDeactivatedAfterEdit()) invalidate_formants(app);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Praat の Maximum formant。成人男性は 5000 Hz、女性は 5500 Hz が目安です。");
        ImGui::TextDisabled("MFA 音響モデル / 辞書");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##acoustic", &app.speech.align_params.acoustic_model);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##dictionary", &app.speech.align_params.dictionary);
        // 使う Python（見つからないときの切り分け用）。ファイルを確かめるので開いたときだけ。
        const std::string py = find_python();
        ImGui::TextDisabled("Python: %s", py.empty() ? "（見つかりません）" : "検出済み");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", py.empty() ? "./.env に環境を置くか、環境変数 MORPHALIGNER_PYTHON で指定してください"
                                               : py.c_str());
        ImGui::TreePop();
    }
}

// 左パネル内の、トラック1つ分の読み込み/再生操作と情報表示。
void draw_track_controls(App& app, Side side, const char* label) {
    Track& tr = app.track(side);
    ImGui::PushID(&tr);
    ImGui::TextUnformatted(label);

    // 「読み込む」「再生」を1行に並べる。
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(app.jobs.ui.busy());
    if (ImGui::Button("読み込む", ImVec2(half, 0))) launch_load_track_job(app, side);
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(!tr.loaded());
    if (ImGui::Button("再生", ImVec2(half, 0))) {
        try {
            app.engine.play_oneshot(tr.path);
        } catch (const std::exception& e) {
            applog::add(tr.name + " 再生失敗: " + e.what());
        }
    }
    ImGui::EndDisabled();

    if (tr.loaded()) {
        // パネルが狭いのでファイル名だけ出す（フルパスはホバーで見せる）。
        ImGui::TextWrapped("%s", std::filesystem::path(tr.path).filename().string().c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr.path.c_str());
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

    // ホイールでカーソル位置を中心に Y をズームする。発火するのは
    //   - 周波数軸ラベル上（プロット領域上のホイールは Y が Lock なので X しか動かない）
    //   - プロット領域上で Ctrl 併用（Ctrl 中は X を Lock して X ズームを抑えている。
    //     draw_spectrogram の SetupAxes を参照）
    // Ctrl は周波数アンカーの操作にも使っており、「Ctrl = 周波数方向」で揃えている。
    ImGuiIO&   io        = ImGui::GetIO();
    const bool wheel_y   = ImPlot::IsAxisHovered(ImAxis_Y1)
                      || (ImPlot::IsPlotHovered() && io.KeyCtrl);
    if (wheel_y && io.MouseWheel != 0.0f) {
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
        ImPlot::PlotImage("##mini", tr.tex.imgui_id(), ImPlotPoint(0, 0),
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
// side はこのプロットがアンカーペアのどちら側を編集するかを選ぶ。
// out_edges には対応線の端点を返す（何も描かなければ空）。minimap で上/下にミニマップ。
// show_seg なら音素セグメンテーションのプロットを添える（base は上、target は下。
// パネル間の対応線がまたがないよう、ミニマップと同じく外側に置く）。
void draw_spectrogram(App& app, Side side, float height, std::vector<EdgePoint>& out_edges,
                      Minimap minimap, bool show_seg) {
    out_edges.clear();

    Track& tr = app.track(side);
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

    // ミニマップと音素セグメンテーションの分だけ本体を低くする（上/下は下で描き分ける）。
    const float gap    = ImGui::GetStyle().ItemSpacing.y;
    const float mini_h = height * 0.22f;
    const float seg_h  = segmentation_plot_height();
    float       spec_h = height;
    if (minimap != Minimap::None) spec_h -= mini_h + gap;
    if (show_seg) spec_h -= seg_h + gap;
    spec_h = std::max(80.0f, spec_h);
    if (minimap == Minimap::Above) draw_minimap(tr, plot_w, mini_h);

    // 音素セグメンテーションとはプロット領域の左右端を揃える（Y 軸の目盛り幅が違っても
    // 時刻の位置が縦に一致するように）。時間軸は tr.view_x0/x1 へのリンクで共有する。
    const bool aligned = show_seg && ImPlot::BeginAlignedPlots("##spec_seg");
    if (show_seg && side == Side::Base) draw_segmentation(tr, plot_w, seg_h);

    // 右クリックをアンカー削除に使うため解放する: NoMenus で既定のコンテキストメニュー、
    // NoBoxSelect で右ドラッグの範囲ズームを無効化。
    if (ImPlot::BeginPlot("##spec", ImVec2(plot_w, spec_h), ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect)) {
        // 周波数軸を Lock し、プロット領域上のホイールが時間(X)だけをズームするようにする。
        // Y の表示範囲は自前の状態(tr.y_min/max)で駆動し、軸ラベル上ホバー時のみ手動ズーム。
        // Ctrl 併用のホイールは周波数(Y)ズームに使うので、その間は X も Lock して
        // ImPlot による時間軸ズームが同時に起きないようにする。
        const ImPlotAxisFlags x_flags =
          ImGui::GetIO().KeyCtrl ? ImPlotAxisFlags_Lock : ImPlotAxisFlags_None;
        ImPlot::SetupAxes("時間 [s]", "周波数 [Hz]", x_flags, ImPlotAxisFlags_Lock);
        // Y軸は ERB レートを座標にし、目盛りは Hz で表示（テクスチャも ERB 等間隔）。
        setup_erb_yaxis_ticks(sp.fs / 2.0);
        // X は tr.view_x0/x1 にリンク（音素セグメンテーションのプロットと共有。初期値は
        // apply_track が全体表示にする）、Y は毎フレーム自前の表示範囲に追従。
        ImPlot::SetupAxisLinks(ImAxis_X1, &tr.view_x0, &tr.view_x1);
        ImPlot::SetupAxisLimits(ImAxis_Y1, tr.y_min, tr.y_max, ImPlotCond_Always);
        // 時間軸を [0, duration] 内に制約（データ範囲外へパン/ズームアウトさせない）。
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, sp.duration);
        // 焼き込み済みテクスチャ: bin * frame 数に依らず1クアッドで描画。Y は ERB レート範囲。
        ImPlot::PlotImage("##env", tr.tex.imgui_id(), ImPlotPoint(0, 0),
                          ImPlotPoint(sp.duration, freqscale::hz_to_erb(sp.fs / 2.0)));
        if (app.speech.show_formants) draw_formants(tr.formants, app.speech.show_formant_ma ? &tr.formants_ma : nullptr);

        handle_freq_axis_input(tr, sp);
        draw_anchors(app, side, sp, out_edges);

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

    if (show_seg && side == Side::Target) draw_segmentation(tr, plot_w, seg_h);
    if (aligned) ImPlot::EndAlignedPlots();
    if (minimap == Minimap::Below) draw_minimap(tr, plot_w, mini_h);

    ImPlot::PopColormap();
    ImGui::PopID();
}

void draw_left_panel(App& app) {
    ImGui::TextUnformatted("操作");
    ImGui::Separator();

    draw_track_controls(app, Side::Base, "Base");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    draw_track_controls(app, Side::Target, "Target");

    ImGui::Spacing();
    ImGui::Separator();
    // アンカー数・操作説明（ホバーで表示）・全消去を1行に。
    ImGui::AlignTextToFramePadding();
    ImGui::Text("アンカー: %d", static_cast<int>(app.anchors.size()));
    help_marker("左クリック: 時間アンカーを追加（線はドラッグで移動）\n"
                "右クリック: 時間アンカーを削除\n"
                "Ctrl+左クリック: 線上に周波数アンカーを追加（点はドラッグで移動）\n"
                "Ctrl+右クリック: 周波数アンカーを削除\n"
                "ホイール: 時間軸ズーム / Ctrl+ホイール: 周波数軸ズーム\n"
                "中ボタンドラッグ: 表示範囲の移動");
    ImGui::BeginDisabled(app.anchors.empty());
    if (right_aligned_button("全消去")) app.anchors.clear();
    ImGui::EndDisabled();

    // ── 表示設定 ─────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Checkbox("ミニマップを表示", &app.view.show_minimap);

    // ── 音声解析（フォルマント・音素セグメンテーション） ─────────
    ImGui::Spacing();
    ImGui::Separator();
    draw_speech_tools_panel(app);

    // ── セッション（アンカー）の保存/読み込み ─────────────────
    ImGui::Spacing();
    ImGui::Separator();
    // 保存は base/target が両方読み込まれているとき（waves パスが有効）だけ許可。
    ImGui::BeginDisabled(!(app.base.loaded() && app.target.loaded()) || app.jobs.ui.busy());
    if (ImGui::Button("セッション保存", ImVec2(-1, 0))) launch_save_session_job(app);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(app.jobs.ui.busy());
    if (ImGui::Button("セッション読み込み", ImVec2(-1, 0))) launch_load_session_job(app);
    ImGui::EndDisabled();
    ImGui::TextDisabled("tcmorph の anchors.json も可");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("tcmorph 形式（objects 配列）の JSON は音声パスを持たないため、\n"
                          "現在の base / target を保ったままアンカーだけを差し替えます。\n"
                          "objects の1番目が base、2番目が target になります。");
    // （モーフィング操作は「モーフィング」タブに移動）
}

void draw_right_panel(App& app) {
    const float labels_h = 2.0f * ImGui::GetTextLineHeightWithSpacing();
    const float avail_h  = ImGui::GetContentRegionAvail().y;
    const float each_h   = std::max(140.0f, (avail_h - labels_h - 12.0f) / 2.0f);

    const Minimap base_mm   = app.view.show_minimap ? Minimap::Above : Minimap::None;
    const Minimap target_mm = app.view.show_minimap ? Minimap::Below : Minimap::None;

    // アンカーの強調対象を確定する。base を描く時点では target 側のホバーが未確定
    // なので、前フレームに集めたものを今フレームの強調に使う（ViewState のコメント参照）。
    app.view.active_anchor = app.view.hover_anchor;
    app.view.hover_anchor  = -1;

    // 音素セグメンテーションは片方にだけ結果があっても両方に枠を出す（base/target の
    // スペクトログラム本体の高さを揃えるため）。
    const bool show_seg = app.speech.show_segmentation
                       && (!app.base.segmentation.empty() || !app.target.segmentation.empty()
                           || app.base.align_busy || app.target.align_busy);

    std::vector<EdgePoint>& base_edges   = app.view.edges[side_index(Side::Base)];
    std::vector<EdgePoint>& target_edges = app.view.edges[side_index(Side::Target)];
    draw_spectrogram(app, Side::Base, each_h, base_edges, base_mm, show_seg);
    ImGui::Spacing();
    draw_spectrogram(app, Side::Target, each_h, target_edges, target_mm, show_seg);

    draw_anchor_connectors(base_edges, target_edges, app.view.active_anchor);
}

}    // namespace

// 「アライメント」タブ: 左=操作パネル（1）、右=スペクトログラム/アンカー編集（4）。
void draw_align_tab(App& app) {
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
