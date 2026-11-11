#include "EnginePCH.h"
#include "Engine/Asset/LutData.h"

#include "Engine/Asset/CookedFormat.h"

#include <utility>

// The LUT's formats (LutData.h). The artifact wrappers and the built-in LUTs' names are the contract's; the payload, the
// validation, the `.cube` parser and the built-in LUTs' generation are contract stubs of M26's C26.1, implemented by stream
// X-B (Docs/Decisions/0037-m26-contract.md decision 15): until then no LUT serializes, parses or reads.

namespace Engine {

	Status ValidateLutData(const LutData& /*lut*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 post-processing contract (Docs/Decisions/0037-m26-contract.md decision 15, stream X-B)");
	}

	Buffer SerializeLutPayload(const LutData& /*lut*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<LutData> DeserializeLutPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 post-processing contract (Docs/Decisions/0037-m26-contract.md decision 15, stream X-B)");
	}

	Result<LutData> ParseCubeLut(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 post-processing contract (Docs/Decisions/0037-m26-contract.md decision 15, stream X-B)");
	}

	Buffer CookLut(const LutData& lut, uint32_t importerVersion)
	{
		const Buffer payload = SerializeLutPayload(lut);
		return WriteCookedArtifact(AssetType::Lut, LutData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<LutData>> LoadCookedLut(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::Lut, LutData::FormatVersion));
		ENGINE_TRY_ASSIGN(LutData lut, DeserializeLutPayload(artifact.Payload));
		return AssetRef<LutData>(CreateRef<LutData>(std::move(lut)));
	}

	std::string_view BuiltinLutToString(BuiltinLut lut)
	{
		switch (lut)
		{
			case BuiltinLut::Identity: return "Identity";
		}
		return "Unknown";
	}

	LutData GenerateBuiltinLut(BuiltinLut /*lut*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
