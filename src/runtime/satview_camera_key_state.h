#pragma once

// SatView's binding of the SHARED camera key table
// (Draxul::PluginSupport::CameraInput). The latch table, the Ctrl+R guard and
// the movement synthesis all live in draxul/camera_input.h now; only the axis
// choice below is SatView's.
//
// WHY THESE BINDINGS: SatView's camera orbits a globe and cannot pan, so every
// directional key group feeds the orbit axes — the arrows and A/D orbit
// horizontally, W/S and T/G orbit vertically. MegaCity's isometric city camera
// pans instead, so it binds the same groups to its pan and pitch axes. Neither
// mapping is "the" intended one; they are different cameras, so the shared
// table exposes the axes separately and each product picks.

#include <draxul/camera_input.h>

namespace draxul::satview
{

inline constexpr camera_input::OrbitKeyBindings kSatViewCameraBindings{
    .horizontal_arrows_orbit = true,
    .vertical_arrows_orbit = true,
    .pitch_folds_into_orbit = true,
    // Ctrl+R stays with the host reload accelerator rather than zooming.
    .zoom_in_guard_modifiers = kModCtrl,
};

} // namespace draxul::satview
