#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Shared/ViewConstants.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>

// The clear-and-triangle view of the Graphics foundation (Roadmap M5): one counter-clockwise triangle drawn through the
// Triangle program (Resources/Shaders/Passes/Triangle.slang) with ViewConstants from a fixed orthographic, reverse-Z
// camera (§8.3). It is what viewport.screenshot renders until the scene renderer arrives (M7), the golden image
// "Triangle", and the subject of "Rasterizer: CCW triangle survives back-face culling": with back-face culling and
// rasterState.frontCounterClockwise = true the triangle must stay visible, which proves the projection's Y convention and
// the pipelines' winding convention agree. NVRHI's Vulkan backend performs the Vulkan Y flip itself (every viewport has a
// negative height), so the projection keeps clip-space +Y up.

namespace Engine {

	class GraphicsDevice;

	struct TrianglePassSpecification
	{
		// The target the pipeline is created for (one color format, optionally a depth format; sample count 1).
		nvrhi::FramebufferInfo Framebuffer{};
		// Back for the winding test and the golden image; None draws either winding.
		nvrhi::RasterCullMode CullMode = nvrhi::RasterCullMode::Back;
	};

	// The clear colour of the fixed scene, RGBA as stored in the UNORM target (display-encoded values, §8.9; a clear writes
	// them unconverted). The triangle's own colours are linear and encoded by the shader.
	inline constexpr std::array<float, 4> TriangleClearColor = { 0.05f, 0.08f, 0.12f, 1.0f };

	// Not copyable or movable; main thread only (§4.11).
	class TrianglePass
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class TrianglePass;
		};

		// Use Create.
		explicit TrianglePass(ConstructionKey key);
		~TrianglePass();

		TrianglePass(const TrianglePass&) = delete;
		TrianglePass& operator=(const TrianglePass&) = delete;

		// Creates the pipeline (PipelineFactory::CreateGraphicsPipeline with GetLayoutDescription), the ViewConstants buffer
		// and its binding set. `device` is a documented back-reference that must outlive the pass. Errors: those of
		// PipelineFactory and of the GraphicsDevice wrappers.
		[[nodiscard]] static Result<Scope<TrianglePass>> Create(GraphicsDevice& device, PipelineFactory& pipelines,
			const TrianglePassSpecification& specification);

		// Records into `commandList`: a clear of the framebuffer's color attachment to TriangleClearColor (and of its depth
		// attachment to 0), the ViewConstants of MakeViewConstants for the framebuffer's size, and the draw. The framebuffer
		// must match the specification's FramebufferInfo (asserted).
		void Render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer);

		// The pipeline's layout description for the CPU-only reflection test (PipelineFactory.h): set 0 with b0 ViewConstants.
		[[nodiscard]] static PipelineLayoutDescription GetLayoutDescription();

		// The fixed camera for a `width` x `height` target (both > 0, asserted): orthographic with OrthoHalfExtents
		// (aspect, 1), Near 0.1, Far 100, at (0, 0, 5) looking down -Z, reverse-Z (§8.3) with clip-space +Y up (NVRHI's
		// viewport performs the Vulkan Y flip), ProjectionKind orthographic. Pure and deterministic.
		[[nodiscard]] static ViewConstants MakeViewConstants(uint32_t width, uint32_t height);
	private:
		TrianglePassSpecification m_Specification;
		GraphicsPipeline m_Pipeline;
		nvrhi::BufferHandle m_ViewConstantsBuffer;
		nvrhi::BindingSetHandle m_BindingSet;
	};

}
