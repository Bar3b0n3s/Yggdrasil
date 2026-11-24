#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The Water pass (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §8.3 "Water"): the snapshot's waters
// (RenderSnapshot::Waters) drawn in the Forward stage after the sky and before SSR, in a Lit view with a camera outside the
// wireframe. Each is a camera-centred clipmap of one fixed grid with a skirt to the horizon (Renderer/WaterGeometry.h), a
// Rectangle's clamped to its footprint, displaced forward by Common/Waves.slang (the GPU mirror of Physics/WaterWaves.h's
// sum) and shaded by the program Water (Passes/Water.slang): refraction of a copy of SceneColor (WaterRefraction),
// absorption over the water column from ViewDepth, the forward pass's specular image-based light and light loop, foam,
// detail normals and fog; it writes SceneColor, SceneNormals, SceneVelocity and SceneRoughness (and EntityId with picking)
// and tests and writes SceneDepth, so the SSR pass traces and composites over it. Set 0 is the forward view set's variant with
// SceneNormals and the ambient occlusion swapped for neutral textures; set 1 is the water's own (its records, the refraction,
// the depth pyramid, the normal and foam maps, the planar slot). An Ocean fragment inside a Rectangle's volume writes zero
// coverage. Main thread.
//
// Frozen by M47's contract C47 (decision 2): the class, its pipeline count, Create and RecordSurface. W47.2's render stream
// implements them and the pipeline set creates the pass; W47.3 adds the Underwater pass's recording.

namespace Engine {

	class GpuResourceCache;
	class GraphicsDevice;

	// The per-view state of the water (SceneRenderer::State, place 2 of Architecture §8.14 item 10): the view's water records
	// and the binding sets that read them, created lazily and released with the view's state.
	struct WaterViewResources
	{
		nvrhi::BufferHandle Records{};
		uint32_t RecordCapacity = 0; // records Records holds
		PassBindingCache Bindings{};
	};

	// What RecordSurface draws with: the view's snapshot (its waters, camera and picking table) and camera (its viewport at the
	// scene resolution); the framebuffer of the surfaces (SceneColor, SceneNormals, SceneVelocity, SceneRoughness, and EntityId
	// when Picking, with SceneDepth); the forward view set's variant (set 0); Refraction, the copy of SceneColor the caller
	// made under the WaterRefraction row before the draws; and the depth pyramid (ViewDepth). None of the pointers is null.
	struct WaterSurfaceInputs
	{
		const RenderSnapshot* Snapshot = nullptr;
		const CameraData* Camera = nullptr;
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		nvrhi::IBindingSet* ViewSet = nullptr;
		nvrhi::ITexture* Refraction = nullptr; // WaterRefraction: SceneColorFormat at the scene resolution, from the target pool
		nvrhi::ITexture* ViewDepth = nullptr;
		bool Picking = false;
	};

	class WaterPass
	{
	public:
		// The Water declaration's pipelines (RenderPassDeclaration.h): the surface, and the surface with EntityId.
		static constexpr uint32_t SurfacePipelineCount = 2;
		static constexpr uint32_t PipelineCount = SurfacePipelineCount;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class WaterPass;
		};

		// Use Create.
		explicit WaterPass(ConstructionKey key);
		~WaterPass();

		WaterPass(const WaterPass&) = delete;
		WaterPass& operator=(const WaterPass&) = delete;

		// Creates the pass's pipelines, its ring geometry and its procedural textures (place 1 of §8.14 item 10:
		// SceneRendererPipelines::CreatePipelines), its set 0 laid out as `forwardViewLayout`. Errors: those of the pipeline
		// factory and of the buffer and texture creations.
		[[nodiscard]] static Result<Scope<WaterPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines, nvrhi::IBindingLayout& forwardViewLayout);

		// The binding layouts of the pass's own sets, for the pipeline set's layout table.
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();

		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Draws every water of the snapshot in its order under the caller's Water row, counting its draws in `counters`, after
		// writing the view's records into `resources`. A water whose NormalMap or FoamTexture does not resolve through `cache`
		// draws with the built-in one. Errors: those of a record buffer's or a binding set's creation, with nothing drawn.
		[[nodiscard]] Status RecordSurface(nvrhi::ICommandList& commandList, GpuResourceCache& cache, WaterViewResources& resources,
			const WaterSurfaceInputs& inputs, RenderPassCounters& counters);
	private:
		struct State;
		Scope<State> m_State;
	};

}
