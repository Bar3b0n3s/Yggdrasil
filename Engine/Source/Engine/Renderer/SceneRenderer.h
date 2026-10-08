#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The scene renderer (Architecture §8.2, §8.3), the walking skeleton's subset (Roadmap M7; Docs/Decisions/0012-m7-decisions.md
// decision 7): it renders a RenderSnapshot, never the ECS, through a fixed pass list:
//   1. Prepare (CPU): every MeshDrawItem resolved through the GpuResourceCache (a missing or failed mesh draws its
//      placeholder, §7.2) and its materials through the AssetManager (MaterialData; a null slot uses the mesh's default
//      material, an unknown one the Error material); submeshes outside the camera frustum (their bounds transformed by
//      World) are culled; opaque draws sorted by pipeline, then material, then mesh (§8.3 pass 1). Every material is
//      opaque in M7 (AlphaMode Mask and Blend arrive with M8).
//   4. Depth/normal prepass: SceneDepth (D32_FLOAT, reverse-Z: cleared to 0, GreaterOrEqual) and SceneNormals (RG16_FLOAT,
//      octahedral view-space normals of the interpolated vertex normals; normal maps arrive with M8). No EntityId target
//      (M9).
//   7. Forward opaque: SceneColor (RGBA16_FLOAT) cleared to the camera's ClearColor, depth test GreaterOrEqual with depth
//      writes off; Lambert diffuse with one directional light (the snapshot's first directional light, radiance Color *
//      Intensity) plus the constant ambient RenderEnvironment::FallbackColor * Intensity, times the material's BaseColor
//      factor (base colour maps and PBR arrive with M8). Without a directional light, only the ambient term lights.
//  11. Tonemap + encode (compute): LdrColor (RGBA8_UNORM) = sRGB OETF(clamp(SceneColor * 2^ExposureEV, 0, 1)): the Linear
//      tonemapper (§8.9) whatever PostProcessSettings::Tonemap says (the others, blue-noise dither, bloom and FXAA arrive with
//      M8).
// Pipelines use frontCounterClockwise = true with back-face culling (§8.3: glTF's counter-clockwise front faces), the
// set-0 per-view layout of §8.4 with the 80-byte DrawConstants push constants, and are created at startup (§8.12), once
// per device: SceneRendererPipelines holds them and every SceneRenderer of the device shares it (the host's game view,
// the ViewportCapture of screenshots, the editor's scene viewport from M10), so the pipeline count does not depend on the
// number of views (§8.5: "The total pipeline count is logged and asserted by a test"). A SceneRenderer owns only its
// size-dependent targets and per-view buffers: Create builds them and Resize rebuilds the targets. A snapshot without a
// camera (HasCamera false) clears SceneColor to the default ClearColor and draws nothing. Rendering is deterministic for
// a snapshot on a given device (§8.3: no temporal effects), which the golden images rely on.
//
// Main thread only (NVRHI recording, §4.11); not copyable or movable. Destroyed before the SceneRendererPipelines, the
// GpuResourceCache and the device (§8.14 item 4).

namespace Engine {

	class AssetManager;
	class GpuResourceCache;
	class GraphicsDevice;

	struct SceneRendererSpecification
	{
		// The size of the targets, both >= 1 (asserted).
		uint32_t Width = 1;
		uint32_t Height = 1;
	};

	// What the last Render did (stats.get's per-pass detail arrives with M9).
	struct SceneRenderStats
	{
		uint32_t MeshDraws = 0;       // submesh draws recorded by the forward pass
		uint32_t CulledSubmeshes = 0; // submeshes outside the frustum
		uint32_t Lights = 0;          // lights used (0 or 1 in M7)
	};

	// The pipelines of the pass list (see the file comment), created once per device at startup and shared by every
	// SceneRenderer of that device. Destroyed after the renderers that use it and before the device. Main thread only; not
	// copyable or movable.
	class SceneRendererPipelines
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SceneRendererPipelines;
		};

		// Use Create.
		explicit SceneRendererPipelines(ConstructionKey key);
		~SceneRendererPipelines();

		SceneRendererPipelines(const SceneRendererPipelines&) = delete;
		SceneRendererPipelines& operator=(const SceneRendererPipelines&) = delete;

		// Creates every pipeline of the pass list through `pipelines`, checking each layout against its reflection
		// (ValidatePipelineLayout), with their binding layouts and samplers. `device` is a documented back-reference that
		// outlives the set. Errors: those of PipelineFactory and of the GraphicsDevice wrappers; a Gpu error is an
		// out-of-memory creation, which a caller creating the set at startup turns into FatalError(OutOfMemory) (§8.14 item 7).
		[[nodiscard]] static Result<Scope<SceneRendererPipelines>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The number of pipelines Create made: the size of GetLayoutDescriptions (§8.5's logged and tested count).
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// The layout description of every pipeline of the set, for the CPU-only reflection test
		// ("Shaders: LayoutsMatchReflection", PipelineFactory.h).
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
	private:
		// SceneRenderer records with the set's pipelines, binding layouts and samplers (State, SceneRenderer.cpp).
		friend class SceneRenderer;
		// The back-reference, the pipelines, their binding layouts and the samplers (SceneRenderer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	class SceneRenderer
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SceneRenderer;
		};

		// Use Create.
		explicit SceneRenderer(ConstructionKey key);
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Creates the per-view buffers and the targets of the specification's size; it records with the shared `pipelines`
		// and creates none. `device`, `pipelines`, `cache` and `assets` are documented back-references that outlive the
		// renderer. Errors: those of the GraphicsDevice wrappers; a Gpu error is an out-of-memory creation, which a caller
		// creating the renderer at startup turns into FatalError(OutOfMemory) (§8.14 item 7).
		[[nodiscard]] static Result<Scope<SceneRenderer>> Create(GraphicsDevice& device, const SceneRendererPipelines& pipelines,
			GpuResourceCache& cache, AssetManager& assets, const SceneRendererSpecification& specification);

		// Recreates the size-dependent targets for `width` x `height` (both >= 1, asserted); no effect at the current size.
		// Errors: those of the target creation (Gpu: FatalError(OutOfMemory) for the caller, §8.14 item 7).
		[[nodiscard]] Status Resize(uint32_t width, uint32_t height);

		// Records the pass list for `snapshot` into `commandList` (open, asserted). The snapshot's camera viewport should
		// match the renderer's size; a different one renders at the renderer's size with the snapshot's projection. Uploads
		// the meshes it needs through the GpuResourceCache first. Afterwards GetFinalTexture holds the image. Errors: none
		// from rendering itself (a missing asset draws its placeholder); InvalidArgument for a non-finite matrix in the
		// snapshot (the draw is skipped and the error names the entity).
		[[nodiscard]] Status Render(nvrhi::ICommandList& commandList, const RenderSnapshot& snapshot);

		// LdrColor (RGBA8_UNORM, display-encoded values, §8.9) after the last Render: what BlitPass presents and
		// ViewportCapture reads back.
		[[nodiscard]] nvrhi::ITexture* GetFinalTexture() const;
		[[nodiscard]] uint32_t GetWidth() const;
		[[nodiscard]] uint32_t GetHeight() const;
		[[nodiscard]] const SceneRenderStats& GetLastStats() const;
	private:
		// The back-references (the shared pipelines among them), the targets, the per-view and material buffers and the
		// stats (SceneRenderer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
