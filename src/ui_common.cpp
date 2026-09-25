#include "ui_common.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <imgui.h>
#include <implot.h>

#include "freqscale.hpp"

namespace {

// Y軸（ERB レート）の目盛りを実周波数 [Hz] で表示するフォーマッタ。
int erb_hz_formatter(double erb, char* buff, int size, void*) {
    return std::snprintf(buff, size, "%.0f", freqscale::erb_to_hz(erb));
}

}    // namespace

// Y軸（ERB レート）に 1-2-5 系列（…100,200,500,1000,…）の目盛りとHz表示を設定する。
// ERB 軸では高域が圧縮されるので、値が大きいほど間隔を空けることでほぼ均等に並ぶ。
void setup_erb_yaxis_ticks(double nyq) {
    ImPlot::SetupAxisFormat(ImAxis_Y1, erb_hz_formatter);
    std::vector<double> yticks { freqscale::hz_to_erb(0.0) };    // 0Hz
    for (double dec = 10.0; dec <= nyq; dec *= 10.0)
        for (double m : { 1.0, 2.0, 5.0 }) {
            const double hz = dec * m;
            if (hz >= 100.0 && hz <= nyq) yticks.push_back(freqscale::hz_to_erb(hz));
        }
    ImPlot::SetupAxisTicks(ImAxis_Y1, yticks.data(), static_cast<int>(yticks.size()), nullptr, false);
}

// 直前の項目と同じ行に "(?)" を出し、ホバーで説明を表示する。
void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// 直前の項目と同じ行の右端にボタンを置く（幅はラベルに合わせる）。
bool right_aligned_button(const char* label) {
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
    return ImGui::Button(label);
}
