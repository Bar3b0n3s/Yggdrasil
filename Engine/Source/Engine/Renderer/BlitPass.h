#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"

#include <nvrhi/nvrhi.h>

// The blit of §8.3 pass 15 ("ImGui or blit"): draws a texture (the SceneRenderer's LdrColor, RGBA8_UNORM with
// display-encoded values) into a framebuffer of another format and size, the swapchain's BGRA8_UNORM or an RGBA8_UNORM
// offscreen target, with one fullscreen triangle sampled through LinearClamp, values copied unconverted (both sides hold
// display-encoded values, §8.9). The Runtime presents its game view with it (Dist has no ImGui), and the editor draws the
// play session's game view under its UI until the viewport panels arrive (M10). Frozen by the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 7).
//
// The pipeline is created for one framebuffer format at Create (§8.12: pipelines at startup); the Runtime's swapchain
// format is known then. Main thread only; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	class BlitPass
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class BlitPass;
		};

		// Use Create.
		explicit BlitPass(ConstructionKey key);
		~BlitPass();

		BlitPass(const BlitPass&) = delete;
		BlitPass& operator=(const BlitPass&) = delete;

		// Creates the pipeline for framebuffers matching `framebuffer` (one colour attachment, no depth, sample count 1) and its
		// sampler. `device` is a documented back-reference that outlives the pass. Errors: InvalidArgument for a
		// FramebufferInfo with depth or not exactly one colour format; those of PipelineFactory and the GraphicsDevice
		// wrappers (Gpu: FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<BlitPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines, const nvrhi::FramebufferInfo& framebuffer);

		// Records the blit of `source` (a sampled texture in a shader-readable state, asserted non-null) covering the whole of
		// `framebuffer` (which matches Create's FramebufferInfo, asserted), stretched to its size. Errors: those of creating
		// the binding set for a new source texture (cached per texture).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, nvrhi::ITexture& source, nvrhi::IFramebuffer& framebuffer);

		// The pipeline's layout description, for "Shaders: LayoutsMatchReflection".
		[[nodiscard]] static PipelineLayoutDescription GetLayoutDescription();
	private:
		// The back-reference, the pipeline, the sampler and the cached binding sets (BlitPass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
