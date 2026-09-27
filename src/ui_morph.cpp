#include "ui_tabs.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>    // std::string 版 InputText
#include <implot.h>

#include "app.hpp"
#include "batch_export.hpp"
#include "file_jobs.hpp"
#include "freqscale.hpp"
#include "morph_controller.hpp"
#include "ui_common.hpp"

namespace {

constexpr ImVec4 kErrCol { 1.00f, 0.45f, 0.40f, 1.0f };
constexpr ImVec4 kWarnCol { 1.00f, 0.80f, 0.35f, 1.0f };

// 軸の表示名（添字は kMorphAxes）。
constexpr const char* kAxisLabels[kMorphAxisCount] = { "時間 (tx)", "周波数 (fx)", "F0 (fo)", "スペクトル (sl)",
                                                       "非周期性 (ap)" };

// double 値を [0,1] スライダーで編集（幅指定つき）。再合成トリガを返す
// （realtime=true なら値が変わったフレーム、false なら離したフレーム）。
// つまみを離したフレームは、自動再生の判定用に released にも立てる（realtime とは
// 独立: リアルタイム更新中でも再生は離した時だけにしたいため）。
bool rate_slider(const char* label, double& v, float width, bool realtime, bool& released) {
    ImGui::SetNextItemWidth(width);
    float      f       = static_cast<float>(v);
    const bool changed = ImGui::SliderFloat(label, &f, 0.0f, 1.0f, "%.2f");
    if (changed) v = f;
    const bool done = ImGui::IsItemDeactivatedAfterEdit();
    released |= done;
    return realtime ? changed : done;
}

// 上段: 軸ごとの率スライダー（一括リンク・リアルタイム更新の切替つき）。
// 再合成すべきフレームで true を返し、つまみを離したフレームは released に立てる。
bool draw_morph_sliders(App& app, bool& released) {
    ImGui::TextUnformatted("モーフィング率 (0 = Base, 1 = Target)");
    ImGui::SameLine();
    if (ImGui::Checkbox("全軸を一括操作", &app.morph.link) && app.morph.link)
        app.morph.rates = MorphRates::uniform(app.morph.rates.tx);
    ImGui::SameLine();
    ImGui::Checkbox("リアルタイム更新", &app.morph.realtime);
    ImGui::SameLine();
    ImGui::Checkbox("離したら再生", &app.morph.autoplay);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("スライダーのつまみを離したら、その率で合成し直して自動で再生します。");
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
    if (app.morph.link) {
        ImGui::SetNextItemWidth(slider_w);
        float      f       = static_cast<float>(app.morph.rates.tx);
        const bool changed = ImGui::SliderFloat("率 (全軸)", &f, 0.0f, 1.0f, "%.2f");
        if (changed) app.morph.rates = MorphRates::uniform(f);
        const bool done = ImGui::IsItemDeactivatedAfterEdit();
        released |= done;
        trigger = app.morph.realtime ? changed : done;
    } else {
        trigger |= rate_slider("時間 (tx)", app.morph.rates.tx, slider_w, app.morph.realtime, released);
        trigger |= rate_slider("周波数 (fx)", app.morph.rates.fx, slider_w, app.morph.realtime, released);
        trigger |= rate_slider("F0 (fo)", app.morph.rates.fo, slider_w, app.morph.realtime, released);
        trigger |= rate_slider("スペクトル (sl)", app.morph.rates.sl, slider_w, app.morph.realtime, released);
        trigger |= rate_slider("非周期性 (ap)", app.morph.rates.ap, slider_w, app.morph.realtime, released);
    }
    ImGui::Unindent(offset);
    return trigger;
}

// 中段: base / morphed / target × f0 / sp / ap の 3×3 グリッド。
// サブプロット（LinkAllX + LinkRows）で軸範囲を共有し、目盛りラベルは左端列と最下行のみ、
// タイトルは最上行のみに出して隙間を最小化する。
void draw_morph_plots(App& app) {
    // 共通レンジ: X は base/target の長い方、F0 は base/target の最大値、sp/ap は ERB 全域
    // （読み込まれている側から算出。morphed は必ずこの範囲に収まる）。
    double              x_max = 0.0, f0_max = 0.0;
    const MorphChannel* ref = nullptr;
    for (Side s : kSides) {
        const MorphChannel* ch = app.morph.channel(s);
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
    const auto          or_empty      = [](const MorphChannel* c) { return c ? c : &kEmptyCh; };
    const MorphChannel* chs[3]        = { or_empty(app.morph.channel(Side::Base)), &app.morph.out.morphed,
                                          or_empty(app.morph.channel(Side::Target)) };
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
                    const GlTexture& tex = row == 1 ? app.morph.tex_sp[col] : app.morph.tex_ap[col];
                    if (tex)
                        ImPlot::PlotImage("##hm", tex.imgui_id(), ImPlotPoint(0, 0),
                                          ImPlotPoint(ch.duration, erb_max));
                }
                ImPlot::EndPlot();
            }
        }
        ImPlot::EndSubplots();
    }
}

