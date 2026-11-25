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
// irradiance probes as an asset (AssetType::IrradianceVolume, format version 2). `lighting.bake` (EditorCore/Lighting/
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
//   - M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3, "Relit probes"): format 2 adds an optional relight
//     block, written by a bake under an enabled Sky: per probe, the irradiance of the scene's own lights (Local: the capture's
//     Sky dark and its Sun and Moon lights off) and the sky's transfer (Transfer: where the probe sees the sky, projected on
//     the SH basis), from which the renderer relights the probe by the sky it renders (Renderer/ProbeRelight.h). Probes then
//     holds what the bake's sky gives (RelightIrradianceSH9 at that sky): what a renderer that does not relight shows.
//
// The source file is the 8-byte source header ("EIRV" then the uint32 version, little-endian) followed by the payload of that
// version: version 1 (M26's layout, byte for byte) for a volume without a relight block, version 2 for one with it, so every
// volume baked without a Sky keeps its bytes. The cooked artifact (§6.8) carries the format 2 payload after its CookedHeader,
// always; a cached version 1 artifact is rebuilt by the payload-version rule (§7.5), so the importer's version stays 1. Frozen
// by the M26 contract C26.1; stream Q-A implements the payload, the validation and the source reader, and M45 format 2.

namespace Engine {

	// The limits ValidateIrradianceVolumeData enforces, so a volume that external input controls cannot exhaust memory: at
	// most 16 probes along each axis (4,096 probes, 442,368 bytes of coefficients; with a relight block 1,622,016 bytes).
	inline constexpr uint32_t MinIrradianceProbesPerAxis = 2;
	inline constexpr uint32_t MaxIrradianceProbesPerAxis = 16;
	// The largest box side a volume accepts, in metres (PhysicsTypes' coordinate bound, MaxPhysicsCoordinate, is far
	// larger; a volume is a room or a valley, never a world).
	inline constexpr float MaxIrradianceVolumeSize = 10000.0f;
	// The source file's magic, its first four bytes.
	inline constexpr std::array<char, 4> IrradianceVolumeSourceMagic = { 'E', 'I', 'R', 'V' };
	// M45: the source file's version of a volume without a relight block (M26's layout).
	inline constexpr uint32_t IrradianceVolumeSourceVersionWithoutRelight = 1;
	// M45: the values of a sky transfer: the upper triangle of a symmetric 9 x 9 matrix.
	inline constexpr size_t IrradianceTransferCount = 45;

	// One probe: its SH9 coefficients (see the file comment).
	struct IrradianceProbe
	{
		std::array<glm::vec3, 9> SH9{};

		bool operator==(const IrradianceProbe&) const = default;
	};

	// M45: one probe's relight block (format 2, see the file comment).
	struct IrradianceProbeRelight
	{
		// The SH9 of the irradiance the scene's own lights give the probe (lamps, fires, emission), in IrradianceProbe's
		// convention.
		std::array<glm::vec3, 9> Local{};
		// The sky's transfer: T_ij, the integral over the directions of V Y_i Y_j, V 1 where the probe sees the sky and 0 where
		// geometry hides it, Y the real SH basis in IrradianceProbe's order; the upper triangle of the symmetric matrix, row by
		// row (T00, T01 ... T08, T11 ... T88). The radiance SH9 of the sky the probe sees is T S, S the sky's own.
		std::array<float, IrradianceTransferCount> Transfer{};

		bool operator==(const IrradianceProbeRelight&) const = default;
	};

	// A loaded irradiance volume (AssetType::IrradianceVolume). Plain data; immutable once loaded.
	struct IrradianceVolumeData : Asset
	{
		static constexpr AssetType StaticType = AssetType::IrradianceVolume;
		// The cooked payload layout below and the newest source file's version (ADR 0028's phase-3 ledger: a new format at
		// version 1; M45: 2, the relight block).
		static constexpr uint16_t FormatVersion = 2;

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
		// M45: empty, or each probe's relight block in Probes' order: the volume is relightable.
		std::vector<IrradianceProbeRelight> Relight{};
	};

