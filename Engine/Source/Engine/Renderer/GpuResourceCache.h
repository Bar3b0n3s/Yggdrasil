#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshDeform.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/HostImageUpload.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// The GPU mirrors of mesh, texture, material and environment assets (Architecture §7.2, §8.14 item 1). M6 delivered mesh and
// texture uploads through the M5 upload paths: textures through HostImageUpload (Vulkan 1.4 host image copy, staging
// fallback), meshes through GraphicsDevice::CreateBuffer and one command list's writeBuffer. M8 adds materials (their
// constants and resolved textures; the renderer caches one set-1 binding set per material generation, §8.4) and
// environments (the skybox and specular cubes, uploaded through HostImageUpload like textures, and the SH9 irradiance).
// Frozen by the M6 contract; the M8 contract added the material and environment members (Docs/Decisions/0013-m8-decisions.md
// decision 7).

namespace Engine {

	class AssetManager;
	class GraphicsDevice;
	struct RenderStaticSet;

	// One submesh of a GPU mesh (MeshSubmesh without the CPU data).
	struct GpuSubmesh
	{
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		uint32_t MaterialSlot = 0;
		Aabb Bounds{};
	};

	// A mesh's GPU mirror: a vertex buffer of MeshVertex (48 bytes each, §6.8), an attribute buffer and a uint32 index
	// buffer, each in its shader-readable state (keepInitialState: VertexBuffer and IndexBuffer).
	struct GpuMesh
	{
		nvrhi::BufferHandle VertexBuffer{};
		// M23 (C23.1, C-MESH2; Docs/Decisions/0034-m23-contract.md decision 9): vertex binding 1, MeshVertexAttributes (24
		// bytes each), bound at offset 0: the mesh's attribute stream (MeshData::Attributes), or for a mesh without one the
		// cache's shared default stream (TexCoord1 (0, 0), opaque white), which holds at least VertexCount entries, all equal.
		nvrhi::BufferHandle AttributeBuffer{};
		nvrhi::BufferHandle IndexBuffer{};
		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27; stream C): the mesh's skin (a raw
		// buffer of VertexCount MeshSkinVertex, 24 bytes each) and morph deltas (a raw buffer of MorphTargetCount x VertexCount
		// MeshMorphDelta, 36 bytes each, target by target), which the Skinning pass reads, null for a mesh without them; their
		// counts; and the bounds the Prepare stage culls a deformed item with (ComputeMeshDeformBounds, a CPU copy), whose
		// DefaultWeights are the morph targets' defaults the Prepare stage resolves each item's MorphWeights against
		// (ResolveMorphWeights), so they travel with the GPU mesh. A mesh with either also has a VertexBuffer that the Skinning
		// pass can read as a raw buffer (its source vertices), and refuses to upload when its vertices or morph deltas exceed
		// the pass's 32-bit byte offsets (4 GiB). DeformBounds is empty for a mesh with neither.
		nvrhi::BufferHandle SkinBuffer{};
		nvrhi::BufferHandle MorphBuffer{};
		uint32_t SkinJointCount = 0;
		uint32_t MorphTargetCount = 0;
		MeshDeformBounds DeformBounds{};
		uint32_t VertexCount = 0;
		uint32_t IndexCount = 0;
		std::vector<GpuSubmesh> Submeshes{};
		std::vector<AssetHandle> DefaultMaterials{}; // per material slot (MeshMaterialSlot::DefaultMaterial)
		Aabb Bounds{};
		uint64_t Version = 0;       // the asset version it mirrors (AssetManager::GetVersion)
		bool IsPlaceholder = false; // the asset was missing or failed, or its upload failed: this is the placeholder's mirror
	};

	// A texture's GPU mirror: an immutable sampled texture in the ShaderResource state (keepInitialState, ADR 0009 decision 21).
	struct GpuTexture
	{
		nvrhi::TextureHandle Texture{}; // null only when even the placeholder could not be created (draws using it are skipped)
		TextureUploadPath Path = TextureUploadPath::Staging;
		// M23 (C23.1, Docs/Decisions/0034-m23-contract.md decisions 10 and 15): the TextureFormat of the asset whose texels
		// Texture holds (the placeholder's when the placeholder stands in), whether it uploaded as it is or through the decode
		// fallback, whose RGBA8 or R8 texture no longer tells a BC5 normal map from an RGBA8 one: GetMaterial's two-channel
		// normal flag reads it.
		TextureFormat Format = TextureFormat::RGBA8Unorm;
		uint64_t Version = 0;
		bool IsPlaceholder = false;
	};

	// A material's GPU mirror (§8.4 set 1, §8.5): its constants buffer (Shared/MaterialConstants.h, written once per
	// generation) and its five maps resolved to textures, an empty slot bound to White (base colour, metallic-roughness,
	// occlusion), FlatNormal (normal) or Black (emissive), a missing or failed texture to its placeholder; plus the members
	// the renderer selects pipelines and sorts by.
	struct GpuMaterial
	{
		nvrhi::BufferHandle Constants{}; // keepInitialState ConstantBuffer; null only when even the placeholder failed
		nvrhi::TextureHandle BaseColorMap{};
		nvrhi::TextureHandle MetallicRoughnessMap{};
		nvrhi::TextureHandle NormalMap{};
		nvrhi::TextureHandle OcclusionMap{};
		nvrhi::TextureHandle EmissiveMap{};
		Engine::AlphaMode AlphaMode = Engine::AlphaMode::Opaque; // the type is spelled Engine:: (GCC's "changes meaning")
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
		// The Flags of Constants (Shared/MaterialConstants.h) as the mirror wrote them. M23 (C23.1, decision 10 of
		// Docs/Decisions/0034-m23-contract.md, stream X): MaterialFlagNormalMapTwoChannel follows the bound normal map's
		// TextureFormat (BC5Unorm), so when a texture change alone changes a flag (a placeholder replaced by a BC5 texture, a
		// Compression setting changed), the mirror writes new constants and takes a new Generation, never keeping stale ones.
		uint32_t Flags = 0;
		uint64_t Version = 0; // the material asset's version it mirrors
		// A new value whenever the mirror is built or rebuilt (a new material version, or a new version of one of its
		// textures), from one counter of the cache that only increases (starting at 1), so no Generation is ever reused
		// during the cache's lifetime, not even by a mirror that CollectStale released and a later GetMaterial rebuilt: the
		// renderer's binding-set cache keys on (handle, Generation) and can never find a set that holds older textures.
		uint64_t Generation = 0;
		bool IsPlaceholder = false; // the material was missing or failed: this mirrors the Error material
	};

	// An environment's GPU mirror (§8.6): EnvironmentData's cubes as immutable sampled cube textures (every mip,
	// ShaderResource: RGBA16_FLOAT, or for a BC6H cube BC6H_UFLOAT, or RGBA16_FLOAT through the decode fallback on a device
	// without BC support, M23) and its SH9 coefficients (EnvironmentData.h's convention) for EnvironmentConstants.
	struct GpuEnvironment
	{
		nvrhi::TextureHandle Skybox{};
		nvrhi::TextureHandle Specular{};
		uint32_t SkyboxMipCount = 0;
		uint32_t SpecularMipCount = 0;
		std::array<glm::vec3, 9> IrradianceSH9{};
		uint64_t Version = 0;
	};

	// M26 (C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16, stream Q-B): an irradiance volume's GPU mirror
	// (Asset/IrradianceVolumeData.h): its probes' SH9 coefficients as a structured buffer of CountX * CountY * CountZ * 9
	// float4 (xyz the coefficient, w 0), probes in the asset's order, which Common/Probes.slang reads trilinearly.
	struct GpuIrradianceVolume
	{
		nvrhi::BufferHandle Probes{};
		uint32_t CountX = 0;
		uint32_t CountY = 0;
		uint32_t CountZ = 0;
		uint64_t Version = 0;
	};

	// M26 (C26.1, R-LUT; decision 15, stream X-B): a LUT's GPU mirror: its table as a Size^3 3D texture, RGBA16_FLOAT (rgb
	// the entry, a 1), ShaderResource, with the domain the tonemap pass maps its input through.
	struct GpuLut
	{
		nvrhi::TextureHandle Texture{};
		uint32_t Size = 0;
		glm::vec3 DomainMin{ 0.0f };
		glm::vec3 DomainMax{ 1.0f };
		uint64_t Version = 0;
	};

	// M26 (C26.1, C-TERR; decision 22, stream T-C): a heightfield's GPU mirror: its samples as a Resolution^2 R16_UNORM
	// texture (sample / 65535, so a shader's height is the texel times HeightScale), ShaderResource, which the terrain's
	// vertex shaders read per chunk vertex (Renderer/TerrainLod.h).
	struct GpuHeightfield
	{
		nvrhi::TextureHandle Heights{};
		uint32_t Resolution = 0;
		uint64_t Version = 0;
	};

	struct GpuResourceCacheStats
	{
		size_t MeshCount = 0; // live mirrors, placeholders included
		size_t TextureCount = 0;
		size_t UploadFailures = 0; // since construction
		// M8 additions.
		size_t MaterialCount = 0;
		size_t EnvironmentCount = 0;
		// M26 (C26.1; Docs/Decisions/0037-m26-contract.md decisions 15, 16 and 22): the live mirrors of M26's types, so the
		// cache tests can show CollectStale, TrimUnused and a scene unload releasing them.
		size_t IrradianceVolumeCount = 0;
		size_t LutCount = 0;
		size_t HeightfieldCount = 0;
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the static cells holding a range of the static record
		// buffer, the records those ranges hold, and the buffer's capacity in records (0 without one).
		size_t StaticCellCount = 0;
		size_t StaticRecordCount = 0;
		size_t StaticRecordCapacity = 0;
	};

	// Owns every asset GPU mirror (§8.14 item 1), keyed by (handle, version): a hot reload bumps the asset's version, the
	// next Get uploads the new version and the old mirror is released by CollectStale (NVRHI defers the destruction until the
	// command lists using it complete, §8.14 item 2). Assets come from AssetManager::GetOrPlaceholder, so a missing or failed
	// asset gets its placeholder's mirror. A failed upload (Gpu, also --gpu-inject-fault=oom-texture, ADR 0009 decision 8)
	// never crashes: the placeholder's mirror is used and the AssetManager records an AssetUploadFailedCode diagnostic
	// (§8.14 item 7), logged once per asset. Placeholder mirrors are created on first need; if even that fails, the texture
	// mirror's handle is null and the caller skips draws using it. M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision
	// 28): an upload that fails while the device is lost (GraphicsDevice::IsLost) is the loss, not the asset's failure: the
	// stand-in mirror is used as above, but nothing is counted in UploadFailures or reported, and the device's recovery
	// drops the stand-in (ReleaseGpuObjects), so the next Get uploads the asset again.
	//
	// Main thread only (NVRHI command recording, §4.11). Destroyed before the device (§8.14 item 4: SceneRenderer, then
	// GpuResourceCache). Not copyable.
	class GpuResourceCache
	{
	public:
		// `device` and `assets` are documented back-references that outlive the cache.
		GpuResourceCache(GraphicsDevice& device, AssetManager& assets);
		~GpuResourceCache();

		GpuResourceCache(const GpuResourceCache&) = delete;
		GpuResourceCache& operator=(const GpuResourceCache&) = delete;

		// The mirror of mesh `handle` at its current version, uploading it when needed (one command list, executed at once).
		// A null handle gives the placeholder (Cube) mirror without a diagnostic. M23 (decision 9): a mesh without the
		// attribute stream binds the cache's shared default stream, created at the first such mesh and replaced by a longer
		// one when a mesh has more vertices than it holds; Clear and ReleaseGpuObjects drop it with the mirrors, and
		// CollectStale and TrimUnused once no mesh mirror binds it. The reference stays valid until the next call that
		// uploads, CollectStale or Clear.
		[[nodiscard]] const GpuMesh& GetMesh(AssetHandle handle);

		// The mirror of texture `handle`, as GetMesh (placeholder: the Missing texture). TextureFormat maps to RGBA8_UNORM,
		// SRGBA8_UNORM and R8_UNORM with every mip uploaded. M23 (C23.1, Docs/Decisions/0034-m23-contract.md decision 15): a
		// block-compressed texture uploads its blocks on a device with BC support (GraphicsDeviceInfo::TextureCompressionBC):
		// BC1_UNORM, BC1_UNORM_SRGB, BC4_UNORM, BC5_UNORM, BC7_UNORM and BC7_UNORM_SRGB. Without it, the decode fallback
		// decompresses every level (DecompressTextureBlocks, Asset/TextureCompression.h) into RGBA8_UNORM (BC1Unorm, BC5Unorm,
		// as (x, y, 0, 255), and BC7Unorm), SRGBA8_UNORM (BC1Srgb and BC7Srgb) or R8_UNORM (BC4Unorm), so a BC texture is
		// never replaced by its placeholder for want of the feature. The cache logs one Info line naming the path at its first
		// block-compressed texture or BC6H environment cube (GetEnvironment).
		[[nodiscard]] const GpuTexture& GetTexture(AssetHandle handle);

		// M8: the mirror of material `handle` at its current version (a null handle: the Default material; a missing or failed
		// one: the Error material's mirror with IsPlaceholder), resolving and uploading its textures through GetTexture and
		// writing its constants through one command list executed at once. Rebuilt when the material's version or one of its
		// textures' versions changes; M23 (decision 10): when the flags its resolved textures imply (a BC5 normal map's
		// MaterialFlagNormalMapTwoChannel) differ from GpuMaterial::Flags, the mirror writes new constants into a new buffer
		// under a new Generation, so a texture change alone (a placeholder replaced, a new version, a Compression change)
		// never leaves stale flags. A constants buffer that cannot be created records an AssetUploadFailedCode diagnostic
		// once per material and leaves Constants null (its draws are skipped). The reference stays valid until the next call
		// that uploads, CollectStale or Clear.
		[[nodiscard]] const GpuMaterial& GetMaterial(AssetHandle handle);

		// M8: the mirror of environment `handle` at its current version, or nullptr for a null handle or an environment that
		// cannot be loaded (AssetManager::Load fails, or the asset is not an environment: environments have no placeholder,
		// §7.2) or uploaded; the renderer then lights with RenderEnvironment::FallbackColor (§5.3). The cache reports every
		// failure itself through AssetManager::ReportDiagnostic, whatever the manager (the editor's, the Runtime's or a test's),
		// once per (handle, version), with GetOrPlaceholder's codes: AssetMissingCode for an unknown handle (Load's NotFound),
		// AssetTypeMismatchCode for an asset of another type, AssetImportFailedCode for any other load error, and
		// AssetUploadFailedCode for an upload failure; each is logged once (Error) by the manager. The pointer stays valid
		// until the next call that uploads, CollectStale or Clear.
		[[nodiscard]] const GpuEnvironment* GetEnvironment(AssetHandle handle);

		// M26 (C26.1; Docs/Decisions/0037-m26-contract.md decisions 15, 16 and 22): the mirrors of M26's asset types, each as
		// GetEnvironment: the mirror of `handle` at its current version, or nullptr for a null handle or an asset that cannot be
		// loaded (none of the three has a placeholder: an irradiance volume that fails lights with the Environment, a LUT that
		// fails grades nothing, a heightfield that fails draws no terrain) or uploaded, each failure reported once per (handle,
		// version) through AssetManager::ReportDiagnostic with GetOrPlaceholder's codes. The pointer stays valid until the next
		// call that uploads, CollectStale, Clear, ReleaseGpuObjects or TrimUnused, which release these mirrors with the others
		// (the placement rule's place 3, so a device loss drops them and the next Get uploads them again).
		[[nodiscard]] const GpuIrradianceVolume* GetIrradianceVolume(AssetHandle handle);
		[[nodiscard]] const GpuLut* GetLut(AssetHandle handle);
		[[nodiscard]] const GpuHeightfield* GetHeightfield(AssetHandle handle);

		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3; §8.14's third place): the GPU-resident instance records of
		// the static sets the renderers draw (Renderer/RenderStaticSet.h), in one device-local structured buffer of
		// InstanceRecord (t21 of set 0, StaticInstances), shared by every renderer and scene of the cache. Each cell, keyed by
		// its object (which the cache holds while the cell has its range), takes a range of consecutive records, one per item
		// in item order, written once when the cell takes it and never rewritten: a changed cell is a new object with a range of
		// its own. A record is MakeMeshInstanceRecord's (RenderPrepare.h) of its item with DrawFlagReceiveShadows for an item that
		// receives shadows, its PreviousWorld its World (a static item has no motion) and its EntityId StaticPickIdBase plus
		// its index in the buffer, the static pick id the renderer resolves against the cell's PickTable (AsyncPicker.h).
		// AcquireStaticRecords gives `set`'s cells their ranges (new ones written through one command list executed at once,
		// as the mirrors' uploads are) and returns each cell's first record, in the set's order; a range kept from an earlier
		// call stays where it is. The buffer grows geometrically (at least StaticRecordMinimumCapacity records), a grown one
		// rewritten whole with every range at its place, and never shrinks but through TrimUnused, Clear and
		// ReleaseGpuObjects, which drop it with every range (the next call writes the ranges it needs again; a renderer that
		// recorded with the old buffer holds it until its command list completes). The creation and the writes go through
		// RetryAfterTrim (Graphics/GpuOutOfMemory.h), whose trim is this cache's TrimUnused among others: called by a render's
		// Prepare stage, before it holds a mirror reference. Errors: OutOfMemory after the trim and retry (Gpu on a lost
		// device), with every range as it was; InvalidArgument when the set holds more records than a static pick id can
		// number (2^31).
		[[nodiscard]] Result<std::vector<uint32_t>> AcquireStaticRecords(const RenderStaticSet& set);
		// M43: the static record buffer AcquireStaticRecords' last success left, null before it and after a TrimUnused, Clear
		// or ReleaseGpuObjects.
		[[nodiscard]] nvrhi::IBuffer* GetStaticRecordBuffer() const;
		// M43: the smallest capacity, in records, of the static record buffer (256 records, 52 KiB).
		static constexpr uint32_t StaticRecordMinimumCapacity = 256;

		// Releases mirrors (meshes, textures, materials, environments, irradiance volumes, LUTs, heightfields) whose version
		// is older than their asset's current version, and those not used through Get since the previous CollectStale when
		// `releaseUnused` is true (scene unload: "GpuResourceCache: live counts return to baseline after unloading a scene",
		// M8). Every host that renders calls it once per frame, and with `releaseUnused` after the first frame of a newly
		// opened scene (SceneRenderer.h, "Stale mirrors"). M43: also frees the static record ranges of the cells no
		// AcquireStaticRecords used since the previous CollectStale, whatever `releaseUnused` says, so a later call may reuse
		// them: called when the frame's renders were submitted, so no recorded command list reads a range it frees.
		void CollectStale(bool releaseUnused = false);

		// Releases every mirror, placeholders included (M43: and the static record buffer with its ranges).
		void Clear();

		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 26 and 27): the cache's Release hook in the recreation
		// protocol; the constructor registers the cache with its device (as "GpuResourceCache"), the placement rule's place
		// for asset mirrors. Drops every mirror, placeholders included (Clear), and every handle a mirror's upload still holds,
		// while keeping the material generation counter (no GpuMaterial::Generation is ever reused, so no renderer's binding
		// set from before the loss is ever found again) and GetStats().UploadFailures. There is no Recreate: the next Get
		// uploads each mirror again from the asset manager's loaded data, as on first use. Main thread, from
		// GraphicsDevice::Recover.
		void ReleaseGpuObjects();

		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 43): the cache's TrimUnused hook (its registration calls
		// it) when a request ran out of GPU memory: releases what CollectStale() releases (the mirrors of replaced versions)
		// and the mirrors of assets the asset manager no longer has loaded (AssetManager::GetState is Unloaded: released or
		// unknown), placeholders and built-in assets excepted. A mirror of a loaded, current asset stays, since the next
		// Render would upload it again at once; a dropped one is uploaded again by its next Get. M43: drops the static record
		// buffer with every range (rebuilt by the next AcquireStaticRecords from the cells, compacted). Keeps the material
		// generation counter and GetStats().UploadFailures, as ReleaseGpuObjects does. Like CollectStale it ends the validity
		// of the references Get handed out, so it runs only between recordings (GraphicsDevice::TrimUnused). Main thread.
		void TrimUnused();

		[[nodiscard]] GpuResourceCacheStats GetStats() const;
	private:
		// The device and asset-manager back-references and the mirror tables (GpuResourceCache.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
