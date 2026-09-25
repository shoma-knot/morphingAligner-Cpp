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
#include "file_jobs.hpp"
#include "launch_options.hpp"
#include "log.hpp"
#include "resource_path.hpp"
#include "speech_tools.hpp"
#include "ui.hpp"

namespace {

// CMake から渡される版（project() の VERSION）。CMake を通さずに単体で
// コンパイルされた場合に備えて既定値を置く。
#ifndef APP_VERSION
    #define APP_VERSION "unknown"
#endif

constexpr const char* kWindowTitle = "morphingAligner v" APP_VERSION;

// ウィンドウのクラス名（X11 の WM_CLASS / Wayland の app_id）。
// GLFW は既定でこれをウィンドウタイトルから決めるため、タイトルに版を入れると
// クラス名まで版込みになり、デスクトップエントリの StartupWMClass と一致しなくなって
// タスクバーのアイコンが外れる。版と切り離すために明示する。
constexpr const char* kWindowClass = "morphingAligner";

// 日本語 UI 用フォント。同梱の Gen Interface JP（OFL v1.1、font/ 以下）を読み、
// 見つからなければ ImGui 既定フォントにフォールバック（文字化けする旨をログに出す）。
// 配布物のルートからの相対パス（find_resource がルート起動と bin/ 起動の両方を試す）。
constexpr const char* kJpFont = "font/Gen Interface JP/GenInterfaceJP-Regular.ttf";

// 音素記号（IPA）の補助フォント。MFA の日本語モデルは ɕ ʑ や無声化の ̥ を出すが、
// Gen Interface JP にはこれらの字形が無い。OS 標準のフォントを見つかった場合だけ
// 日本語フォントに合成（MergeMode）して、足りない字形だけを補う。同梱はしない。
constexpr const char* kIpaFontCandidates[] = {
    "C:/Windows/Fonts/segoeui.ttf",                         // Windows
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",      // Debian / Ubuntu
    "/usr/share/fonts/TTF/DejaVuSans.ttf",                  // Arch など
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",    // Fedora
};

// 見つかった補助フォントを直前に追加したフォントへ合成する。
void merge_ipa_font(float size) {
    for (const char* path : kIpaFontCandidates) {
        std::ifstream probe { path };
        if (!probe.good()) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &cfg);
        applog::add(std::string { "補助フォント（音素記号用）: " } + path);
        return;
    }
    applog::add("音素記号用の補助フォントが見つかりません（一部の IPA 記号が表示できません）");
}

// フォントを読み込む。UI 全体は日本語フォント、戻り値はライセンス表示用の
// 等幅フォント（ImGui 埋め込みの ProggyClean。ASCII のみだがライセンス英文には十分）。
ImFont* load_fonts() {
    ImGuiIO&          io   = ImGui::GetIO();
    const std::string path = find_resource(kJpFont);
    if (!path.empty()) {
        // 最初に追加したフォントが既定になるため、日本語フォントを先に読む。
        io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
        applog::add("フォント読み込み: " + path);
        merge_ipa_font(18.0f);
        return io.Fonts->AddFontDefault();    // 2番目: 等幅（ProggyClean）
    }
    applog::add("日本語フォントが見つからないため既定フォントを使用します（文字化けの可能性）");
    return nullptr;    // 既定が ProggyClean になるので等幅の追加は不要
}

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

}    // namespace

int main(int argc, char** argv) {
    // コマンドライン引数。--help / --version と引数の誤りは、ウィンドウを開く前に表示して終わる
    // （GUI の無い環境、たとえば CI でも --version は動く）。
    const Result<LaunchOptions> parsed = parse_launch_options(utf8_arguments(argc, argv));
    if (!parsed.ok() || parsed.value.help || parsed.value.version) {
        use_utf8_console();
        if (!parsed.ok()) {
            std::fprintf(stderr, "%s\n\n%s", parsed.error.c_str(), launch_usage().c_str());
            return 2;
        }
        if (parsed.value.help) std::fputs(launch_usage().c_str(), stdout);
        else std::printf("morphingAligner v%s\n", APP_VERSION);
        return 0;
    }
    LaunchOptions launch = parsed.value;
    make_paths_absolute(launch);

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // OpenGL 3.3 core を要求する。3.0 だと ImGui の OpenGL3 バックエンドが頂点オフセット
    // （RendererHasVtxOffset。GL 3.2 以上で有効）を使わず、1つのウィンドウの描画が 16bit
    // インデックスの上限（65536 頂点）を超えると、それ以降の描画が崩れる。実際に Intel の
    // Windows ドライバで 3.0.0 が返り、base/target の両方にフォルマントと移動平均を描くと
    // target 側の線やカラースケールが消えた。3.3 core を作れない環境向けに 3.0 へ戻す。
    struct GlRequest {
        int         major, minor;
        bool        core;
        const char* glsl;
    };
    constexpr GlRequest kGlRequests[] = { { 3, 3, true, "#version 330" }, { 3, 0, false, "#version 130" } };

    GLFWwindow* window       = nullptr;
    const char* glsl_version = nullptr;
    for (const GlRequest& req : kGlRequests) {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, req.major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, req.minor);
        if (req.core) {
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);    // macOS で core を作るのに必要
        }
        // タイトルに版を入れてもクラス名が変わらないよう固定する（kWindowClass のコメント参照）。
        glfwWindowHintString(GLFW_X11_CLASS_NAME, kWindowClass);
        glfwWindowHintString(GLFW_X11_INSTANCE_NAME, kWindowClass);
        glfwWindowHintString(GLFW_WAYLAND_APP_ID, kWindowClass);

        window = glfwCreateWindow(1280, 720, kWindowTitle, nullptr, nullptr);
        if (window) {
            glsl_version = req.glsl;
            break;
        }
    }
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

    // 左ボタンをアンカーの追加・ドラッグに使うため、プロットのパン（表示範囲の移動）は中ボタンに移す。
    ImPlot::GetInputMap().Pan = ImGuiMouseButton_Middle;
    // 既定では Ctrl は OverrideMod（押下中は入力を無視して DnD ソース化）に割り当た
    // っている。Ctrl+左クリックを周波数アンカー追加に使うため無効化する。
    ImPlot::GetInputMap().OverrideMod = ImGuiMod_None;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // OpenGL の版と、描画リストの頂点数上限（16bit インデックスで 65536）を超えられるか。
    // 頂点オフセットが無効だと、1つのウィンドウで上限を超えた分の描画が崩れる。
    {
        const char* ver     = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const bool  vtx_off = (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0;
        applog::add(std::string { "OpenGL: " } + (ver ? ver : "?") + "（頂点オフセット: "
                    + (vtx_off ? "有効" : "無効。描画が多いと表示が欠けることがあります") + "）");
    }

    try {
        App app;
        app.view.mono_font = mono_font;
        applog::add("起動しました");
        apply_launch_options(app, launch);    // コマンドライン引数で指定された音声・セッションを読み込む

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
        // 実行中の Python ツール（MFA は数十秒かかる）を止める。App を破棄すると実行中
        // ジョブの future が子プロセスの終了を待つため、止めないと閉じた後も固まる。
        terminate_speech_tools();
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
