#include "ui_tabs.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "app.hpp"
#include "file_jobs.hpp"
#include "freqscale.hpp"
#include "morph_controller.hpp"
#include "ui_common.hpp"

namespace {

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

// 下段: 出力設定（生成/再生/WAV保存）。
void draw_morph_output(App& app) {
    const bool ready = app.morph.channel(Side::Base) && app.morph.channel(Side::Target);
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("生成して再生")) {
        app.morph.play_request = true;    // 完了回収時に再生
        request_morph(app);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(app.morph.out.wave.empty());
    if (ImGui::Button("再生")) play_wave(app, app.morph.out.wave, app.morph.out.fs);
    ImGui::SameLine();
    ImGui::BeginDisabled(app.jobs.ui.busy());
    if (ImGui::Button("WAV 保存")) launch_save_wav_job(app);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // 実行中インジケータ。
    if (app.morph.job_running) {
        ImGui::SameLine();
        ImGui::TextDisabled("生成中...");
    }
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
    rates_changed = draw_morph_sliders(app, rates_released);
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
}
