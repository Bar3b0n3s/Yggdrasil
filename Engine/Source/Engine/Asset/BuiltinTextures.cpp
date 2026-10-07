#include "EnginePCH.h"
#include "Engine/Asset/BuiltinTextures.h"

// M6 contract stub (Roadmap rule 3): stream F (built-ins, GPU cache, shipped assets) generates the built-in textures.

namespace Engine {

	std::string_view BuiltinTextureToString(BuiltinTexture texture)
	{
		switch (texture)
		{
			case BuiltinTexture::White:      return "White";
			case BuiltinTexture::Black:      return "Black";
			case BuiltinTexture::FlatNormal: return "FlatNormal";
			case BuiltinTexture::Checker:    return "Checker";
			case BuiltinTexture::Missing:    return "Missing";
		}
		return "Unknown";
	}

	TextureData GenerateBuiltinTexture(BuiltinTexture /*texture*/)
	{
		ENGINE_CONTRACT_STUB();
		return TextureData();
	}

}
