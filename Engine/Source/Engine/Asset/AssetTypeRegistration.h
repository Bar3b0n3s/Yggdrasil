#pragma once

#include "Engine/Core/Base.h"

namespace Engine {

	class TypeRegistry;

	// Registers the Asset module's reflected types (Architecture §5.4): the enum "AlphaMode" and the struct "Material"
	// (MaterialData.h, RegisterMaterialTypes), then M18's sprite sheet structs "SpriteRect", "SpriteBorders", "SpriteFrame",
	// "SpriteClipFrame", "SpriteClip" and "SpriteSheet" (SpriteSheetData.h, RegisterSpriteSheetTypes). Every EngineContext
	// calls it in its Services step, after the built-in components and the project settings types, so materials and sprite
	// sheets read and write in the editor and in exported games alike; the registry suite's registry
	// (Test::CreateBuiltinRegistry) includes it, so every Asset type gets the §5.4 suite.
	void RegisterAssetTypes(TypeRegistry& registry);

}
