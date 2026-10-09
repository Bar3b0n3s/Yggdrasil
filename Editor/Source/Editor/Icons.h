#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/DebugDrawList.h"

#include <imgui.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace Engine {

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
		Camera,
		DirectionalLight,
		PointLight,
		SpotLight,
		Trigger
	};

	// First-party vector glyphs, no downloaded assets. Main thread. All arguments borrowed for this call.
	// UI glyphs use ImDrawList; viewport glyphs append world-space DebugDrawList primitives, never an Engine->Editor edge.
	// M9 screenshot annotation glyphs are independently owned by Renderer; no Engine call reaches these functions.
	void DrawEditorIcon(ImDrawList& drawList, EditorIcon icon, const glm::vec2& minimum, float size, uint32_t color);
	void AppendEditorIcon(DebugDrawList& drawList, EditorIcon icon, const glm::mat4& world, float size, const glm::vec4& color);
	[[nodiscard]] EditorIcon AssetTypeToEditorIcon(AssetType type);

}
