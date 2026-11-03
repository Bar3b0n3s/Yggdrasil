#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/LightClusters.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/LightClusterConstants.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The LightClusters pass of the clustered light loop (M23, R-CLC; Docs/Decisions/0034-m23-contract.md decision 5): one
// compute pipeline of the program LightClusters (Passes/LightClusters.slang) that writes the view's cluster masks
// (Shared/LightClusterConstants.h) at the end of the Ambient stage. It dispatches one thread per cluster and mask word; each
// thread tests the 32 lights of its word against its cluster's view-space bounds and is the only writer of its word, so no
// atomic decides a bit. The test is conservative: the cluster is its tile, widened by a pixel on every side, times its
// slice's depth range, widened by a thousandth at either end (the first slice from depth 0, the last open beyond the far
// clip), and a point or spot light is its sphere of Range, widened by a hundredth and by the rounding of the positions'
// magnitudes, so a light that lights any point of the cluster in the forward pass is in its mask. Directional lights are
// in every cluster. The forward passes and the LightCount view read the masks (t11 of the forward passes' set 0) through
// the grid's constants (b3), whose Mode makes them loop over every light of the list instead when the pass did not run.
//
// The pass is created by SceneRendererPipelines::CreatePipelines (the placement rule's place 1); the masks and the
// constants are each SceneRenderer's (place 2: SceneRenderer::State::LightClusterBuffer and LightClusterConstantsBuffer),
// created by its Render and dropped with its state by a device loss's release. Main thread; no borrowed pointer survives
// Record.

namespace Engine {

	class GraphicsDevice;

	// The clusters of `grid`: TilesX * TilesY * Slices.
	[[nodiscard]] uint32_t GetLightClusterCount(const LightClusterGrid& grid);

	// The constants of `grid` (b3 of the forward passes' set 0) with Mode.x `mode`: LightClusterModeClustered or
	// LightClusterModeBruteForce.
	[[nodiscard]] LightClusterConstants MakeLightClusterConstants(const LightClusterGrid& grid, uint32_t mode);

	// What one Record reads and writes, borrowed for the call.
	struct LightClusterPassInputs
	{
		LightClusterGrid Grid{};                    // the view's grid, at least one cluster
		nvrhi::IBuffer* ViewConstants = nullptr;    // the view's ViewConstants (View, InverseProjection, LightCount)
		nvrhi::IBuffer* ClusterConstants = nullptr; // GetConstantsDesc: Record writes the grid's clustered constants into it
		nvrhi::IBuffer* Lights = nullptr;           // the view's light list (Shared/ShaderLight.h)
		nvrhi::IBuffer* Masks = nullptr;            // GetMaskBufferDesc of at least the grid's cluster count
	};

	class LightClusterPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;
		// The threads of one group of the pass's dispatch, along x.
		static constexpr uint32_t ThreadGroupSize = 64;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class LightClusterPass;
		};

		explicit LightClusterPass(ConstructionKey key);
		~LightClusterPass();

		LightClusterPass(const LightClusterPass&) = delete;
		LightClusterPass& operator=(const LightClusterPass&) = delete;

		// Creates the pass's pipeline through `pipelines`, its layout checked against the reflection. `device` is a documented
		// back-reference that outlives the pass. Errors: those of PipelineFactory (a Gpu error is an out-of-memory creation).
		[[nodiscard]] static Result<Scope<LightClusterPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		// The layout of the pass's one pipeline: set 0 with b0 ViewConstants, b1 LightClusterConstants, t0 the light list and
		// u0 the masks.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// The grid's constants: a constant buffer of LightClusterConstants kept in ConstantBuffer between command lists.
		[[nodiscard]] static nvrhi::BufferDesc GetConstantsDesc();
		// The masks of `clusterCount` clusters (at least one): a structured buffer of LightClusterWordCount uint words per
		// cluster, with UAVs, kept in ShaderResource between command lists.
		[[nodiscard]] static nvrhi::BufferDesc GetMaskBufferDesc(uint32_t clusterCount);

		// Records the LightClusters row: writes MakeLightClusterConstants(inputs.Grid, LightClusterModeClustered) into
		// inputs.ClusterConstants, then dispatches the masks of every cluster of the grid into inputs.Masks, after the view's
		// ViewConstants and light list are written. Errors, with nothing recorded: InvalidArgument for a missing buffer, an
		// empty grid or a mask buffer smaller than the grid's; those of the binding set's creation (Gpu). Main thread; the
		// command list is open.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
			const LightClusterPassInputs& inputs);
	private:
		struct State;
		Scope<State> m_State;
	};

}
