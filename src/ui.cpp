#include "ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// Y軸（ERB レート）に 1-2-5 系列（…100,200,500,1000,…）の目盛りとHz表示を設定する。
// ERB 軸では高域が圧縮されるので、値が大きいほど間隔を空けることでほぼ均等に並ぶ。
void setup_erb_yaxis_ticks(double nyq) {
    ImPlot::SetupAxisFormat(ImAxis_Y1, erb_hz_formatter);
    std::vector<double> yticks { freqscale::hz_to_erb(0.0) };    // 0Hz
    for (double dec = 10.0; dec <= nyq; dec *= 10.0)
        for (double m : { 1.0, 2.0, 5.0 }) {
            const double hz = dec * m;
            if (hz >= 100.0 && hz <= nyq) yticks.push_back(freqscale::hz_to_erb(hz));
        }
    ImPlot::SetupAxisTicks(ImAxis_Y1, yticks.data(), static_cast<int>(yticks.size()), nullptr, false);
}

// セッションの既定保存パス。実行ファイルのパス取得は OS 固有になるため、移植性を優先
// してカレントディレクトリ（多くは起動ディレクトリ＝バイナリのある場所）を使う。
std::string default_session_path() {
    std::error_code ec;
    const auto      dir = std::filesystem::current_path(ec);
    return ec ? "session.json" : (dir / "session.json").string();
}

// ── 汎用 UI ジョブ ───────────────────────────────────────────
// ファイルダイアログ（tinyfd はダイアログを閉じるまでブロックする）や音声解析などを
// ワーカースレッドで実行し、完了時に「メインスレッドで適用する処理」を受け取って実行する。
// GL（テクスチャ）や App の状態変更は必ず適用処理側＝メインスレッドで行うこと。

// ジョブを開始する（同時に1本のみ。実行中は無視されるので、ボタン側でも無効化しておく）。
void launch_ui_job(App& app, std::function<std::function<void(App&)>()> work) {
    if (app.ui_job_running) return;
    app.ui_job_running = true;
    app.ui_job         = std::async(std::launch::async, std::move(work));
}

