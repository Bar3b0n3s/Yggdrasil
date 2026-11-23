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
// fills its other levels, each the area-weighted mean of the whole level above (2 x 2 texels, up to 3 x 3 where the level
// above is odd: Passes/PlanarReflection.slang), so the forward passes' level curve (Common/PlanarReflections.slang) blurs a
// glossy reflection by its roughness. The chain's length follows that curve at the view's SsrMaxRoughness
// (Renderer/PlanarReflections.h's ComputePlanarReflectionMipCount). The mirror views themselves are the scene renderer's
// (SceneRendererReflections.cpp). Main thread; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	class PlanarReflectionPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;

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
		// The reflection target of a `width` x `height` slot with `mipCount` levels (both sizes >= 1, and from 1 to the full
		// chain, floor(log2(max(width, height))) + 1, asserted; ComputePlanarReflectionMipCount's): SceneColorFormat, a render
		// target (level 0, the mirror view's colour), a storage image (the other levels) and a shader resource, kept in
		// ShaderResource between command lists (keepInitialState); the renderer's pool keys on it.
		[[nodiscard]] static nvrhi::TextureDesc GetColorDesc(uint32_t width, uint32_t height, uint32_t mipCount);
		// Records the levels after the first of `color` (made with GetColorDesc), each from the one above (level k's size
		// max(1, size >> k)), one dispatch per level, into `commandList` (open), adding its dispatches to `counters`; opens no stats row (the slot's row times
		// it). Errors: Gpu when a binding set cannot be created (the levels before it stay recorded).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, nvrhi::ITexture& color, RenderPassCounters& counters) const;
	private:
		struct State;
		Scope<State> m_State;
	};

}
