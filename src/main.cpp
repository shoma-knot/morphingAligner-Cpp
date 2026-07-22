#include <cstdio>
#include <exception>
#include <fstream>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <GLFW/glfw3.h>

#include "app.hpp"
#include "log.hpp"
#include "ui.hpp"

namespace {

// System CJK font so the Japanese UI labels render (falls back to the built-in
// ASCII font if it is not present).
constexpr const char* kJpFontPath = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";

void load_japanese_font() {
    std::ifstream probe { kJpFontPath };
    if (!probe.good()) return;    // keep the default font
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontFromFileTTF(kJpFontPath, 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
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
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);    // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();
    load_japanese_font();

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
