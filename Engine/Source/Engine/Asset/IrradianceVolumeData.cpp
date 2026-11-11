#include "EnginePCH.h"
#include "Engine/Asset/IrradianceVolumeData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"

#include <utility>

// The irradiance volume's formats (IrradianceVolumeData.h). The artifact wrappers and the probe index are the contract's;
// the payload, the validation and the source reader and writer are contract stubs of M26's C26.1, implemented by stream Q-A
// (Docs/Decisions/0037-m26-contract.md decision 8): until then no volume serializes, reads or validates.

namespace Engine {

	size_t GetIrradianceProbeIndex(const IrradianceVolumeData& volume, uint32_t x, uint32_t y, uint32_t z)
	{
		ENGINE_CORE_ASSERT(x < volume.CountX && y < volume.CountY && z < volume.CountZ, "Probe ({}, {}, {}) of a {} x {} x {} volume", x, y, z,
			volume.CountX, volume.CountY, volume.CountZ);
		return static_cast<size_t>(x) + static_cast<size_t>(volume.CountX) * (static_cast<size_t>(y) + static_cast<size_t>(volume.CountY) * z);
	}

	Status ValidateIrradianceVolumeData(const IrradianceVolumeData& /*volume*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 probes contract (Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A)");
	}

	Buffer SerializeIrradianceVolumePayload(const IrradianceVolumeData& /*volume*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<IrradianceVolumeData> DeserializeIrradianceVolumePayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 probes contract (Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A)");
	}

	Buffer WriteIrradianceVolumeSource(const IrradianceVolumeData& /*volume*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<IrradianceVolumeData> ReadIrradianceVolumeSource(std::span<const std::byte> /*source*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 probes contract (Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A)");
	}

	Buffer CookIrradianceVolume(const IrradianceVolumeData& volume, uint32_t importerVersion)
	{
		const Buffer payload = SerializeIrradianceVolumePayload(volume);
		return WriteCookedArtifact(AssetType::IrradianceVolume, IrradianceVolumeData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<IrradianceVolumeData>> LoadCookedIrradianceVolume(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::IrradianceVolume, IrradianceVolumeData::FormatVersion));
		ENGINE_TRY_ASSIGN(IrradianceVolumeData volume, DeserializeIrradianceVolumePayload(artifact.Payload));
		return AssetRef<IrradianceVolumeData>(CreateRef<IrradianceVolumeData>(std::move(volume)));
	}

}
