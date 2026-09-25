cmake_minimum_required(VERSION 4.0)
project(morphingAlignerCpp VERSION 26.09.25)
# 同じ日付の版を出し直すときの接尾辞（英小文字1字。例: "a" → 26.09.25a。無ければ空）。
# project() の VERSION は数字しか書けないので別に持つ。タイトルバーの版は「VERSION + 接尾辞」
# になり、リリースのタグ（v<VERSION><接尾辞>）との一致も build.yml がこの2つを合わせて検査する。
set(APP_VERSION_SUFFIX "b")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)    # -fPIC（GCC/Clang）

# 出力先: 実行ファイルは bin/、ライブラリは lib/（Visual Studio など複数構成のジェネレータでは
# その下に Release/ などが付く）。配布物の作成（build.yml）は bin/ の実行ファイルを拾う。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib)

# コンパイラのオプションは CMAKE_CXX_FLAGS を上書きせずに足す（MSVC の既定値には /EHsc などが
# 入っており、丸ごと上書きすると例外処理が壊れる。本体は try/catch に依存している）。
# C/C++ に限るのは、Windows のリソースコンパイラ（rc.exe）にこれらを渡さないため。
if (MSVC)
    # ソースは UTF-8（日本語のコメント・文字列）。
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
elseif (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # ビルド種別が未指定のとき（CMakePresets の vcpkg プリセットなど）も最適化する。
    # CMAKE_BUILD_TYPE=Release などを指定したときは、その種別の最適化レベルが使われる。
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-O2>)
endif()

find_package(imgui CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(tinyfiledialogs CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# tcmorph(モーフィングエンジン)が要求する。
# 版を指定しないのは、vcpkg の eigen3 が名乗る版(5.0.1)と Eigen 自身のバージョン
# マクロ(3.5.0)が食い違っており、tcmorph が要求する 3.4 を書くと通らないため。
# 実際に使う版は vcpkg-configuration.json の baseline が固定しているので、ここで
# 重ねて縛る必要はない。3.4.0 と 5.0.1 でモーフィング結果が WAV のバイト単位で
# 一致することは確認済み。
find_package(Eigen3 CONFIG REQUIRED)
find_package(Threads REQUIRED)    # std::async(モーフィングの別スレッド実行)用
add_subdirectory(third-party/miniaudio_cpp)
set(WORLD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/world)


# コア: GUI（ImGui / ImPlot / GLFW / OpenGL）に依存しない処理。音声の解析とモーフィング、
# アンカーの整形と自動生成、フォルマントの移動平均、セッション JSON の読み書き、Python
# ツールの呼び出し。本体と単体テスト（tests/core_test.cpp）の両方がリンクする。
# ここに GUI のヘッダ（app.hpp など）を読み込むファイルを入れないこと。
add_library(morphaligner_core STATIC
    src/analysis.cpp
    src/auto_anchors.cpp
    src/formant_smoothing.cpp
    src/morphing.cpp
    src/resource_path.cpp
    src/session_io.cpp
    src/speech_tools.cpp)
target_include_directories(morphaligner_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tcmorph/include)    # tcmorph はヘッダオンリーの外部ライブラリ
target_link_libraries(morphaligner_core PUBLIC
    nlohmann_json::nlohmann_json
    Eigen3::Eigen
    Threads::Threads
    miniaudio_cpp
    world::core)

# 本体（GUI）。
add_executable(morphingAlignerCpp
    src/anchors.cpp
    src/app.cpp
    src/app_icon.cpp
    src/file_jobs.cpp
    src/gl_texture.cpp
    src/jobs.cpp
    src/log.cpp
    src/main.cpp
    src/morph_controller.cpp
    src/session.cpp
    src/speech_controller.cpp
    src/speech_view.cpp
    src/ui.cpp
    src/ui_align.cpp
    src/ui_common.cpp
    src/ui_license.cpp
    src/ui_morph.cpp)
# タイトルバーに出す版。上の project() の VERSION と APP_VERSION_SUFFIX を合わせたもの
# なので、リリース時はそちらを更新すること。
target_compile_definitions(morphingAlignerCpp PRIVATE APP_VERSION="${PROJECT_VERSION}${APP_VERSION_SUFFIX}")

# Windows では実行ファイル自体にもアイコンを埋め込む。glfwSetWindowIcon が設定する
# ウィンドウのアイコンとは別物で、こちらはエクスプローラーの表示やピン留めが参照する。
# .rc は .ico を絶対パスで指す必要があるため configure_file で展開する。
if (WIN32)
    enable_language(RC)
    if (MSVC)
        # rc.exe は .rc をシステムの ANSI コードページとして読むため、UTF-8 の日本語
        # コメントが化けて構文エラーになりうる（/utf-8 は C++ コンパイラ向けで rc には
        # 効かない）。コードページを明示する。
        string(APPEND CMAKE_RC_FLAGS " /c65001")
    endif()
    set(MORPHINGALIGNER_ICO "${CMAKE_SOURCE_DIR}/icon/morphingaligner.ico")
    configure_file(${CMAKE_SOURCE_DIR}/src/app_icon.rc.in ${CMAKE_BINARY_DIR}/app_icon.rc @ONLY)
    target_sources(morphingAlignerCpp PRIVATE ${CMAKE_BINARY_DIR}/app_icon.rc)
endif()
target_link_libraries(morphingAlignerCpp PRIVATE
    morphaligner_core
    imgui::imgui
    implot::implot
    glfw
    OpenGL::GL
    tinyfiledialogs::tinyfiledialogs)

# テスト。既定では作らない。CI（.github/workflows/）が ON にして各 OS で実行する。
#   core_test         … コアの単体テスト。どこで実行してもよい（build.yml がビルド直後に実行）。
#   speech_tools_test … C++ から Python の音声解析ツールを子プロセスで呼ぶ経路の結合テスト。
#                       ルートに .env（install-win.bat / install.sh で作る Python 環境）が
#                       要るので、WORKING_DIRECTORY はソースのルートにする（ci.yml が実行）。
option(MORPHALIGNER_BUILD_TESTS "単体テストと speech_tools の結合テストを作る" OFF)
if (MORPHALIGNER_BUILD_TESTS)
    enable_testing()
    add_executable(core_test tests/core_test.cpp)
    target_link_libraries(core_test PRIVATE morphaligner_core)
    add_test(NAME core COMMAND core_test)
    add_executable(speech_tools_test tests/speech_tools_test.cpp)
    target_link_libraries(speech_tools_test PRIVATE morphaligner_core)
    add_test(NAME speech_tools COMMAND speech_tools_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
