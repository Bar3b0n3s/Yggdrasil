#include "EditorPCH.h"
#include "Editor/Viewport/TilemapBrush.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/Automation/TilemapMethods.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Viewport/EditorViewportState.h"
#include "EditorCore/Viewport/GizmoController.h"
#include "EditorCore/Viewport/TilemapPainting.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <format>
#include <span>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		// "<k>: <sheet> #<frame>" for the palette entry `tile` of cell value `value`, the sheet named by its file's stem.
		std::string DescribeTilemapEntry(const EditorAssetManager& assets, const TilemapTile& tile, uint32_t value)
		{
			std::string sheet = "no sheet";
			if (const AssetHandle handle = tile.Sheet.GetHandle(); handle.IsValid())
			{
				const std::string path = assets.GetReferencePath(handle);
				std::string_view name = path;
				if (const size_t slash = name.find_last_of('/'); slash != std::string_view::npos)
					name.remove_prefix(slash + 1);
				if (const size_t dot = name.find_last_of('.'); dot != std::string_view::npos && dot != 0)
					name = name.substr(0, dot);
				sheet = name.empty() ? handle.ToString() : std::string(name);
			}
			return std::format("{}: {} #{}{}", value, sheet, tile.Frame, tile.Solid ? "" : " (not solid)");
		}

		// Continues the toolbar's row when an item of `width` still fits before `toolbarRight` (SceneViewportPanel's rule).
		void PlaceToolbarItem(float toolbarRight, float width)
		{
			if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= toolbarRight)
				ImGui::SameLine();
		}

	}

	void TilemapBrush::DrawControls(EditorPanelContext& context, const EditorViewportImage& image, float toolbarRight)
	{
		m_Target.reset();
		m_Enabled = false;
		EditorContext& editor = context.Editor;
		// The displayed scene, as the view's drop and frame actions take it: the play scene only while its tab is shown.
		PlaySession* session = editor.ShowsPlayScene() ? editor.GetPlay().GetSession() : nullptr;
		const Scene* scene = session != nullptr ? &session->GetScene() : editor.HasScene() ? &editor.GetScene()
																						   : nullptr;
		const SceneTarget shown = session != nullptr ? SceneTarget::Play : SceneTarget::Edit;
		const std::span<const UUID> selection = editor.GetSelection();
		ConstEntity entity;
		if (scene != nullptr && editor.GetSelectionTarget() == shown && selection.size() == 1)
			entity = scene->FindEntityByID(selection.front());
		const TilemapRendererComponent* renderer = entity.IsValid() ? entity.TryGetComponent<TilemapRendererComponent>() : nullptr;
		if (renderer == nullptr || !renderer->Tilemap.GetHandle().IsValid())
		{
			m_PickedFor = UUID();
			m_Tile = 0;
			m_Erase = false;
			return;
		}
		if (m_PickedFor != entity.GetUUID())
		{
			m_PickedFor = entity.GetUUID();
			m_Tile = 0;
			m_Erase = false;
		}
		const AssetHandle handle = renderer->Tilemap.GetHandle();
		EditorAssetManager& assets = editor.GetAssets();
		const Result<AssetRef<Asset>> loaded = assets.Load(handle);
		const AssetRef<TilemapData> tilemap = loaded ? AssetCast<TilemapData>(*loaded) : AssetRef<TilemapData>();
		const float scale = Utils::EditorUiScale();
		const float pickerWidth = 160.0f * scale;
		PlaceToolbarItem(toolbarRight, pickerWidth);
		if (!tilemap)
		{
			// The renderer draws nothing for it either; the asset's own diagnostics say why.
			ImGui::TextDisabled("Tilemap unavailable");
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", loaded ? "The TilemapRenderer's asset is not a tilemap" : loaded.error().GetMessageText().c_str());
			m_Tile = 0;
			m_Erase = false;
			return;
		}
		const std::vector<TilemapTile>& palette = tilemap->Palette;
		if (m_Tile > palette.size())
			m_Tile = 0;
		// The pose the displayed image shows: the entity's rendered world matrix at the image's interpolation alpha, as the click
		// selection picks tiles among the image's own tilemaps, so a moving tilemap of a play scene is painted where it is seen.
		m_Target = Target{ .Entity = entity.GetUUID(),
			.Tilemap = handle,
			.PlayScene = session != nullptr,
			.World = ComputeRenderedWorldMatrix(entity, image.Alpha),
			.CellSize = tilemap->CellSize,
			.PaletteSize = static_cast<uint32_t>(palette.size()) };

		const bool disabled = editor.IsReadOnly() || context.Gizmos.IsDragging();
		ImGui::BeginDisabled(disabled);
		ImGui::SetNextItemWidth(pickerWidth);
		const std::string preview = m_Tile == 0 ? std::string("Tile: off") : "Tile " + DescribeTilemapEntry(assets, palette[m_Tile - 1], m_Tile);
		if (ImGui::BeginCombo("##TilemapBrushTile", preview.c_str()))
		{
			if (ImGui::Selectable("Off###TilemapBrushOff", m_Tile == 0))
				m_Tile = 0;
			if (m_Tile == 0)
				ImGui::SetItemDefaultFocus();
			// A palette may hold up to MaxTilemapPaletteEntries entries: only the visible rows are described.
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(palette.size()));
			if (m_Tile != 0)
				clipper.IncludeItemByIndex(static_cast<int>(m_Tile - 1));
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				{
					const uint32_t value = static_cast<uint32_t>(row) + 1;
					const std::string label = std::format("{}###TilemapBrushEntry{}", DescribeTilemapEntry(assets, palette[static_cast<size_t>(row)], value), value);
					if (ImGui::Selectable(label.c_str(), m_Tile == value))
						m_Tile = value;
					if (m_Tile == value)
						ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Tile brush: the left button paints this palette entry into the cells it crosses and the right button erases "
							  "them, each stroke one undoable tilemap.paint");
		const char* eraseLabel = "Erase###TilemapBrushErase";
		PlaceToolbarItem(toolbarRight, ImGui::CalcTextSize("Erase").x + ImGui::GetStyle().FramePadding.x * 2.0f);
		if (Utils::EditorToolbarButton(eraseLabel, "Erase with the left button too (the right button always erases)", m_Erase))
			m_Erase = !m_Erase;
		ImGui::EndDisabled();
		m_Enabled = !disabled;
	}

	bool TilemapBrush::IsArmed() const
	{
		return m_Target.has_value() && (m_Tile != 0 || m_Erase);
	}

	Status TilemapBrush::ExtendStroke(const EditorViewportRect& rectangle, const EditorViewportImage& image)
	{
		const ImVec2 pointer = ImGui::GetIO().MousePos;
		const std::optional<EditorViewportPixel> pixel = ToViewportPixel(rectangle, { pointer.x, pointer.y });
		if (!pixel || image.Width == 0 || image.Height == 0)
			return {};
		// The scene view's editor camera has no PixelPerfectCamera output: the identity mapping, as the view's drop uses.
		const RenderOutputMapping mapping = ComputeRenderOutputMapping(image.Width, image.Height, RenderOutputSettings{});
		ENGINE_TRY_ASSIGN(const std::optional<RenderRayInterval> ray, ComputeOutputPixelRayInterval(image.Camera, mapping, pixel->X, pixel->Y));
		if (!ray)
			return {};
		const std::optional<glm::ivec2> cell = IntersectTilemapCell(ray->Ray, m_Target->World, m_Target->CellSize);
		if (!cell)
			return {};
		Stroke& stroke = *m_Stroke;
		const std::vector<glm::ivec2> crossed = stroke.Last ? GetTilemapStrokeCells(*stroke.Last, *cell) : std::vector<glm::ivec2>{ *cell };
		stroke.Last = *cell;
		for (const glm::ivec2 next : crossed)
		{
			if (!stroke.Seen.emplace(next.x, next.y).second)
				continue;
			if (stroke.Seen.size() > MaxTilemapPaintCells)
				return MakeError(ErrorCode::InvalidArgument, "The tile brush stroke crosses more than {} cells; paint it in shorter strokes", MaxTilemapPaintCells);
			stroke.Cells.push_back(next);
		}
		return {};
	}

	Result<std::optional<Json>> TilemapBrush::Update(const EditorViewportRect& rectangle, const EditorViewportImage& image, const PointerInput& input)
	{
		if (m_Stroke)
		{
			const bool sameTarget = m_Target && m_Target->Entity == m_Stroke->Entity && m_Target->Tilemap == m_Stroke->Tilemap
				&& m_Target->PlayScene == m_Stroke->PlayScene;
			if (!sameTarget || !input.ImageShown)
			{
				m_Stroke.reset();
				return MakeError(ErrorCode::Conflict, "The tile brush stroke was cancelled: its tilemap, the selection or the scene view changed");
			}
			const bool held = ImGui::IsMouseDown(m_Stroke->RightButton ? ImGuiMouseButton_Right : ImGuiMouseButton_Left);
			// The cursor's cell of this frame, the release frame's included.
			if (const Status extended = ExtendStroke(rectangle, image); !extended)
			{
				m_Stroke.reset();
				return std::unexpected(extended.error());
			}
			if (held)
				return std::optional<Json>();
			const Stroke stroke = std::move(*m_Stroke);
			m_Stroke.reset();
			if (stroke.Cells.empty())
				return std::optional<Json>();
			ENGINE_TRY_ASSIGN(Json params, MakeTilemapPaintParams({ .Tilemap = stroke.Tilemap, .Cells = stroke.Cells, .Tile = stroke.Tile }));
			return std::optional<Json>(std::move(params));
		}
		if (!IsArmed() || !m_Enabled || !input.ImageShown || input.Captured)
			return std::optional<Json>();
		// Alt with the left button stays the camera's (orbit, or pan in the 2D mode).
		const bool left = input.LeftPressed && !ImGui::GetIO().KeyAlt;
		if (!left && !input.RightPressed)
			return std::optional<Json>();
		m_Stroke = Stroke{ .Entity = m_Target->Entity,
			.Tilemap = m_Target->Tilemap,
			.PlayScene = m_Target->PlayScene,
			.RightButton = !left,
			.Tile = left && !m_Erase ? m_Tile : 0 };
		if (const Status extended = ExtendStroke(rectangle, image); !extended)
		{
			m_Stroke.reset();
			return std::unexpected(extended.error());
		}
		return std::optional<Json>();
	}

}
