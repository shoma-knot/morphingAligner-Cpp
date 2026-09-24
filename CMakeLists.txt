cmake_minimum_required(VERSION 4.0)
project(morphingAlignerCpp VERSION 26.09.25)
set(CMAKE_CXX_STANDARD 17)

# MSVC の CMAKE_CXX_FLAGS には /EHsc などの既定値が入っており、丸ごと上書きすると
# 例外処理が壊れる（本体は try/catch に依存している）。-fPIC/-O2 も cl では無効なので、
# GCC/Clang のときだけ設定する。最適化レベルは CMAKE_BUILD_TYPE でも指定できる。
if (NOT MSVC)
    set(CMAKE_CXX_FLAGS "-fPIC -O2")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(LIBRARY_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/lib)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/bin)

if (MSVC)
    add_definitions(/utf-8)
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


file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(morphingAlignerCpp ${SOURCES})
# tcmorph はヘッダオンリーの外部ライブラリ。
target_include_directories(morphingAlignerCpp PRIVATE ${CMAKE_SOURCE_DIR}/tcmorph/include)
# タイトルバーに出す版。上の project() の VERSION をそのまま使うので、リリース時は
# そちらを更新すること。
target_compile_definitions(morphingAlignerCpp PRIVATE APP_VERSION="${PROJECT_VERSION}")

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
    imgui::imgui
    implot::implot
    glfw
    OpenGL::GL
    tinyfiledialogs::tinyfiledialogs
    nlohmann_json::nlohmann_json
    Eigen3::Eigen
    Threads::Threads
    miniaudio_cpp
    world::core)