// 毎フレーム: 完了した UI ジョブを回収し、適用処理をメインスレッドで実行する。
void poll_ui_job(App& app) {
    if (!app.ui_job_running) return;
    if (app.ui_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::function<void(App&)> apply = app.ui_job.get();
    app.ui_job_running              = false;
    if (apply) apply(app);
}

// 音声ファイルの選択→解析をワーカーで行い、完了時に Track へ反映する。
void launch_load_track_job(App& app, bool is_base) {
    const std::string name = is_base ? app.base.name : app.target.name;
    launch_ui_job(app, [name, is_base]() -> std::function<void(App&)> {
        static const char* kAudioFilter[] = { "*.wav", "*.flac", "*.mp3", "*.ogg" };
        const std::string  title          = name + " 音声を選択";
        const char* picked = tinyfd_openFileDialog(title.c_str(), "", 4, kAudioFilter, "音声ファイル", 0);
        if (!picked) return {};    // キャンセル
        const std::string path = picked;
        applog::add(name + " 解析中...: " + path);
        try {
            // std::function はコピー可能な呼び出し体を要求するので shared_ptr で持ち回す。
            auto spec = std::make_shared<Spectrogram>(analyze_file(path));
            return [is_base, path, spec](App& a) {
                apply_track(is_base ? a.base : a.target, path, std::move(*spec));
            };
        } catch (const std::exception& e) {
            applog::add(name + " 読み込み失敗: " + e.what());
            return {};
        }
    });
}

// 左パネル内の、トラック1つ分の読み込み/再生操作と情報表示。
void draw_track_controls(App& app, Track& tr, const char* label) {
    ImGui::PushID(&tr);
    ImGui::TextUnformatted(label);

    ImGui::BeginDisabled(app.ui_job_running);
    if (ImGui::Button("読み込む", ImVec2(-1, 0))) launch_load_track_job(app, &tr == &app.base);
    ImGui::EndDisabled();

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
        setup_erb_yaxis_ticks(sp.fs / 2.0);
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
    // 既定の保存先はカレントディレクトリ。読み込みも同じ場所から開始。
    static const std::string kDefaultPath = default_session_path();

    // 保存は base/target が両方読み込まれているとき（waves パスが有効）だけ許可。
    // ダイアログはブロックするのでワーカーで開く（書き出し自体は速いのでメインで）。
    ImGui::BeginDisabled(!(app.base.loaded() && app.target.loaded()) || app.ui_job_running);
    if (ImGui::Button("セッション保存", ImVec2(-1, 0))) {
        launch_ui_job(app, [def = kDefaultPath]() -> std::function<void(App&)> {
            static const char* kJsonFilter[] = { "*.json" };
            const char* p = tinyfd_saveFileDialog("セッションを保存", def.c_str(), 1, kJsonFilter, "JSON");
            if (!p) return {};    // キャンセル
            const std::string path = p;
            return [path](App& a) { save_session(a, path); };
        });
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(app.ui_job_running);
    if (ImGui::Button("セッション読み込み", ImVec2(-1, 0))) {
        launch_ui_job(app, [def = kDefaultPath]() -> std::function<void(App&)> {
            static const char* kJsonFilter[] = { "*.json" };
            const char* p = tinyfd_openFileDialog("セッションを読み込み", def.c_str(), 1, kJsonFilter, "JSON", 0);
            if (!p) return {};    // キャンセル
            // JSON パース＋両音声の解析までワーカーで行う（GL なし）。
            auto d = std::make_shared<SessionLoadData>(load_session_data(p));
            if (!d->ok) return {};
            return [d](App& a) { apply_session_data(a, std::move(*d)); };
        });
    }
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

    const Minimap base_mm   = app.show_minimap ? Minimap::Above : Minimap::None;
    const Minimap target_mm = app.show_minimap ? Minimap::Below : Minimap::None;

    static std::vector<EdgePoint> base_edges, target_edges;
    draw_spectrogram(app, app.base, /*is_base=*/true, each_h, base_edges, base_mm);
    ImGui::Spacing();
    draw_spectrogram(app, app.target, /*is_base=*/false, each_h, target_edges, target_mm);

    draw_anchor_connectors(base_edges, target_edges);
}

// 画面下部の動作ログ領域。ヘッダで折りたたみ可能（開閉状態は app.log_open に保持し、
// draw_root が前フレームの状態から高さを決める）。最下部にいるときは自動追従。
void draw_log_panel(App& app) {
    app.log_open = ImGui::CollapsingHeader("ログ", ImGuiTreeNodeFlags_DefaultOpen);
    if (!app.log_open) return;

    ImGui::BeginChild("log_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : applog::lines()) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

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

// double 値を [0,1] スライダーで編集（幅指定つき）。再合成トリガを返す
// （realtime=true なら値が変わったフレーム、false なら離したフレーム）。
bool rate_slider(const char* label, double& v, float width, bool realtime) {
    ImGui::SetNextItemWidth(width);
    float      f       = static_cast<float>(v);
    const bool changed = ImGui::SliderFloat(label, &f, 0.0f, 1.0f, "%.2f");
    if (changed) v = f;
    return realtime ? changed : ImGui::IsItemDeactivatedAfterEdit();
}

// 上段: 軸ごとの率スライダー（一括リンク・リアルタイム更新の切替つき）。
// 再合成すべきフレームで true を返す。
bool draw_morph_sliders(App& app) {
    ImGui::TextUnformatted("モーフィング率 (0 = Base, 1 = Target)");
    ImGui::SameLine();
    if (ImGui::Checkbox("全軸を一括操作", &app.morph_link) && app.morph_link)
        app.morph_rates = MorphRates::uniform(app.morph_rates.tx);
    ImGui::SameLine();
    ImGui::Checkbox("リアルタイム更新", &app.morph_realtime);
    ImGui::Separator();

    // スライダーを中央揃えにする。バー位置が行間で揃うよう、最長ラベルを基準に
    // 「バー＋ラベル」全体の幅から共通のオフセットを計算して Indent する。
    const float avail    = ImGui::GetContentRegionAvail().x;
    const float slider_w = avail * 0.5f;
    float       label_w  = 0.0f;
    for (const char* l : { "時間 (tx)", "周波数 (fx)", "F0 (fo)", "スペクトル (sl)", "非周期性 (ap)", "率 (全軸)" })
        label_w = std::max(label_w, ImGui::CalcTextSize(l).x);
    const float offset =
      std::max(0.0f, (avail - (slider_w + ImGui::GetStyle().ItemInnerSpacing.x + label_w)) * 0.5f);

    ImGui::Indent(offset);
    bool trigger = false;
    if (app.morph_link) {
        ImGui::SetNextItemWidth(slider_w);
        float      f       = static_cast<float>(app.morph_rates.tx);
        const bool changed = ImGui::SliderFloat("率 (全軸)", &f, 0.0f, 1.0f, "%.2f");
        if (changed) app.morph_rates = MorphRates::uniform(f);
        trigger = app.morph_realtime ? changed : ImGui::IsItemDeactivatedAfterEdit();
    } else {
        trigger |= rate_slider("時間 (tx)", app.morph_rates.tx, slider_w, app.morph_realtime);
        trigger |= rate_slider("周波数 (fx)", app.morph_rates.fx, slider_w, app.morph_realtime);
        trigger |= rate_slider("F0 (fo)", app.morph_rates.fo, slider_w, app.morph_realtime);
        trigger |= rate_slider("スペクトル (sl)", app.morph_rates.sl, slider_w, app.morph_realtime);
        trigger |= rate_slider("非周期性 (ap)", app.morph_rates.ap, slider_w, app.morph_realtime);
    }
    ImGui::Unindent(offset);
    return trigger;
}

// タブ表示時に base/target の解析チャンネルを最新化する。読み込まれている音声のパスが
// 変わったときだけワーカーで再解析し（UI ジョブとして1本ずつ）、反映時に共通 dB レンジと
// テクスチャを作り直して morphed を無効化する。
void ensure_morph_channels(App& app) {
    if (app.ui_job_running) return;    // 進行中の UI ジョブと直列化

    // 古くなっている方（base 優先）を1つだけ処理する。両方古い場合は次のフレームで続き。
    const auto stale = [](const Track& tr, const std::string& ch_path) {
        return tr.loaded() && ch_path != tr.path;
    };
    bool is_base;
    if (stale(app.base, app.morph_base_path)) is_base = true;
    else if (stale(app.target, app.morph_target_path)) is_base = false;
    else return;

    const std::string name = is_base ? app.base.name : app.target.name;
    const std::string path = is_base ? app.base.path : app.target.path;
    // 先に試行済みパスを記録して、失敗時に毎フレーム再解析されるのを防ぐ。
    (is_base ? app.morph_base_path : app.morph_target_path) = path;

    launch_ui_job(app, [name, path, is_base]() -> std::function<void(App&)> {
        applog::add(name + " をモーフィング用に解析中...");
        const auto  t0 = std::chrono::steady_clock::now();
        std::string err;
        MorphChannel c = analyze_channel(path, err);

        std::shared_ptr<const MorphChannel> ch;    // 失敗時は nullptr のまま反映
        if (!err.empty()) {
            applog::add(name + " 解析失敗: " + err);
        } else {
            ch = std::make_shared<const MorphChannel>(std::move(c));
            const double ms =
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s 解析完了 (%.2f ms)", name.c_str(), ms);
            applog::add(buf);
        }
        return [is_base, ch](App& a) {
            (is_base ? a.morph_base : a.morph_target) = ch;
            ++a.morph_epoch;     // 実行中モーフの結果は古い base/target のものなので破棄対象に
            a.morph_out = {};    // 元が変わったので以前の morphed は無効
            rebuild_morph_bt_textures(a);
        };
    });
}

// 非同期でモーフィングを開始する（実行中なら「最新条件で1回だけ再実行」を予約）。
// ワーカーは shared_ptr 経由の immutable なチャンネルと、コピーしたアンカー/率だけを使う。
void request_morph(App& app) {
    if (!app.morph_base || !app.morph_target) return;
    if (app.morph_job_running) {
        app.morph_job_pending = true;
        return;
    }
    app.morph_job_running = true;
    app.morph_job_pending = false;
    app.morph_job_epoch   = app.morph_epoch;
    app.morph_job_t0      = std::chrono::steady_clock::now();

    const auto base    = app.morph_base;
    const auto target  = app.morph_target;
    const auto anchors = app.anchors;    // コピー（ジョブ中の編集と分離）
    const auto rates   = app.morph_rates;
    app.morph_job      = std::async(std::launch::async, [base, target, anchors, rates] {
        return morphing_channels(*base, *target, anchors, rates);
    });
}

// 中段: base / morphed / target × f0 / sp / ap の 3×3 グリッド。
// サブプロット（LinkAllX + LinkRows）で軸範囲を共有し、目盛りラベルは左端列と最下行のみ、
// タイトルは最上行のみに出して隙間を最小化する。
void draw_morph_plots(App& app) {
    // 共通レンジ: X は base/target の長い方、F0 は base/target の最大値、sp/ap は ERB 全域
    // （読み込まれている側から算出。morphed は必ずこの範囲に収まる）。
    double              x_max = 0.0, f0_max = 0.0;
    const MorphChannel* ref = nullptr;
    for (const MorphChannel* ch : { app.morph_base.get(), app.morph_target.get() }) {
        if (ch == nullptr || ch->empty()) continue;
        ref   = ch;
        x_max = std::max(x_max, ch->duration);
        if (ch->f0().size() > 0) f0_max = std::max(f0_max, ch->f0().maxCoeff());
    }
    if (ref == nullptr) {
        ImGui::TextDisabled("音声を読み込むとここに base / morphed / target のプロットを表示します");
        return;
    }
    const double y_f0    = f0_max > 0.0 ? f0_max * 1.1 : 500.0;
    const double nyq     = ref->fs / 2.0;
    const double erb_max = freqscale::hz_to_erb(nyq);

    static const MorphChannel kEmptyCh;    // 未解析の列は空データとして描く（軸のみ）
    const MorphChannel* chs[3]        = { app.morph_base ? app.morph_base.get() : &kEmptyCh,
                                          &app.morph_out.morphed,
                                          app.morph_target ? app.morph_target.get() : &kEmptyCh };
    const char*         col_titles[3] = { "Base", "Morphed", "Target" };
    const char*         row_ylabel[3] = { "F0 [Hz]", "SP [Hz]", "AP [Hz]" };

    if (ImPlot::BeginSubplots("##morphgrid", 3, 3, ImVec2(-1, -1),
                              ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_LinkRows
                                | ImPlotSubplotFlags_NoMenus | ImPlotSubplotFlags_NoResize)) {
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                const MorphChannel& ch = *chs[col];

                char label[32];
                std::snprintf(label, sizeof label, "%s###cell%d%d", col_titles[col], row, col);
                ImPlotFlags flags = ImPlotFlags_NoLegend | ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;
                if (row != 0) flags |= ImPlotFlags_NoTitle;    // タイトルは最上行のみ

                if (!ImPlot::BeginPlot(label, ImVec2(-1, 0), flags)) continue;

                // 目盛りラベルは左端列・最下行のみ（位置は全セル共通なのでグリッドは揃う）。
                const ImPlotAxisFlags xf = row == 2 ? ImPlotAxisFlags_None : ImPlotAxisFlags_NoTickLabels;
                const ImPlotAxisFlags yf = col == 0 ? ImPlotAxisFlags_None : ImPlotAxisFlags_NoTickLabels;
                // 時間軸ラベルは非表示（目盛りは最下行のみ）。
                ImPlot::SetupAxes(nullptr, col == 0 ? row_ylabel[row] : nullptr, xf, yf);
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, x_max, ImPlotCond_Always);
                if (row == 0) {
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, y_f0, ImPlotCond_Always);
                } else {
                    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, erb_max, ImPlotCond_Always);
                    setup_erb_yaxis_ticks(nyq);    // 位置は全セル同じ、ラベル表示は yf が制御
                }

                if (row == 0) {
                    // F0 ライン。
                    std::vector<double> xs(ch.n_frames), ys(ch.n_frames);
                    for (int i = 0; i < ch.n_frames; ++i) {
                        xs[i] = i * ch.frame_period / 1000.0;
                        ys[i] = ch.f0()[i];    // base/target は無声が 0（morphed は全フレーム有声）
                    }
                    if (ch.n_frames > 0) ImPlot::PlotLine("F0", xs.data(), ys.data(), ch.n_frames);
                } else {
                    // sp / ap のヒートマップ（ERB 等間隔テクスチャ、色スケールは3枚共通）。
                    const unsigned int tex = row == 1 ? app.morph_tex_sp[col] : app.morph_tex_ap[col];
                    if (tex)
                        ImPlot::PlotImage("##hm", static_cast<ImTextureID>(tex), ImPlotPoint(0, 0),
                                          ImPlotPoint(ch.duration, erb_max));
                }
                ImPlot::EndPlot();
            }
        }
        ImPlot::EndSubplots();
    }
}

