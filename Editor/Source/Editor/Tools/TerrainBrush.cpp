#include "EditorPCH.h"
#include "Editor/Tools/TerrainBrush.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/EditorViewportState.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace {

		// The tool picker's entries: "Off" first, then terrain.edit's tools in their registry order.
		constexpr std::array<std::pair<TerrainTool, std::string_view>, 5> BrushTools = { { { TerrainTool::Raise, "Raise" }, { TerrainTool::Lower, "Lower" },
			{ TerrainTool::Smooth, "Smooth" }, { TerrainTool::Flatten, "Flatten" }, { TerrainTool::Paint, "Paint" } } };

		// The bisection steps that refine a crossing found by the march, and the most steps the march takes.
		constexpr uint32_t SurfaceRefinements = 24;
		constexpr uint32_t MaxSurfaceSteps = 1U << 16U;
		// The segments of the brush's circle.
		constexpr uint32_t CircleSegments = 48;

		// Continues the toolbar's row when an item of `width` still fits before `toolbarRight` (SceneViewportPanel's rule).
		void PlaceToolbarItem(float toolbarRight, float width)
		{
			if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= toolbarRight)
				ImGui::SameLine();
		}

		std::string_view GetToolName(TerrainTool tool)
		{
			for (const auto& [entry, name] : BrushTools)
			{
				if (entry == tool)
					return name;
			}
			return "Raise";
		}

		// How far below the surface the local point `point` is (positive below), or nullopt outside the terrain.
		std::optional<float> GetDepthBelowSurface(const HeightfieldData& heightfield, const TerrainExtent& extent, const glm::vec3& point)
		{
			const std::optional<float> surface = SampleTerrainHeight(heightfield, extent, glm::vec2(point.x, point.z));
			if (!surface.has_value())
				return std::nullopt;
			return *surface - point.y;
		}

	}

	std::optional<glm::vec3> IntersectTerrainSurface(const RenderRay& ray, const glm::mat4& world, const HeightfieldData& heightfield, const TerrainExtent& extent)
	{
		if (!ValidateHeightfieldData(heightfield).has_value() || !(extent.Size.x > 0.0f) || !(extent.Size.y > 0.0f) || !(extent.HeightScale > 0.0f))
			return std::nullopt;
		const glm::mat4 toLocal = glm::affineInverse(world);
		const glm::vec3 origin(toLocal * glm::vec4(ray.Origin, 1.0f));
		const glm::vec3 direction(toLocal * glm::vec4(ray.Direction, 0.0f));
		if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) || !std::isfinite(direction.x) || !std::isfinite(direction.y)
			|| !std::isfinite(direction.z))
			return std::nullopt;

		// The ray's interval inside the terrain's box (the slab test), from its origin on.
		const glm::vec3 boxMax(extent.Size.x, extent.HeightScale, extent.Size.y);
		float enter = 0.0f;
		float leave = std::numeric_limits<float>::max();
		for (glm::length_t axis = 0; axis < 3; ++axis)
		{
			if (direction[axis] == 0.0f)
			{
				if (origin[axis] < 0.0f || origin[axis] > boxMax[axis])
					return std::nullopt;
				continue;
			}
			float nearSide = (0.0f - origin[axis]) / direction[axis];
			float farSide = (boxMax[axis] - origin[axis]) / direction[axis];
			if (nearSide > farSide)
				std::swap(nearSide, farSide);
			enter = std::max(enter, nearSide);
			leave = std::min(leave, farSide);
		}
		if (!(enter <= leave))
			return std::nullopt;

		// March in steps of at most half a cell of the ray's horizontal travel until the ray passes below the surface.
		const float span = leave - enter;
		const float cell = std::min(extent.Size.x, extent.Size.y) / static_cast<float>(heightfield.Resolution - 1);
		const float horizontal = std::sqrt(direction.x * direction.x + direction.z * direction.z) * span;
		const float stepCount = std::ceil(horizontal / (0.5f * cell));
		const uint32_t steps = std::isfinite(stepCount) ? static_cast<uint32_t>(std::clamp(stepCount, 1.0f, static_cast<float>(MaxSurfaceSteps))) : MaxSurfaceSteps;
		std::optional<float> above;
		for (uint32_t index = 0; index <= steps; ++index)
		{
			const float distance = index == steps ? leave : enter + span * (static_cast<float>(index) / static_cast<float>(steps));
			const std::optional<float> depth = GetDepthBelowSurface(heightfield, extent, origin + direction * distance);
			if (!depth.has_value())
				continue;
			if (*depth < 0.0f)
			{
				above = distance;
				continue;
			}
			// The first point at or below the surface: a ray that starts below it finds no crossing from above.
			if (index == 0 && enter == 0.0f)
				return std::nullopt;
			float below = distance;
			if (above.has_value())
			{
				float top = *above;
				for (uint32_t refinement = 0; refinement < SurfaceRefinements; ++refinement)
				{
					const float middle = 0.5f * (top + below);
					const std::optional<float> middleDepth = GetDepthBelowSurface(heightfield, extent, origin + direction * middle);
					if (middleDepth.has_value() && *middleDepth >= 0.0f)
						below = middle;
					else
						top = middle;
				}
			}
			const glm::vec3 local = origin + direction * below;
			const float height = SampleTerrainHeight(heightfield, extent, glm::vec2(local.x, local.z)).value_or(local.y);
			return glm::vec3(world * glm::vec4(local.x, height, local.z, 1.0f));
		}
		return std::nullopt;
	}

	Json MakeTerrainEditParams(UUID entity, const TerrainBrushSettings& settings, std::span<const glm::vec3> points)
	{
		Json list = Json::array();
		for (const glm::vec3& point : points)
			list.push_back(Json::array({ point.x, point.y, point.z }));
		return Json{ { "entity", entity.ToString() }, { "tool", GetToolName(settings.Tool) }, { "points", std::move(list) }, { "radius", settings.Radius },
			{ "strength", settings.Strength }, { "height", settings.Height }, { "layer", settings.Layer } };
	}

	void TerrainBrush::DrawControls(EditorPanelContext& context, float toolbarRight)
	{
		m_Target.reset();
		m_Enabled = false;
		EditorContext& editor = context.Editor;
		// The edit scene only, while the scene view shows it: a play scene's terrain is never edited.
		const Scene* scene = !editor.ShowsPlayScene() && editor.HasScene() ? &editor.GetScene() : nullptr;
		const std::span<const UUID> selection = editor.GetSelection();
		ConstEntity entity;
		if (scene != nullptr && editor.GetSelectionTarget() == SceneTarget::Edit && selection.size() == 1)
			entity = scene->FindEntityByID(selection.front());
		const TerrainComponent* terrain = entity.IsValid() ? entity.TryGetComponent<TerrainComponent>() : nullptr;
		if (terrain == nullptr || !terrain->Heightfield.GetHandle().IsValid())
		{
			m_PickedFor = UUID();
			m_Active = false;
			m_Heightfield.reset();
			return;
		}
		if (m_PickedFor != entity.GetUUID())
		{
			m_PickedFor = entity.GetUUID();
			m_Active = false;
		}
		const AssetHandle handle = terrain->Heightfield.GetHandle();
		EditorAssetManager& assets = editor.GetAssets();
		const float scale = Utils::EditorUiScale();
		const float pickerWidth = 120.0f * scale;
		PlaceToolbarItem(toolbarRight, pickerWidth);
		const uint64_t version = assets.GetVersion(handle);
		const Result<AssetRef<Asset>> loaded = assets.Load(handle);
		const AssetRef<HeightfieldData> heightfield = loaded ? AssetCast<HeightfieldData>(*loaded) : AssetRef<HeightfieldData>();
		if (!heightfield)
		{
			// The terrain draws nothing for it either (TERRAIN_NO_HEIGHTFIELD); the asset's own diagnostics say why.
			ImGui::TextDisabled("Terrain unavailable");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", loaded ? "The Terrain's Heightfield is not a heightfield" : loaded.error().GetMessageText().c_str());
			m_Active = false;
			m_Heightfield.reset();
			return;
		}
		// A copy of the samples, taken again when the handle or its version changes (an edit, a reimport).
		if (!m_Heightfield.has_value() || m_HeightfieldHandle != handle || m_HeightfieldVersion != version)
		{
			m_Heightfield = *heightfield;
			m_HeightfieldHandle = handle;
			m_HeightfieldVersion = version;
		}
		m_Target = Target{ .Entity = entity.GetUUID(),
			.Heightfield = handle,
			.World = TransformSystem::ComputeWorldMatrix(entity),
			.Extent = TerrainExtent{ .Size = terrain->Size, .HeightScale = terrain->HeightScale } };

		const bool disabled = editor.IsReadOnly() || context.Gizmos.IsDragging() || editor.GetPlay().IsPlaying();
		ImGui::BeginDisabled(disabled);
		ImGui::SetNextItemWidth(pickerWidth);
		const std::string preview = m_Active ? std::format("Terrain: {}", GetToolName(m_Settings.Tool)) : std::string("Terrain: off");
		if (ImGui::BeginCombo("##TerrainBrushTool", preview.c_str()))
		{
			if (ImGui::Selectable("Off###TerrainBrushOff", !m_Active))
				m_Active = false;
			if (!m_Active)
				ImGui::SetItemDefaultFocus();
			for (const auto& [tool, name] : BrushTools)
			{
				const bool picked = m_Active && m_Settings.Tool == tool;
				const std::string label = std::format("{}###TerrainBrush{}", name, name);
				if (ImGui::Selectable(label.c_str(), picked))
				{
					m_Settings.Tool = tool;
					m_Active = true;
				}
				if (picked)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Terrain tool: the left button sculpts or paints the terrain under the cursor, each stroke one undoable terrain.edit");
		PlaceToolbarItem(toolbarRight, ImGui::CalcTextSize("Brush").x + ImGui::GetStyle().FramePadding.x * 2.0f);
		if (Utils::EditorToolbarButton("Brush###TerrainBrushSettings", "The terrain brush's radius, strength, Flatten's height and Paint's layer"))
			ImGui::OpenPopup("TerrainBrushSettings");
		if (ImGui::BeginPopup("TerrainBrushSettings"))
		{
			ImGui::SetNextItemWidth(160.0f * scale);
			ImGui::DragFloat("Radius (m)", &m_Settings.Radius, 0.05f, 0.05f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(160.0f * scale);
			ImGui::SliderFloat("Strength", &m_Settings.Strength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(160.0f * scale);
			ImGui::SliderFloat("Flatten height (m)", &m_Settings.Height, 0.0f, m_Target->Extent.HeightScale, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			int layer = static_cast<int>(m_Settings.Layer);
			ImGui::SetNextItemWidth(160.0f * scale);
			if (ImGui::SliderInt("Paint layer", &layer, 0, static_cast<int>(MaxTerrainLayers) - 1, "%d", ImGuiSliderFlags_AlwaysClamp))
				m_Settings.Layer = static_cast<uint32_t>(std::clamp(layer, 0, static_cast<int>(MaxTerrainLayers) - 1));
			ImGui::EndPopup();
		}
		ImGui::EndDisabled();
		m_Enabled = !disabled;
	}

	bool TerrainBrush::IsArmed() const
	{
		return m_Target.has_value() && m_Active;
	}

	Result<std::optional<glm::vec3>> TerrainBrush::FindCursorPoint(const EditorViewportRect& rectangle, const EditorViewportImage& image) const
	{
		const ImVec2 pointer = ImGui::GetIO().MousePos;
		const std::optional<EditorViewportPixel> pixel = ToViewportPixel(rectangle, { pointer.x, pointer.y });
		if (!pixel || image.Width == 0 || image.Height == 0 || !m_Target.has_value() || !m_Heightfield.has_value())
			return std::optional<glm::vec3>();
		// The scene view's editor camera has no PixelPerfectCamera output: the identity mapping, as the view's drop uses.
		const RenderOutputMapping mapping = ComputeRenderOutputMapping(image.Width, image.Height, RenderOutputSettings{});
		ENGINE_TRY_ASSIGN(const std::optional<RenderRayInterval> ray, ComputeOutputPixelRayInterval(image.Camera, mapping, pixel->X, pixel->Y));
		if (!ray)
			return std::optional<glm::vec3>();
		return IntersectTerrainSurface(ray->Ray, m_Target->World, *m_Heightfield, m_Target->Extent);
	}

	Status TerrainBrush::ExtendStroke(const EditorViewportRect& rectangle, const EditorViewportImage& image)
	{
		ENGINE_TRY_ASSIGN(const std::optional<glm::vec3> point, FindCursorPoint(rectangle, image));
		if (!point)
			return {};
		Stroke& stroke = *m_Stroke;
		const glm::vec3 local(glm::affineInverse(m_Target->World) * glm::vec4(*point, 1.0f));
		const glm::vec2 localXZ(local.x, local.z);
		if (stroke.LastLocal.has_value())
		{
			const glm::vec2 moved = localXZ - *stroke.LastLocal;
			if (glm::dot(moved, moved) < 0.0625f * stroke.Settings.Radius * stroke.Settings.Radius)
				return {};
		}
		if (stroke.Points.size() == MaxTerrainBrushPoints)
			return MakeError(ErrorCode::InvalidArgument, "The terrain brush stroke holds more than {} points; sculpt or paint in shorter strokes", MaxTerrainBrushPoints);
		stroke.Points.push_back(*point);
		stroke.LastLocal = localXZ;
		return {};
	}

	void TerrainBrush::DrawCircle(const EditorViewportRect& rectangle, const EditorViewportImage& image, const glm::vec3& centre) const
	{
		if (image.Width == 0 || image.Height == 0 || rectangle.Size.x <= 0.0f || rectangle.Size.y <= 0.0f)
			return;
		const glm::mat4 viewProjection = image.Camera.Projection * image.Camera.View;
		const glm::vec3 local(glm::affineInverse(m_Target->World) * glm::vec4(centre, 1.0f));
		std::array<ImVec2, CircleSegments> outline{};
		for (uint32_t segment = 0; segment < CircleSegments; ++segment)
		{
			const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(segment) / static_cast<float>(CircleSegments);
			const glm::vec2 around(local.x + m_Settings.Radius * std::cos(angle), local.z + m_Settings.Radius * std::sin(angle));
			const float height = SampleTerrainHeight(*m_Heightfield, m_Target->Extent, around).value_or(local.y);
			const glm::vec4 clip = viewProjection * m_Target->World * glm::vec4(around.x, height, around.y, 1.0f);
			if (!(clip.w > 0.0f))
				return;
			const float x = (clip.x / clip.w * 0.5f + 0.5f) * rectangle.Size.x;
			const float y = (0.5f - clip.y / clip.w * 0.5f) * rectangle.Size.y;
			outline[segment] = ImVec2(rectangle.Min.x + x, rectangle.Min.y + y);
		}
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(ImVec2(rectangle.Min.x, rectangle.Min.y), ImVec2(rectangle.Min.x + rectangle.Size.x, rectangle.Min.y + rectangle.Size.y), true);
		drawList->AddPolyline(outline.data(), static_cast<int>(outline.size()), IM_COL32(255, 220, 90, 230), ImDrawFlags_Closed, 1.5f);
		drawList->PopClipRect();
	}

	Result<std::optional<Json>> TerrainBrush::Update(const EditorViewportRect& rectangle, const EditorViewportImage& image, const PointerInput& input)
	{
		if (m_Stroke)
		{
			const bool sameTarget = m_Target && m_Active && m_Enabled && m_Target->Entity == m_Stroke->Entity && m_Target->Heightfield == m_Stroke->Heightfield;
			if (!sameTarget || !input.ImageShown)
			{
				m_Stroke.reset();
				return MakeError(ErrorCode::Conflict, "The terrain brush stroke was cancelled: its terrain, the selection, play or the scene view changed");
			}
			const bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
			// The cursor's point of this frame, the release frame's included.
			if (const Status extended = ExtendStroke(rectangle, image); !extended)
			{
				m_Stroke.reset();
				return std::unexpected(extended.error());
			}
			if (held)
			{
				if (const auto point = FindCursorPoint(rectangle, image); point && point->has_value())
					DrawCircle(rectangle, image, **point);
				return std::optional<Json>();
			}
			const Stroke stroke = std::move(*m_Stroke);
			m_Stroke.reset();
			if (stroke.Points.empty())
				return std::optional<Json>();
			return std::optional<Json>(MakeTerrainEditParams(stroke.Entity, stroke.Settings, stroke.Points));
		}
		if (!IsArmed() || !m_Enabled || !input.ImageShown || input.Captured)
			return std::optional<Json>();
		// The circle under the hovered cursor; a ray the image's camera cannot make (a stale image) shows none.
		if (rectangle.Hovered)
		{
			if (const Result<std::optional<glm::vec3>> hovered = FindCursorPoint(rectangle, image); hovered && hovered->has_value())
				DrawCircle(rectangle, image, **hovered);
		}
		// Alt with the left button stays the camera's (orbit).
		if (!input.LeftPressed || ImGui::GetIO().KeyAlt)
			return std::optional<Json>();
		m_Stroke = Stroke{ .Entity = m_Target->Entity, .Heightfield = m_Target->Heightfield, .Settings = m_Settings, .Points = {}, .LastLocal = std::nullopt };
		if (const Status extended = ExtendStroke(rectangle, image); !extended)
		{
			m_Stroke.reset();
			return std::unexpected(extended.error());
		}
		return std::optional<Json>();
	}

}
