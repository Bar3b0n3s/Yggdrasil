#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// The output stage's upscale (RenderStage::Output; M18, C18.1, R-2D-PIXEL; Docs/Decisions/0029-m18-contract.md decision 6):
// the scene image (the scene-resolution LDR target after Tonemap, FXAA and the scene overlays) drawn into the
// view-resolution OutputColor target at the integer scale of a RenderOutputMapping, with point sampling, centred in a
// letterbox. Two pipelines, one program ("Upscale", its declaration in RenderPassDeclaration.h):
//   - Color: clears OutputColor to the letterbox colour (linear, encoded with the sRGB OETF like a clear colour), then draws
//     the scaled scene rectangle (the mapping's offset and Scale times the scene size) reading each scene texel with
//     texelFetch at floor(viewPixel - Offset) / Scale, so every scene pixel becomes exactly a Scale x Scale block of
//     identical view pixels (values copied unconverted: both targets hold display-encoded values);
//   - Depth: when an editor overlay needs depth, clears OutputDepth (SceneDepthFormat at the view size) to 0 (the far
//     plane, reverse-Z) and writes the scene depth into the same rectangle the same way (SV_Depth), so the view-resolution
//     editor overlays test against the depth of the pixel they cover.
// Viewport and scissor: the viewport is the scaled scene rectangle itself, which a cropped scene (larger than the view:
// Scale 1, negative offsets, RenderOutputMapping) places partly outside the view; the scissor is that rectangle intersected
// with the view, [0, ViewWidth) x [0, ViewHeight), since Vulkan refuses a negative scissor offset
// (VUID-vkCmdSetScissor-x-00595) and NVRHI passes the rectangle through unclamped. The EditorOverlay stage draws with the
// same viewport and scissor.
// The pass is recorded only when the mapping is not the identity: with the default scene resolution the scene targets are
// the view's and the editor overlays draw straight into the scene's LDR target, as before the output stage.
//
// Created once per device at startup and owned by SceneRendererPipelines (through CreatePipelines); the pass keeps no
// binding sets: each Record takes them from the view's PassBindingCache. Main thread only; not copyable or movable.

namespace Engine {

	struct RenderPassCounters;

	class GraphicsDevice;

	struct UpscalePassInputs
	{
		// The scene image: LdrColorFormat at the mapping's scene size, shader-readable; never null.
		nvrhi::ITexture* Source = nullptr;
		// OutputColor's framebuffer (GetColorFramebufferInfo) at the mapping's view size; never null.
		nvrhi::IFramebuffer* Color = nullptr;
		// Optional: SceneDepth at the scene size, shader-readable, and OutputDepth's framebuffer (GetDepthFramebufferInfo) at
		// the view size; both null, or both set, for the depth upscale.
		nvrhi::ITexture* SourceDepth = nullptr;
		nvrhi::IFramebuffer* Depth = nullptr;
		RenderOutputMapping Mapping{};              // not the identity (asserted)
		glm::vec3 LetterboxColor = glm::vec3(0.0f); // RenderOutputSettings::LetterboxColor
	};

	class UpscalePass
	{
	public:
		// Color and Depth.
		static constexpr uint32_t PipelineCount = 2;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class UpscalePass;
		};

		// Use Create.
		explicit UpscalePass(ConstructionKey key);
		~UpscalePass();

		UpscalePass(const UpscalePass&) = delete;
		UpscalePass& operator=(const UpscalePass&) = delete;

		// Creates the two pipelines (checked against their reflection). `device` is a documented back-reference that outlives
		// the pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (Gpu: an out-of-memory creation, §8.14
		// item 7).
		[[nodiscard]] static Result<Scope<UpscalePass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout descriptions of both pipelines, for "Shaders: LayoutsMatchReflection".
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// OutputColor: LdrColorFormat, no depth.
		[[nodiscard]] static nvrhi::FramebufferInfo GetColorFramebufferInfo();
		// OutputDepth: SceneDepthFormat, no colour.
		[[nodiscard]] static nvrhi::FramebufferInfo GetDepthFramebufferInfo();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the upscale of `inputs` (asserted: sizes match the mapping, the mapping is not the identity), with binding
		// sets from the view's `bindings`, adding its draws to `counters`. Errors: those of creating a binding set for new
		// resources (Gpu).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const UpscalePassInputs& inputs,
			RenderPassCounters& counters);
	private:
		// The back-reference and the pipelines (UpscalePass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
