#include "EnginePCH.h"
#include "Engine/Renderer/QuadBatcher.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/FogConstants.h"
#include "Shared/QuadVertex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Record lays the quads out on the CPU into one vertex list (four QuadVertex per quad, in the order of QuadVertex.h's
// corners), cutting it into batches wherever the texture or the sampler changes (and, for the Normals target, the normal
// map), and at every MaxQuadsPerRecord quads, so the quads keep their order and no batch crosses a chunk of the vertex
// buffer's capacity; then it uploads the list one chunk at a time and draws each chunk's batches with one indexed draw each
// over the static index buffer (0, 1, 2, 0, 2, 3 per quad, absolute vertex numbers, so a batch starts at its first quad's
// sixth index within its chunk) before it writes the next chunk (M21, C21.2, Docs/Decisions/0032-m21-contract.md decision
// 24: NVRHI's write tracks the buffer's state, so the write after the chunk's draws waits for them). The index buffer grows
// with the vertex buffer's capacity and is written through the recording command list the first time a capacity is used,
// so creating the batcher records no command list.
//
// Four binding layouts: Ldr and M22's LdrDepth bind the texture and the sampler; M21's Normals (decision 22) also binds a
// second texture, the Normals batch's normal map (the white texture without one, with the push constants' HasNormalMap 0;
// NormalMapTwoChannel 1 for a map that stores x and y only, M23's milestone review); M32's fogged targets (Hdr and M26's
// HdrAdditive, then M21's HdrLit with its light accumulation as the second texture) add the view's fog constants at b1
// (Docs/Decisions/0047-m32-fog-ssr.md decision 5): QuadBatchInputs::Fog, or the batcher's own neutral buffer (Enabled 0)
// for a record without it, so every set of a fogged target binds one.

namespace Engine {

	namespace Utils {

		constexpr std::string_view QuadProgram = "Quad";
		// Indices of the six pipelines (QuadBatchTarget's values and GetLayoutDescriptions' order).
		constexpr size_t QuadHdrPipeline = 0;
		constexpr size_t QuadLdrPipeline = 1;
		constexpr size_t QuadHdrLitPipeline = 2;
		constexpr size_t QuadNormalsPipeline = 3;
		constexpr size_t QuadLdrDepthPipeline = 4;    // M22 (C22.3, Docs/Decisions/0033-m22-contract.md decision 57)
		constexpr size_t QuadHdrAdditivePipeline = 5; // M26 (C26.1, Docs/Decisions/0037-m26-contract.md decision 7)
		static_assert(static_cast<size_t>(QuadBatchTarget::Hdr) == QuadHdrPipeline && static_cast<size_t>(QuadBatchTarget::Ldr) == QuadLdrPipeline
			&& static_cast<size_t>(QuadBatchTarget::HdrLit) == QuadHdrLitPipeline && static_cast<size_t>(QuadBatchTarget::Normals) == QuadNormalsPipeline
			&& static_cast<size_t>(QuadBatchTarget::LdrDepth) == QuadLdrDepthPipeline
			&& static_cast<size_t>(QuadBatchTarget::HdrAdditive) == QuadHdrAdditivePipeline);
		static_assert(QuadHdrAdditivePipeline + 1 == QuadBatcher::PipelineCount);
		// The first capacity of the vertex and index buffers, in quads; it doubles until a record fits, up to
		// QuadBatcher::MaxQuadsPerRecord.
		constexpr uint32_t InitialQuadCapacity = 1024;
		constexpr uint32_t VerticesPerQuad = 4;
		constexpr uint32_t IndicesPerQuad = 6;
		// A quad's two triangles over its corners (top left, top right, bottom right, bottom left).
		constexpr std::array<uint32_t, IndicesPerQuad> QuadCornerIndices = { 0, 1, 2, 0, 2, 3 };

		// The register of the fog constants (b1: the push constants hold slot 0).
		constexpr uint32_t QuadFogConstantsSlot = 1;

		// Whether `target` binds the lit layout's second texture (QuadLitTexture, t1).
		static bool IsLitQuadTarget(QuadBatchTarget target)
		{
			return target == QuadBatchTarget::HdrLit || target == QuadBatchTarget::Normals;
		}

