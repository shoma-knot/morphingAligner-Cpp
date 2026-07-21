#include <algorithm>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <GLFW/glfw3.h>
#include <miniaudio_cpp/audio.hpp>
#include <tinyfiledialogs.h>

#include "analysis.hpp"

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

// Viridis is used both for the pre-baked spectrogram texture and its legend.
constexpr ImPlotColormap kColormap = ImPlotColormap_Viridis;

// Bake the dB spectrogram into an RGBA OpenGL texture once. Drawing it then
// costs a single textured quad per frame instead of one CPU-rebuilt cell per
// (bin * frame), which is what made the ImPlot heatmap crawl on large inputs.
unsigned int make_spectrogram_texture(const Spectrogram& sp) {
    // 256-entry colour lookup table sampled from the colormap.
    unsigned char lut[256][4];
    for (int i = 0; i < 256; ++i) {
        const ImVec4 c = ImPlot::SampleColormap(i / 255.0f, kColormap);
        lut[i][0]      = static_cast<unsigned char>(c.x * 255.0f);
        lut[i][1]      = static_cast<unsigned char>(c.y * 255.0f);
        lut[i][2]      = static_cast<unsigned char>(c.z * 255.0f);
        lut[i][3]      = 255;
    }

    const double               range = sp.db_max > sp.db_min ? sp.db_max - sp.db_min : 1.0;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(sp.num_bins) * sp.num_frames * 4);
    for (std::size_t k = 0; k < sp.values.size(); ++k) {
        const double   t  = std::clamp((sp.values[k] - sp.db_min) / range, 0.0, 1.0);
        const int      li = static_cast<int>(t * 255.0);
        unsigned char* px = &pixels[k * 4];
        px[0]             = lut[li][0];
        px[1]             = lut[li][1];
        px[2]             = lut[li][2];
        px[3]             = lut[li][3];
    }

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Texture width = frames (time / x), height = bins (frequency / y).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sp.num_frames, sp.num_bins, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// One loaded audio source (base or target) with its analysis and GPU texture.
// Owns a GL texture, so it is non-copyable.
struct Track {
    std::string  name;       // "base" / "target", shown in dialog + labels
    std::string  path;       // loaded file ("" = none)
    std::string  status;     // last message for this track
    Spectrogram  spec;       // spectral envelope
    unsigned int tex = 0;    // baked GPU texture

    explicit Track(std::string n) : name(std::move(n)) {}
    ~Track() {
        if (tex) glDeleteTextures(1, &tex);
    }
    Track(const Track&)            = delete;
    Track& operator=(const Track&) = delete;

    bool loaded() const {
        return !path.empty();
    }
};

// A time-axis correspondence: one vertical anchor on each spectrogram.
// Created with both times equal; dragged apart later to define the mapping.
struct Anchor {
    double base_t;
    double target_t;
};

// Application state shared across the frame.
struct App {
    ma::engine          engine;    // audio output device (shared by both tracks)
    Track               base { "base" };
    Track               target { "target" };
    std::vector<Anchor> anchors;    // base<->target time correspondences
};

void load_track(Track& tr) {
    static const char* filters[] = { "*.wav", "*.flac", "*.mp3", "*.ogg" };
    const std::string  title     = tr.name + " 音声を選択";
    const char*        picked    = tinyfd_openFileDialog(title.c_str(), "", 4, filters, "音声ファイル", 0);
    if (!picked) return;    // cancelled

    if (tr.tex) {
        glDeleteTextures(1, &tr.tex);
        tr.tex = 0;
    }
    try {
        tr.spec   = analyze_file(picked);
        tr.tex    = make_spectrogram_texture(tr.spec);
        tr.path   = picked;
        tr.status = "読み込み完了";
    } catch (const std::exception& e) {
        tr.spec = {};
        tr.path.clear();
        tr.status = std::string { "読み込み失敗: " } + e.what();
    }
}

