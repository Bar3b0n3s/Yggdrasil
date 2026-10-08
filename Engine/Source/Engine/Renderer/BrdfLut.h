#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The DFG (BRDF integration) LUT of the split-sum specular IBL with multi-scatter energy compensation (Architecture §8.5,
// §8.6 step 5): RG16_FLOAT, Size x Size, generated once at device startup by one compute dispatch (well under 1 ms), then
// immutable: a keepInitialState texture in ShaderResource (not a permanent state, so readback can copy out of it, §8.2).
//
// Content (the multi-scatter form of Karis 2014 with Fdez-Aguera 2019, as Filament stores it): texel (i, j) holds, for
// NdotV = (i + 0.5) / Size and perceptual roughness (j + 0.5) / Size (row j = 0, the top row, is the smoothest; alpha =
// roughness squared),
//   R = DFG1 = the hemisphere integral of Fc * Gv, and G = DFG2 = the integral of Gv,
// where Fc = (1 - VdotH)^5 and Gv is the height-correlated Smith visibility times 4 NdotL VdotH / NdotH, estimated with
// LutSampleCount GGX importance samples on the Hammersley sequence. The forward shader takes the single-scatter specular
// as lerp(DFG1, DFG2, f0) per channel and the energy compensation as 1 + f0 * (1 / DFG2 - 1). The CPU reference is
// Test::ComputeDfg (Tests/Source/Support/RenderReference.h); "BrdfLut: DFG matches the CPU reference" compares them.
//
// Owned by SceneRendererPipelines (its forward pass binds the texture at t5 of set 0, §8.4). Main thread only; not copyable
// or movable. Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 7).

namespace Engine {

	class GraphicsDevice;

	class BrdfLut
	{
	public:
		static constexpr uint32_t Size = 128;
		static constexpr nvrhi::Format Format = nvrhi::Format::RG16_FLOAT;
		static constexpr uint32_t LutSampleCount = 1024;
		// The generator's compute pipeline, kept with the texture.
		static constexpr uint32_t PipelineCount = 1;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class BrdfLut;
		};

		// Use Create.
		explicit BrdfLut(ConstructionKey key);
		~BrdfLut();

		BrdfLut(const BrdfLut&) = delete;
		BrdfLut& operator=(const BrdfLut&) = delete;

		// Creates the pipeline and the texture and generates the LUT: one command list executed on the graphics queue (later
		// work on the queue sees the result; nothing waits on the CPU). `device` is a documented back-reference that outlives
		// the LUT. Errors: those of PipelineFactory and the GraphicsDevice wrappers (Gpu: FatalError(OutOfMemory) at startup,
		// §8.14 item 7).
		[[nodiscard]] static Result<Scope<BrdfLut>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// The LUT (Format, Size x Size, one mip, ShaderResource); null until the LUT exists.
		[[nodiscard]] nvrhi::ITexture* GetTexture() const;
	private:
		// The back-reference, the pipeline and the texture (BrdfLut.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
