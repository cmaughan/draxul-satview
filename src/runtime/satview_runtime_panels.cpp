#include <draxul/satview/satview_runtime.h>

#include "camera.h"
#include "camera_manipulator.h"
#include "satview_simulation_worker.h"
#include "satview_time_format.h"
#include "satview_view_controller.h"
#include <draxul/satview/satview_cloud_service.h>
#include <draxul/satview/satview_geodetic.h>
#include <draxul/satview/satview_moon_ephemeris.h>
#include <draxul/satview/satview_object_style.h>
#include <draxul/satview/satview_scene_pass.h>
#include <draxul/satview/satview_sky_projection.h>
#include <draxul/satview/satview_solar_system.h>
#include <draxul/satview/satview_surface_catalog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <draxul/imgui_input_bridge.h>
#include <glm/common.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace draxul::satview
{

namespace
{

constexpr float kControlPanelDefaultWidth = 430.0f;
constexpr float kControlPanelDefaultHeight = 500.0f;
constexpr float kControlPanelMinWidth = 380.0f;
constexpr float kControlPanelMinHeight = 360.0f;
constexpr float kControlMinWidgetWidth = 96.0f;
constexpr const char* kSatViewDockspaceName = "SatViewDockspace";
constexpr const char* kSatViewSceneWindowName = "Scene";
constexpr const char* kSatViewViewWindowName = "View";
constexpr const char* kSatViewRenderingWindowName = "Rendering";
constexpr const char* kSatViewFilterWindowName = "Filter";
constexpr const char* kSatViewSelectionWindowName = "Selection";
constexpr const char* kSatViewAboutWindowName = "About";
constexpr float kGroundMinimumFovDegrees = 20.0f;
constexpr float kGroundMinimumMarkerScale = 0.05f;
constexpr float kGroundMaximumMarkerScale = 2.0f;

const char* camera_pov_name(SatViewCameraPov pov)
{
    return satview_solar_system_body(pov).name.data();
}

glm::vec4 population_color(SatellitePopulation population, float alpha)
{
    switch (population)
    {
    case SatellitePopulation::ActivePayload:
        return glm::vec4(0.18f, 0.88f, 1.00f, alpha);
    case SatellitePopulation::InactivePayload:
        return glm::vec4(0.48f, 0.55f, 0.72f, alpha);
    case SatellitePopulation::RocketBody:
        return glm::vec4(1.00f, 0.58f, 0.18f, alpha);
    case SatellitePopulation::Debris:
        return glm::vec4(1.00f, 0.28f, 0.36f, alpha);
    case SatellitePopulation::Unknown:
        return glm::vec4(0.72f, 0.78f, 0.86f, alpha);
    }
    return glm::vec4(0.72f, 0.78f, 0.86f, alpha);
}

int orbit_class_sort_key(OrbitClass orbit_class)
{
    switch (orbit_class)
    {
    case OrbitClass::LowEarth:
        return 0;
    case OrbitClass::MediumEarth:
        return 1;
    case OrbitClass::Geosynchronous:
        return 2;
    case OrbitClass::HighlyElliptical:
        return 3;
    case OrbitClass::Other:
        return 4;
    }
    return 4;
}

int population_sort_key(SatellitePopulation population)
{
    switch (population)
    {
    case SatellitePopulation::ActivePayload:
        return 0;
    case SatellitePopulation::InactivePayload:
        return 1;
    case SatellitePopulation::RocketBody:
        return 2;
    case SatellitePopulation::Debris:
        return 3;
    case SatellitePopulation::Unknown:
        return 4;
    }
    return 4;
}

std::string object_tree_label(const SatelliteRecord& record)
{
    std::string label = std::to_string(record.norad_catalog_id);
    if (!record.object_name.empty())
    {
        label += " - ";
        label += record.object_name;
    }
    if (!record.object_id.empty())
    {
        label += " [";
        label += record.object_id;
        label += "]";
    }
    if (!record.renderable)
        label += " (catalog only)";
    return label;
}

bool satellite_visible(
    const SatViewFilterState& filter,
    const SatellitePropagatedState& state,
    std::string_view source_label)
{
    return satview_filter_matches(filter, make_satview_filter_candidate(state, source_label));
}

bool satellite_display_shows_tracks(SatViewSatelliteDisplayMode mode)
{
    return mode != SatViewSatelliteDisplayMode::MarkersOnly;
}

bool satellite_display_shows_markers(SatViewSatelliteDisplayMode mode)
{
    return mode != SatViewSatelliteDisplayMode::TracksOnly;
}

double render_simulation_seconds(const SatViewSimulationSnapshot& snapshot)
{
    return satview_snapshot_render_seconds(snapshot, std::chrono::steady_clock::now());
}

float clamp_ground_marker_scale(float scale)
{
    return std::clamp(
        scale,
        kGroundMinimumMarkerScale,
        kGroundMaximumMarkerScale);
}

float control_widget_width(const char* label)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float available = ImGui::GetContentRegionAvail().x;
    const float label_width = ImGui::CalcTextSize(label).x;
    const float row_spacing = style.ItemInnerSpacing.x;
    return std::max(kControlMinWidgetWidth, available - label_width - row_spacing);
}

bool surface_kind_visible(
    const SatViewSurfaceObject& object,
    bool show_landers,
    bool show_rovers,
    bool show_instruments,
    bool show_impacts,
    bool show_crewed_artifacts,
    bool show_approximate_locations)
{
    if (object.crewed_mission && !show_crewed_artifacts)
        return false;
    if (object.location_quality == SatViewSurfaceLocationQuality::Approximate
        && !show_approximate_locations)
    {
        return false;
    }
    switch (object.kind)
    {
    case SatViewSurfaceKind::Lander:
        return show_landers;
    case SatViewSurfaceKind::Rover:
        return show_rovers;
    case SatViewSurfaceKind::DeployedInstrument:
    case SatViewSurfaceKind::Retroreflector:
        return show_instruments;
    case SatViewSurfaceKind::ImpactSite:
    case SatViewSurfaceKind::RocketStage:
        return show_impacts;
    case SatViewSurfaceKind::CrewedArtifact:
        return show_crewed_artifacts;
    case SatViewSurfaceKind::Unknown:
        return show_landers;
    }
    return false;
}

} // namespace

void SatViewRuntime::rebuild_object_tree(const SatViewSimulationSnapshot* snapshot)
{
    if (catalog_snapshot_.objects.empty())
    {
        object_tree_catalog_generation_ = 0;
        object_tree_state_count_ = 0;
        object_tree_entries_.clear();
        filtered_object_tree_indices_.clear();
        return;
    }
    const std::size_t state_count = snapshot ? snapshot->states.size() : 0;
    if (object_tree_catalog_generation_ == simulation_catalog_generation_
        && object_tree_state_count_ == state_count)
    {
        return;
    }

    object_tree_catalog_generation_ = simulation_catalog_generation_;
    object_tree_state_count_ = state_count;
    object_tree_entries_.clear();
    object_tree_entries_.reserve(catalog_snapshot_.objects.size());
    std::unordered_map<std::int64_t, std::size_t> state_indices;
    if (snapshot)
    {
        state_indices.reserve(snapshot->states.size());
        for (std::size_t state_index = 0; state_index < snapshot->states.size(); ++state_index)
            state_indices.emplace(snapshot->states[state_index].norad_catalog_id, state_index);
    }
    for (std::size_t catalog_index = 0; catalog_index < catalog_snapshot_.objects.size(); ++catalog_index)
    {
        const SatelliteRecord& record = catalog_snapshot_.objects[catalog_index];
        const auto state = state_indices.find(record.norad_catalog_id);
        object_tree_entries_.push_back({
            record.central_body,
            record.population,
            record.orbit_class,
            normalized_satellite_prefix(record.object_name),
            object_tree_label(record),
            record.object_name,
            record.norad_catalog_id,
            catalog_index,
            state == state_indices.end()
                ? std::optional<std::size_t>{}
                : std::optional<std::size_t>{ state->second },
        });
    }

    std::sort(object_tree_entries_.begin(), object_tree_entries_.end(),
        [](const ObjectTreeEntry& a, const ObjectTreeEntry& b) {
            const int population_a = population_sort_key(a.population);
            const int population_b = population_sort_key(b.population);
            if (population_a != population_b)
                return population_a < population_b;
            const int orbit_a = orbit_class_sort_key(a.orbit_class);
            const int orbit_b = orbit_class_sort_key(b.orbit_class);
            if (orbit_a != orbit_b)
                return orbit_a < orbit_b;
            if (a.prefix != b.prefix)
                return a.prefix < b.prefix;
            if (a.object_name != b.object_name)
                return a.object_name < b.object_name;
            return a.norad_catalog_id < b.norad_catalog_id;
        });
}