// wave をメモリから再生。
void play_wave(App& app, const std::vector<double>& wave, int fs) {
    std::vector<float> pcm(wave.size());
    for (std::size_t i = 0; i < wave.size(); ++i) pcm[i] = static_cast<float>(std::clamp(wave[i], -1.0, 1.0));
    try {
        app.engine.play_pcm(pcm.data(), pcm.size(), 1, static_cast<unsigned>(fs));
    } catch (const std::exception& e) {
        applog::add(std::string { "再生失敗: " } + e.what());
    }
}

// 毎フレーム呼ぶ: 完了した非同期ジョブを回収し、morph_out とテクスチャを更新する
// （GL への反映はここ＝メインスレッドで行う）。再要求が予約されていれば最新条件で再実行。
void poll_morph_job(App& app) {
    if (!app.morph_job_running) return;
    if (app.morph_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;

    MorphOutput out       = app.morph_job.get();
    app.morph_job_running = false;

    if (app.morph_job_epoch != app.morph_epoch) {
        // base/target が差し替わった後に完了した古い結果は捨てる。
        app.morph_play_when_done = false;
    } else {
        const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - app.morph_job_t0).count();
        app.morph_out = std::move(out);
        // 警告はアンカーが同じなら毎回同じ内容になる。リアルタイム更新でスライダーを
        // 動かすたびに積むとログが埋まるので、内容が変わったときだけ出す。
        static std::vector<std::string> last_warnings;
        if (app.morph_out.warnings != last_warnings) {
            last_warnings = app.morph_out.warnings;
            for (const std::string& w : last_warnings) applog::add("警告: " + w);
        }
        if (!app.morph_out.ok()) {
            applog::add(app.morph_out.error);
        } else {
            char buf[128];
            std::snprintf(buf, sizeof buf, "モーフィング生成: %zu samples (%.2f ms)", app.morph_out.wave.size(),
                          ms);
            applog::add(buf);
        }
        rebuild_morphed_texture(app);
        if (app.morph_play_when_done && app.morph_out.ok()) play_wave(app, app.morph_out.wave, app.morph_out.fs);
        app.morph_play_when_done = false;
    }

    if (app.morph_job_pending) request_morph(app);
}

