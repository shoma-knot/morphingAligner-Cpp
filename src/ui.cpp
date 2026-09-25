#include "ui.hpp"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "app.hpp"
#include "log.hpp"
#include "morph_controller.hpp"
#include "speech_controller.hpp"
#include "ui_tabs.hpp"

namespace {

// 画面下部の動作ログ領域。ヘッダで折りたたみ可能（開閉状態は app.view.log_open に保持し、
// draw_root が前フレームの状態から高さを決める）。最下部にいるときは自動追従。
void draw_log_panel(App& app) {
    app.view.log_open = ImGui::CollapsingHeader("ログ", ImGuiTreeNodeFlags_DefaultOpen);
    if (!app.view.log_open) return;

    ImGui::BeginChild("log_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : applog::lines()) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

}    // namespace

void draw_root(App& app) {
    // 非同期ジョブの完了回収（どのタブにいても回収できるようここで毎フレーム）。
    app.jobs.ui.poll(app);
    app.jobs.tools.poll(app);
    poll_morph_job(app);
    ensure_speech_env(app);    // 音声解析の環境を確認（起動時と「再確認」時）
    ensure_formants(app);      // 読み込まれた音声のフォルマントを自動で推定（環境が使えるときだけ）

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                   | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoCollapse);

    // 上=タブ（アライメント/モーフィング）、下=動作ログ（全タブ共通・折りたたみ可）。
    // ログ高さは従来(0.2)の 2/3。折りたたみ時はヘッダ分だけ確保する（前フレームの開閉状態を使用）。
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float log_h   = app.view.log_open
                            ? std::max(70.0f, avail_h * (0.2f * 2.0f / 3.0f))
                            : ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f + 2.0f;
    const float tab_h   = avail_h - log_h - ImGui::GetStyle().ItemSpacing.y;

    ImGui::BeginChild("tabarea", ImVec2(0, tab_h), false);
    // UI ジョブ実行中（ダイアログ表示中や解析中）はタブ切替も無効化する。
    // タブの中身は操作可能なままにしたいので、選択中タブの描画中だけ無効化を解除する。
    const bool tabs_locked = app.jobs.ui.busy();
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
