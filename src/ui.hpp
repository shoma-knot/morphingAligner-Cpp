#pragma once

/// @file ui.hpp
/// @brief Per-frame drawing of the two panels.

struct App;

// Left panel: per-track load/play controls and the anchor list.
void draw_left_panel(App& app);

// Right panel: base and target spectral-envelope spectrograms stacked
// vertically, with draggable time anchors and the cross-panel connectors.
void draw_right_panel(App& app);
