#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// §8.3 pass 10, bloom (Architecture §8.3, §8.9): compute passes over a mip chain from half resolution (GetChainDesc), in the
// device's Bloom format: R11G11B10_FLOAT, or RGBA16_FLOAT where R11G11B10 is not supported as a storage image (§8.1,
// GraphicsDeviceInfo::BloomFormat; a shader permutation, BLOOM_RGBA16, logged once by device selection).
//   - Downsample: 13-tap filter (Jimenez 2014), SceneColor into mip 0 with the Karis average (luma-weighted, against
//     fireflies), then mip i into mip i + 1 without it.
//   - Upsample: from the smallest mip up, mip i = mip i + tent(mip i + 1) (a 3x3 tent filter of bilinear taps); the last
//     step, into mip 0, also divides by the chain's mip count.
// Mip 0 then holds the average of the chain's blurred levels (with a one-mip chain, the downsampled image itself), so the
// bloom is normalized: a uniform SceneColor c gives a uniform mip 0 of c (the Karis average of equal texels is the texel),
// and TonemapPass's composite lerp(scene, bloom, BloomIntensity) (§8.3 pass 10) leaves a uniform image unchanged. No
// thresholding and no gain: the lerp only redistributes energy (physically based bloom). Deterministic (no temporal state).
//
// Created once per device at startup for one format and owned by SceneRendererPipelines; the per-view chain belongs to the
// SceneRenderer (created from GetChainDesc at Create and Resize). The pass keeps no binding sets: each Record takes them
// from the view's PassBindingCache. Main thread only; not copyable or movable. Frozen by the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 8).

namespace Engine {

	class GraphicsDevice;

	struct BloomPassInputs
	{
		nvrhi::ITexture* SceneColor = nullptr; // SceneColorFormat, shader-readable; never null
		// The per-view chain: a texture created from GetChainDesc(SceneColor's width, height, the pass's format); never null.
		nvrhi::ITexture* Chain = nullptr;
	};

	class BloomPass
	{
	public:
		// Downsample with the Karis average, downsample, upsample.
		static constexpr uint32_t PipelineCount = 3;
		// The longest chain (§8.3: "6-mip chain from half res"); smaller views get the full chain of their half size.
		static constexpr uint32_t MaxMipCount = 6;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class BloomPass;
		};

		// Use Create.
		explicit BloomPass(ConstructionKey key);
		~BloomPass();

		BloomPass(const BloomPass&) = delete;
		BloomPass& operator=(const BloomPass&) = delete;

		// Creates the pipelines for `format` (R11G11B10_FLOAT or RGBA16_FLOAT, asserted) and the LinearClamp sampler. `device`
		// is a documented back-reference that outlives the pass. Errors: those of PipelineFactory and the GraphicsDevice
		// wrappers (Gpu: FatalError(OutOfMemory) at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<BloomPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines, nvrhi::Format format);

		// The layout descriptions of the pipelines for `format` (each storage image declares "r11f_g11f_b10f" or "rgba16f").
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions(nvrhi::Format format);

		// The chain of a `width` x `height` view (both >= 1, asserted): a 2D texture of max(1, width / 2) x max(1, height / 2)
		// in `format` with min(MaxMipCount, its full mip count) mips, a shader resource and a storage image, kept in
		// ShaderResource between command lists (keepInitialState), named "SceneRenderer.Bloom". Pure.
		[[nodiscard]] static nvrhi::TextureDesc GetChainDesc(uint32_t width, uint32_t height, nvrhi::Format format);

		[[nodiscard]] nvrhi::Format GetFormat() const;
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the downsample and upsample chain over `inputs.Chain`, with binding sets from the view's `bindings`. Returns
		// the texture whose mip 0 holds the bloom (the chain), or nullptr when nothing was recorded, so a caller never
		// composites a chain that was not written. Asserts the inputs and that the chain matches GetChainDesc. Errors: those
		// of creating binding sets for new resources (Gpu).
		[[nodiscard]] Result<nvrhi::ITexture*> Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const BloomPassInputs& inputs);
	private:
		// The back-reference, the format, the pipelines and the sampler (BloomPass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