// 下段: 出力設定（生成/再生/WAV保存/連続書き出し）。
void draw_morph_output(App& app) {
    MorphState& m        = app.morph;
    const bool  ready    = m.channel(Side::Base) && m.channel(Side::Target);
    const bool  batching = m.batch_running();

    // 連続書き出し中は、同じ音声を取り合わないよう対話的な生成と再生を止める。
    ImGui::BeginDisabled(!ready || batching);
    if (ImGui::Button("生成して再生")) {
        m.play_request = true;    // 完了回収時に再生
        request_morph(app);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(m.out.wave.empty() || batching);
    if (ImGui::Button("再生")) play_wave(app, m.out.wave, m.out.fs);
    ImGui::SameLine();
    ImGui::BeginDisabled(app.jobs.ui.busy());
    if (ImGui::Button("WAV 保存")) launch_save_wav_job(app);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // 連続書き出し。実行中も押せるようにして、閉じた進捗のダイアログを開き直せるようにする。
    ImGui::SameLine();
    ImGui::BeginDisabled(!ready || (app.jobs.ui.busy() && !batching));
    if (ImGui::Button("連続書き出し...")) {
        if (!batching) {
            if (!m.batch_initialized) {
                m.batch.fixed       = m.rates;
                m.batch_initialized = true;
            }
            if (m.batch.dir.empty()) m.batch.dir = batch_default_dir(app);
            if (m.batch.prefix.empty()) m.batch.prefix = batch_default_prefix(app);
            m.batch_existing = -1;    // 開くたびに数え直す（外でファイルが増えているかもしれない）
        }
        m.batch_open = true;
    }
    ImGui::EndDisabled();

    // 実行中インジケータ。
    if (m.job_running) {
        ImGui::SameLine();
        ImGui::TextDisabled("生成中...");
    }
    if (batching) {
        ImGui::SameLine();
        ImGui::TextDisabled("連続書き出し %d / %d", m.batch_progress->done.load(), m.batch_total);
    }
}

// ボタンを右寄せで並べるための幅（ラベルの幅＋枠の余白＋ボタン間の間隔）。
float buttons_width(std::initializer_list<const char*> labels) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float             w  = 0.0f;
    for (const char* l : labels) w += ImGui::CalcTextSize(l, nullptr, true).x + st.FramePadding.x * 2.0f;
    return w + st.ItemSpacing.x * static_cast<float>(labels.size() - 1);
}

// 続けて置くボタン群が右端に揃うよう、カーソルを動かす。
void align_right(float width) {
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - width));
}

// フォームの項目どうしの間隔 [px]。項目の行の間に、この高さの空の行を挟んで空ける。
constexpr float kFormRowGap = 8.0f;

// フォームの1行の見出し（左の列）。右の列の入力欄と縦位置を揃える。
// 2行目以降は、前の項目との間に空の行を挟む（セルの余白を広げると、項目の中の行間まで開くため）。
void form_label(const char* label) {
    if (ImGui::TableGetRowIndex() >= 0) ImGui::TableNextRow(ImGuiTableRowFlags_None, kFormRowGap);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
}