// 下段: 出力設定（生成/再生/WAV保存）。
void draw_morph_output(App& app) {
    const bool ready = app.morph_base && app.morph_target;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("生成して再生")) {
        app.morph_play_when_done = true;    // 完了回収時に再生
        request_morph(app);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(app.morph_out.wave.empty());
    if (ImGui::Button("再生")) play_wave(app, app.morph_out.wave, app.morph_out.fs);
    ImGui::SameLine();
    ImGui::BeginDisabled(app.ui_job_running);
    if (ImGui::Button("WAV 保存")) {
        // 波形をコピーしてワーカーへ（ダイアログ→書き出しまでワーカーで完結。GL なし）。
        auto      wave = std::make_shared<const std::vector<double>>(app.morph_out.wave);
        const int fs   = app.morph_out.fs;
        launch_ui_job(app, [wave, fs]() -> std::function<void(App&)> {
            static const char* kWavFilter[] = { "*.wav" };
            const char* p = tinyfd_saveFileDialog("モーフィング結果を保存", "morph.wav", 1, kWavFilter, "WAV");
            if (!p) return {};    // キャンセル
            std::string werr;
            if (write_wav(p, *wave, fs, werr))
                applog::add(std::string { "WAV 保存: " } + p);
            else
                applog::add("WAV 保存失敗: " + werr);
            return {};
        });
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // 実行中インジケータ。
    if (app.morph_job_running) {
        ImGui::SameLine();
        ImGui::TextDisabled("生成中...");
    }
}

// 「モーフィング」タブ: 上=スライダー(内容の高さ) / 中=プロット(残り全部) / 下=出力設定(内容の高さ)。
void draw_morph_tab(App& app) {
    // タブ表示中は base/target の解析チャンネルを最新化（パス変更時のみ再解析）。
    ensure_morph_channels(app);

    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float fh      = ImGui::GetFrameHeight();
    const float wpy     = ImGui::GetStyle().WindowPadding.y;
    // 出力設定はボタン1行ぶんの高さ（子ウィンドウのパディング＋枠込み）。
    const float out_h = fh + wpy * 2.0f + 2.0f;
    // スライダー部は「ラベル行＋区切り＋スライダー5本」の固定高さ。
    // 一括操作の切替（1本/5本）でレイアウトが変わらないよう、常に5本分を確保する。
    const float slider_h = (fh + spacing)              // ラベル＋チェックボックス行
                         + (1.0f + spacing)            // セパレータ
                         + (5 * fh + 4 * spacing)      // スライダー5本
                         + wpy * 2.0f + 2.0f;          // 子ウィンドウのパディング＋枠

    bool rates_changed = false;
    ImGui::BeginChild("morph_sliders", ImVec2(0, slider_h), true);
    rates_changed = draw_morph_sliders(app);
    ImGui::EndChild();

    // 再合成トリガ（リアルタイム更新 ON=値が変わった各フレーム / OFF=離した時）。
    // 非同期実行なので UI はブロックしない。実行中の再要求は最新条件に畳まれる。
    if (rates_changed) request_morph(app);

    // プロットは出力設定ぶんを下に残して、残り全部を使う。
    ImGui::BeginChild("morph_plots", ImVec2(0, -(out_h + spacing)), true);
    draw_morph_plots(app);
    ImGui::EndChild();

    ImGui::BeginChild("morph_output", ImVec2(0, 0), true);
    draw_morph_output(app);
    ImGui::EndChild();
}

// 「ライセンス表示」タブ: 左=同梱物のリスト(1) / 右=選択した条文の表示(4)。
// 同梱物が増えたら kLicenseEntries に1行追加する（パスはカレント起動と bin/ 起動の2候補）。
void draw_license_tab(App& app) {
    struct Entry {
        const char* name;     // リスト表示名
        const char* path;     // 条文ファイル（カレントディレクトリ起動）
        const char* alt;      // 同（bin/ 起動）
    };
    // バイナリ配布時に条文の同梱が必要なもの（MIT/BSD/OFL）。zlib 系（GLFW,
    // tinyfiledialogs）と public domain/MIT-0 の miniaudio は明記義務がないため省略。
    static const Entry kLicenseEntries[] = {
        { "Gen Interface JP（フォント / OFL v1.1）", "font/Gen Interface JP/OFL.txt",
          "../font/Gen Interface JP/OFL.txt" },
        { "Dear ImGui（MIT）", "licenses/imgui.txt", "../licenses/imgui.txt" },
        { "ImPlot（MIT）", "licenses/implot.txt", "../licenses/implot.txt" },
        { "nlohmann JSON（MIT）", "licenses/nlohmann-json.txt", "../licenses/nlohmann-json.txt" },
        { "WORLD（修正BSD）", "licenses/world.txt", "../licenses/world.txt" },
    };
    static int         selected = 0;
    static int         loaded   = -1;    // 読み込み済みの選択（変わったら読み直す）
    static std::string text;

    // 左: リスト。
    const float left_w = ImGui::GetContentRegionAvail().x * (1.0f / 5.0f);
    ImGui::BeginChild("license_list", ImVec2(left_w, 0), true);
    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kLicenseEntries)); ++i)
        if (ImGui::Selectable(kLicenseEntries[i].name, selected == i)) selected = i;
    ImGui::EndChild();

    ImGui::SameLine();

    // 右: 選択された条文。
    ImGui::BeginChild("license_view", ImVec2(0, 0), true);
    if (loaded != selected) {
        loaded = selected;
        text   = "(ライセンスファイルが見つかりません)";
        for (const char* p : { kLicenseEntries[selected].path, kLicenseEntries[selected].alt }) {
            std::ifstream is { p };
            if (!is) continue;
            std::ostringstream ss;
            ss << is.rdbuf();
            text = ss.str();
            break;
        }
    }
    ImGui::TextUnformatted(kLicenseEntries[selected].name);
    ImGui::Separator();
    ImGui::BeginChild("license_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    // 条文は等幅前提で整形されているため、等幅フォント（ImGui 埋め込みの ProggyClean）で表示する。
    if (app.mono_font) ImGui::PushFont(app.mono_font);
    ImGui::TextUnformatted(text.c_str());
    if (app.mono_font) ImGui::PopFont();
    ImGui::EndChild();
    ImGui::EndChild();
}

}    // namespace

