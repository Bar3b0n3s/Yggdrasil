#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// §8.3 pass 12, FXAA 3.11 (Lottes; quality preset 12) as one compute pass from an LdrColor target into its ping-pong partner
// (both LdrColorFormat, display-encoded values, §8.9). Luma is computed from the encoded colour, as FXAA expects perceptual
// input. Optional per view (PostProcessSettings::FxaaEnabled); the golden images render with it on (§15.4).
//
// Binding layout: the source (sampled through LinearClamp) and the destination (storage image, [vk::image_format("rgba8")]).
// Created once per device at startup and owned by SceneRendererPipelines; the pass keeps no binding sets: each Record takes
// them from the view's PassBindingCache. Main thread only; not copyable or movable. Frozen by the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 8); stream C implements it.

namespace Engine {

	class GraphicsDevice;

	struct FxaaPassInputs
	{
		nvrhi::ITexture* Source = nullptr;      // LdrColorFormat, shader-readable; never null
		nvrhi::ITexture* Destination = nullptr; // LdrColorFormat with isUAV, the size of Source; never null, never Source
	};

	class FxaaPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class FxaaPass;
		};

		// Use Create.
		explicit FxaaPass(ConstructionKey key);
		~FxaaPass();

		FxaaPass(const FxaaPass&) = delete;
		FxaaPass& operator=(const FxaaPass&) = delete;

		// Creates the pipeline and its sampler. `device` is a documented back-reference that outlives the pass. Errors: those
		// of PipelineFactory and the GraphicsDevice wrappers (Gpu: FatalError(OutOfMemory) at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<FxaaPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records FXAA from Source into Destination, with binding sets from the view's `bindings`. Returns the texture that
		// holds the result: Destination when the pass was recorded, Source when nothing was, so the caller always presents the
		// returned texture. Asserts the inputs. Errors: those of creating a binding set for new resources (Gpu).
		[[nodiscard]] Result<nvrhi::ITexture*> Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const FxaaPassInputs& inputs);
	private:
		// The back-reference, the pipeline and the sampler (FxaaPass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
