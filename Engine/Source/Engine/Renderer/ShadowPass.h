#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/SpotShadowAtlas.h"

#include <nvrhi/nvrhi.h>

#include <span>
#include <vector>

namespace Engine {

	class AssetManager;
	class GpuResourceCache;
	class GraphicsDevice;

	// The caster-constant slots of one record: one per cascade (slots [0, 4)), then one per spot tile (from slot 4). A slot is
	// 256 bytes, the largest minUniformBufferOffsetAlignment Vulkan allows, so every slot offset is a valid binding offset.
	inline constexpr uint32_t ShadowCasterSlotCount = MaxShadowCascades + MaxSpotShadowLights;
	inline constexpr uint32_t ShadowCasterSlotStride = 256;

	struct ShadowRenderInputs
	{
		std::span<const MeshDrawItem> Casters{};    // full snapshot list, not camera-culled: off-screen casters must survive
		const ShadowCascadeSet* Cascades = nullptr; // borrowed for Record only; null means no directional shadows
		const SpotShadowAtlas* Spots = nullptr;     // borrowed for Record only
		nvrhi::ITexture* CascadeTarget = nullptr;   // D32 array, ShadowMapSize square, 4 slices
		nvrhi::ITexture* AtlasTarget = nullptr;     // D32, 4096 square
		// Per-view objects the caller keeps with its targets (ADR 0017: per-view bindings and targets belong to the caller), so
		// a steady-state record creates no GPU object: CreateFramebuffers of each target and a GetCasterConstantsDesc buffer.
		std::span<const nvrhi::FramebufferHandle> CascadeFramebuffers{}; // one per CascadeTarget slice, in slice order
		nvrhi::IFramebuffer* AtlasFramebuffer = nullptr;                 // the AtlasTarget's framebuffer
		nvrhi::IBuffer* CasterConstants = nullptr;                       // rewritten by every Record
		float DepthBias = 1.0f;                                          // directional light's texel-scaled factors
		float NormalBias = 1.0f;
	};

	struct ShadowRenderResult
	{
		uint32_t DrawCalls = 0;
		uint32_t Triangles = 0;
	};

	// Shared immutable pipelines; per-view targets, framebuffers, caster constants and binding cache supplied by the caller.
	// Main thread, no stored spans. Six variants: Opaque/Mask x back/front/none cull, matching M8 alpha test, mirrored
	// determinant and DoubleSided. Blend does not cast. Reverse-Z clear 0/GreaterOrEqual. Directional casters nearer the light
	// than the map's near plane are pancaked per fragment (GraphicsDeviceInfo::DepthClamp, no device feature: Shadow.slang
	// clamps the exactly interpolated depth); GraphicsSpecification::DisableDepthClamp forces the extended-near fallback,
	// which clips them. Both paths are tested.
	class ShadowPass
	{
	public:
		static constexpr uint32_t PipelineCount = 6;
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ShadowPass;
		};
		explicit ShadowPass(ConstructionKey key);
		~ShadowPass();
		ShadowPass(const ShadowPass&) = delete;
		ShadowPass& operator=(const ShadowPass&) = delete;

		// device back-reference outlives pass; PipelineFactory used only during creation. Gpu/compile/reflection errors
		// propagate; startup caller maps creation failure to FatalError(OutOfMemory) per §8.14.
		[[nodiscard]] static Result<Scope<ShadowPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] static nvrhi::TextureDesc GetCascadeTargetDesc(uint32_t shadowMapSize);
		[[nodiscard]] static nvrhi::TextureDesc GetAtlasTargetDesc();
		// The per-view caster-constant buffer ShadowRenderInputs::CasterConstants takes.
		[[nodiscard]] static nvrhi::BufferDesc GetCasterConstantsDesc();
		// One depth-only framebuffer per array slice of `target` (a cascade or atlas target). Gpu errors propagate.
		[[nodiscard]] static Result<std::vector<nvrhi::FramebufferHandle>> CreateFramebuffers(GraphicsDevice& device, nvrhi::ITexture& target);

		// Resolves meshes/materials through shared asset mirrors, culls per light, and records shadow passes only.
		// All inputs borrowed for this call; commandList open, targets match descriptions (asserted). Gpu errors from
		// bindings; malformed external light/settings data must have been rejected by the pure planning functions.
		[[nodiscard]] Result<ShadowRenderResult> Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
			GpuResourceCache& cache, AssetManager& assets, const ShadowRenderInputs& inputs);
		// Record opens/closes DirectionalShadows and SpotShadows.
		// Borrow recording only for this call; disabled work has no row, errors still close scopes and report recorded counters.
	private:
		struct State;
		Scope<State> m_State;
	};

}
