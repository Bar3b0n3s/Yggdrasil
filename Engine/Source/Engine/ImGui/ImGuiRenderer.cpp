#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiRenderer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/ImGuiConstants.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace Engine {

	// What io.BackendRendererName points at (Dear ImGui keeps the pointer).
	static constexpr const char* RendererName = "Engine NVRHI renderer";

	// The first capacities of the dynamic buffers, in elements; they double until a frame fits (§8.11).
	static constexpr size_t InitialVertexCapacity = 8192;
	static constexpr size_t InitialIndexCapacity = 16384;

	// ImDrawIdx is 16-bit by default (Vendor/imgui/VENDOR.md); large meshes are split by VtxOffset (RendererHasVtxOffset).
	static_assert(sizeof(ImDrawIdx) == 2, "the index buffer format is R16_UINT");
	// The vertex layout the ImGui program reads (Passes/ImGui.slang VertexInput: POSITION, TEXCOORD0, COLOR0).
	static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert is position, UV and a packed RGBA8 colour");

	namespace Utils {

		// The smallest capacity at least `required` reached by doubling from `current` (or from `initial` when smaller).
		static size_t GrowCapacity(size_t current, size_t initial, size_t required)
		{
			size_t capacity = std::max(current, initial);
			while (capacity < required)
				capacity *= 2;
			return capacity;
		}

		// vkCmdUpdateBuffer, which NVRHI's writeBuffer uses for small writes, writes whole 4-byte words, so a 16-bit index
		// upload is padded to an even count; the pad index is never drawn.
		static size_t PadIndexCount(size_t indexCount)
		{
			return (indexCount + 1) & ~static_cast<size_t>(1);
		}

	}

	ImGuiRenderer::ImGuiRenderer(ConstructionKey /*key*/)
	{
	}

	ImGuiRenderer::~ImGuiRenderer()
	{
		if (m_Context == nullptr)
			return;

		const bool isContextCurrent = ImGui::GetCurrentContext() == m_Context;
		ENGINE_CORE_ASSERT(isContextCurrent, "ImGuiRenderer destroyed while another Dear ImGui context (or none) is current");
		DestroyTextures();
		if (isContextCurrent)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.BackendRendererName = nullptr;
			io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
		}
	}

	Result<Scope<ImGuiRenderer>> ImGuiRenderer::Create(GraphicsDevice& device, PipelineFactory& pipelines,
		const ImGuiRendererSpecification& specification)
	{
		ImGuiContext* context = ImGui::GetCurrentContext();
		ENGINE_CORE_ASSERT(context != nullptr, "ImGuiRenderer::Create needs a current Dear ImGui context");
		if (context == nullptr)
			return MakeError(ErrorCode::InvalidState, "the ImGui renderer needs a current Dear ImGui context");
		if (specification.FramesInFlight == 0)
			return MakeError(ErrorCode::InvalidArgument, "the ImGui renderer needs at least one frame in flight");

		// The layout is checked against the ImGui program's reflection now, so missing or stale shaders fail at startup,
		// not at the first frame; PipelineFactory checks it again for every target format.
		ENGINE_TRY(WithContext(ValidatePipelineLayout(GetLayoutDescription(), pipelines.GetShaderLibrary()),
			"while creating the ImGui renderer"));

		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(nvrhi::SamplerHandle sampler, device.CreateSampler(samplerDesc));

		Scope<ImGuiRenderer> renderer = CreateScope<ImGuiRenderer>(ConstructionKey());
		renderer->m_Device = &device;
		renderer->m_PipelineFactory = &pipelines;
		renderer->m_FramesInFlight = specification.FramesInFlight;
		renderer->m_Sampler = std::move(sampler);
		renderer->m_Geometry.resize(specification.FramesInFlight);

		ImGuiIO& io = ImGui::GetIO();
		io.BackendRendererName = RendererName;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
		renderer->m_Context = context;
		return renderer;
	}

	void ImGuiRenderer::BeginFrame(uint32_t frameSlot)
	{
		ENGINE_CORE_ASSERT(frameSlot < m_FramesInFlight, "ImGuiRenderer::BeginFrame: frame slot {} of {}", frameSlot, m_FramesInFlight);
		m_FrameSlot = frameSlot;
		// The frame that last used this slot has completed (FramePacer::BeginFrame waited for it), so the textures retired
		// during it are no longer referenced by any submission.
		std::erase_if(m_RetiredTextures, [frameSlot](const RetiredTexture& retired)
		{
			return retired.FrameSlot == frameSlot;
		});
	}

	Status ImGuiRenderer::RenderDrawData(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, ImDrawData& drawData)
	{
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiRenderer::RenderDrawData with another Dear ImGui context current");

		// Creations and updates first: their uploads are recorded before the draws that sample them.
		if (drawData.Textures != nullptr)
		{
			for (ImTextureData* texture : *drawData.Textures)
			{
				if (texture->Status == ImTextureStatus_WantCreate)
					CreateImGuiTexture(commandList, *texture);
				else if (texture->Status == ImTextureStatus_WantUpdates)
					UpdateImGuiTexture(commandList, *texture);
			}
		}

		commandList.beginMarker("ImGui");
		const Status recorded = RecordDrawLists(commandList, framebuffer, drawData);
		commandList.endMarker();

		// Destructions last: Dear ImGui requests them for textures the frame no longer uses (UnusedFrames > 0), and the
		// handles stay in the retirement list until this frame slot comes round again.
		if (drawData.Textures != nullptr)
		{
			for (ImTextureData* texture : *drawData.Textures)
			{
				if (texture->Status == ImTextureStatus_WantDestroy)
					DestroyImGuiTexture(*texture);
			}
		}
		return recorded;
	}

	ImTextureID ImGuiRenderer::AddTexture(nvrhi::ITexture& texture, uint32_t mipLevel, uint32_t arraySlice)
	{
		return AllocateEntry(texture, mipLevel, arraySlice, false);
	}

	void ImGuiRenderer::RemoveTexture(ImTextureID textureID)
	{
		TextureEntry* entry = FindEntry(textureID);
		ENGINE_CORE_ASSERT(entry != nullptr && !entry->IsImGuiTexture,
			"ImGuiRenderer::RemoveTexture of an ImTextureID that AddTexture did not return or that was already removed");
		if (entry != nullptr && !entry->IsImGuiTexture)
			RetireEntry(*entry);
	}

	void ImGuiRenderer::DestroyTextures()
	{
		// Dear ImGui's textures first, so that their ImTextureData no longer name a key.
		const bool isContextCurrent = m_Context != nullptr && ImGui::GetCurrentContext() == m_Context;
		ENGINE_CORE_ASSERT(isContextCurrent, "ImGuiRenderer::DestroyTextures needs the renderer's Dear ImGui context to be current");
		if (isContextCurrent)
		{
			for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
			{
				TextureEntry* entry = FindEntry(texture->TexID);
				if (entry != nullptr && entry->IsImGuiTexture)
				{
					RetireEntry(*entry);
					texture->SetTexID(ImTextureID_Invalid);
					// The texture keeps its pixels, so SetStatus turns Destroyed into WantCreate: a renderer created later
					// for this context uploads it again.
					texture->SetStatus(ImTextureStatus_Destroyed);
				}
				else if (texture->Status == ImTextureStatus_OK && texture->TexID == ImTextureID_Invalid && texture->RefCount == 1)
				{
					// A texture whose creation failed (see the class comment): a later renderer may try again.
					texture->SetStatus(ImTextureStatus_Destroyed);
				}
			}
		}

		// Then every AddTexture key. The caller waited for the device to be idle, so nothing needs to stay retired.
		for (TextureEntry& entry : m_Textures)
		{
			if (entry.IsLive)
				RetireEntry(entry);
		}
		m_RetiredTextures.clear();
	}

	size_t ImGuiRenderer::GetTextureCount() const
	{
		const size_t liveCount = static_cast<size_t>(std::ranges::count_if(m_Textures, [](const TextureEntry& entry)
		{
			return entry.IsLive;
		}));
		return liveCount + m_RetiredTextures.size();
	}

	PipelineLayoutDescription ImGuiRenderer::GetLayoutDescription()
	{
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::All;
		layout.registerSpace = 0;
		layout.registerSpaceIsDescriptorSet = true;
		layout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ImGuiConstants)),
		};

		PipelineLayoutDescription description;
		description.Name = "ImGui";
		description.Program = "ImGui";
		description.Entries = { "VSMain", "PSMain" };
		description.BindingLayouts = { layout };
		return description;
	}

	void ImGuiRenderer::CreateImGuiTexture(nvrhi::ICommandList& commandList, ImTextureData& texture)
	{
		ENGINE_CORE_ASSERT(texture.TexID == ImTextureID_Invalid, "Dear ImGui asks to create texture {} that already has an ImTextureID", texture.UniqueID);
		ENGINE_CORE_ASSERT(texture.Width > 0 && texture.Height > 0, "Dear ImGui asks to create the empty texture {}", texture.UniqueID);

		nvrhi::TextureDesc desc;
		desc.width = static_cast<uint32_t>(texture.Width);
		desc.height = static_cast<uint32_t>(texture.Height);
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = std::format("ImGui.Texture{}", texture.UniqueID);

		Result<nvrhi::TextureHandle> created = m_Device->CreateTexture(desc);
		if (!created.has_value())
		{
			// Once per texture: the status becomes OK, so Dear ImGui does not ask again, and the invalid ImTextureID makes the
			// draws that use the texture skip (§8.14 item 8: a failed creation never crashes).
			ENGINE_CORE_ERROR("The ImGui renderer cannot create texture {} ({}x{}); draws that use it are skipped: {}", texture.UniqueID,
				texture.Width, texture.Height, created.error());
			texture.SetTexID(ImTextureID_Invalid);
			texture.SetStatus(ImTextureStatus_OK);
			return;
		}

		UploadPixels(commandList, **created, texture);
		texture.SetTexID(AllocateEntry(**created, 0, 0, true));
		texture.SetStatus(ImTextureStatus_OK);
	}

	void ImGuiRenderer::UpdateImGuiTexture(nvrhi::ICommandList& commandList, ImTextureData& texture)
	{
		// writeTexture replaces the whole subresource; the CPU copy holds every pixel, so the regions in texture.Updates are
		// uploaded together with the unchanged rest (§8.11: updated with writeTexture on the frame's command list).
		// A texture whose creation failed has no entry and stays without one.
		if (TextureEntry* entry = FindEntry(texture.TexID); entry != nullptr)
			UploadPixels(commandList, *entry->Texture, texture);
		texture.SetStatus(ImTextureStatus_OK);
	}

	void ImGuiRenderer::DestroyImGuiTexture(ImTextureData& texture)
	{
		if (TextureEntry* entry = FindEntry(texture.TexID); entry != nullptr && entry->IsImGuiTexture)
			RetireEntry(*entry);
		texture.SetTexID(ImTextureID_Invalid);
		texture.SetStatus(ImTextureStatus_Destroyed);
	}

	void ImGuiRenderer::UploadPixels(nvrhi::ICommandList& commandList, nvrhi::ITexture& destination, ImTextureData& texture)
	{
		if (texture.Format == ImTextureFormat_RGBA32)
		{
			commandList.writeTexture(&destination, 0, 0, texture.GetPixels(), static_cast<size_t>(texture.GetPitch()));
			return;
		}

		// Alpha8: white with the texture's alpha, the colour Dear ImGui expects from a font atlas sampled as RGBA, packed as
		// IM_COL32 packs an RGBA8 texel (the three colour bytes are all 0xFF, whatever their order).
		ENGINE_CORE_ASSERT(texture.Format == ImTextureFormat_Alpha8, "unknown ImTextureFormat {}", static_cast<int>(texture.Format));
		const size_t pixelCount = static_cast<size_t>(texture.Width) * static_cast<size_t>(texture.Height);
		const auto* alpha = static_cast<const unsigned char*>(texture.GetPixels());
		m_PixelStaging.resize(pixelCount);
		for (size_t i = 0; i < pixelCount; ++i)
			m_PixelStaging[i] = (static_cast<uint32_t>(alpha[i]) << IM_COL32_A_SHIFT) | 0x00FF'FFFFu;
		commandList.writeTexture(&destination, 0, 0, m_PixelStaging.data(), static_cast<size_t>(texture.Width) * sizeof(uint32_t));
	}

	Status ImGuiRenderer::RecordDrawLists(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, const ImDrawData& drawData)
	{
		const nvrhi::FramebufferInfoEx& target = framebuffer.getFramebufferInfo();
		// A minimized window has an empty display; there is nothing to draw.
		if (drawData.TotalVtxCount <= 0 || drawData.DisplaySize.x <= 0.0f || drawData.DisplaySize.y <= 0.0f || target.width == 0 || target.height == 0)
			return {};

		// Everything that can fail, before the first command (see the declaration).
		ENGINE_TRY_ASSIGN(const size_t pipelineIndex, GetTargetPipeline(target));
		const size_t vertexCount = static_cast<size_t>(drawData.TotalVtxCount);
		const size_t indexCount = Utils::PadIndexCount(static_cast<size_t>(drawData.TotalIdxCount));
		ENGINE_TRY(ReserveGeometry(vertexCount, indexCount));
		for (const ImDrawList* drawList : drawData.CmdLists)
		{
			for (const ImDrawCmd& command : drawList->CmdBuffer)
			{
				if (command.UserCallback != nullptr)
					continue;
				if (TextureEntry* entry = FindEntry(GetCommandTextureID(command)); entry != nullptr)
					ENGINE_TRY(CreateBindingSet(*entry, pipelineIndex));
			}
		}

		// One upload each for the frame's vertices and indices, into this frame slot's buffers.
		m_VertexStaging.clear();
		m_IndexStaging.clear();
		for (const ImDrawList* drawList : drawData.CmdLists)
		{
			m_VertexStaging.insert(m_VertexStaging.end(), drawList->VtxBuffer.begin(), drawList->VtxBuffer.end());
			m_IndexStaging.insert(m_IndexStaging.end(), drawList->IdxBuffer.begin(), drawList->IdxBuffer.end());
		}
		m_IndexStaging.resize(indexCount, 0);
		const GeometryBuffers& geometry = m_Geometry[m_FrameSlot];
		commandList.writeBuffer(geometry.Vertices, m_VertexStaging.data(), m_VertexStaging.size() * sizeof(ImDrawVert));
		commandList.writeBuffer(geometry.Indices, m_IndexStaging.data(), m_IndexStaging.size() * sizeof(ImDrawIdx));

		// ImGui's display rectangle maps onto the whole target; the clip rectangles scale the same way (FramebufferScale when
		// the target has the UI's framebuffer size).
		const float targetWidth = static_cast<float>(target.width);
		const float targetHeight = static_cast<float>(target.height);
		const ImVec2 clipOffset = drawData.DisplayPos;
		const ImVec2 clipScale(targetWidth / drawData.DisplaySize.x, targetHeight / drawData.DisplaySize.y);

		// NVRHI's Vulkan viewport has a negative height (Direct3D's convention), so clip-space +Y is the top of the target.
		ImGuiConstants constants{};
		constants.Scale = Float2(2.0f / drawData.DisplaySize.x, -2.0f / drawData.DisplaySize.y);
		constants.Translate = Float2(-1.0f - drawData.DisplayPos.x * constants.Scale.x, 1.0f - drawData.DisplayPos.y * constants.Scale.y);

		const TargetPipeline& pipeline = m_TargetPipelines[pipelineIndex];
		nvrhi::GraphicsState state;
		state.pipeline = pipeline.Pipeline.Pipeline;
		state.framebuffer = &framebuffer;
		state.viewport.addViewport(nvrhi::Viewport(targetWidth, targetHeight));
		state.viewport.addScissorRect(nvrhi::Rect(0, 0, 0, 0));
		state.bindings.push_back(nullptr);
		state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(geometry.Vertices).setSlot(0).setOffset(0));
		state.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(geometry.Indices).setFormat(nvrhi::Format::R16_UINT).setOffset(0));

		// The graphics state is set again when the texture or the scissor changes, and after a user callback, which may
		// have changed anything. NVRHI requires the push constants after every setGraphicsState.
		bool isStateCurrent = false;
		uint32_t globalVertexOffset = 0;
		uint32_t globalIndexOffset = 0;
		for (const ImDrawList* drawList : drawData.CmdLists)
		{
			for (const ImDrawCmd& command : drawList->CmdBuffer)
			{
				if (command.UserCallback != nullptr)
				{
					// ImDrawCallback_ResetRenderState asks for the renderer's state back; it is not a function to call.
					if (command.UserCallback != ImDrawCallback_ResetRenderState)
						command.UserCallback(drawList, &command);
					isStateCurrent = false;
					continue;
				}

				const float clipMinX = std::max((command.ClipRect.x - clipOffset.x) * clipScale.x, 0.0f);
				const float clipMinY = std::max((command.ClipRect.y - clipOffset.y) * clipScale.y, 0.0f);
				const float clipMaxX = std::min((command.ClipRect.z - clipOffset.x) * clipScale.x, targetWidth);
				const float clipMaxY = std::min((command.ClipRect.w - clipOffset.y) * clipScale.y, targetHeight);
				if (clipMaxX <= clipMinX || clipMaxY <= clipMinY || command.ElemCount == 0)
					continue;

				TextureEntry* entry = FindEntry(GetCommandTextureID(command));
				if (entry == nullptr || pipelineIndex >= entry->BindingSets.size() || entry->BindingSets[pipelineIndex] == nullptr)
					continue; // a texture whose creation failed, or one added by a user callback during this recording

				const nvrhi::Rect scissor(static_cast<int>(clipMinX), static_cast<int>(clipMaxX), static_cast<int>(clipMinY),
					static_cast<int>(clipMaxY));
				nvrhi::IBindingSet* bindingSet = entry->BindingSets[pipelineIndex];
				if (!isStateCurrent || state.bindings[0] != bindingSet || state.viewport.scissorRects[0] != scissor)
				{
					state.bindings[0] = bindingSet;
					state.viewport.scissorRects[0] = scissor;
					commandList.setGraphicsState(state);
					commandList.setPushConstants(&constants, sizeof(constants));
					isStateCurrent = true;
				}

				nvrhi::DrawArguments arguments;
				arguments.vertexCount = command.ElemCount;
				arguments.startIndexLocation = command.IdxOffset + globalIndexOffset;
				arguments.startVertexLocation = command.VtxOffset + globalVertexOffset;
				commandList.drawIndexed(arguments);
			}
			globalVertexOffset += static_cast<uint32_t>(drawList->VtxBuffer.Size);
			globalIndexOffset += static_cast<uint32_t>(drawList->IdxBuffer.Size);
		}
		return {};
	}

	Result<size_t> ImGuiRenderer::GetTargetPipeline(const nvrhi::FramebufferInfo& framebuffer)
	{
		for (size_t i = 0; i < m_TargetPipelines.size(); ++i)
		{
			if (m_TargetPipelines[i].Framebuffer == framebuffer)
				return i;
		}

		// Straight alpha (§8.11): ImGui's vertex colours and the targets hold display-encoded values (§8.9), blended as is.
		GraphicsPipelineSpecification specification;
		specification.Layout = GetLayoutDescription();
		specification.VertexAttributes = {
			nvrhi::VertexAttributeDesc()
				.setName("POSITION")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(static_cast<uint32_t>(offsetof(ImDrawVert, pos)))
				.setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc()
				.setName("TEXCOORD")
				.setFormat(nvrhi::Format::RG32_FLOAT)
				.setOffset(static_cast<uint32_t>(offsetof(ImDrawVert, uv)))
				.setElementStride(sizeof(ImDrawVert)),
			nvrhi::VertexAttributeDesc()
				.setName("COLOR")
				.setFormat(nvrhi::Format::RGBA8_UNORM)
				.setOffset(static_cast<uint32_t>(offsetof(ImDrawVert, col)))
				.setElementStride(sizeof(ImDrawVert)),
		};
		specification.Primitive = nvrhi::PrimitiveType::TriangleList;
		nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
		blend.enableBlend()
			.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
			.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
			.setBlendOp(nvrhi::BlendOp::Add)
			.setSrcBlendAlpha(nvrhi::BlendFactor::One)
			.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
			.setBlendOpAlpha(nvrhi::BlendOp::Add);
		specification.RenderState.rasterState.setCullNone().enableScissor();
		specification.RenderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();
		specification.Framebuffer = framebuffer;

		ENGINE_TRY_ASSIGN(GraphicsPipeline pipeline,
			WithContext(m_PipelineFactory->CreateGraphicsPipeline(specification), "while creating the ImGui pipeline for a new target format"));
		m_TargetPipelines.push_back({ .Framebuffer = framebuffer, .Pipeline = std::move(pipeline) });
		return m_TargetPipelines.size() - 1;
	}

	Status ImGuiRenderer::ReserveGeometry(size_t vertexCount, size_t indexCount)
	{
		GeometryBuffers& geometry = m_Geometry[m_FrameSlot];
		if (geometry.Vertices == nullptr || geometry.VertexCapacity < vertexCount)
		{
			const size_t capacity = Utils::GrowCapacity(geometry.VertexCapacity, InitialVertexCapacity, vertexCount);
			nvrhi::BufferDesc desc;
			desc.byteSize = capacity * sizeof(ImDrawVert);
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = std::format("ImGui.Vertices{}", m_FrameSlot);
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, m_Device->CreateBuffer(desc));
			geometry.Vertices = std::move(buffer);
			geometry.VertexCapacity = capacity;
		}
		if (geometry.Indices == nullptr || geometry.IndexCapacity < indexCount)
		{
			const size_t capacity = Utils::GrowCapacity(geometry.IndexCapacity, InitialIndexCapacity, indexCount);
			nvrhi::BufferDesc desc;
			desc.byteSize = capacity * sizeof(ImDrawIdx);
			desc.isIndexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			desc.debugName = std::format("ImGui.Indices{}", m_FrameSlot);
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, m_Device->CreateBuffer(desc));
			geometry.Indices = std::move(buffer);
			geometry.IndexCapacity = capacity;
		}
		return {};
	}

	Status ImGuiRenderer::CreateBindingSet(TextureEntry& entry, size_t pipelineIndex)
	{
		if (entry.BindingSets.size() <= pipelineIndex)
			entry.BindingSets.resize(pipelineIndex + 1);
		if (entry.BindingSets[pipelineIndex] != nullptr)
			return {};

		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, entry.Texture, nvrhi::Format::UNKNOWN,
				nvrhi::TextureSubresourceSet(entry.MipLevel, 1, entry.ArraySlice, 1)),
			nvrhi::BindingSetItem::Sampler(0, m_Sampler),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(ImGuiConstants)),
		};
		const TargetPipeline& pipeline = m_TargetPipelines[pipelineIndex];
		ENGINE_TRY_ASSIGN(nvrhi::BindingSetHandle bindingSet, m_Device->CreateBindingSet(desc, *pipeline.Pipeline.BindingLayouts[0]));
		entry.BindingSets[pipelineIndex] = std::move(bindingSet);
		return {};
	}

	ImTextureID ImGuiRenderer::GetCommandTextureID(const ImDrawCmd& command)
	{
		return command.TexRef._TexData != nullptr ? command.TexRef._TexData->TexID : command.TexRef._TexID;
	}

	ImTextureID ImGuiRenderer::AllocateEntry(nvrhi::ITexture& texture, uint32_t mipLevel, uint32_t arraySlice, bool isImGuiTexture)
	{
		size_t index = m_Textures.size();
		if (!m_FreeTextureSlots.empty())
		{
			index = m_FreeTextureSlots.back();
			m_FreeTextureSlots.pop_back();
		}
		else
		{
			ENGINE_CORE_VERIFY(index < std::numeric_limits<uint32_t>::max(), "the ImGui renderer's texture slot map is full");
			m_Textures.emplace_back();
		}

		TextureEntry& entry = m_Textures[index];
		entry.Texture = &texture;
		entry.MipLevel = mipLevel;
		entry.ArraySlice = arraySlice;
		entry.BindingSets.clear();
		entry.IsLive = true;
		entry.IsImGuiTexture = isImGuiTexture;
		return MakeKey(index);
	}

	ImGuiRenderer::TextureEntry* ImGuiRenderer::FindEntry(ImTextureID textureID)
	{
		if (textureID == ImTextureID_Invalid)
			return nullptr;
		const uint64_t slot = textureID & 0xFFFF'FFFFu;
		const uint32_t generation = static_cast<uint32_t>(textureID >> 32);
		if (slot == 0 || slot > m_Textures.size())
			return nullptr;
		TextureEntry& entry = m_Textures[static_cast<size_t>(slot - 1)];
		if (!entry.IsLive || entry.Generation != generation)
			return nullptr;
		return &entry;
	}

	void ImGuiRenderer::RetireEntry(TextureEntry& entry)
	{
		m_RetiredTextures.push_back({ .Texture = std::move(entry.Texture), .BindingSets = std::move(entry.BindingSets), .FrameSlot = m_FrameSlot });
		entry.Texture = nullptr;
		entry.BindingSets.clear();
		entry.IsLive = false;
		entry.IsImGuiTexture = false;
		++entry.Generation;
		m_FreeTextureSlots.push_back(static_cast<uint32_t>(&entry - m_Textures.data()));
	}

	ImTextureID ImGuiRenderer::MakeKey(size_t index) const
	{
		return (static_cast<uint64_t>(m_Textures[index].Generation) << 32) | static_cast<uint64_t>(index + 1);
	}

}
