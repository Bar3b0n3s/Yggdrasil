#pragma once

#include "Engine/Asset/AssetType.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace Engine {

	// The UI's asset and folder glyphs (content browser, folder picker, hierarchy prefab marker, launcher).
	enum class EditorIcon : uint8_t
	{
		Folder,
		Scene,
		Prefab,
		Mesh,
		Material,
		Texture,
		Environment,
		Audio,
		Script,
		Font,
		Replay,
		SpriteSheet,      // M18: a texture cut into a grid of frames (Docs/Decisions/0029-m18-contract.md decision 18)
		Tilemap,          // M21: a grid with a staircase of painted cells (Docs/Decisions/0032-m21-contract.md decision 23)
		StringTable,      // M22: a page of lines beside a speech bubble (Docs/Decisions/0033-m22-contract.md decision 10)
		Skeleton,         // M23: a chain of joints from a root (Docs/Decisions/0034-m23-contract.md decision 23)
		AnimationClip,    // M23: a keyed curve on a timeline (Docs/Decisions/0034-m23-contract.md decision 23)
		Graph,            // M24: two nodes joined by a link, a Script asset of the Graph importer (Docs/Decisions/0035-m24-contract.md decision 15)
		IrradianceVolume, // M26: a box of probe spheres (Docs/Decisions/0037-m26-contract.md decision 3)
		Lut,              // M26: a cube of graded colour cells (Docs/Decisions/0037-m26-contract.md decision 3)
		Heightfield       // M26: a ridge of hills over a grid (Docs/Decisions/0037-m26-contract.md decision 3)
	};

	// First-party vector glyphs drawn with ImDrawList lines, no downloaded assets or icon licence. Main thread, inside an
	// ImGui frame; all arguments borrowed for this call. The scene view's world icons (cameras, lights, audio sources,
	// triggers) are not drawn here: with RenderViewFlags::Icons, Scene/RenderAnnotations collects them and
	// Renderer/EditorOverlay draws them, the scene view's single icon producer (ADR 0018).
	// Draws nothing for a non-finite or nonpositive size or position.
	void DrawEditorIcon(ImDrawList& drawList, EditorIcon icon, const glm::vec2& minimum, float size, uint32_t color);
	[[nodiscard]] EditorIcon AssetTypeToEditorIcon(AssetType type);
	// The glyph of an asset of `type` that the importer `importer` imports: AssetTypeToEditorIcon's, but Graph for a Script asset
	// of the Graph importer (a .graph source, M24: graphs are Script assets, Docs/Decisions/0035-m24-contract.md decision 2).
	[[nodiscard]] EditorIcon AssetToEditorIcon(AssetType type, std::string_view importer);

}
