#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderStats.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The planar reflections' mip chain (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3; Architecture §8.3): the
// PlanarReflections declaration's one compute pipeline (the program PlanarReflection, Passes/PlanarReflection.slang), owned by
// SceneRendererPipelines (the placement rule's place 1). After a mirror view has rendered a reflection's level 0, Record
// fills its other levels, each the 2 x 2 mean of the level above, so the forward passes' level curve
// (Common/PlanarReflections.slang) blurs a glossy reflection by its roughness. The mirror views themselves are the scene
// renderer's (SceneRendererReflections.cpp). Main thread; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	class PlanarReflectionPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;
		// The levels a reflection keeps: level 0 and five halvings (fewer for a reflection whose full chain is shorter). The
		// forward passes' level curve clamps to its last level, which a GGX alpha of about 0.24 reaches at 1080p's default half
		// resolution.
		static constexpr uint32_t MaxMipCount = 6;
		// The pixels by which a mirror view's rasterization rectangle is widened on every side (SceneRendererReflections.cpp):
		// the footprint of a bilinear sample at the last level, so every texel the forward passes sample on the glass was
		// rendered.
		static constexpr uint32_t BlurMargin = 1U << MaxMipCount;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PlanarReflectionPass;
		};

		// Use Create.
		explicit PlanarReflectionPass(ConstructionKey key);
		~PlanarReflectionPass();

		PlanarReflectionPass(const PlanarReflectionPass&) = delete;
		PlanarReflectionPass& operator=(const PlanarReflectionPass&) = delete;

		// Creates the pipeline through `pipelines`, checked against its reflection. `device` is a documented back-reference
		// that outlives the pass. Errors: those of PipelineFactory (a Gpu error is an out-of-memory creation).
		[[nodiscard]] static Result<Scope<PlanarReflectionPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		// The pipeline's layout description, for the CPU-only reflection test ("Shaders: LayoutsMatchReflection").
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// The reflection target of a `width` x `height` slot (both >= 1, asserted): SceneColorFormat with min(MaxMipCount, the
		// full chain) levels, a render target (level 0, the mirror view's colour), a storage image (the other levels) and a
		// shader resource, kept in ShaderResource between command lists (keepInitialState); the renderer's pool keys on it.
		[[nodiscard]] static nvrhi::TextureDesc GetColorDesc(uint32_t width, uint32_t height);
		// Records the levels after the first of `color` (made with GetColorDesc), each from the one above, one dispatch per
		// level, into `commandList` (open), adding its dispatches to `counters`; opens no stats row (the slot's row times
		// it). Errors: Gpu when a binding set cannot be created (the levels before it stay recorded).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, nvrhi::ITexture& color, RenderPassCounters& counters) const;
	private:
		struct State;
		Scope<State> m_State;
	};

}
