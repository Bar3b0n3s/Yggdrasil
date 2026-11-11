#include "EnginePCH.h"
#include "Engine/Asset/HeightfieldData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"

#include <utility>

// The heightfield's formats (HeightfieldData.h). The artifact wrappers and the flat heightfield are the contract's; the
// payload, the validation and the source reader and writer are contract stubs of M26's C26.1, implemented by stream T-A
// (Docs/Decisions/0037-m26-contract.md decision 20): until then no heightfield serializes or reads.

namespace Engine {

	HeightfieldData MakeFlatHeightfield(uint32_t resolution, uint16_t height)
	{
		ENGINE_CORE_ASSERT(resolution >= MinHeightfieldResolution && resolution <= MaxHeightfieldResolution, "A heightfield of {} samples a side",
			resolution);
		HeightfieldData heightfield;
		heightfield.Resolution = resolution;
		heightfield.Heights.assign(static_cast<size_t>(resolution) * resolution, height);
		return heightfield;
	}

	Status ValidateHeightfieldData(const HeightfieldData& /*heightfield*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 20, stream T-A)");
	}

	Buffer SerializeHeightfieldPayload(const HeightfieldData& /*heightfield*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<HeightfieldData> DeserializeHeightfieldPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 20, stream T-A)");
	}

	Buffer WriteHeightfieldSource(const HeightfieldData& /*heightfield*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<HeightfieldData> ReadHeightfieldSource(std::span<const std::byte> /*source*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 terrain contract (Docs/Decisions/0037-m26-contract.md decision 20, stream T-A)");
	}

	Buffer CookHeightfield(const HeightfieldData& heightfield, uint32_t importerVersion)
	{
		const Buffer payload = SerializeHeightfieldPayload(heightfield);
		return WriteCookedArtifact(AssetType::Heightfield, HeightfieldData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<HeightfieldData>> LoadCookedHeightfield(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::Heightfield, HeightfieldData::FormatVersion));
		ENGINE_TRY_ASSIGN(HeightfieldData heightfield, DeserializeHeightfieldPayload(artifact.Payload));
		return AssetRef<HeightfieldData>(CreateRef<HeightfieldData>(std::move(heightfield)));
	}

}
