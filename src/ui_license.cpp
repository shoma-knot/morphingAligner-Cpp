#include "ui_tabs.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <imgui.h>

#include "app.hpp"
#include "resource_path.hpp"

// 「ライセンス表示」タブ: 左=同梱物のリスト(1) / 右=選択した条文の表示(4)。
// 同梱物が増えたら kLicenseEntries に1行追加する（パスは配布物のルートからの相対。find_resource で探す）。
void draw_license_tab(App& app) {
    struct Entry {
        const char* name;    // リスト表示名
        const char* path;    // 条文ファイル
    };
    // バイナリ配布時に条文の同梱が必要なもの（MIT/BSD/OFL/Apache-2.0/MPL-2.0）。zlib 系（GLFW,
    // tinyfiledialogs）と public domain/MIT-0 の miniaudio は明記義務がないため省略。
    // Python 環境（parselmouth・MFA など）は配布物に含めず、ユーザーが install-win.bat /
    // install.sh で入れるので、ここには載せない（README で案内する）。
    static const Entry kLicenseEntries[] = {
        { "Gen Interface JP（フォント / OFL v1.1）", "font/Gen Interface JP/OFL.txt" },
        { "Dear ImGui（MIT）", "licenses/imgui.txt" },
        { "ImPlot（MIT）", "licenses/implot.txt" },
        { "nlohmann JSON（MIT）", "licenses/nlohmann-json.txt" },
        { "WORLD（修正BSD）", "licenses/world.txt" },
        { "tcmorph（Apache-2.0）", "licenses/tcmorph.txt" },
        { "Eigen（MPL-2.0）", "licenses/eigen.txt" },
    };
    int&         selected = app.view.license_selected;
    int&         loaded   = app.view.license_loaded;
    std::string& text     = app.view.license_text;

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
        const std::string path = find_resource(kLicenseEntries[selected].path);
        if (std::ifstream is { std::filesystem::u8path(path) }; !path.empty() && is) {
            std::ostringstream ss;
            ss << is.rdbuf();
            text = ss.str();
        }
    }
    ImGui::TextUnformatted(kLicenseEntries[selected].name);
    ImGui::Separator();
    ImGui::BeginChild("license_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    // 条文は等幅前提で整形されているため、等幅フォント（ImGui 埋め込みの ProggyClean）で表示する。
    if (app.view.mono_font) ImGui::PushFont(app.view.mono_font);
    ImGui::TextUnformatted(text.c_str());
    if (app.view.mono_font) ImGui::PopFont();
    ImGui::EndChild();
    ImGui::EndChild();
}
