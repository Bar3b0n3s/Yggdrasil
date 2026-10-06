#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>

// The engine's Dear ImGui renderer backend over NVRHI (Architecture §8.11), for Dear ImGui 1.92.9b (docking):
// ImGuiBackendFlags_RendererHasTextures and RendererHasVtxOffset (16-bit indices). It services ImDrawData::Textures
// (ImTextureStatus_WantCreate, WantUpdates and WantDestroy, then ImTextureData::SetTexID and SetStatus), uses one
// pipeline per target format with straight-alpha blending and per-command scissor rectangles, puts the projection in push
// constants (Shared/ImGuiConstants.h) and keeps dynamic vertex and index buffers per frame slot that grow geometrically.

namespace Engine {

	class GraphicsDevice;

	struct ImGuiRendererSpecification
	{
		// GraphicsSpecification::FramesInFlight: the number of buffer and retirement slots. At least 1.
		uint32_t FramesInFlight = 2;
	};

	// Not copyable or movable; main thread only (§4.11), with the ImGui context current that the renderer was created for.
	//
	// Textures: ImTextureID (ImU64) is a key into a slot map of {nvrhi::TextureHandle, mip, slice, cached binding set}.
	// ImGui's own textures (the font atlas) get keys when RenderDrawData services their WantCreate; other textures
	// (ImGui::Image of a viewport, a thumbnail) get one from AddTexture. A removed or destroyed entry keeps its handle until
	// the frame slot that last used it comes round again (BeginFrame), so a texture never dies under an in-flight
	// submission. Every texture is created through GraphicsDevice::CreateTexture and updated with writeTexture on the frame's
	// command list. A failed creation (out of memory, or --gpu-inject-fault=oom-texture) never crashes: the texture keeps
	// the invalid ImTextureID, its status becomes ImTextureStatus_OK so ImGui does not ask again every frame, the failure is
	// logged once, and draw commands that reference it are skipped.
	class ImGuiRenderer
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ImGuiRenderer;
		};

		// Use Create.
		explicit ImGuiRenderer(ConstructionKey key);
		// Calls DestroyTextures when the caller has not.
		~ImGuiRenderer();

		ImGuiRenderer(const ImGuiRenderer&) = delete;
		ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

		// Sets the backend flags and renderer name on the current context's ImGuiIO (a current context is required,
		// asserted) and creates the linear-clamp sampler and the binding layout. Pipelines are created per target format
		// on first use in RenderDrawData, through `pipelines` (validated against reflection, GetLayoutDescription).
		// `device` and `pipelines` are documented back-references that must outlive the renderer. Errors: those of
		// PipelineFactory and of the GraphicsDevice wrappers.
		[[nodiscard]] static Result<Scope<ImGuiRenderer>> Create(GraphicsDevice& device, PipelineFactory& pipelines,
			const ImGuiRendererSpecification& specification);

		// Starts the frame of `frameSlot` (FramePacer::GetFrameSlot): releases the texture handles whose last use was in
		// that slot's previous frame, which has completed.
		void BeginFrame(uint32_t frameSlot);

		// Services drawData.Textures, then records every draw list into `commandList` targeting `framebuffer` (one color
		// attachment; its format selects the pipeline): uploads vertices and indices, sets the projection from
		// DisplayPos/DisplaySize, and issues one draw per command with its scissor rectangle and texture (VtxOffset honoured).
		// User callbacks (ImDrawCallback_ResetRenderState included) are invoked as Dear ImGui specifies. Errors: Gpu when a
		// buffer or a pipeline for a new target format cannot be created (nothing is drawn then).
		[[nodiscard]] Status RenderDrawData(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, ImDrawData& drawData);

		// A key for drawing `texture` (mip `mipLevel`, slice `arraySlice`) with ImGui::Image. The renderer holds a handle
		// until RemoveTexture and the retirement of the last frame that used it.
		[[nodiscard]] ImTextureID AddTexture(nvrhi::ITexture& texture, uint32_t mipLevel = 0, uint32_t arraySlice = 0);
		// Forgets a key from AddTexture (unknown keys are a programmer error, asserted).
		void RemoveTexture(ImTextureID textureID);

		// Shutdown (§8.11): destroys every texture in ImGui::GetPlatformIO().Textures that the renderer created and drops
		// every AddTexture key, so the GPU resources return to their baseline. The caller has waited for the device to be
		// idle. Idempotent.
		void DestroyTextures();

		// Textures the renderer holds a handle to: ImGui's live textures, AddTexture keys and entries awaiting retirement.
		[[nodiscard]] size_t GetTextureCount() const;

		// The layout of the ImGui program for the CPU-only reflection test (PipelineFactory.h): set 0 with t0 Texture,
		// s0 Sampler and the push constants (ImGuiConstants).
		[[nodiscard]] static PipelineLayoutDescription GetLayoutDescription();
	};

}