	// The index of probe (x, y, z) in IrradianceVolumeData::Probes (each coordinate below its count, asserted). Pure.
	[[nodiscard]] size_t GetIrradianceProbeIndex(const IrradianceVolumeData& volume, uint32_t x, uint32_t y, uint32_t z);

	// The invariants every producer guarantees and every reader re-checks on untrusted data: each count in
	// [MinIrradianceProbesPerAxis, MaxIrradianceProbesPerAxis]; each Size component finite and in (0, MaxIrradianceVolumeSize];
	// exactly CountX * CountY * CountZ probes; every coefficient finite; M45: no relight block or one per probe, each of its
	// values finite. Pure. Errors: Validation naming the first violation ("probe 12's coefficient 3 is not finite").
	[[nodiscard]] Status ValidateIrradianceVolumeData(const IrradianceVolumeData& volume);

	// The payload (FormatVersion 2), little-endian, floats as their bits:
	//     uint32 CountX; uint32 CountY; uint32 CountZ; f32x3 Size;
	//     CountX * CountY * CountZ x { 9 x f32x3 SH9 }  (x fastest, then y, then z)
	//     uint32 HasRelight (0 or 1);
	//     with HasRelight 1: CountX * CountY * CountZ x { 9 x f32x3 Local; 45 x f32 Transfer }  (the probes' order)
	// Version 1's payload, a version 1 source file's, is the same without HasRelight and the block. Asserts
	// ValidateIrradianceVolumeData. Pure; equal volumes give equal bytes in every configuration (no padding), so two bakes that
	// compute equal coefficients write byte-identical files.
	[[nodiscard]] Buffer SerializeIrradianceVolumePayload(const IrradianceVolumeData& volume);

	// Reads a payload written by SerializeIrradianceVolumePayload: every count is checked against its limit and the bytes
	// left before anything is allocated, then ValidateIrradianceVolumeData. Never asserts on data (fuzzed: the fuzz target
	// IrradianceVolumeData, "IrradianceVolumeData: 10,000 seeded mutations of a payload never crash"). Errors: Parse for
	// truncation, a count above its limit, a HasRelight other than 0 or 1, or trailing bytes; Validation from
	// ValidateIrradianceVolumeData.
	[[nodiscard]] Result<IrradianceVolumeData> DeserializeIrradianceVolumePayload(std::span<const std::byte> payload);

	// The source file of `volume`: the source header (IrradianceVolumeSourceMagic, then the uint32 version) followed by the
	// payload of that version: version 1 (IrradianceVolumeSourceVersionWithoutRelight, M26's bytes) for a volume without a
	// relight block, FormatVersion's (SerializeIrradianceVolumePayload) for one with it. What lighting.bake writes. Asserts
	// ValidateIrradianceVolumeData.
	[[nodiscard]] Buffer WriteIrradianceVolumeSource(const IrradianceVolumeData& volume);

	// Reads a source file (the importer's input, external input): the magic, then a version of 1 or FormatVersion, then the
	// payload of that version, checked as DeserializeIrradianceVolumePayload checks it. Errors: Parse for a missing or wrong
	// magic; UnsupportedVersion for another version (naming it); those of DeserializeIrradianceVolumePayload.
	[[nodiscard]] Result<IrradianceVolumeData> ReadIrradianceVolumeSource(std::span<const std::byte> source);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `volume`, produced by a source of version
	// `importerVersion`. Asserts ValidateIrradianceVolumeData (through SerializeIrradianceVolumePayload).
	[[nodiscard]] Buffer CookIrradianceVolume(const IrradianceVolumeData& volume, uint32_t importerVersion);

	// The volume of a cooked artifact (both asset managers' loader for AssetType::IrradianceVolume, RegisterBuiltinLoaders):
	// ReadCookedArtifact with AssetType::IrradianceVolume and FormatVersion, then the payload. Errors: as ReadCookedArtifact
	// and DeserializeIrradianceVolumePayload.
	[[nodiscard]] Result<AssetRef<IrradianceVolumeData>> LoadCookedIrradianceVolume(std::span<const std::byte> cooked);

}
