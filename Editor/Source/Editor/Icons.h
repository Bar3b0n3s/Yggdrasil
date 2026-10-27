#pragma once

#include "Engine/Asset/AssetType.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <cstdint>

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
		SpriteSheet // M18: a texture cut into a grid of frames (Docs/Decisions/0029-m18-contract.md decision 18)
	};

	// First-party vector glyphs drawn with ImDrawList lines, no downloaded assets or icon licence. Main thread, inside an
	// ImGui frame; all arguments borrowed for this call. The scene view's world icons (cameras, lights, audio sources,
	// triggers) are not drawn here: with RenderViewFlags::Icons, Scene/RenderAnnotations collects them and
	// Renderer/EditorOverlay draws them, the scene view's single icon producer (ADR 0018).
	// Draws nothing for a non-finite or nonpositive size or position.
	void DrawEditorIcon(ImDrawList& drawList, EditorIcon icon, const glm::vec2& minimum, float size, uint32_t color);
	[[nodiscard]] EditorIcon AssetTypeToEditorIcon(AssetType type);

}
