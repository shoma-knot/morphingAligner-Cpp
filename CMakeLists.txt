cmake_minimum_required(VERSION 4.0)
project(morphingAlignerCpp VERSION 0.1.0)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS "-fPIC -O2")
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
find_package(Eigen3 3.4 CONFIG REQUIRED)    # tcmorph(モーフィングエンジン)が要求
find_package(Threads REQUIRED)    # std::async(モーフィングの別スレッド実行)用
add_subdirectory(third-party/miniaudio_cpp)
set(WORLD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/world)


# src 直下のみ。src/tcmorph は外部ライブラリ(ヘッダオンリー)で、examples に
# それぞれ main() を持つので再帰 glob に含めるとリンクが壊れる。
file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(main ${SOURCES})
target_include_directories(main PRIVATE ${CMAKE_SOURCE_DIR}/src/tcmorph/include)
target_link_libraries(main PRIVATE
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
