#pragma once

#include <cstddef>
#include <cstdint>
#include <draxul/satview/satview_config.h>
#include <draxul/satview/satview_filter.h>
#include <glm/glm.hpp>
#include <optional>

namespace draxul::satview
{

struct SatViewSurfaceSelection
{
    CentralBody body = CentralBody::Moon;
    std::size_t catalog_index = 0;

    bool operator==(const SatViewSurfaceSelection&) const = default;
};

struct SatViewSelectionState
{
    std::optional<std::int64_t> satellite_id;
    std::optional<SatViewSurfaceSelection> surface;
    std::optional<SatViewCameraPov> natural_body;

    [[nodiscard]] bool empty() const;
    bool operator==(const SatViewSelectionState&) const = default;
};

struct SatViewPovTransition
{
    SatViewCameraPov pov = SatViewCameraPov::Earth;
    SatViewProjectionMode projection = SatViewProjectionMode::Globe;
    CentralBody central_body = CentralBody::Earth;
    bool pov_changed = false;
    bool central_body_changed = false;
};

struct SatViewGroundEntry
{
    SatViewCameraPov pov = SatViewCameraPov::Earth;
    SatViewProjectionMode projection = SatViewProjectionMode::Ground;
    CentralBody central_body = CentralBody::Earth;
    glm::dvec2 location_radians{ 0.0 };
};

// Device-free transition policy for the host's view and mutually-exclusive
// selection state. SatViewRuntime still owns service, camera, renderer, and
// frame-side effects and applies these values on the main thread.
class SatViewViewController
{
public:
    [[nodiscard]] SatViewPovTransition change_pov(
        SatViewCameraPov current_pov,
        SatViewCameraPov requested_pov,
        SatViewProjectionMode current_projection,
        const SatViewFilterState& filter) const;
    [[nodiscard]] glm::vec2 pan_map(
        glm::vec2 current_center_radians,
        glm::vec2 delta_radians) const;
    [[nodiscard]] SatViewGroundEntry enter_ground(
        glm::dvec2 longitude_latitude_radians) const;

    [[nodiscard]] SatViewSelectionState select_satellite(
        std::optional<std::int64_t> satellite_id) const;
    [[nodiscard]] SatViewSelectionState select_surface(
        CentralBody body,
        std::size_t catalog_index) const;
    [[nodiscard]] SatViewSelectionState select_natural_body(
        SatViewCameraPov body) const;
    [[nodiscard]] SatViewSelectionState clear_selection() const;
};

} // namespace draxul::satview
