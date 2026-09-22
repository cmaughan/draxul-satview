#include "satview_view_controller.h"

#include <draxul/satview/satview_map_projection.h>

namespace draxul::satview
{

namespace
{

CentralBody central_body_for_pov(SatViewCameraPov pov)
{
    if (pov == SatViewCameraPov::Earth)
        return CentralBody::Earth;
    if (pov == SatViewCameraPov::Moon)
        return CentralBody::Moon;
    if (pov == SatViewCameraPov::Mars)
        return CentralBody::Mars;
    return CentralBody::Other;
}

bool filter_selects_body(const SatViewFilterState& filter, CentralBody body)
{
    if (body == CentralBody::Earth)
        return filter.show_earth && !filter.show_moon && !filter.show_mars;
    if (body == CentralBody::Moon)
        return filter.show_moon && !filter.show_earth && !filter.show_mars;
    if (body == CentralBody::Mars)
        return filter.show_mars && !filter.show_earth && !filter.show_moon;
    return !filter.show_earth && !filter.show_moon && !filter.show_mars;
}

} // namespace

bool SatViewSelectionState::empty() const
{
    return !satellite_id.has_value()
        && !surface.has_value()
        && !natural_body.has_value();
}

SatViewPovTransition SatViewViewController::change_pov(
    SatViewCameraPov current_pov,
    SatViewCameraPov requested_pov,
    SatViewProjectionMode current_projection,
    const SatViewFilterState& filter) const
{
    const CentralBody central_body = central_body_for_pov(requested_pov);
    return {
        .pov = requested_pov,
        .projection = requested_pov != SatViewCameraPov::Earth
                && current_projection == SatViewProjectionMode::Ground
            ? SatViewProjectionMode::Globe
            : current_projection,
        .central_body = central_body,
        .pov_changed = current_pov != requested_pov,
        .central_body_changed = !filter_selects_body(filter, central_body),
    };
}

glm::vec2 SatViewViewController::pan_map(
    glm::vec2 current_center_radians,
    glm::vec2 delta_radians) const
{
    return normalized_satview_map_center(current_center_radians + delta_radians);
}

SatViewGroundEntry SatViewViewController::enter_ground(
    glm::dvec2 longitude_latitude_radians) const
{
    return {
        .location_radians = longitude_latitude_radians,
    };
}

SatViewSelectionState SatViewViewController::select_satellite(
    std::optional<std::int64_t> satellite_id) const
{
    return { .satellite_id = satellite_id };
}

SatViewSelectionState SatViewViewController::select_surface(
    CentralBody body,
    std::size_t catalog_index) const
{
    return { .surface = SatViewSurfaceSelection{ body, catalog_index } };
}

SatViewSelectionState SatViewViewController::select_natural_body(
    SatViewCameraPov body) const
{
    return { .natural_body = body };
}

SatViewSelectionState SatViewViewController::clear_selection() const
{
    return {};
}

} // namespace draxul::satview
