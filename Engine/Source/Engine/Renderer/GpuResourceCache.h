#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Graphics/HostImageUpload.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// The GPU mirrors of mesh and texture assets (Architecture §7.2, §8.14 item 1). M6 delivers mesh and texture uploads only,
// through the M5 upload paths: textures through HostImageUpload (Vulkan 1.4 host image copy, staging fallback), meshes
// through GraphicsDevice::CreateBuffer and one command list's writeBuffer. Materials (binding sets per (material, version),
// §8.4) and environments join with the PBR renderer in M8.

namespace Engine {

	class AssetManager;
	class GraphicsDevice;

	// One submesh of a GPU mesh (MeshSubmesh without the CPU data).
	struct GpuSubmesh
	{
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		uint32_t MaterialSlot = 0;
		Aabb Bounds{};
	};

	// A mesh's GPU mirror: a vertex buffer of MeshVertex (48 bytes each, §6.8) and a uint32 index buffer, both in their
	// shader-readable states (keepInitialState: VertexBuffer and IndexBuffer).
	struct GpuMesh
	{
		nvrhi::BufferHandle VertexBuffer{};
		nvrhi::BufferHandle IndexBuffer{};
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
		uint64_t Version = 0;
		bool IsPlaceholder = false;
	};

	struct GpuResourceCacheStats
	{
		size_t MeshCount = 0; // live mirrors, placeholders included
		size_t TextureCount = 0;
		size_t UploadFailures = 0; // since construction
	};

	// Owns every asset GPU mirror (§8.14 item 1), keyed by (handle, version): a hot reload bumps the asset's version, the
	// next Get uploads the new version and the old mirror is released by CollectStale (NVRHI defers the destruction until the
	// command lists using it complete, §8.14 item 2). Assets come from AssetManager::GetOrPlaceholder, so a missing or failed
	// asset gets its placeholder's mirror. A failed upload (Gpu, also --gpu-inject-fault=oom-texture, ADR 0009 decision 8)
	// never crashes: the placeholder's mirror is used and the AssetManager records an AssetUploadFailedCode diagnostic
	// (§8.14 item 7), logged once per asset. Placeholder mirrors are created on first need; if even that fails, the texture
	// mirror's handle is null and the caller skips draws using it.
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
		// A null handle gives the placeholder (Cube) mirror without a diagnostic. The reference stays valid until the next call
		// that uploads, CollectStale or Clear.
		[[nodiscard]] const GpuMesh& GetMesh(AssetHandle handle);

		// The mirror of texture `handle`, as GetMesh (placeholder: the Missing texture). TextureFormat maps to RGBA8_UNORM,
		// SRGBA8_UNORM and R8_UNORM with every mip uploaded.
		[[nodiscard]] const GpuTexture& GetTexture(AssetHandle handle);

		// Releases mirrors whose version is older than their asset's current version, and those not used through Get since
		// the previous CollectStale when `releaseUnused` is true (scene unload: "GpuResourceCache: live counts return to
		// baseline after unloading a scene", M8).
		void CollectStale(bool releaseUnused = false);

		// Releases every mirror, placeholders included.
		void Clear();

		[[nodiscard]] GpuResourceCacheStats GetStats() const;
	private:
		// The device and asset-manager back-references and the mirror tables (GpuResourceCache.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
