#include "EnginePCH.h"
#include "Engine/Asset/FontData.h"

// M6 contract stub (Roadmap rule 3): stream B (texture, material and font importers) implements the font payload.

namespace Engine {

	const FontGlyph* FindGlyph(const FontData& /*font*/, uint32_t /*codepoint*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Status ValidateFontData(const FontData& /*font*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValidateFontData is an M6 contract stub");
	}

	Buffer SerializeFontPayload(const FontData& /*font*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<FontData> DeserializeFontPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "DeserializeFontPayload is an M6 contract stub");
	}

	Buffer CookFont(const FontData& /*font*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<FontData>> LoadCookedFont(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "LoadCookedFont is an M6 contract stub");
	}

}
