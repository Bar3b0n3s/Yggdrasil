#include "EnginePCH.h"
#include "Engine/Renderer/GpuResourceCache.h"

// M6 contract stub (Roadmap rule 3): stream F (built-ins, GPU cache, shipped assets) implements the GPU mirrors.

namespace Engine {

	struct GpuResourceCache::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		AssetManager* Assets = nullptr;   // documented back-reference
		GpuMesh UnavailableMesh;
		GpuTexture UnavailableTexture;
	};

	GpuResourceCache::GpuResourceCache(GraphicsDevice& device, AssetManager& assets)
		: m_State(CreateScope<State>())
	{
		m_State->Device = &device;
		m_State->Assets = &assets;
	}

	GpuResourceCache::~GpuResourceCache() = default;

	const GpuMesh& GpuResourceCache::GetMesh(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return m_State->UnavailableMesh;
	}

	const GpuTexture& GpuResourceCache::GetTexture(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return m_State->UnavailableTexture;
	}

	void GpuResourceCache::CollectStale(bool /*releaseUnused*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GpuResourceCache::Clear()
	{
		ENGINE_CONTRACT_STUB();
	}

	GpuResourceCacheStats GpuResourceCache::GetStats() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