void draw_root(App& app) {
    // 非同期ジョブの完了回収（どのタブにいても回収できるようここで毎フレーム）。
    poll_ui_job(app);
    poll_morph_job(app);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                   | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoCollapse);

    // 上=タブ（アライメント/モーフィング）、下=動作ログ（全タブ共通・折りたたみ可）。
    // ログ高さは従来(0.2)の 2/3。折りたたみ時はヘッダ分だけ確保する（前フレームの開閉状態を使用）。
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float log_h   = app.log_open
                            ? std::max(70.0f, avail_h * (0.2f * 2.0f / 3.0f))
                            : ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f + 2.0f;
    const float tab_h   = avail_h - log_h - ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("tabarea", ImVec2(0, tab_h), false);
    // UI ジョブ実行中（ダイアログ表示中や解析中）はタブ切替も無効化する。
    // タブの中身は操作可能なままにしたいので、選択中タブの描画中だけ無効化を解除する。
    const bool tabs_locked = app.ui_job_running;
    ImGui::BeginDisabled(tabs_locked);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("アライメント")) {
            ImGui::EndDisabled();
            draw_align_tab(app);
            ImGui::BeginDisabled(tabs_locked);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("モーフィング")) {
            ImGui::EndDisabled();
            draw_morph_tab(app);
            ImGui::BeginDisabled(tabs_locked);
            ImGui::EndTabItem();
        }
        // ライセンス表示タブはグレー系（濃いめ）にして機能タブと見分けやすくする。
        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.34f, 0.34f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.16f, 0.16f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
        if (ImGui::BeginTabItem("ライセンス表示")) {
            ImGui::EndDisabled();
            draw_license_tab(app);
            ImGui::BeginDisabled(tabs_locked);
            ImGui::EndTabItem();
        }
        ImGui::PopStyleColor(5);
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    ImGui::BeginChild("log", ImVec2(0, 0), true);
    draw_log_panel(app);
    ImGui::EndChild();

    ImGui::End();
}