// 変化させる軸の表（軸ごとのチェックと、外した軸の固定値）。設定が変わったら true。
bool draw_batch_axes(MorphState& m) {
    bool changed = false;
    if (ImGui::BeginTable("##axes", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("軸", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("値", ImGuiTableColumnFlags_WidthStretch);
        constexpr double kZero = 0.0, kOne = 1.0;
        for (int i = 0; i < kMorphAxisCount; ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // ファイル名に入る軸の名前が変わるので、上書きの数を数え直す。
            changed |= ImGui::Checkbox(kAxisLabels[i], &m.batch.sweep[i]);
            ImGui::TableNextColumn();
            if (m.batch.sweep[i]) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("0 → 1 に変化");
            } else {
                // 固定値は JSON にそのまま残るので、float を経由せず double のまま編集する。
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderScalar("##fixed", ImGuiDataType_Double, &(m.batch.fixed.*kMorphAxes[i].rate), &kZero,
                                    &kOne, "固定 %.2f");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::BeginDisabled(batch_sweeps_all_axes(m.batch));
    if (ImGui::Button("すべて選択")) {
        for (bool& b : m.batch.sweep) b = true;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("固定値に今のスライダーの率を使う")) m.batch.fixed = m.rates;
    return changed;
}

// 連続書き出しの設定ダイアログ。実行中は進捗と「中止」に切り替わる。
// Esc で閉じられても書き出しは続く（進捗は上の出力行にも出る）。
// 大きさはメインウィンドウの 8 割に合わせて毎フレーム決め直す（ウィンドウの大きさを変えても追従する）。
// 設定欄は伸縮する領域に入れてはみ出したらスクロールさせ、結果の見込みとボタンは下端に固定する。
void draw_batch_dialog(App& app) {
    constexpr const char* kPopup     = "連続書き出し";
    constexpr float       kSizeRatio = 0.8f;

    MorphState& m = app.morph;
    if (!m.batch_open) return;
    if (!ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * kSizeRatio, vp->WorkSize.y * kSizeRatio), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(kPopup, nullptr,
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        m.batch_open = false;
        return;
    }

    const bool        running = m.batch_running();
    const ImGuiStyle& style   = ImGui::GetStyle();

    // 下端に固定する部分の高さ: 区切り2本＋見込み3行＋操作2行（実行中の進捗バーと「中止」）。
    // 行数を実行中かどうかで変えないのは、切り替わった瞬間に設定欄の高さが跳ねないようにするため。
    const float footer_h = 2.0f * (1.0f + style.ItemSpacing.y) + 3.0f * ImGui::GetTextLineHeightWithSpacing()
                         + 2.0f * ImGui::GetFrameHeightWithSpacing();

    bool changed = false;
    ImGui::BeginChild("##batch_body", ImVec2(0.0f, -footer_h));
    ImGui::TextWrapped("チェックした軸のモーフィング率を、連動させて 0（Base）から 1（Target）まで"
                       "等間隔に変えながら WAV を書き出します。");
    ImGui::Separator();

    ImGui::BeginDisabled(running);
    if (ImGui::BeginTable("##form", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("見出し", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("入力", ImGuiTableColumnFlags_WidthStretch);

        form_label("本数");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputInt("##count", &m.batch.count)) {
            m.batch.count =
              std::clamp(m.batch.count, BatchExportSettings::kCountMin, BatchExportSettings::kCountMax);
            changed = true;
        }
        char count_help[192];
        std::snprintf(count_help, sizeof count_help,
                      "率 0 と 1 を含めた本数です（%d〜%d）。11 なら 0.00, 0.10, ... 1.00 の 11 本になります。",
                      BatchExportSettings::kCountMin, BatchExportSettings::kCountMax);
        help_marker(count_help);

        // チェックした軸はすべて同じ値で変化し、外した軸は固定値のまま。
        form_label("変化させる軸");
        help_marker("チェックした軸を同じ率で 0 → 1 に変化させます（連動）。すべて選ぶと全軸を一括で変化させます。\n"
                    "チェックを外した軸は右の固定値のままです（Ctrl+クリックで数値を入力できます）。");
        ImGui::TableNextColumn();
        changed |= draw_batch_axes(m);

        // 出力先（ネイティブのダイアログはブロックするのでワーカーで開く）。
        form_label("出力先");
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(app.jobs.ui.busy());
        if (ImGui::Button("選択...")) launch_select_batch_dir_job(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::InputText("##dir", &m.batch.dir);

        form_label("接頭辞");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::InputText("##prefix", &m.batch.prefix);

        form_label("");    // 見出しなし（出力の設定の続き）
        ImGui::TableNextColumn();
        changed |= ImGui::Checkbox("率とアンカーを JSON に残す", &m.batch.write_manifest);
        help_marker("書き出した各ファイルの率のほか、base/target のパスとアンカーを "
                    "<接頭辞>_batch.json（軸を選んだときは軸の名前入り）に残します。"
                    "中身はセッション形式なので、そのまま「セッション読み込み」で開けます。");

        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();

    // 上書きの数を数えるのはディスクを見るので、設定が変わったときだけにする。
    if (changed) m.batch_existing = -1;
    if (m.batch_existing < 0 && !running) m.batch_existing = count_existing_batch_files(m.batch);

    // ── 下端: 結果の見込みと操作 ──
    ImGui::Separator();

    const Status st = validate_batch_settings(m.batch);
    if (!st.ok()) {
        ImGui::TextColored(kErrCol, "%s", st.error.c_str());
    } else {
        const std::vector<double> rates = batch_rates(m.batch.count);
        ImGui::Text("%d ファイル: %s … %s", static_cast<int>(rates.size()),
                    batch_wav_name(m.batch, rates.front()).c_str(), batch_wav_name(m.batch, rates.back()).c_str());
        // 所要時間は直前の対話的な生成の実測から見積もる（未生成なら出せない）。
        if (m.last_ms > 0.0)
            ImGui::TextDisabled("直前の生成 %.0f ms/本 → 所要 約 %.0f 秒", m.last_ms,
                                m.last_ms * static_cast<double>(rates.size()) / 1000.0);
        else
            ImGui::TextDisabled("一度モーフィングを生成すると、所要時間を見積もれます");
        if (m.batch_existing > 0)
            ImGui::TextColored(kWarnCol, "既にある %d ファイルを上書きします", m.batch_existing);
    }

    // 操作（区切り＋2行）は下端に揃える（見込みの行数が変わってもボタンの位置が動かないように）。
    // 2行の高さは 2 * FrameHeightWithSpacing - ItemSpacing.y、区切りは 1 + ItemSpacing.y なので、
    // 区切りの位置は下端から 2 * FrameHeightWithSpacing + 1 だけ上。
    const float controls_y = ImGui::GetWindowHeight() - style.WindowPadding.y
                           - 2.0f * ImGui::GetFrameHeightWithSpacing() - 1.0f;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), controls_y));
    ImGui::Separator();

    if (running) {
        const int   done = m.batch_progress->done.load();
        const float frac = m.batch_total > 0 ? static_cast<float>(done) / m.batch_total : 0.0f;
        char        bar[32];
        std::snprintf(bar, sizeof bar, "%d / %d", done, m.batch_total);
        ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0.0f), bar);

        const bool canceling = m.batch_progress->cancel.load();
        if (canceling) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("中止します（書き出し中の1本が終わるまで待ちます）");
            ImGui::SameLine();
        }
        align_right(buttons_width({ "中止" }));
        ImGui::BeginDisabled(canceling);
        if (ImGui::Button("中止")) m.batch_progress->cancel.store(true);
        ImGui::EndDisabled();
    } else {
        // 実行中と行数を揃えるため、ボタンは2行目（下端）に置く。
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
        align_right(buttons_width({ "実行", "閉じる" }));
        ImGui::BeginDisabled(!st.ok() || app.jobs.ui.busy());
        if (ImGui::Button("実行")) start_batch_export(app);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("閉じる")) {
            m.batch_open = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

}    // namespace

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

    bool rates_changed = false, rates_released = false;
    ImGui::BeginChild("morph_sliders", ImVec2(0, slider_h), true);
    // 連続書き出し中は率を動かせない（動かすと対話的な再合成が始まり、CPU を取り合う）。
    ImGui::BeginDisabled(app.morph.batch_running());
    rates_changed = draw_morph_sliders(app, rates_released);
    ImGui::EndDisabled();
    ImGui::EndChild();

    // 再合成トリガ（リアルタイム更新 ON=値が変わった各フレーム / OFF=離した時）。
    // 非同期実行なので UI はブロックしない。実行中の再要求は最新条件に畳まれる。
    // 「離したら再生」時は、離したフレームでも必ず合成し直す。リアルタイム更新 ON だと
    // 離したフレーム自体は値が変わらず rates_changed が立たないため、これがないと
    // 最後の率の結果を再生できない（実行中なら pending に畳まれて1回で済む）。
    const bool play_on_release = rates_released && app.morph.autoplay;
    if (play_on_release) app.morph.play_request = true;
    if (rates_changed || play_on_release) request_morph(app);

    // プロットは出力設定ぶんを下に残して、残り全部を使う。
    ImGui::BeginChild("morph_plots", ImVec2(0, -(out_h + spacing)), true);
    draw_morph_plots(app);
    ImGui::EndChild();

    ImGui::BeginChild("morph_output", ImVec2(0, 0), true);
    draw_morph_output(app);
    ImGui::EndChild();

    // モーダルは子ウィンドウの外（タブの直下）で開く。
    draw_batch_dialog(app);
}
