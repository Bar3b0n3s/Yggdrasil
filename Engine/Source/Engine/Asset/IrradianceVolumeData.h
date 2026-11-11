#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Irradiance volumes (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decisions 3, 8 and 9): a baked grid of
// irradiance probes as an asset (AssetType::IrradianceVolume, format version 1). `lighting.bake` (EditorCore/Lighting/
// LightingBake.h) renders each probe's surroundings and writes the volume's source file, `<name>.irradiance`, committed with
// the scene it was baked for; IrradianceVolumeImporter (version 1) cooks it, and an IrradianceVolume component's Data
// references it, so the renderer adds its indirect diffuse term to M23's light loop (Common/Probes.slang). Data only:
//   - The grid: CountX x CountY x CountZ probes (each 2 to MaxIrradianceProbesPerAxis) spanning the volume's box of Size
//     metres centred on the volume's origin, in its local axes: probe (x, y, z) sits at
//     -Size / 2 + Size * (x / (CountX - 1), y / (CountY - 1), z / (CountZ - 1)), so the grid's corner probes are the box's
//     corners. Probes are stored x fastest, then y, then z: probe (x, y, z) is Probes[x + CountX * (y + CountY * z)].
//   - A probe is the SH9 projection of the irradiance it receives, in EnvironmentData's convention (EnvironmentData.h:
//     the coefficients of E(n) / pi, the cosine-convolved irradiance divided by pi, over the real SH basis in the order
//     l = 0, then l = 1 (m = -1, 0, 1), then l = 2 (m = -2 ... 2), with EnvironmentData::IrradianceShWindow's Hanning window
//     applied), in world-space directions and linear radiance units, so evaluating it at a normal gives the radiance a
//     white Lambertian surface reflects there: a furnace of uniform radiance L gives L in every direction.
//   - Size is the box the bake used (the component's Size at bake time); the renderer places the grid by the component's
//     current transform and Size, so a moved or resized volume shades with its old probes until it is baked again
//     (PROBE_NOT_BAKED reports only a missing bake, decision 9).
//
// The source file is the 8-byte source header ("EIRV" then the uint32 version 1, little-endian) followed by the payload
// below; the cooked artifact (§6.8) carries the same payload after its CookedHeader. Frozen by the M26 contract C26.1;
// stream Q-A implements the payload, the validation and the source reader.

namespace Engine {

	// The limits ValidateIrradianceVolumeData enforces, so a volume that external input controls cannot exhaust memory: at
	// most 16 probes along each axis (4,096 probes, 442,368 bytes of coefficients).
	inline constexpr uint32_t MinIrradianceProbesPerAxis = 2;
	inline constexpr uint32_t MaxIrradianceProbesPerAxis = 16;
	// The largest box side a volume accepts, in metres (PhysicsTypes' coordinate bound, MaxPhysicsCoordinate, is far
	// larger; a volume is a room or a valley, never a world).
	inline constexpr float MaxIrradianceVolumeSize = 10000.0f;
	// The source file's magic, its first four bytes.
	inline constexpr std::array<char, 4> IrradianceVolumeSourceMagic = { 'E', 'I', 'R', 'V' };

	// One probe: its SH9 coefficients (see the file comment).
	struct IrradianceProbe
	{
		std::array<glm::vec3, 9> SH9{};

		bool operator==(const IrradianceProbe&) const = default;
	};

	// A loaded irradiance volume (AssetType::IrradianceVolume). Plain data; immutable once loaded.
	struct IrradianceVolumeData : Asset
	{
		static constexpr AssetType StaticType = AssetType::IrradianceVolume;
		// The cooked payload layout below and the source file's version (ADR 0028's phase-3 ledger: a new format at version 1).
		static constexpr uint16_t FormatVersion = 1;

		IrradianceVolumeData()
			: Asset(StaticType)
		{
		}

