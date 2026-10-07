#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

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
		// asserted), checks GetLayoutDescription against the ImGui program's reflection (ValidatePipelineLayout, so stale or
		// missing shaders fail here) and creates the linear-clamp sampler. Pipelines, each with the binding layout
		// PipelineFactory creates for it, are created per target format on first use in RenderDrawData, through
		// `pipelines`; binding sets are cached per texture and pipeline. `device` and `pipelines` are documented
		// back-references that must outlive the renderer. Errors: InvalidState without a current Dear ImGui context (also
		// asserted); InvalidArgument for FramesInFlight 0; those of ValidatePipelineLayout and of the GraphicsDevice wrappers.
		[[nodiscard]] static Result<Scope<ImGuiRenderer>> Create(GraphicsDevice& device, PipelineFactory& pipelines,
			const ImGuiRendererSpecification& specification);

		// Starts the frame of `frameSlot` (FramePacer::GetFrameSlot): releases the texture handles whose last use was in
		// that slot's previous frame, which has completed.
		void BeginFrame(uint32_t frameSlot);

		// Services drawData.Textures, then records every draw list into `commandList` targeting `framebuffer` (one color
		// attachment; its format selects the pipeline): uploads vertices and indices, sets the projection from
		// DisplayPos/DisplaySize, and issues one draw per command with its scissor rectangle and texture (VtxOffset honoured).
		// User callbacks (ImDrawCallback_ResetRenderState included) are invoked as Dear ImGui specifies. Errors: Gpu when a
		// buffer, a binding set or a pipeline for a new target format cannot be created (nothing is drawn then).
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
	private:
		// One slot of the texture slot map. A key (ImTextureID) is (Generation << 32) | (index + 1), so 0 is never a key and
		// a key whose slot was freed and reused no longer matches.
		struct TextureEntry
		{
			nvrhi::TextureHandle Texture{};
			uint32_t MipLevel = 0;
			uint32_t ArraySlice = 0;
			// One binding set per target pipeline (index into m_TargetPipelines), created on first use: every pipeline comes
			// from PipelineFactory with binding layouts of its own, and a binding set is bound only with its own layout.
			std::vector<nvrhi::BindingSetHandle> BindingSets{};
			uint32_t Generation = 0;
			bool IsLive = false;
			bool IsImGuiTexture = false; // created for an ImTextureData (WantCreate), not by AddTexture
		};

		// A texture handle (and its binding sets) kept until the frame slot it was removed in comes round again.
		struct RetiredTexture
		{
			nvrhi::TextureHandle Texture{};
			std::vector<nvrhi::BindingSetHandle> BindingSets{};
			uint32_t FrameSlot = 0;
		};

		// The pipeline for one target format (§8.11: one pipeline per target format).
		struct TargetPipeline
		{
			nvrhi::FramebufferInfo Framebuffer{};
			GraphicsPipeline Pipeline{};
		};

		// The dynamic vertex and index buffers of one frame slot, grown geometrically; capacities in elements.
		struct GeometryBuffers
		{
			nvrhi::BufferHandle Vertices{};
			nvrhi::BufferHandle Indices{};
			size_t VertexCapacity = 0;
			size_t IndexCapacity = 0;
		};

		// Texture requests (§8.11): WantCreate and WantUpdates before the draws, WantDestroy after them.
		void CreateImGuiTexture(nvrhi::ICommandList& commandList, ImTextureData& texture);
		void UpdateImGuiTexture(nvrhi::ICommandList& commandList, ImTextureData& texture);
		void DestroyImGuiTexture(ImTextureData& texture);
		// Uploads the whole of `texture`'s pixels (RGBA8; Alpha8 expanded to white with alpha) into `destination`.
		void UploadPixels(nvrhi::ICommandList& commandList, nvrhi::ITexture& destination, ImTextureData& texture);

		// Records the draw lists (after the texture requests). Everything that can fail (the pipeline, the buffers, the
		// binding sets) is created before the first command is recorded, so a frame is drawn completely or not at all.
		// Errors: Gpu from those creations.
		[[nodiscard]] Status RecordDrawLists(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, const ImDrawData& drawData);
		// The index into m_TargetPipelines of the pipeline for `framebuffer`'s format, created on first use.
		[[nodiscard]] Result<size_t> GetTargetPipeline(const nvrhi::FramebufferInfo& framebuffer);
		// Grows the current frame slot's buffers to hold `vertexCount` vertices and `indexCount` indices.
		[[nodiscard]] Status ReserveGeometry(size_t vertexCount, size_t indexCount);
		// Creates the binding set of `entry` for target pipeline `pipelineIndex` unless it exists.
		[[nodiscard]] Status CreateBindingSet(TextureEntry& entry, size_t pipelineIndex);
		// The ImTextureID a draw command samples, read without ImDrawCmd::GetTexID's assertion: the texture of a failed
		// creation keeps ImTextureID_Invalid, and its draws are skipped rather than asserted on.
		[[nodiscard]] static ImTextureID GetCommandTextureID(const ImDrawCmd& command);

		// Slot map operations.
		[[nodiscard]] ImTextureID AllocateEntry(nvrhi::ITexture& texture, uint32_t mipLevel, uint32_t arraySlice, bool isImGuiTexture);
		// The live entry of `textureID`, or nullptr for ImTextureID_Invalid and for keys that are not (or no longer) live.
		[[nodiscard]] TextureEntry* FindEntry(ImTextureID textureID);
		// Moves the entry's handles to the retirement list of the current frame slot and frees the slot.
		void RetireEntry(TextureEntry& entry);
		[[nodiscard]] ImTextureID MakeKey(size_t index) const;
	private:
		GraphicsDevice* m_Device = nullptr;           // documented back-reference
		PipelineFactory* m_PipelineFactory = nullptr; // documented back-reference
		ImGuiContext* m_Context = nullptr;            // the context Create configured
		uint32_t m_FramesInFlight = 0;
		uint32_t m_FrameSlot = 0;
		nvrhi::SamplerHandle m_Sampler{};
		std::vector<TextureEntry> m_Textures{};
		std::vector<uint32_t> m_FreeTextureSlots{};
		std::vector<RetiredTexture> m_RetiredTextures{};
		std::vector<TargetPipeline> m_TargetPipelines{};
		std::vector<GeometryBuffers> m_Geometry{}; // one per frame slot
		// Reused per frame: the draw lists' vertices and indices gathered for one upload each, and Alpha8 pixels expanded.
		std::vector<ImDrawVert> m_VertexStaging{};
		std::vector<ImDrawIdx> m_IndexStaging{};
		std::vector<uint32_t> m_PixelStaging{};
	};

}
