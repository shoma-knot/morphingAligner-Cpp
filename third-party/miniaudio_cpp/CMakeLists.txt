cmake_minimum_required(VERSION 3.16)
project(miniaudio_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Static library ───────────────────────────────────────────

add_library(miniaudio_cpp STATIC
    src/miniaudio_impl.c   # miniaudio implementation (pure C)
    src/audio.cpp           # C++ wrapper implementation
)

# PUBLIC  : consumers see include/miniaudio_cpp/audio.hpp
# PRIVATE : miniaudio.h is only visible inside this library
target_include_directories(miniaudio_cpp
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external
)

# Platform-specific link dependencies required by miniaudio
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    target_link_libraries(miniaudio_cpp PRIVATE Threads::Threads m dl)
elseif(APPLE)
    find_library(COREAUDIO_LIB    CoreAudio)
    find_library(AUDIOUNIT_LIB    AudioUnit)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    target_link_libraries(miniaudio_cpp PRIVATE
        ${COREAUDIO_LIB} ${AUDIOUNIT_LIB} ${COREFOUNDATION_LIB})
endif()
# Windows: no extra link libs needed (COM is loaded at runtime)