		uint32_t CountX = MinIrradianceProbesPerAxis;
		uint32_t CountY = MinIrradianceProbesPerAxis;
		uint32_t CountZ = MinIrradianceProbesPerAxis;
		glm::vec3 Size{ 1.0f };
		// CountX * CountY * CountZ probes, x fastest (see the file comment).
		std::vector<IrradianceProbe> Probes{};
	};

	// The index of probe (x, y, z) in IrradianceVolumeData::Probes (each coordinate below its count, asserted). Pure.
	[[nodiscard]] size_t GetIrradianceProbeIndex(const IrradianceVolumeData& volume, uint32_t x, uint32_t y, uint32_t z);

	// The invariants every producer guarantees and every reader re-checks on untrusted data: each count in
	// [MinIrradianceProbesPerAxis, MaxIrradianceProbesPerAxis]; each Size component finite and in (0, MaxIrradianceVolumeSize];
	// exactly CountX * CountY * CountZ probes; every coefficient finite. Pure. Errors: Validation naming the first violation
	// ("probe 12's coefficient 3 is not finite").
	[[nodiscard]] Status ValidateIrradianceVolumeData(const IrradianceVolumeData& volume);

	// The payload (FormatVersion 1), little-endian, floats as their bits:
	//     uint32 CountX; uint32 CountY; uint32 CountZ; f32x3 Size;
	//     CountX * CountY * CountZ x { 9 x f32x3 SH9 }  (x fastest, then y, then z)
	// Asserts ValidateIrradianceVolumeData. Pure; equal volumes give equal bytes in every configuration (no padding), so two
	// bakes that compute equal coefficients write byte-identical files.
	[[nodiscard]] Buffer SerializeIrradianceVolumePayload(const IrradianceVolumeData& volume);

	// Reads a payload written by SerializeIrradianceVolumePayload: every count is checked against its limit and the bytes
	// left before anything is allocated, then ValidateIrradianceVolumeData. Never asserts on data (fuzzed: the fuzz target
	// IrradianceVolumeData, "IrradianceVolumeData: 10,000 seeded mutations of a payload never crash"). Errors: Parse for
	// truncation, a count above its limit or trailing bytes; Validation from ValidateIrradianceVolumeData.
	[[nodiscard]] Result<IrradianceVolumeData> DeserializeIrradianceVolumePayload(std::span<const std::byte> payload);

	// The source file of `volume`: the source header (IrradianceVolumeSourceMagic, then uint32 FormatVersion) followed by
	// SerializeIrradianceVolumePayload. What lighting.bake writes. Asserts ValidateIrradianceVolumeData.
	[[nodiscard]] Buffer WriteIrradianceVolumeSource(const IrradianceVolumeData& volume);

	// Reads a source file (the importer's input, external input): the magic, then a version of exactly FormatVersion, then
	// DeserializeIrradianceVolumePayload of the rest. Errors: Parse for a missing or wrong magic; UnsupportedVersion for
	// another version (naming it); those of DeserializeIrradianceVolumePayload.
	[[nodiscard]] Result<IrradianceVolumeData> ReadIrradianceVolumeSource(std::span<const std::byte> source);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `volume`, produced by a source of version
	// `importerVersion`. Asserts ValidateIrradianceVolumeData (through SerializeIrradianceVolumePayload).
	[[nodiscard]] Buffer CookIrradianceVolume(const IrradianceVolumeData& volume, uint32_t importerVersion);

	// The volume of a cooked artifact (both asset managers' loader for AssetType::IrradianceVolume, RegisterBuiltinLoaders):
	// ReadCookedArtifact with AssetType::IrradianceVolume and FormatVersion, then the payload. Errors: as ReadCookedArtifact
	// and DeserializeIrradianceVolumePayload.
	[[nodiscard]] Result<AssetRef<IrradianceVolumeData>> LoadCookedIrradianceVolume(std::span<const std::byte> cooked);

}