// Load / play controls plus info for a single track, in the left panel.
void draw_track_controls(App& app, Track& tr, const char* label) {
    ImGui::PushID(&tr);
    ImGui::TextUnformatted(label);

    if (ImGui::Button("読み込む", ImVec2(-1, 0))) load_track(tr);

    ImGui::BeginDisabled(!tr.loaded());
    if (ImGui::Button("再生", ImVec2(-1, 0))) {
        try {
            app.engine.play_oneshot(tr.path);
        } catch (const std::exception& e) {
            tr.status = std::string { "再生失敗: " } + e.what();
        }
    }
    ImGui::EndDisabled();

    if (tr.loaded()) {
        ImGui::TextWrapped("%s", tr.path.c_str());
        ImGui::Text("%d Hz, %.2f s", tr.spec.fs, tr.spec.duration);
    } else {
        ImGui::TextDisabled("未読み込み");
    }
    if (!tr.status.empty()) ImGui::TextWrapped("%s", tr.status.c_str());
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
    ImGui::TextDisabled("スペクトログラムを左クリックで追加");
    ImGui::BeginDisabled(app.anchors.empty());
    if (ImGui::Button("アンカーを全消去", ImVec2(-1, 0))) app.anchors.clear();
    ImGui::EndDisabled();
}

// Colour of the time-axis anchors.
constexpr ImVec4 kAnchorCol { 1.0f, 0.35f, 0.2f, 1.0f };

// One spectral-envelope spectrogram of the given pixel height, with the
// draggable time anchors overlaid. `is_base` selects which side of each
// anchor pair this plot edits.
void draw_spectrogram(App& app, Track& tr, bool is_base, float height) {
    ImGui::PushID(&tr);
    ImGui::TextUnformatted(tr.name.c_str());

    if (!tr.loaded()) {
        ImGui::TextDisabled("音声を読み込むとスペクトル包絡を表示します");
        ImGui::PopID();
        return;
    }

    const Spectrogram& sp = tr.spec;
    ImPlot::PushColormap(kColormap);

    // Reserve room on the right for the dB colour scale (legend).
    constexpr float kScaleW = 90.0f;
    const float     plot_w  = ImGui::GetContentRegionAvail().x - kScaleW;

    if (ImPlot::BeginPlot("##spec", ImVec2(plot_w, height))) {
        ImPlot::SetupAxes("時間 [s]", "周波数 [Hz]");
        // Once (not Always) so the user can zoom/pan to place anchors precisely.
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, sp.duration, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, sp.fs / 2.0, ImPlotCond_Once);
        // Pre-baked texture: one quad regardless of the bin * frame count.
        ImPlot::PlotImage(
          "##env", static_cast<ImTextureID>(tr.tex), ImPlotPoint(0, 0), ImPlotPoint(sp.duration, sp.fs / 2.0));

        // Draggable time anchors. DragLineX stays interactive (movable) rather
        // than being baked into the draw list.
        bool any_active = false;
        for (std::size_t i = 0; i < app.anchors.size(); ++i) {
            double* xp      = is_base ? &app.anchors[i].base_t : &app.anchors[i].target_t;
            bool    hovered = false, held = false;
            ImPlot::DragLineX(
              static_cast<int>(i), xp, kAnchorCol, 2.0f, ImPlotDragToolFlags_None, nullptr, &hovered, &held);
            ImPlot::TagX(*xp, kAnchorCol, "%.2f", *xp);
            any_active |= hovered || held;
        }

        // Left-click on empty plot area adds a new anchor pair at that time
        // (same time on both tracks initially).
        if (ImPlot::IsPlotHovered() && !any_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const double t = std::clamp(ImPlot::GetPlotMousePos().x, 0.0, sp.duration);
            app.anchors.push_back(Anchor { t, t });
        }
        ImPlot::EndPlot();
    }
    ImGui::SameLine();
    ImPlot::ColormapScale("dB", sp.db_min, sp.db_max, ImVec2(kScaleW, height));

    ImPlot::PopColormap();
    ImGui::PopID();
}

// Base and target spectrograms stacked vertically (base on top).
void draw_right_panel(App& app) {
    const float labels_h = 2.0f * ImGui::GetTextLineHeightWithSpacing();
    const float avail_h  = ImGui::GetContentRegionAvail().y;
    const float each_h   = std::max(140.0f, (avail_h - labels_h - 12.0f) / 2.0f);

    draw_spectrogram(app, app.base, /*is_base=*/true, each_h);
    ImGui::Spacing();
    draw_spectrogram(app, app.target, /*is_base=*/false, each_h);
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

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    try {
        App app;

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // Single full-viewport window split 1:4 into two columns.
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::Begin(
              "root", nullptr,
              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoCollapse);

            const float avail  = ImGui::GetContentRegionAvail().x;
            const float left_w = avail * (1.0f / 5.0f);

            ImGui::BeginChild("left", ImVec2(left_w, 0), true);
            draw_left_panel(app);
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("right", ImVec2(0, 0), true);
            draw_right_panel(app);
            ImGui::EndChild();

            ImGui::End();

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