		// M32: whether `target` meets the view's fog (its fragment entry reads the fog constants at b1): the HDR targets.
		static bool IsFoggedQuadTarget(QuadBatchTarget target)
		{
			return target == QuadBatchTarget::Hdr || target == QuadBatchTarget::HdrLit || target == QuadBatchTarget::HdrAdditive;
		}

		// A quad pipeline's layout: the texture and the sampler, for the lit entries the second texture, and for the fogged ones
		// (M32) the fog constants; the lit and fogged entries draw with VSLit, which passes the batch-space position on.
		static PipelineLayoutDescription MakeQuadDescription(std::string name, std::string fragmentEntry, QuadBatchTarget target)
		{
			const bool lit = IsLitQuadTarget(target);
			const bool fogged = IsFoggedQuadTarget(target);
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0) };
			if (lit)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(1));
			layout.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(0));
			if (fogged)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::ConstantBuffer(QuadFogConstantsSlot));
			layout.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(QuadConstants)));
			PipelineLayoutDescription description{
				.Name = std::move(name),
				.Program = std::string(QuadProgram),
				.Entries = { lit || fogged ? "VSLit" : "VSMain", std::move(fragmentEntry) },
				.BindingLayouts = { layout },
			};
			if (fogged)
				description.ConstantBuffers = { { .Set = 0, .Register = QuadFogConstantsSlot, .ByteSize = sizeof(FogConstants) } };
			return description;
		}

		// LdrDepth shades as Ldr does (its fragment entry); only its depth state differs. M26's HdrAdditive shades as Hdr does;
		// only its blend state (and, M32, its fog mode) differs.
		static std::vector<PipelineLayoutDescription> MakeQuadDescriptions()
		{
			return { MakeQuadDescription("QuadHdr", "PSHdr", QuadBatchTarget::Hdr), MakeQuadDescription("QuadLdr", "PSLdr", QuadBatchTarget::Ldr),
				MakeQuadDescription("QuadHdrLit", "PSHdrLit", QuadBatchTarget::HdrLit),
				MakeQuadDescription("QuadNormals", "PSNormals", QuadBatchTarget::Normals),
				MakeQuadDescription("QuadLdrDepth", "PSLdr", QuadBatchTarget::LdrDepth),
				MakeQuadDescription("QuadHdrAdditive", "PSHdr", QuadBatchTarget::HdrAdditive) };
		}

		static std::vector<nvrhi::VertexAttributeDesc> MakeQuadVertexAttributes()
		{
			constexpr uint32_t Stride = sizeof(QuadVertex);
			const auto attribute = [](const char* name, nvrhi::Format format, size_t offset)
			{
				return nvrhi::VertexAttributeDesc().setName(name).setFormat(format).setOffset(static_cast<uint32_t>(offset)).setElementStride(Stride);
			};
			return {
				attribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(QuadVertex, Position)),
				attribute("TEXCOORD", nvrhi::Format::RG32_FLOAT, offsetof(QuadVertex, TexCoord)),
				attribute("COLOR", nvrhi::Format::RGBA32_FLOAT, offsetof(QuadVertex, Color)),
			};
		}

		// A quad pipeline: double-sided quads with straight alpha (as TextRenderer; the Normals one without blending, M26's
		// HdrAdditive adding the colour times its alpha and keeping the target's alpha), no depth writes; the Hdr, HdrLit,
		// Normals, LdrDepth and HdrAdditive ones test against SceneDepth (reverse-Z, GreaterOrEqual), the Ldr one draws on top.
		static GraphicsPipelineSpecification MakeQuadSpecification(PipelineLayoutDescription layout, QuadBatchTarget target,
			const std::vector<nvrhi::BindingLayoutHandle>& sharedLayouts)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.SharedBindingLayouts = sharedLayouts;
			specification.VertexAttributes = MakeQuadVertexAttributes();
			specification.Primitive = nvrhi::PrimitiveType::TriangleList;
			if (target == QuadBatchTarget::HdrAdditive)
			{
				nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
				blend.enableBlend()
					.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
					.setDestBlend(nvrhi::BlendFactor::One)
					.setBlendOp(nvrhi::BlendOp::Add)
					.setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
					.setDestBlendAlpha(nvrhi::BlendFactor::One)
					.setBlendOpAlpha(nvrhi::BlendOp::Add);
			}
			else if (target != QuadBatchTarget::Normals)
			{
				nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
				blend.enableBlend()
					.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
					.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
					.setBlendOp(nvrhi::BlendOp::Add)
					.setSrcBlendAlpha(nvrhi::BlendFactor::One)
					.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
					.setBlendOpAlpha(nvrhi::BlendOp::Add);
			}
			specification.RenderState.rasterState.setCullNone().enableDepthClip();
			specification.RenderState.depthStencilState.setDepthTestEnable(target != QuadBatchTarget::Ldr)
				.setDepthWriteEnable(false)
				.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
				.setStencilEnable(false);
			specification.Framebuffer = QuadBatcher::GetFramebufferInfo(target);
			return specification;
		}

		static bool IsQuadFinite(const QuadDraw& quad)
		{
			const auto finite = [](float value)
			{
				return std::isfinite(value);
			};
			for (const glm::vec3& corner : quad.Corners)
			{
				if (!finite(corner.x) || !finite(corner.y) || !finite(corner.z))
					return false;
			}
			return finite(quad.TexCoordMin.x) && finite(quad.TexCoordMin.y) && finite(quad.TexCoordMax.x) && finite(quad.TexCoordMax.y)
				&& finite(quad.Color.r) && finite(quad.Color.g) && finite(quad.Color.b) && finite(quad.Color.a);
		}

		// The intersection of `scissor` (or the whole extent) with the framebuffer's extent; empty when they do not overlap.
		static nvrhi::Rect ClipQuadScissor(const std::optional<nvrhi::Rect>& scissor, uint32_t width, uint32_t height)
		{
			constexpr uint32_t Largest = static_cast<uint32_t>(std::numeric_limits<int>::max());
			const int extentX = static_cast<int>(std::min(width, Largest));
			const int extentY = static_cast<int>(std::min(height, Largest));
			if (!scissor)
				return nvrhi::Rect(0, extentX, 0, extentY);
			nvrhi::Rect clipped;
			clipped.minX = std::clamp(scissor->minX, 0, extentX);
			clipped.maxX = std::clamp(scissor->maxX, 0, extentX);
			clipped.minY = std::clamp(scissor->minY, 0, extentY);
			clipped.maxY = std::clamp(scissor->maxY, 0, extentY);
			return clipped;
		}

	}

	namespace {

		// A run of consecutive quads drawn with one texture and one sampler (and, for the Normals target, one normal map), all
		// in one chunk of MaxQuadsPerRecord quads.
		struct QuadBatch
		{
			nvrhi::ITexture* Texture = nullptr; // the white texture for a quad without one
			QuadSampler Sampler = QuadSampler::Point;
			nvrhi::ITexture* NormalMap = nullptr; // the Normals target's; null for the flat normal and for the other targets
			bool NormalMapTwoChannel = false;     // the Normals target's NormalMap stores x and y only (QuadDraw)
			uint32_t FirstQuad = 0;
			uint32_t QuadCount = 0;
		};

	}

	struct QuadBatcher::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::array<GraphicsPipeline, PipelineCount> Pipelines{};
		std::array<nvrhi::SamplerHandle, 2> Samplers{}; // QuadSampler's values
		nvrhi::TextureHandle White{};                   // 1x1 opaque white: a quad without a texture samples it
		// M32: the fog constants of a record without the view's fog (FogConstants' defaults, Enabled 0), bound by the fogged
		// targets' sets.
		nvrhi::BufferHandle NoFog{};
		nvrhi::BufferHandle Vertices{};
		nvrhi::BufferHandle Indices{};
		uint32_t VertexCapacity = 0; // quads
		uint32_t IndexCapacity = 0;  // quads
		// Reused by every Record.
		std::vector<QuadVertex> Staging{};

		// Batches the quad Staging just received into `batches`: it extends the last batch when it has the same texture,
		// sampler and normal map (with its two-channel flag) and the quad does not begin a new chunk of MaxQuadsPerRecord
		// quads.
		void AddQuad(std::vector<QuadBatch>& batches, nvrhi::ITexture* texture, QuadSampler sampler, nvrhi::ITexture* normalMap,
			bool normalMapTwoChannel) const;
		// Records the indices of IndexCapacity quads into the index buffer.
		void WriteIndices(nvrhi::ICommandList& commandList) const;
		// The record itself: QuadBatcher::Record's work, before it releases an outsized Staging.
		[[nodiscard]] Result<uint32_t> RecordQuads(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const QuadBatchInputs& inputs,
			RenderPassCounters& counters);
	};

	void QuadBatcher::State::WriteIndices(nvrhi::ICommandList& commandList) const
	{
		std::vector<uint32_t> indices;
		indices.reserve(static_cast<size_t>(IndexCapacity) * Utils::IndicesPerQuad);
		for (uint32_t quad = 0; quad < IndexCapacity; ++quad)
		{
			for (const uint32_t corner : Utils::QuadCornerIndices)
				indices.push_back(quad * Utils::VerticesPerQuad + corner);
		}
		commandList.writeBuffer(Indices, indices.data(), indices.size() * sizeof(uint32_t), 0);
	}

	void QuadBatcher::State::AddQuad(std::vector<QuadBatch>& batches, nvrhi::ITexture* texture, QuadSampler sampler, nvrhi::ITexture* normalMap,
		bool normalMapTwoChannel) const
	{
		const uint32_t quad = static_cast<uint32_t>(Staging.size() / Utils::VerticesPerQuad) - 1;
		if (!batches.empty() && batches.back().Texture == texture && batches.back().Sampler == sampler && batches.back().NormalMap == normalMap
			&& batches.back().NormalMapTwoChannel == normalMapTwoChannel && quad % MaxQuadsPerRecord != 0)
		{
			++batches.back().QuadCount;
			return;
		}
		batches.push_back({ .Texture = texture,
			.Sampler = sampler,
			.NormalMap = normalMap,
			.NormalMapTwoChannel = normalMapTwoChannel,
			.FirstQuad = quad,
			.QuadCount = 1 });
	}

	NineSliceExpansion ExpandNineSlice(const NineSliceSource& source, glm::vec2 size, float borderScale)
	{
		ENGINE_CORE_ASSERT(source.TextureSize.x >= 1 && source.TextureSize.y >= 1, "a 9-slice source needs a texture of at least 1x1 texels");
		ENGINE_CORE_ASSERT(source.Borders.x + source.Borders.z <= source.RectSize.x && source.Borders.y + source.Borders.w <= source.RectSize.y,
			"a 9-slice frame's borders must fit inside its rectangle");
		ENGINE_CORE_ASSERT(std::isfinite(size.x) && std::isfinite(size.y) && size.x >= 0.0f && size.y >= 0.0f, "a 9-slice destination needs a finite size of at least 0");
		ENGINE_CORE_ASSERT(std::isfinite(borderScale) && borderScale > 0.0f, "a 9-slice border scale must be finite and positive");

		// One axis: the destination edges and the texel edges of its three columns (or rows). The two borders keep their
		// texel size times the border scale, shrinking in proportion when together they are wider than the destination.
		struct Axis
		{
			std::array<float, 4> Destination{};
			std::array<float, 4> TexCoord{};
		};
		const auto makeAxis = [borderScale](float destination, uint32_t textureSize, uint32_t rectMin, uint32_t rectSize, uint32_t first, uint32_t second)
		{
			float firstSize = static_cast<float>(first) * borderScale;
			float secondSize = static_cast<float>(second) * borderScale;
			const float borders = firstSize + secondSize;
			if (borders > destination)
			{
				firstSize = destination * (firstSize / borders);
				secondSize = destination - firstSize;
			}
			const float texture = static_cast<float>(textureSize);
			Axis axis;
			axis.Destination = { 0.0f, firstSize, destination - secondSize, destination };
			axis.TexCoord = { static_cast<float>(rectMin) / texture, static_cast<float>(rectMin + first) / texture,
				static_cast<float>(rectMin + rectSize - second) / texture, static_cast<float>(rectMin + rectSize) / texture };
			return axis;
		};
		const Axis columns = makeAxis(size.x, source.TextureSize.x, source.RectMin.x, source.RectSize.x, source.Borders.x, source.Borders.z);
		const Axis rows = makeAxis(size.y, source.TextureSize.y, source.RectMin.y, source.RectSize.y, source.Borders.y, source.Borders.w);

		NineSliceExpansion expansion;
		for (size_t row = 0; row < 3; ++row)
		{
			for (size_t column = 0; column < 3; ++column)
			{
				const NineSlicePatch patch{
					.Min = glm::vec2(columns.Destination[column], rows.Destination[row]),
					.Max = glm::vec2(columns.Destination[column + 1], rows.Destination[row + 1]),
					.TexCoordMin = glm::vec2(columns.TexCoord[column], rows.TexCoord[row]),
					.TexCoordMax = glm::vec2(columns.TexCoord[column + 1], rows.TexCoord[row + 1]),
				};
				// A zero border or a vanished middle leaves a patch without area.
				if (patch.Max.x > patch.Min.x && patch.Max.y > patch.Min.y)
					expansion.Patches[expansion.Count++] = patch;
			}
		}
		return expansion;
	}

	glm::mat4 MakeQuadPixelTransform(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(width >= 1 && height >= 1, "the quad pixel transform needs a target of at least 1x1, got {}x{}", width, height);
		// x' = 2x / width - 1 and y' = 1 - 2y / height (clip-space +y is up, §8.3), z unchanged.
		glm::mat4 transform(1.0f);
		transform[0][0] = 2.0f / static_cast<float>(std::max(width, 1U));
		transform[1][1] = -2.0f / static_cast<float>(std::max(height, 1U));
		transform[3][0] = -1.0f;
		transform[3][1] = 1.0f;
		return transform;
	}

	QuadBatcher::QuadBatcher(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	QuadBatcher::~QuadBatcher() = default;

	Result<Scope<QuadBatcher>> QuadBatcher::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<QuadBatcher> batcher = CreateScope<QuadBatcher>(ConstructionKey());
		State& state = *batcher->m_State;
		state.Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = Utils::MakeQuadDescriptions();
		// Pipelines of one layout shape share one binding layout, so one binding set per texture and sampler (and fog buffer)
		// serves them: Hdr and HdrAdditive (fogged), Ldr and LdrDepth, HdrLit (lit, fogged) and Normals (lit).
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> foggedLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::QuadHdrPipeline]), "while creating the quad pipelines"));
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> plainLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::QuadLdrPipeline]), "while creating the quad pipelines"));
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> litFoggedLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::QuadHdrLitPipeline]), "while creating the quad pipelines"));
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> litLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::QuadNormalsPipeline]), "while creating the quad pipelines"));
		for (size_t index = 0; index < descriptions.size(); ++index)
		{
			const auto target = static_cast<QuadBatchTarget>(index);
			const bool lit = Utils::IsLitQuadTarget(target);
			const bool fogged = Utils::IsFoggedQuadTarget(target);
			const std::vector<nvrhi::BindingLayoutHandle>& layouts = lit ? (fogged ? litFoggedLayouts : litLayouts) : (fogged ? foggedLayouts : plainLayouts);
			ENGINE_TRY_ASSIGN(state.Pipelines[index],
				WithContext(pipelines.CreateGraphicsPipeline(Utils::MakeQuadSpecification(std::move(descriptions[index]), target, layouts)),
					"while creating the quad pipelines"));
		}
		ENGINE_TRY_ASSIGN(state.Samplers[static_cast<size_t>(QuadSampler::Point)],
			WithContext(device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)),
				"while creating the quad batcher"));
		ENGINE_TRY_ASSIGN(state.Samplers[static_cast<size_t>(QuadSampler::Linear)],
			WithContext(device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setMipFilter(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)),
				"while creating the quad batcher"));

		// The white texture is a startup object of the pass list like the scene renderer's neutral textures, not an asset
		// upload: a render target cleared once, so the texture out-of-memory injection (which strikes asset textures) leaves
		// it alone and startup never fails for it.
		nvrhi::TextureDesc whiteDesc;
		whiteDesc.width = 1;
		whiteDesc.height = 1;
		whiteDesc.format = nvrhi::Format::RGBA8_UNORM;
		whiteDesc.dimension = nvrhi::TextureDimension::Texture2D;
		whiteDesc.isShaderResource = true;
		whiteDesc.isRenderTarget = true;
		whiteDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		whiteDesc.keepInitialState = true;
		whiteDesc.debugName = "QuadBatcher.White";
		ENGINE_TRY_ASSIGN(state.White, WithContext(device.CreateTexture(whiteDesc), "while creating the quad batcher"));

		nvrhi::BufferDesc indexDesc;
		indexDesc.byteSize = static_cast<uint64_t>(Utils::InitialQuadCapacity) * Utils::IndicesPerQuad * sizeof(uint32_t);
		indexDesc.isIndexBuffer = true;
		indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
		indexDesc.keepInitialState = true;
		indexDesc.debugName = "QuadBatcher.Indices";
		ENGINE_TRY_ASSIGN(state.Indices, WithContext(device.CreateBuffer(indexDesc), "while creating the quad batcher"));
		state.IndexCapacity = Utils::InitialQuadCapacity;

		nvrhi::BufferDesc noFogDesc;
		noFogDesc.byteSize = sizeof(FogConstants);
		noFogDesc.isConstantBuffer = true;
		noFogDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		noFogDesc.keepInitialState = true;
		noFogDesc.debugName = "QuadBatcher.NoFog";
		ENGINE_TRY_ASSIGN(state.NoFog, WithContext(device.CreateBuffer(noFogDesc), "while creating the quad batcher"));

		// Not an immediate command list: a caller may have the frame's immediate list open (NVRHI's validation allows one).
		ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
			WithContext(device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)), "while creating the quad batcher"));
		CommandListRecording recording(device, *commandList);
		commandList->clearTextureFloat(state.White, nvrhi::AllSubresources, nvrhi::Color(1.0f));
		const FogConstants noFog{};
		commandList->writeBuffer(state.NoFog, &noFog, sizeof(noFog));
		state.WriteIndices(*commandList);
		recording.Execute();
		return batcher;
	}

	std::vector<PipelineLayoutDescription> QuadBatcher::GetLayoutDescriptions()
	{
		return Utils::MakeQuadDescriptions();
	}

	nvrhi::FramebufferInfo QuadBatcher::GetFramebufferInfo(QuadBatchTarget target)
	{
		switch (target)
		{
			case QuadBatchTarget::Hdr:
			case QuadBatchTarget::HdrLit:
			case QuadBatchTarget::HdrAdditive: return GetSceneColorFramebufferInfo();
			case QuadBatchTarget::Ldr:         return GetOverlayFramebufferInfo();
			case QuadBatchTarget::Normals:     return GetLights2DNormalsFramebufferInfo();
			// LdrDepth draws into Ldr's target, the scene overlay's framebuffer with SceneDepth.
			case QuadBatchTarget::LdrDepth: return GetOverlayFramebufferInfo();
		}
		return GetOverlayFramebufferInfo();
	}

	uint32_t QuadBatcher::GetPipelineCount() const
	{
		return static_cast<uint32_t>(std::ranges::count_if(m_State->Pipelines, [](const GraphicsPipeline& pipeline)
		{
			return pipeline.Pipeline != nullptr;
		}));
	}

	nvrhi::ISampler* QuadBatcher::GetSampler(QuadSampler sampler) const
	{
		const auto index = static_cast<size_t>(sampler);
		ENGINE_CORE_ASSERT(index < m_State->Samplers.size(), "unknown quad sampler {}", index);
		return index < m_State->Samplers.size() ? m_State->Samplers[index].Get() : nullptr;
	}

	Result<uint32_t> QuadBatcher::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const QuadBatchInputs& inputs,
		RenderPassCounters& counters)
	{
		Result<uint32_t> recorded = m_State->RecordQuads(commandList, bindings, inputs, counters);
		// M21's milestone review: between records the staging keeps room for at most one batch, the vertex buffer's capacity, so
		// its CPU copy never stays larger than the GPU buffer it fills; a record of more quads releases it.
		if (m_State->Staging.capacity() > static_cast<size_t>(MaxQuadsPerRecord) * Utils::VerticesPerQuad)
			std::vector<QuadVertex>().swap(m_State->Staging);
		return recorded;
	}

	size_t QuadBatcher::GetStagingCapacity() const
	{
		return m_State->Staging.capacity() / Utils::VerticesPerQuad;
	}

	Result<uint32_t> QuadBatcher::State::RecordQuads(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const QuadBatchInputs& inputs,
		RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Framebuffer != nullptr, "QuadBatcher::Record needs a framebuffer");
		if (inputs.Framebuffer == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the quad batcher needs a framebuffer");
		ENGINE_CORE_ASSERT(inputs.Target != QuadBatchTarget::HdrLit || inputs.Light != nullptr, "QuadBatcher::Record's HdrLit target needs the light accumulation");
		if (inputs.Target == QuadBatchTarget::HdrLit && inputs.Light == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the quad batcher's HdrLit target needs the light accumulation");
		State& state = *this;
		state.Staging.clear();
		// The record's batches name its textures, so they live only for the call.
		std::vector<QuadBatch> batches;

		const nvrhi::FramebufferInfoEx& target = inputs.Framebuffer->getFramebufferInfo();
		const uint32_t width = std::max(target.width, 1U);
		const uint32_t height = std::max(target.height, 1U);
		const nvrhi::Rect scissor = Utils::ClipQuadScissor(inputs.Scissor, width, height);
		if (inputs.Quads.empty() || scissor.maxX <= scissor.minX || scissor.maxY <= scissor.minY)
			return 0U;

		// Lay out every quad with finite values, in the given order; only the Normals target batches by normal map.
		const bool normals = inputs.Target == QuadBatchTarget::Normals;
		for (const QuadDraw& quad : inputs.Quads)
		{
			if (!Utils::IsQuadFinite(quad))
				continue;
			const std::array<glm::vec2, 4> texCoords = {
				quad.TexCoordMin,
				glm::vec2(quad.TexCoordMax.x, quad.TexCoordMin.y),
				quad.TexCoordMax,
				glm::vec2(quad.TexCoordMin.x, quad.TexCoordMax.y),
			};
			for (size_t corner = 0; corner < quad.Corners.size(); ++corner)
				state.Staging.push_back({ .Position = quad.Corners[corner], .TexCoord = texCoords[corner], .Color = quad.Color });
			nvrhi::ITexture* normalMap = normals ? quad.NormalMap : nullptr;
			state.AddQuad(batches, quad.Texture != nullptr ? quad.Texture : state.White.Get(), quad.Sampler, normalMap,
				normalMap != nullptr && quad.NormalMapTwoChannel);
		}
		if (state.Staging.empty())
			return 0U;
		const size_t quadTotal = state.Staging.size() / Utils::VerticesPerQuad;
		// The quads one chunk writes into the vertex buffer: the record's, up to MaxQuadsPerRecord.
		const auto largestChunk = static_cast<uint32_t>(std::min(quadTotal, static_cast<size_t>(MaxQuadsPerRecord)));

		// Everything that can fail, before the first command and before any change to the state but the vertex buffer's: the
		// binding sets, then the buffers. A grown vertex buffer may be kept after a later failure (every record writes its
		// vertices before drawing), but a new index buffer is kept only together with the recording of its indices, so the
		// state never holds an index buffer whose indices were not written (a failed record leaves IndexCapacity below
		// VertexCapacity, and the next record creates and writes it). A chunk never exceeds MaxQuadsPerRecord, so neither do
		// the buffers.
		const auto pipeline = static_cast<size_t>(inputs.Target);
		const bool lit = Utils::IsLitQuadTarget(inputs.Target);
		const bool fogged = Utils::IsFoggedQuadTarget(inputs.Target);
		nvrhi::IBindingLayout& layout = *state.Pipelines[pipeline].BindingLayouts[0];
		std::vector<nvrhi::IBindingSet*> sets;
		sets.reserve(batches.size());
		for (const QuadBatch& batch : batches)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::Texture_SRV(0, batch.Texture) };
			if (lit)
			{
				// HdrLit's light accumulation, or the Normals batch's normal map (white for the flat normal, unread).
				nvrhi::ITexture* second = inputs.Target == QuadBatchTarget::HdrLit ? inputs.Light : batch.NormalMap;
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(1, second != nullptr ? second : state.White.Get()));
			}
			desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, state.Samplers[static_cast<size_t>(batch.Sampler)]));
			if (fogged)
				desc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(Utils::QuadFogConstantsSlot, inputs.Fog != nullptr ? inputs.Fog : state.NoFog.Get()));
			desc.bindings.push_back(nvrhi::BindingSetItem::PushConstants(0, sizeof(QuadConstants)));
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, WithContext(bindings.GetOrCreate(*state.Device, desc, layout), "while recording the quads"));
			sets.push_back(set);
		}
		if (state.Vertices == nullptr || state.VertexCapacity < largestChunk)
		{
			uint32_t capacity = std::max(state.VertexCapacity, Utils::InitialQuadCapacity);
			while (capacity < largestChunk)
				capacity *= 2;
			capacity = std::min(capacity, MaxQuadsPerRecord);
			nvrhi::BufferDesc desc;
			desc.byteSize = static_cast<uint64_t>(capacity) * Utils::VerticesPerQuad * sizeof(QuadVertex);
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "QuadBatcher.Vertices";
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, WithContext(state.Device->CreateBuffer(desc), "while recording the quads"));
			state.Vertices = std::move(buffer);
			state.VertexCapacity = capacity;
		}
		// The index buffer follows the vertex buffer's capacity; a new one is kept below, where its indices are recorded.
		nvrhi::BufferHandle newIndices;
		if (state.IndexCapacity < state.VertexCapacity)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = static_cast<uint64_t>(state.VertexCapacity) * Utils::IndicesPerQuad * sizeof(uint32_t);
			desc.isIndexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "QuadBatcher.Indices";
			ENGINE_TRY_ASSIGN(newIndices, WithContext(state.Device->CreateBuffer(desc), "while recording the quads"));
		}

		commandList.beginMarker("Quads");
		if (newIndices != nullptr)
		{
			state.Indices = std::move(newIndices);
			state.IndexCapacity = state.VertexCapacity;
			state.WriteIndices(commandList);
		}
		// M32: the fogged targets meet the view's fog when the record has it; the additive one fades rather than tints.
		uint32_t fogMode = QuadFogModeNone;
		if (fogged && inputs.Fog != nullptr)
			fogMode = inputs.Target == QuadBatchTarget::HdrAdditive ? QuadFogModeFade : QuadFogModeBlend;
		QuadConstants constants{ .Transform = inputs.Transform, .FogMode = fogMode };
		nvrhi::GraphicsState graphics;
		graphics.pipeline = state.Pipelines[pipeline].Pipeline;
		graphics.framebuffer = inputs.Framebuffer;
		graphics.viewport.addViewport(nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height)));
		graphics.viewport.addScissorRect(scissor);
		graphics.bindings.push_back(nullptr);
		graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(state.Vertices).setSlot(0).setOffset(0));
		graphics.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(state.Indices).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
		// One chunk of at most MaxQuadsPerRecord quads at a time: its vertices written at the start of the buffer, then its
		// batches drawn (each lies in one chunk, State::AddQuad), before the next chunk's write.
		size_t index = 0;
		for (size_t chunkStart = 0; chunkStart < quadTotal; chunkStart += MaxQuadsPerRecord)
		{
			const size_t chunkQuads = std::min(quadTotal - chunkStart, static_cast<size_t>(MaxQuadsPerRecord));
			commandList.writeBuffer(state.Vertices, state.Staging.data() + chunkStart * Utils::VerticesPerQuad,
				chunkQuads * Utils::VerticesPerQuad * sizeof(QuadVertex), 0);
			// The write leaves the buffer in its copy state: the chunk's first draw sets the graphics state again.
			bool stateSet = false;
			for (; index < batches.size() && batches[index].FirstQuad < chunkStart + chunkQuads; ++index)
			{
				const QuadBatch& batch = batches[index];
				const uint32_t hasNormalMap = normals && batch.NormalMap != nullptr ? 1U : 0U;
				const uint32_t twoChannel = hasNormalMap != 0 && batch.NormalMapTwoChannel ? 1U : 0U;
				if (!stateSet || graphics.bindings[0] != sets[index] || constants.HasNormalMap != hasNormalMap || constants.NormalMapTwoChannel != twoChannel)
				{
					stateSet = true;
					graphics.bindings[0] = sets[index];
					constants.HasNormalMap = hasNormalMap;
					constants.NormalMapTwoChannel = twoChannel;
					// Setting the graphics state invalidates the push constants, so every change sets them again.
					commandList.setGraphicsState(graphics);
					commandList.setPushConstants(&constants, sizeof(constants));
				}
				const auto firstInChunk = static_cast<uint32_t>(batch.FirstQuad - chunkStart);
				commandList.drawIndexed(
					nvrhi::DrawArguments().setVertexCount(batch.QuadCount * Utils::IndicesPerQuad).setStartIndexLocation(firstInChunk * Utils::IndicesPerQuad));
				++counters.DrawCalls;
				counters.Triangles += batch.QuadCount * 2;
			}
		}
		commandList.endMarker();
		return static_cast<uint32_t>(quadTotal);
	}

}
