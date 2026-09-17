#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <GLFW/glfw3.h>

#include "app.hpp"
#include "app_icon.hpp"
#include "log.hpp"
#include "ui.hpp"

namespace {

// 日本語 UI 用フォント。同梱の Gen Interface JP（OFL v1.1、font/ 以下）を読み、
// 見つからなければ ImGui 既定フォントにフォールバック（文字化けする旨をログに出す）。
// 相対パスはカレントディレクトリ起動（プロジェクト/配布ルート）と bin/ 起動の両方を試す。
constexpr const char* kJpFontCandidates[] = {
    "font/Gen Interface JP/GenInterfaceJP-Regular.ttf",
    "../font/Gen Interface JP/GenInterfaceJP-Regular.ttf",
};

// フォントを読み込む。UI 全体は日本語フォント、戻り値はライセンス表示用の
// 等幅フォント（ImGui 埋め込みの ProggyClean。ASCII のみだがライセンス英文には十分）。
ImFont* load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path: kJpFontCandidates) {
        std::ifstream probe { path };
        if (!probe.good()) continue;
        // 最初に追加したフォントが既定になるため、日本語フォントを先に読む。
        io.Fonts->AddFontFromFileTTF(path, 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
        applog::add(std::string { "フォント読み込み: " } + path);
        return io.Fonts->AddFontDefault();    // 2番目: 等幅（ProggyClean）
    }
    applog::add("日本語フォントが見つからないため既定フォントを使用します（文字化けの可能性）");
    return nullptr;    // 既定が ProggyClean になるので等幅の追加は不要
}

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

}    // namespace

int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "morphingAligner", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return 1;
    }
    set_window_icon(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);    // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();
    ImFont* mono_font = load_fonts();

    // Free the left mouse button for anchor placement / dragging by moving the
    // plot pan gesture onto the middle button.
    ImPlot::GetInputMap().Pan = ImGuiMouseButton_Middle;
    // 既定では Ctrl は OverrideMod（押下中は入力を無視して DnD ソース化）に割り当た
    // っている。Ctrl+左クリックを周波数アンカー追加に使うため無効化する。
    ImPlot::GetInputMap().OverrideMod = ImGuiMod_None;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    try {
        App app;
        app.mono_font = mono_font;
        applog::add("起動しました");

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            draw_root(app);

            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