void SatViewRuntime::render_object_tree(const SatViewSimulationSnapshot* snapshot, bool& changed)
{
    rebuild_object_tree(snapshot);

    if (catalog_snapshot_.objects.empty())
    {
        ImGui::TextDisabled("Object tree pending catalog.");
        return;
    }

    filtered_object_tree_indices_.clear();
    filtered_object_tree_indices_.reserve(object_tree_entries_.size());
    for (std::size_t entry_index = 0; entry_index < object_tree_entries_.size(); ++entry_index)
    {
        const ObjectTreeEntry& entry = object_tree_entries_[entry_index];
        const bool visible_state = snapshot
            && entry.state_index.has_value()
            && *entry.state_index < snapshot->states.size()
            && satellite_visible(filter_, snapshot->states[*entry.state_index], snapshot->source_label);
        const bool visible_catalog_only = !entry.state_index.has_value()
            && entry.catalog_index < catalog_snapshot_.objects.size()
            && satview_filter_matches(
                filter_,
                make_satview_filter_candidate(catalog_snapshot_.objects[entry.catalog_index]));
        if (visible_state || visible_catalog_only)
        {
            filtered_object_tree_indices_.push_back(entry_index);
        }
    }

    ImGui::Text("Objects: %zu filtered / %zu total",
        filtered_object_tree_indices_.size(),
        object_tree_entries_.size());
    if (filtered_object_tree_indices_.empty())
    {
        ImGui::TextDisabled("No objects match the current filters.");
        return;
    }

    const auto filtered_entry = [this](std::size_t filtered_index) -> const ObjectTreeEntry& {
        return object_tree_entries_[filtered_object_tree_indices_[filtered_index]];
    };
    const float tree_height = std::max(180.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f);
    if (!ImGui::BeginChild(
            "##satview_object_tree",
            ImVec2(0.0f, tree_height),
            ImGuiChildFlags_Border,
            ImGuiWindowFlags_AlwaysVerticalScrollbar))
    {
        ImGui::EndChild();
        return;
    }

    const ImGuiTreeNodeFlags group_flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    std::size_t population_begin = 0;
    while (population_begin < filtered_object_tree_indices_.size())
    {
        const SatellitePopulation population = filtered_entry(population_begin).population;
        std::size_t population_end = population_begin + 1;
        while (population_end < filtered_object_tree_indices_.size()
            && filtered_entry(population_end).population == population)
        {
            ++population_end;
        }

        const std::string_view population_name = satellite_population_name(population);
        ImGui::PushID(population_sort_key(population));
        if (color_mode_ == SatViewColorMode::Population)
        {
            const glm::vec4 color = population_color(population, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.r, color.g, color.b, color.a));
        }
        const bool population_open = ImGui::TreeNodeEx(
            "##population",
            group_flags,
            "%.*s (%zu)",
            static_cast<int>(population_name.size()),
            population_name.data(),
            population_end - population_begin);
        if (color_mode_ == SatViewColorMode::Population)
            ImGui::PopStyleColor();
        if (population_open)
        {
            std::size_t orbit_begin = population_begin;
            while (orbit_begin < population_end)
            {
                const OrbitClass orbit_class = filtered_entry(orbit_begin).orbit_class;
                std::size_t orbit_end = orbit_begin + 1;
                while (orbit_end < population_end
                    && filtered_entry(orbit_end).orbit_class == orbit_class)
                {
                    ++orbit_end;
                }

                const std::string_view orbit_name = orbit_class_name(orbit_class);
                ImGui::PushID(orbit_class_sort_key(orbit_class));
                const bool orbit_open = ImGui::TreeNodeEx(
                    "##orbit",
                    group_flags,
                    "%.*s (%zu)",
                    static_cast<int>(orbit_name.size()),
                    orbit_name.data(),
                    orbit_end - orbit_begin);
                if (orbit_open)
                {
                    std::size_t prefix_begin = orbit_begin;
                    while (prefix_begin < orbit_end)
                    {
                        const std::string& prefix = filtered_entry(prefix_begin).prefix;
                        std::size_t prefix_end = prefix_begin + 1;
                        while (prefix_end < orbit_end && filtered_entry(prefix_end).prefix == prefix)
                            ++prefix_end;

                        ImGui::PushID(prefix.c_str());
                        if (color_mode_ == SatViewColorMode::NamePrefix)
                        {
                            const glm::vec4 prefix_color = satellite_prefix_color(stable_color_hash(prefix));
                            ImGui::PushStyleColor(
                                ImGuiCol_Text,
                                ImVec4(prefix_color.r, prefix_color.g, prefix_color.b, prefix_color.a));
                        }
                        const bool prefix_open = ImGui::TreeNodeEx(
                            "##prefix",
                            group_flags,
                            "%s (%zu)",
                            prefix.c_str(),
                            prefix_end - prefix_begin);
                        if (color_mode_ == SatViewColorMode::NamePrefix)
                            ImGui::PopStyleColor();
                        if (prefix_open)
                        {
                            ImGuiListClipper clipper;
                            clipper.Begin(static_cast<int>(prefix_end - prefix_begin));
                            while (clipper.Step())
                            {
                                for (int local_index = clipper.DisplayStart;
                                    local_index < clipper.DisplayEnd;
                                    ++local_index)
                                {
                                    const ObjectTreeEntry& entry = filtered_entry(
                                        prefix_begin + static_cast<std::size_t>(local_index));
                                    const bool selected = selected_norad_catalog_id_.has_value()
                                        && entry.norad_catalog_id == *selected_norad_catalog_id_;
                                    ImGui::PushID(entry.label.c_str());
                                    if (ImGui::Selectable(entry.label.c_str(), selected))
                                    {
                                        apply_selection(view_controller_->select_satellite(
                                            entry.norad_catalog_id));
                                        simulation_settings_dirty_ = true;
                                        changed = true;
                                    }
                                    if (selected)
                                        ImGui::SetItemDefaultFocus();
                                    ImGui::PopID();
                                }
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                        prefix_begin = prefix_end;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
                orbit_begin = orbit_end;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        population_begin = population_end;
    }

    ImGui::EndChild();
}

void SatViewRuntime::render_surface_tree(
    CentralBody body,
    const SatViewSurfaceCatalog* catalog,
    const SurfaceFilterControls& filters,
    bool& changed)
{
    const std::string_view body_name = central_body_name(body);
    if (!catalog)
    {
        ImGui::TextDisabled("%.*s surface catalogue pending.",
            static_cast<int>(body_name.size()),
            body_name.data());
        return;
    }
    if (!catalog->error.empty())
    {
        ImGui::TextWrapped("%s", catalog->error.c_str());
        return;
    }

    std::vector<std::size_t> visible_indices;
    visible_indices.reserve(catalog->objects.size());
    for (std::size_t index = 0; index < catalog->objects.size(); ++index)
    {
        const SatViewSurfaceObject& object = catalog->objects[index];
        const bool selected = selected_surface_object_.has_value()
            && selected_surface_object_->body == body
            && selected_surface_object_->catalog_index == index;
        if (selected || surface_kind_visible(object, filters.show_landers, filters.show_rovers, filters.show_instruments, filters.show_impacts, filters.show_crewed_artifacts, filters.show_approximate_locations))
        {
            visible_indices.push_back(index);
        }
    }
    std::sort(visible_indices.begin(), visible_indices.end(), [&](std::size_t left, std::size_t right) {
        const SatViewSurfaceObject& a = catalog->objects[left];
        const SatViewSurfaceObject& b = catalog->objects[right];
        if (a.mission_name != b.mission_name)
            return a.mission_name < b.mission_name;
        if (a.parent_site_id != b.parent_site_id)
            return a.parent_site_id < b.parent_site_id;
        if (a.display_rank != b.display_rank)
            return a.display_rank < b.display_rank;
        return a.display_name < b.display_name;
    });

    ImGui::Text("%.*s surface: %zu filtered / %zu total (%zu sites)",
        static_cast<int>(body_name.size()),
        body_name.data(),
        visible_indices.size(),
        catalog->objects.size(),
        catalog->site_count);
    if (visible_indices.empty())
    {
        ImGui::TextDisabled("No %.*s surface objects match the filters.",
            static_cast<int>(body_name.size()),
            body_name.data());
        return;
    }

    const ImGuiTreeNodeFlags group_flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    const float tree_height = std::max(180.0f, ImGui::GetTextLineHeightWithSpacing() * 14.0f);
    ImGui::PushID(static_cast<int>(body));
    if (!ImGui::BeginChild(
            "##satview_surface_tree",
            ImVec2(0.0f, tree_height),
            ImGuiChildFlags_Border,
            ImGuiWindowFlags_AlwaysVerticalScrollbar))
    {
        ImGui::EndChild();
        ImGui::PopID();
        return;
    }

    const auto object_at = [&](std::size_t visible_index) -> const SatViewSurfaceObject& {
        return catalog->objects[visible_indices[visible_index]];
    };
    std::size_t mission_begin = 0;
    while (mission_begin < visible_indices.size())
    {
        const std::string& mission_id = object_at(mission_begin).mission_id;
        std::size_t mission_end = mission_begin + 1;
        while (mission_end < visible_indices.size()
            && object_at(mission_end).mission_id == mission_id)
        {
            ++mission_end;
        }

        ImGui::PushID(mission_id.c_str());
        const SatViewSurfaceObject& first = object_at(mission_begin);
        const bool mission_open = ImGui::TreeNodeEx(
            "##mission",
            group_flags,
            "%s (%zu)",
            first.mission_name.c_str(),
            mission_end - mission_begin);
        if (mission_open)
        {
            std::size_t site_begin = mission_begin;
            while (site_begin < mission_end)
            {
                const std::string& site_id = object_at(site_begin).parent_site_id;
                std::size_t site_end = site_begin + 1;
                while (site_end < mission_end
                    && object_at(site_end).parent_site_id == site_id)
                {
                    ++site_end;
                }

                const SatViewSurfaceObject* representative = &object_at(site_begin);
                for (std::size_t index = site_begin; index < site_end; ++index)
                {
                    if (object_at(index).site_representative)
                    {
                        representative = &object_at(index);
                        break;
                    }
                }
                ImGui::PushID(site_id.c_str());
                const bool site_open = ImGui::TreeNodeEx(
                    "##site",
                    group_flags,
                    "%s (%zu)",
                    representative->display_name.c_str(),
                    site_end - site_begin);
                if (site_open)
                {
                    for (std::size_t index = site_begin; index < site_end; ++index)
                    {
                        const std::size_t object_index = visible_indices[index];
                        const SatViewSurfaceObject& object = object_at(index);
                        const bool selected = selected_surface_object_.has_value()
                            && selected_surface_object_->body == body
                            && selected_surface_object_->catalog_index == object_index;
                        const std::string label = object.display_name + " ("
                            + std::string(satview_surface_kind_name(object.kind)) + ")";
                        ImGui::PushID(object.id.c_str());
                        if (ImGui::Selectable(label.c_str(), selected))
                        {
                            apply_selection(view_controller_->select_surface(body, object_index));
                            sync_simulation_render_settings();
                            changed = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
                site_begin = site_end;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        mission_begin = mission_end;
    }

    ImGui::EndChild();
    ImGui::PopID();
}

void SatViewRuntime::render_dockspace(bool keep_alive_only)
{
    ImGuiWindowFlags root_flags = ImGuiWindowFlags_NoDocking
        | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoNavFocus
        | ImGuiWindowFlags_NoBackground;
    if (keep_alive_only)
        root_flags |= ImGuiWindowFlags_NoInputs;
    const ImVec2 pane_position(
        static_cast<float>(viewport_.pixel_pos.x),
        static_cast<float>(viewport_.pixel_pos.y));
    const ImVec2 pane_size(
        static_cast<float>(std::max(viewport_.pixel_size.x, 1)),
        static_cast<float>(std::max(viewport_.pixel_size.y, 1)));
    ImGui::SetNextWindowPos(pane_position);
    ImGui::SetNextWindowSize(pane_size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##satview_dockspace_root", nullptr, root_flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspace_id = ImGui::GetID(kSatViewDockspaceName);
    if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
    {
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, pane_size);
        const float left_ratio = std::clamp(
            kControlPanelDefaultWidth / pane_size.x,
            0.20f,
            0.48f);
        ImGuiID dock_left = 0;
        ImGuiID dock_scene = 0;
        ImGui::DockBuilderSplitNode(
            dockspace_id,
            ImGuiDir_Left,
            left_ratio,
            &dock_left,
            &dock_scene);
        ImGui::DockBuilderDockWindow(kSatViewSceneWindowName, dock_scene);
        ImGui::DockBuilderDockWindow(kSatViewViewWindowName, dock_left);
        ImGui::DockBuilderDockWindow(kSatViewRenderingWindowName, dock_left);
        ImGui::DockBuilderDockWindow(kSatViewFilterWindowName, dock_left);
        ImGui::DockBuilderDockWindow(kSatViewSelectionWindowName, dock_left);
        ImGui::DockBuilderDockWindow(kSatViewAboutWindowName, dock_left);
        ImGui::DockBuilderFinish(dockspace_id);
        ImGui::SetWindowFocus(kSatViewViewWindowName);
    }
    const ImGuiDockNodeFlags dockspace_flags = keep_alive_only
        ? ImGuiDockNodeFlags_KeepAliveOnly
        : ImGuiDockNodeFlags_None;
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    ImGui::End();
}

void SatViewRuntime::render_scene_panel()
{
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
        | ImGuiWindowFlags_NoScrollbar
        | ImGuiWindowFlags_NoScrollWithMouse
        | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(kSatViewSceneWindowName, nullptr, flags);
    ImGui::PopStyleVar();
    if (visible)
    {
        const ImVec2 content_position = ImGui::GetCursorScreenPos();
        const ImVec2 content_size = ImGui::GetContentRegionAvail();
        const int pane_left = viewport_.pixel_pos.x;
        const int pane_top = viewport_.pixel_pos.y;
        const int pane_right = pane_left + std::max(viewport_.pixel_size.x, 1);
        const int pane_bottom = pane_top + std::max(viewport_.pixel_size.y, 1);
        const int left = std::clamp(static_cast<int>(std::floor(content_position.x)), pane_left, pane_right);
        const int top = std::clamp(static_cast<int>(std::floor(content_position.y)), pane_top, pane_bottom);
        const int right = std::clamp(
            static_cast<int>(std::ceil(content_position.x + content_size.x)),
            left,
            pane_right);
        const int bottom = std::clamp(
            static_cast<int>(std::ceil(content_position.y + content_size.y)),
            top,
            pane_bottom);
        scene_viewport_.pixel_pos = glm::ivec2(left, top);
        scene_viewport_.pixel_size = glm::ivec2(
            std::max(right - left, 1),
            std::max(bottom - top, 1));
        ImGui::Dummy(ImVec2(
            std::max(content_size.x, 1.0f),
            std::max(content_size.y, 1.0f)));
    }
    ImGui::End();
}

void SatViewRuntime::render_host_imgui(float dt, const SatViewSimulationSnapshot* snapshot)
{
    if (!imgui_.begin_frame(viewport_.pixel_pos.x, viewport_.pixel_pos.y,
            viewport_.pixel_size.x, viewport_.pixel_size.y, dt))
        return;

    render_dockspace(!show_ui_panel_);
    if (show_ui_panel_)
    {
        render_scene_panel();
        render_control_panel(snapshot);
    }
    if (show_hdr_debug_panel_ && scene_pass_)
        scene_pass_->render_hdr_debug_ui();

    ImGui::Render();
}

void SatViewRuntime::render_view_display_controls(bool& changed)
{
    auto set_control_width = [](const char* label) {
        ImGui::SetNextItemWidth(control_widget_width(label));
    };

    ImGui::SeparatorText("Show");
    if (ImGui::Checkbox("Clouds", &clouds_enabled_))
        request_redraw();
    if (ImGui::Checkbox("Show atmosphere", &atmosphere_enabled_))
        request_redraw();
    ImGui::SameLine();
    if (camera_pov_ == SatViewCameraPov::Moon)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Moon", &moon_enabled_))
        request_redraw();
    if (camera_pov_ == SatViewCameraPov::Moon)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (camera_pov_ == SatViewCameraPov::Sun)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Sun", &sun_enabled_))
        request_redraw();
    if (camera_pov_ == SatViewCameraPov::Sun)
        ImGui::EndDisabled();
    if (ImGui::Checkbox("Surface objects", &surface_filters_.enabled))
        changed = true;
    const bool moon_track_available = camera_pov_ == SatViewCameraPov::Earth;
    if (!moon_track_available)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Moon track", &moon_track_enabled_))
    {
        track_buffer_dirty_ = true;
        request_redraw();
    }
    if (!moon_track_available)
        ImGui::EndDisabled();
    ImGui::SameLine();
    const bool earth_track_available = camera_pov_ == SatViewCameraPov::Sun;
    if (!earth_track_available)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Earth track", &earth_track_enabled_))
    {
        track_buffer_dirty_ = true;
        request_redraw();
    }
    if (!earth_track_available)
        ImGui::EndDisabled();
    if (camera_pov_ == SatViewCameraPov::Sun)
    {
        ImGui::SeparatorText("Planet tracks");
        const auto planet_track_checkbox = [&](SatViewCameraPov body) {
            bool enabled = satview_planet_track_enabled(planet_tracks_, body);
            if (ImGui::Checkbox(camera_pov_name(body), &enabled))
            {
                satview_set_planet_track_enabled(planet_tracks_, body, enabled);
                track_buffer_dirty_ = true;
                request_redraw();
            }
        };
        for (std::size_t index = 0; index < kSatViewPlanetTrackBodies.size(); ++index)
        {
            if (index % 2 == 1)
                ImGui::SameLine();
            planet_track_checkbox(kSatViewPlanetTrackBodies[index]);
        }
        if (ImGui::Button("All planet tracks"))
        {
            for (const SatViewCameraPov body : kSatViewPlanetTrackBodies)
                satview_set_planet_track_enabled(planet_tracks_, body, true);
            track_buffer_dirty_ = true;
            request_redraw();
        }
        ImGui::SameLine();
        if (ImGui::Button("No planet tracks"))
        {
            for (const SatViewCameraPov body : kSatViewPlanetTrackBodies)
                satview_set_planet_track_enabled(planet_tracks_, body, false);
            track_buffer_dirty_ = true;
            request_redraw();
        }
    }

    float star_min_magnitude = star_min_magnitude_;
    set_control_width("Star min mag");
    if (ImGui::SliderFloat(
            "Star min mag", &star_min_magnitude,
            kMinimumStarMagnitude, kMaximumStarMagnitude, "%.1f"))
    {
        star_min_magnitude_ = std::min(
            std::clamp(star_min_magnitude, kMinimumStarMagnitude, kMaximumStarMagnitude),
            star_max_magnitude_);
        rebuild_visible_stars();
        request_redraw();
    }
    float star_max_magnitude = star_max_magnitude_;
    set_control_width("Star max mag");
    if (ImGui::SliderFloat(
            "Star max mag", &star_max_magnitude,
            kMinimumStarMagnitude, kMaximumStarMagnitude, "%.1f"))
    {
        star_max_magnitude_ = std::max(
            std::clamp(star_max_magnitude, kMinimumStarMagnitude, kMaximumStarMagnitude),
            star_min_magnitude_);
        rebuild_visible_stars();
        request_redraw();
    }
    if (ImGui::Checkbox("Constellation figures", &constellation_lines_enabled_))
        request_redraw();
    if (ImGui::Checkbox("Constellation boundaries", &constellation_boundaries_enabled_))
        request_redraw();
    if (ImGui::Checkbox("Constellation labels", &constellation_labels_enabled_))
        request_redraw();
    if (ImGui::Checkbox("Milky Way background", &milky_way_enabled_))
        request_redraw();

    int satellite_display_index = 0;
    switch (satellite_display_mode_)
    {
    case SatViewSatelliteDisplayMode::TracksAndMarkers:
        satellite_display_index = 0;
        break;
    case SatViewSatelliteDisplayMode::TracksOnly:
        satellite_display_index = 1;
        break;
    case SatViewSatelliteDisplayMode::MarkersOnly:
        satellite_display_index = 2;
        break;
    }
    const char* satellite_display_modes[] = {
        "Tracks + Satellites", "Tracks Only", "Satellites Only"
    };
    set_control_width("Display");
    if (ImGui::Combo("Display", &satellite_display_index, satellite_display_modes, 3))
    {
        satellite_display_mode_ = satellite_display_index == 1
            ? SatViewSatelliteDisplayMode::TracksOnly
            : satellite_display_index == 2
            ? SatViewSatelliteDisplayMode::MarkersOnly
            : SatViewSatelliteDisplayMode::TracksAndMarkers;
        changed = true;
    }

    if (!satellite_display_shows_tracks(satellite_display_mode_))
        ImGui::BeginDisabled();
    int track_display_index = track_display_mode_ == SatViewTrackDisplayMode::SelectedOnly ? 1 : 0;
    const char* track_display_modes[] = { "All Sampled", "Selected Only" };
    set_control_width("Paths");
    if (ImGui::Combo("Paths", &track_display_index, track_display_modes, 2))
    {
        track_display_mode_ = track_display_index == 1
            ? SatViewTrackDisplayMode::SelectedOnly
            : SatViewTrackDisplayMode::AllSampled;
        changed = true;
    }
    if (!satellite_display_shows_tracks(satellite_display_mode_))
        ImGui::EndDisabled();
}

void SatViewRuntime::render_visual_controls()
{
    auto set_control_width = [](const char* label) {
        ImGui::SetNextItemWidth(control_widget_width(label));
    };

    if (!clouds_enabled_)
        ImGui::BeginDisabled();
    if (ImGui::Checkbox("Realistic clouds", &realistic_clouds_enabled_))
        request_redraw();
    if (!clouds_enabled_)
        ImGui::EndDisabled();

    float star_brightness = star_brightness_scale_;
    set_control_width("Star brightness");
    if (ImGui::SliderFloat(
            "Star brightness", &star_brightness,
            kMinimumStarBrightnessScale, kMaximumStarBrightnessScale, "%.2fx"))
    {
        star_brightness_scale_ = std::clamp(
            star_brightness,
            kMinimumStarBrightnessScale,
            kMaximumStarBrightnessScale);
        request_redraw();
    }
    float constellation_figure_width = constellation_figure_width_;
    set_control_width("Constellation figure width");
    if (ImGui::SliderFloat(
            "Constellation figure width", &constellation_figure_width,
            kMinimumConstellationLineWidth, kMaximumConstellationLineWidth, "%.1f px"))
    {
        constellation_figure_width_ = std::clamp(
            constellation_figure_width,
            kMinimumConstellationLineWidth,
            kMaximumConstellationLineWidth);
        update_constellation_line_styles();
        request_redraw();
    }
    float constellation_boundary_width = constellation_boundary_width_;
    set_control_width("Constellation boundary width");
    if (ImGui::SliderFloat(
            "Constellation boundary width", &constellation_boundary_width,
            kMinimumConstellationLineWidth, kMaximumConstellationLineWidth, "%.1f px"))
    {
        constellation_boundary_width_ = std::clamp(
            constellation_boundary_width,
            kMinimumConstellationLineWidth,
            kMaximumConstellationLineWidth);
        update_constellation_line_styles();
        request_redraw();
    }
    float milky_way_brightness = milky_way_brightness_;
    set_control_width("Milky Way brightness");
    if (ImGui::SliderFloat(
            "Milky Way brightness", &milky_way_brightness,
            kMinimumMilkyWayBrightness, kMaximumMilkyWayBrightness, "%.2f"))
    {
        milky_way_brightness_ = std::clamp(
            milky_way_brightness,
            kMinimumMilkyWayBrightness,
            kMaximumMilkyWayBrightness);
        request_redraw();
    }

    if (projection_mode_ != SatViewProjectionMode::Ground)
        ImGui::BeginDisabled();
    float ground_marker_scale = ground_marker_scale_;
    set_control_width("Ground marker scale");
    if (ImGui::SliderFloat(
            "Ground marker scale",
            &ground_marker_scale,
            kGroundMinimumMarkerScale,
            kGroundMaximumMarkerScale,
            "%.2fx",
            ImGuiSliderFlags_Logarithmic))
    {
        ground_marker_scale_ = clamp_ground_marker_scale(ground_marker_scale);
        marker_buffer_dirty_ = true;
        request_redraw();
    }
    if (projection_mode_ != SatViewProjectionMode::Ground)
        ImGui::EndDisabled();
}

void SatViewRuntime::render_control_panel(const SatViewSimulationSnapshot* snapshot)
{
    bool changed = false;
    const SatViewConfig config_before = current_config();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    auto set_control_width = [](const char* label) {
        ImGui::SetNextItemWidth(control_widget_width(label));
    };
    const double displayed_simulation_seconds = snapshot
        ? render_simulation_seconds(*snapshot)
        : last_draw_simulation_seconds_;
    const auto catalog_snapshot = catalog_service_.status();
    const bool show_tracks = satellite_display_shows_tracks(satellite_display_mode_);
    const bool show_markers = satellite_display_shows_markers(satellite_display_mode_);

    if (ImGui::Begin(kSatViewViewWindowName, nullptr, flags))
    {
        if (ImGui::Button(paused_ ? "Resume" : "Pause"))
        {
            paused_ = !paused_;
            sync_simulation_controls();
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Real Time"))
            set_real_time();
        ImGui::SameLine();
        if (ImGui::Button("Reset Camera"))
        {
            reset_camera();
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
        {
            catalog_service_.request_refresh();
            if (cloud_service_)
                cloud_service_->request_refresh();
            changed = true;
        }
        if (ImGui::Button("Reset Defaults"))
        {
            reset_to_default_settings();
            changed = true;
        }

        ImGui::TextUnformatted("View");
        ImGui::SameLine();
        if (ImGui::RadioButton("Globe", projection_mode_ == SatViewProjectionMode::Globe))
        {
            projection_mode_ = SatViewProjectionMode::Globe;
            simulation_settings_dirty_ = true;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Map", projection_mode_ == SatViewProjectionMode::Map))
        {
            projection_mode_ = SatViewProjectionMode::Map;
            simulation_settings_dirty_ = true;
            camera_->ClearMotion();
            camera_manipulator_->Cancel();
            camera_keys_->reset();
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Ground", projection_mode_ == SatViewProjectionMode::Ground))
        {
            enter_ground_view_at(ground_location_radians_);
            changed = true;
        }

        ImGui::TextUnformatted("POV");
        ImGui::SameLine();
        ImGui::PushID("camera_pov");
        if (projection_mode_ == SatViewProjectionMode::Ground)
            ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(std::max(190.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##body", satview_solar_system_body(camera_pov_).name.data()))
        {
            std::string_view previous_system;
            for (const SatViewSolarSystemBody& body : satview_solar_system_bodies())
            {
                if (body.system_name != previous_system)
                {
                    if (!previous_system.empty())
                        ImGui::Separator();
                    ImGui::TextDisabled("%.*s",
                        static_cast<int>(body.system_name.size()), body.system_name.data());
                    previous_system = body.system_name;
                }
                const bool selected = camera_pov_ == body.id;
                const std::string label = body.parent.has_value()
                        && *body.parent != SatViewCameraPov::Sun
                    ? std::string("   ") + std::string(body.name)
                    : std::string(body.name);
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    set_camera_pov(body.id, displayed_simulation_seconds);
                    changed = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (projection_mode_ == SatViewProjectionMode::Ground)
            ImGui::EndDisabled();
        ImGui::PopID();
        const SatViewSolarSystemBody& selected_body = satview_solar_system_body(camera_pov_);
        if (satview_uses_generic_body_view(camera_pov_))
        {
            ImGui::TextDisabled(
                "Equatorial radius: %.0f km%s",
                selected_body.equatorial_radius_km,
                selected_body.polar_radius_km < selected_body.equatorial_radius_km * 0.995
                    ? " (ellipsoid)"
                    : "");
        }

        if (!satview_uses_generic_body_view(camera_pov_))
        {
            int central_body_index = filter_.show_earth && !filter_.show_moon && !filter_.show_mars
                ? 0
                : (!filter_.show_earth && filter_.show_moon && !filter_.show_mars)
                ? 1
                : (!filter_.show_earth && !filter_.show_moon && filter_.show_mars) ? 2
                                                                                   : 3;
            const char* central_body_options[] = { "Earth", "Moon", "Mars", "All" };
            set_control_width("Central body");
            if (ImGui::Combo("Central body", &central_body_index, central_body_options, 4))
            {
                filter_.show_earth = central_body_index == 0 || central_body_index == 3;
                filter_.show_moon = central_body_index == 1 || central_body_index == 3;
                filter_.show_mars = central_body_index == 2 || central_body_index == 3;
                simulation_settings_dirty_ = true;
                changed = true;
            }
        }
        else
        {
            ImGui::TextDisabled("Approximate local tracks: major natural satellites");
            if (camera_pov_ != SatViewCameraPov::Sun)
            {
                if (selected_body.parent.has_value()
                    && *selected_body.parent != SatViewCameraPov::Sun)
                {
                    const SatViewSolarSystemBody& parent = satview_solar_system_body(*selected_body.parent);
                    ImGui::TextDisabled(
                        "Sky context: Sun and %.*s",
                        static_cast<int>(parent.name.size()),
                        parent.name.data());
                }
                else
                {
                    ImGui::TextDisabled("Sky context: Sun");
                }
            }
        }

        render_view_display_controls(changed);

        float speed = time_speed_;
        set_control_width("Speed");
        if (ImGui::SliderFloat("Speed", &speed, 1.0f, 3600.0f, "%.0fx", ImGuiSliderFlags_Logarithmic))
        {
            time_speed_ = std::clamp(speed, 1.0f, 3600.0f);
            sync_simulation_controls();
            changed = true;
        }

        const std::string local_time = format_local_simulation_time(displayed_simulation_seconds);
        ImGui::Text("Local time: %s", local_time.c_str());
        if (projection_mode_ == SatViewProjectionMode::Ground)
        {
            ImGui::Text("Ground lat: %.3f", glm::degrees(ground_location_radians_.y));
            ImGui::Text("Ground lon: %.3f", glm::degrees(ground_location_radians_.x));
            ImGui::TextUnformatted("Projection");
            if (ImGui::RadioButton(
                    "Stereographic",
                    ground_projection_ == SatViewGroundProjection::Stereographic))
            {
                ground_projection_ = SatViewGroundProjection::Stereographic;
                ground_fov_degrees_ = satview_clamp_ground_fov_degrees(
                    ground_projection_,
                    ground_fov_degrees_);
                changed = true;
            }
            if (ImGui::RadioButton(
                    "Perspective",
                    ground_projection_ == SatViewGroundProjection::Perspective))
            {
                ground_projection_ = SatViewGroundProjection::Perspective;
                ground_fov_degrees_ = satview_clamp_ground_fov_degrees(
                    ground_projection_,
                    ground_fov_degrees_);
                changed = true;
            }
            float ground_fov = ground_fov_degrees_;
            set_control_width("Angle of view");
            if (ImGui::SliderFloat(
                    "Angle of view",
                    &ground_fov,
                    kGroundMinimumFovDegrees,
                    satview_maximum_ground_fov_degrees(ground_projection_),
                    "%.0f deg"))
            {
                ground_fov_degrees_ = satview_clamp_ground_fov_degrees(
                    ground_projection_,
                    ground_fov);
                changed = true;
            }
            if (ImGui::Checkbox("Show ground", &ground_visible_))
                changed = true;
            if (ImGui::Checkbox("Horizon occlusion", &ground_horizon_occlusion_))
            {
                invalidate_visual_buffers();
                changed = true;
            }
            if (ImGui::Checkbox("Observatory horizon", &observatory_horizon_enabled_))
                changed = true;
            if (ImGui::Checkbox("Cardinal labels", &cardinal_labels_enabled_))
                changed = true;
            if (ImGui::Button("Back to Globe"))
            {
                projection_mode_ = SatViewProjectionMode::Globe;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Back to Map"))
            {
                projection_mode_ = SatViewProjectionMode::Map;
                changed = true;
            }
        }
        else if (projection_mode_ == SatViewProjectionMode::Map)
        {
            ImGui::Text("Center lat: %.1f", glm::degrees(map_center_radians_.y));
            ImGui::Text("Center lon: %.1f", glm::degrees(map_center_radians_.x));
            ImGui::TextDisabled("Double-click the map to enter ground view.");
        }
        else
        {
            ImGui::TextDisabled("Double-click Earth to enter ground view.");
        }
    }
    ImGui::End();

    if (ImGui::Begin(kSatViewFilterWindowName, nullptr, flags))
    {
        if (ImGui::BeginTabBar("##satview_filter_tabs"))
        {
            if (ImGui::BeginTabItem("Orbits"))
            {
                ImGui::SeparatorText("Objects");
                set_control_width("Search");
                if (ImGui::InputText("Search", search_buffer_, sizeof(search_buffer_)))
                {
                    filter_.search_text = search_buffer_;
                    changed = true;
                }
                set_control_width("Type");
                if (ImGui::InputText("Type", object_type_buffer_, sizeof(object_type_buffer_)))
                {
                    filter_.object_type_text = object_type_buffer_;
                    changed = true;
                }
                set_control_width("Source");
                if (ImGui::InputText("Source", source_buffer_, sizeof(source_buffer_)))
                {
                    filter_.source_text = source_buffer_;
                    changed = true;
                }

                float max_age_days = static_cast<float>(filter_.max_epoch_age_days);
                set_control_width("Age days");
                if (ImGui::DragFloat("Age days", &max_age_days, 0.1f, 0.0f, 30.0f, "%.1f"))
                {
                    filter_.max_epoch_age_days = static_cast<double>(std::max(0.0f, max_age_days));
                    changed = true;
                }

                changed |= ImGui::Checkbox("LEO", &filter_.show_low_earth);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("MEO", &filter_.show_medium_earth);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("GEO", &filter_.show_geosynchronous);
                changed |= ImGui::Checkbox("HEO", &filter_.show_highly_elliptical);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Other", &filter_.show_other);
                changed |= ImGui::Checkbox("SSO candidates only", &filter_.sun_synchronous_only);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Derived from the current orbit's J2 nodal precession; "
                        "this is independent of the LEO/MEO/GEO/HEO class.");
                }
                ImGui::BeginDisabled(!filter_.sun_synchronous_only);
                ImGui::Indent();
                changed |= ImGui::Checkbox(
                    "Dawn/dusk (terminator)", &filter_.show_sun_synchronous_terminator);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Local time of the ascending node near 06:00/18:00; the plane rides "
                        "the terminator and stays sunlit.");
                }
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Other SSO", &filter_.show_sun_synchronous_other);
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Morning/afternoon or noon/midnight sun-synchronous orbits; these pass "
                        "through Earth's shadow each orbit.");
                }
                ImGui::Unindent();
                ImGui::EndDisabled();

                ImGui::SeparatorText("Central Body");
                changed |= ImGui::Checkbox("Earth objects", &filter_.show_earth);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Moon objects", &filter_.show_moon);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Mars objects", &filter_.show_mars);

                ImGui::SeparatorText("Population");
                const auto population_checkbox = [&](const char* label,
                                                     bool& visible,
                                                     SatellitePopulation population,
                                                     std::size_t count) {
                    const glm::vec4 color = population_color(population, 1.0f);
                    ImGui::TextColored(ImVec4(color.r, color.g, color.b, color.a), "%zu", count);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox(label, &visible);
                };
                population_checkbox("Active payloads", filter_.show_active_payloads,
                    SatellitePopulation::ActivePayload, catalog_snapshot.populations.active_payloads);
                population_checkbox("Inactive payloads", filter_.show_inactive_payloads,
                    SatellitePopulation::InactivePayload, catalog_snapshot.populations.inactive_payloads);
                population_checkbox("Rocket bodies", filter_.show_rocket_bodies,
                    SatellitePopulation::RocketBody, catalog_snapshot.populations.rocket_bodies);
                population_checkbox("Debris", filter_.show_debris,
                    SatellitePopulation::Debris, catalog_snapshot.populations.debris);
                population_checkbox("Unknown", filter_.show_unknown_population,
                    SatellitePopulation::Unknown, catalog_snapshot.populations.unknown);
                changed |= ImGui::Checkbox("Show SATCAT summary estimates", &filter_.show_summary_estimates);
                changed |= ImGui::Checkbox("Show catalog-only objects", &filter_.show_catalog_only);
                ImGui::TextDisabled("Earth SATCAT-only positions are estimates; Moon and Mars rows need ephemerides.");
                if (filter_.show_mars && !filter_.show_earth && !filter_.show_moon)
                {
                    ImGui::TextDisabled(
                        "Mars orbit rows are catalog-only until a Mars-relative ephemeris source is imported.");
                }

                render_object_tree(snapshot, changed);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Surface"))
            {
                changed |= ImGui::Checkbox("Visible", &surface_filters_.enabled);
                changed |= ImGui::Checkbox("Landing sites and landers", &surface_filters_.show_landers);
                changed |= ImGui::Checkbox("Rovers", &surface_filters_.show_rovers);
                changed |= ImGui::Checkbox("Instruments and reflectors", &surface_filters_.show_instruments);
                changed |= ImGui::Checkbox("Impacts and rocket stages", &surface_filters_.show_impacts);
                changed |= ImGui::Checkbox("Crewed artifacts", &surface_filters_.show_crewed_artifacts);
                changed |= ImGui::Checkbox("Approximate locations", &surface_filters_.show_approximate_locations);
                if (const std::optional<CentralBody> body = active_surface_body())
                {
                    ImGui::SeparatorText("Objects");
                    render_surface_tree(*body, surface_catalog(*body), surface_filters_, changed);
                }
                else
                {
                    ImGui::TextDisabled("Select Moon or Mars to inspect surface objects.");
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    if (ImGui::Begin(kSatViewRenderingWindowName, nullptr, flags))
    {
        render_visual_controls();
        ImGui::SeparatorText("Tone Mapping");
        float exposure = tone_map_exposure_;
        set_control_width("Exposure");
        if (ImGui::SliderFloat(
                "Exposure",
                &exposure,
                kMinimumToneMapExposure,
                kMaximumToneMapExposure,
                "%.2f"))
        {
            tone_map_exposure_ = std::clamp(
                exposure,
                kMinimumToneMapExposure,
                kMaximumToneMapExposure);
            request_redraw();
        }
        float white_point = tone_map_white_point_;
        set_control_width("White point");
        if (ImGui::SliderFloat(
                "White point",
                &white_point,
                kMinimumToneMapWhitePoint,
                kMaximumToneMapWhitePoint,
                "%.2f",
                ImGuiSliderFlags_Logarithmic))
        {
            tone_map_white_point_ = std::clamp(
                white_point,
                kMinimumToneMapWhitePoint,
                kMaximumToneMapWhitePoint);
            request_redraw();
        }
        if (ImGui::Checkbox("HDR buffer debug", &show_hdr_debug_panel_))
            request_redraw();

        int color_mode_index = static_cast<int>(color_mode_);
        const char* color_modes[] = { "Population", "Name Prefix", "Orbit Class", "Object Type" };
        set_control_width("Color");
        if (ImGui::Combo("Color", &color_mode_index, color_modes, 4))
        {
            color_mode_ = static_cast<SatViewColorMode>(color_mode_index);
            changed = true;
        }

        if (!show_tracks)
            ImGui::BeginDisabled();

        const std::size_t available_track_count = snapshot ? snapshot->states.size() : 0;
        const std::size_t track_limit_ceiling = available_track_count > 0
            ? available_track_count
            : std::max(track_satellite_limit_, kDefaultTrackSatelliteLimit);
        const int maximum_track_limit = static_cast<int>(std::min(
            track_limit_ceiling,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        const int minimum_track_limit = 1;
        int track_limit = std::clamp(
            static_cast<int>(std::min(
                track_satellite_limit_,
                static_cast<std::size_t>(std::numeric_limits<int>::max()))),
            minimum_track_limit,
            maximum_track_limit);
        set_control_width("Track count");
        if (ImGui::SliderInt("Track count", &track_limit, minimum_track_limit, maximum_track_limit))
        {
            track_satellite_limit_ = static_cast<std::size_t>(track_limit);
            simulation_settings_dirty_ = true;
            changed = true;
        }

        int track_samples = static_cast<int>(track_sample_count_);
        set_control_width("Track samples");
        if (ImGui::SliderInt("Track samples", &track_samples, 12,
                static_cast<int>(kMaximumTrackSampleCount)))
        {
            track_sample_count_ = static_cast<std::size_t>(std::max(12, track_samples));
            simulation_settings_dirty_ = true;
            changed = true;
        }

        if (ImGui::Checkbox("Refresh paths every step", &refresh_tracks_each_step_))
        {
            simulation_settings_dirty_ = true;
            changed = true;
        }
        if (!show_tracks)
            ImGui::EndDisabled();

        static constexpr std::size_t kMarkerLimitValues[] = { 0, 8192, 4096, 2048, 1024, 512 };
        static constexpr const char* kMarkerLimitLabels[] = { "All", "8192", "4096", "2048", "1024", "512" };
        int marker_limit_index = 0;
        for (int i = 0; i < 6; ++i)
        {
            if (marker_satellite_limit_ == kMarkerLimitValues[i])
            {
                marker_limit_index = i;
                break;
            }
        }
        if (!show_markers)
            ImGui::BeginDisabled();
        set_control_width("Marker cap");
        if (ImGui::Combo("Marker cap", &marker_limit_index, kMarkerLimitLabels, 6))
        {
            marker_satellite_limit_ = kMarkerLimitValues[marker_limit_index];
            changed = true;
        }
        if (!show_markers)
            ImGui::EndDisabled();
    }
    ImGui::End();

    if (ImGui::Begin(kSatViewAboutWindowName, nullptr, flags))
    {
        ImGui::SeparatorText("Sources");
        const std::string catalog_status = catalog_service_.status_text();
        ImGui::TextWrapped("%s", catalog_status.empty() ? "catalog pending" : catalog_status.c_str());
        const auto data_source_name = [](SatViewCatalogService::DataSource source) {
            switch (source)
            {
            case SatViewCatalogService::DataSource::Live:
                return "live";
            case SatViewCatalogService::DataSource::Cache:
                return "cache";
            case SatViewCatalogService::DataSource::Sample:
                return "sample";
            case SatViewCatalogService::DataSource::None:
                return "pending";
            }
            return "pending";
        };
        ImGui::Text("GP: %s, %zu records",
            data_source_name(catalog_snapshot.gp.data_source),
            catalog_snapshot.gp.object_count);
        ImGui::Text("SATCAT: %s, %zu retained, %zu excluded",
            data_source_name(catalog_snapshot.satcat.data_source),
            catalog_snapshot.satcat.object_count,
            catalog_snapshot.satcat.excluded_records);
        ImGui::Text("Merged: %zu total, %zu renderable",
            catalog_snapshot.object_count,
            catalog_snapshot.renderable_count);
        ImGui::Text("Skipped from scene: %zu (%zu retained without an orbit)",
            catalog_snapshot.skipped_records,
            catalog_snapshot.non_renderable_count);
        if (cloud_service_)
        {
            const std::string cloud_status = cloud_service_->status_text();
            ImGui::TextWrapped("Clouds: %s", cloud_status.c_str());
            ImGui::TextDisabled("Contains modified EUMETSAT data");
        }
        const std::string_view source_label = snapshot ? std::string_view(snapshot->source_label) : std::string_view{};
        if (source_label.empty())
            ImGui::TextDisabled("Source: pending");
        else
            ImGui::Text("Source: %.*s", static_cast<int>(source_label.size()), source_label.data());
        const std::size_t filtered_markers = snapshot
            ? static_cast<std::size_t>(std::ranges::count_if(
                  snapshot->states,
                  [this, source_label](const SatellitePropagatedState& state) {
                      return satellite_visible(filter_, state, source_label);
                  }))
            : 0;
        const std::size_t rendered_markers = !show_markers
            ? 0
            : (marker_satellite_limit_ == 0
                      ? filtered_markers
                      : std::min(filtered_markers, marker_satellite_limit_));
        const std::size_t total_markers = snapshot ? snapshot->states.size() : 0;
        const std::size_t total_tracks = (snapshot && snapshot->tracks) ? snapshot->tracks->size() : 0;
        ImGui::Text("Markers: %zu / %zu", rendered_markers, total_markers);
        ImGui::Text("Paths: %zu / %zu", visible_track_count(snapshot), total_tracks);
        ImGui::Text("Stars: %zu / %zu",
            visible_stars_.size(),
            stars_.size());
        if (lunar_surface_catalog_ && lunar_surface_catalog_->error.empty())
        {
            ImGui::Text("Lunar surface: %zu objects, %zu sites",
                lunar_surface_catalog_->objects.size(),
                lunar_surface_catalog_->site_count);
            ImGui::TextDisabled("Coordinates: LROC; identity enrichment: GCAT CC BY 4.0");
        }
        else if (lunar_surface_catalog_)
        {
            ImGui::TextWrapped("Lunar surface: %s", lunar_surface_catalog_->error.c_str());
        }
        if (mars_surface_catalog_ && mars_surface_catalog_->error.empty())
        {
            ImGui::Text("Mars surface: %zu objects, %zu sites",
                mars_surface_catalog_->objects.size(),
                mars_surface_catalog_->site_count);
            ImGui::TextDisabled("Coordinates: curated landing-site references");
        }
        else if (mars_surface_catalog_)
        {
            ImGui::TextWrapped("Mars surface: %s", mars_surface_catalog_->error.c_str());
        }
    }
    ImGui::End();

    if (ImGui::Begin(kSatViewSelectionWindowName, nullptr, flags))
    {
        if (const SatViewSurfaceObject* selected = selected_surface_object())
        {
            ImGui::TextWrapped("%s", selected->display_name.c_str());
            ImGui::Text("Body: %.*s",
                static_cast<int>(central_body_name(selected->body).size()),
                central_body_name(selected->body).data());
            ImGui::Text("Mission: %s", selected->mission_name.c_str());
            if (!selected->vehicle_name.empty() && selected->vehicle_name != selected->display_name)
                ImGui::TextWrapped("Vehicle: %s", selected->vehicle_name.c_str());
            const std::string_view kind = satview_surface_kind_name(selected->kind);
            ImGui::Text("Kind: %.*s", static_cast<int>(kind.size()), kind.data());
            if (!selected->status.empty())
                ImGui::Text("Status: %s", selected->status.c_str());
            if (selected->renderable())
            {
                ImGui::Text("Latitude: %.5f %c",
                    std::abs(selected->latitude_degrees),
                    selected->latitude_degrees >= 0.0 ? 'N' : 'S');
                ImGui::Text("Longitude: %.5f %c",
                    std::abs(selected->longitude_east_degrees),
                    selected->longitude_east_degrees >= 0.0 ? 'E' : 'W');
            }
            else
            {
                ImGui::TextDisabled("No independently located coordinate.");
            }
            if (!selected->arrival_date.empty())
                ImGui::Text("Arrival: %s", selected->arrival_date.c_str());
            const std::string_view quality = satview_surface_location_quality_name(
                selected->location_quality);
            ImGui::Text("Location: %.*s", static_cast<int>(quality.size()), quality.data());
            if (selected->coordinate_uncertainty_m.has_value())
                ImGui::Text("Uncertainty: %.1f m", *selected->coordinate_uncertainty_m);
            if (!selected->coordinate_source.empty())
                ImGui::TextWrapped("Coordinate source: %s", selected->coordinate_source.c_str());
            if (!selected->owner.empty() || !selected->country.empty())
                ImGui::Text("Owner: %s%s%s",
                    selected->owner.c_str(),
                    !selected->owner.empty() && !selected->country.empty() ? " / " : "",
                    selected->country.c_str());
            if (!selected->cospar_id.empty())
                ImGui::Text("COSPAR: %s", selected->cospar_id.c_str());
            if (!selected->gcat_id.empty())
                ImGui::Text("GCAT: %s", selected->gcat_id.c_str());
            if (!selected->references.empty())
            {
                ImGui::SeparatorText("References");
                for (const std::string& reference : selected->references)
                    ImGui::TextWrapped("%s", reference.c_str());
            }
            if (!selected->renderable())
                ImGui::BeginDisabled();
            if (ImGui::Button("Center on site"))
            {
                center_selected_surface_object(displayed_simulation_seconds);
                changed = true;
            }
            if (!selected->renderable())
                ImGui::EndDisabled();
            if (ImGui::Button("Clear Selection"))
            {
                apply_selection(view_controller_->clear_selection());
                changed = true;
            }
        }
        else if (const SatViewSolarSystemBody* selected = selected_natural_body())
        {
            ImGui::TextWrapped("%s", selected->name.data());
            ImGui::Text("System: %s", selected->system_name.data());
            if (selected->parent.has_value())
                ImGui::Text("Orbits: %s", camera_pov_name(*selected->parent));
            ImGui::Text("Equatorial radius: %.1f km", selected->equatorial_radius_km);
            ImGui::Text("Polar radius: %.1f km", selected->polar_radius_km);
            if (selected->semi_major_axis_km > 0.0)
                ImGui::Text("Semi-major axis: %.0f km", selected->semi_major_axis_km);
            if (selected->orbital_period_days > 0.0)
                ImGui::Text("Orbital period: %.3f days", selected->orbital_period_days);
            if (ImGui::Button("Go to body"))
            {
                set_camera_pov(selected->id, displayed_simulation_seconds);
                changed = true;
            }
            if (ImGui::Button("Clear Selection"))
            {
                apply_selection(view_controller_->clear_selection());
                changed = true;
            }
        }
        else if (const SatellitePropagatedState* selected = selected_satellite(snapshot))
        {
            const SatelliteStaticMetadata* metadata = selected->metadata.get();
            ImGui::TextWrapped("%s",
                metadata && !metadata->object_name.empty() ? metadata->object_name.c_str() : "Unnamed object");
            ImGui::Text("NORAD: %lld", static_cast<long long>(selected->norad_catalog_id));
            if (metadata && !metadata->object_id.empty())
                ImGui::Text("Object ID: %s", metadata->object_id.c_str());
            ImGui::Text("Orbit: %.*s",
                static_cast<int>(orbit_class_name(selected->orbit_class).size()),
                orbit_class_name(selected->orbit_class).data());
            ImGui::Text("Central body: %.*s",
                static_cast<int>(central_body_name(selected->central_body).size()),
                central_body_name(selected->central_body).data());
            ImGui::Text("Sun-synchronous: %s",
                !selected->sun_synchronous_candidate
                    ? "No"
                    : selected->sun_synchronous_terminator
                    ? "Dawn/dusk terminator (derived)"
                    : "Other (derived)");
            if (metadata && !metadata->object_type.empty())
                ImGui::Text("Type: %s", metadata->object_type.c_str());
            ImGui::Text("Kind: %.*s",
                static_cast<int>(satellite_object_kind_name(selected->object_kind).size()),
                satellite_object_kind_name(selected->object_kind).data());
            ImGui::Text("Population: %.*s",
                static_cast<int>(satellite_population_name(selected->population).size()),
                satellite_population_name(selected->population).data());
            ImGui::Text("Solution: %.*s",
                static_cast<int>(orbit_solution_kind_name(selected->solution_kind).size()),
                orbit_solution_kind_name(selected->solution_kind).data());
            if (metadata && !metadata->owner.empty())
                ImGui::Text("Owner: %s", metadata->owner.c_str());
            if (metadata && !metadata->operational_status_code.empty())
                ImGui::Text("Status: %s", metadata->operational_status_code.c_str());
            if (metadata && !metadata->data_status_code.empty())
                ImGui::Text("Data status: %s", metadata->data_status_code.c_str());
            if (metadata && !metadata->ephemeris_source.empty())
                ImGui::TextWrapped("Ephemeris: %s", metadata->ephemeris_source.c_str());
            if (metadata && !metadata->ephemeris_frame.empty())
                ImGui::Text("Frame: %s", metadata->ephemeris_frame.c_str());
            if (metadata && metadata->ephemeris_start_unix_seconds.has_value()
                && metadata->ephemeris_end_unix_seconds.has_value())
            {
                const std::string valid_from = format_local_simulation_time(
                    *metadata->ephemeris_start_unix_seconds);
                const std::string valid_until = format_local_simulation_time(
                    *metadata->ephemeris_end_unix_seconds);
                ImGui::TextWrapped("Valid: %s to %s", valid_from.c_str(), valid_until.c_str());
            }
            if (metadata && metadata->radar_cross_section_m2.has_value())
                ImGui::Text("Radar cross section: %.3g m^2", *metadata->radar_cross_section_m2);
            if (metadata && !metadata->classification_type.empty())
                ImGui::Text("Class: %s", metadata->classification_type.c_str());
            ImGui::Text("Period: %.1f min", selected->period_minutes);
            if (std::isfinite(selected->minutes_since_epoch))
                ImGui::Text("Epoch age: %.1f h", selected->minutes_since_epoch / 60.0);
            else
                ImGui::TextDisabled("Epoch age: unavailable for SATCAT summary");
            if (selected->central_body == CentralBody::Earth)
            {
                const SatViewGeodeticPosition geodetic = satview_geodetic_from_ecef(selected->ecef_position_km);
                ImGui::Text("Latitude: %.3f %c",
                    std::abs(geodetic.latitude_degrees),
                    geodetic.latitude_degrees >= 0.0 ? 'N' : 'S');
                ImGui::Text("Longitude: %.3f %c",
                    std::abs(geodetic.longitude_degrees),
                    geodetic.longitude_degrees >= 0.0 ? 'E' : 'W');
            }
            const glm::dvec3 central_position = selected->central_body == CentralBody::Moon
                ? satview_moon_position(displayed_simulation_seconds).equatorial_position_km
                : glm::dvec3(0.0);
            const double central_radius_km = selected->central_body == CentralBody::Moon
                ? kSatViewMoonMeanRadiusKm
                : kSatViewEarthEquatorialRadiusKm;
            const double altitude_km = glm::length(selected->teme_position_km - central_position)
                - central_radius_km;
            ImGui::Text("Altitude: %.0f km", altitude_km);
            const double speed_km_s = glm::length(selected->teme_velocity_km_per_s);
            ImGui::Text("Speed: %.2f km/s", speed_km_s);
            if (ImGui::Button("Clear Selection"))
            {
                apply_selection(view_controller_->clear_selection());
                simulation_settings_dirty_ = true;
                changed = true;
            }
        }
        else if (const SatelliteRecord* selected = selected_catalog_record())
        {
            ImGui::TextWrapped("%s",
                selected->object_name.empty() ? "Unnamed object" : selected->object_name.c_str());
            ImGui::Text("NORAD: %lld", static_cast<long long>(selected->norad_catalog_id));
            if (!selected->object_id.empty())
                ImGui::Text("Object ID: %s", selected->object_id.c_str());
            ImGui::Text("Central body: %.*s",
                static_cast<int>(central_body_name(selected->central_body).size()),
                central_body_name(selected->central_body).data());
            ImGui::Text("Kind: %.*s",
                static_cast<int>(satellite_object_kind_name(selected->object_kind).size()),
                satellite_object_kind_name(selected->object_kind).data());
            ImGui::Text("Population: %.*s",
                static_cast<int>(satellite_population_name(selected->population).size()),
                satellite_population_name(selected->population).data());
            ImGui::Text("Fidelity: %.*s",
                static_cast<int>(orbit_solution_kind_name(selected->solution_kind).size()),
                orbit_solution_kind_name(selected->solution_kind).data());
            if (!selected->owner.empty())
                ImGui::Text("Owner: %s", selected->owner.c_str());
            if (!selected->operational_status_code.empty())
                ImGui::Text("Status: %s", selected->operational_status_code.c_str());
            ImGui::TextDisabled("No usable public orbital state; this object is not rendered.");
            if (ImGui::Button("Clear Selection"))
            {
                apply_selection(view_controller_->clear_selection());
                simulation_settings_dirty_ = true;
                changed = true;
            }
        }
        else
        {
            ImGui::TextDisabled("No object selected.");
        }
    }
    ImGui::End();

    if (changed)
    {
        invalidate_visual_buffers();
        clear_selection_if_missing(snapshot);
        request_redraw();
    }

    if (current_config() != config_before)
        config_dirty_ = true;
    if (config_dirty_ && !ImGui::IsAnyItemActive())
    {
        persist_config();
        config_dirty_ = false;
    }
}


} // namespace draxul::satview
