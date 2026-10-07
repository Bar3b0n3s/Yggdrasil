#pragma once

#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <string_view>

// The procedural built-in textures (Architecture §7.1: engine://Textures/{White, Black, FlatNormal, Checker, Missing}).
// The renderer binds White, FlatNormal and Black for empty material slots (§8.4); Missing is the texture placeholder (§7.2).

namespace Engine {

	enum class BuiltinTexture : uint8_t
	{
		White,      // 4x4 RGBA8Srgb (255, 255, 255, 255)
		Black,      // 4x4 RGBA8Srgb (0, 0, 0, 255)
		FlatNormal, // 4x4 RGBA8Unorm (128, 128, 255, 255): the tangent-space normal (0, 0, 1)
		Checker,    // 64x64 RGBA8Srgb, 8x8-texel squares of light and dark grey, full mip chain
		Missing     // 64x64 RGBA8Srgb, 8x8-texel squares of magenta and black, full mip chain: obvious on any surface
	};

	// "White", "Black", ...; the name in engine://Textures/<Name>. "Unknown" for a value outside the enumeration (a pure
	// name lookup, like FieldTypeToString). Pure and thread-safe.
	[[nodiscard]] std::string_view BuiltinTextureToString(BuiltinTexture texture);

	// Generates `texture` deterministically (integer arithmetic only; mips by exact 2x2 box filtering of the sRGB-decoded
	// values through a fixed table, so the bytes never depend on the C runtime). The result passes ValidateTextureData.
	[[nodiscard]] TextureData GenerateBuiltinTexture(BuiltinTexture texture);

}
