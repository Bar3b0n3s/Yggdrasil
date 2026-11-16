#include "EnginePCH.h"
#include "Engine/Renderer/ShadowPass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/ForwardPipelines.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/DrawConstants.h"
#include "Shared/MaterialConstants.h"
#include "Shared/ShadowCasterConstants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <span>
#include <tuple>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static PipelineLayoutDescription ShadowLayout(uint32_t variant)
		{
			const bool mask = variant >= 3;
			constexpr std::array<const char*, 3> Names{ "Back", "Front", "None" };
			nvrhi::BindingLayoutDesc view;
			view.visibility = nvrhi::ShaderType::All;
			view.registerSpace = 0;
			view.registerSpaceIsDescriptorSet = true;
			view.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::StructuredBuffer_SRV(InstancesRegister),
				nvrhi::BindingLayoutItem::PushConstants(DrawConstantsSlot, sizeof(DrawConstants)) };
			if (mask)
			{
				// M31 (decision 6): the dissolve noise and its sampler.
				view.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(DissolveNoiseRegister));
				view.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(2));
			}
			PipelineLayoutDescription result;
			result.Name = std::format("Shadow{}Cull{}", mask ? "Mask" : "Opaque", Names[variant % 3]);
			result.Program = "Shadow";
			result.Entries = { "VSMain", "PSMain" };
			result.Permutation = { { .Key = "ALPHA_MASK", .Value = mask ? "1" : "0" } };
			result.BindingLayouts = { view };
			result.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ShadowCasterConstants) } };
			if (mask)
			{
				nvrhi::BindingLayoutDesc material;
				material.visibility = nvrhi::ShaderType::Pixel;
				material.registerSpace = 1;
				material.registerSpaceIsDescriptorSet = true;
				material.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0), nvrhi::BindingLayoutItem::Texture_SRV(0) };
				result.BindingLayouts.push_back(material);
				result.ConstantBuffers.push_back({ .Set = 1, .Register = 0, .ByteSize = sizeof(MaterialConstants) });
			}
			return result;
		}

		// M29 (decision 4): the bytes of a value into a shadow signature.
		template<typename T>
		static void HashShadowValue(XXH64Hasher& hasher, const T& value)
		{
			hasher.Update(std::as_bytes(std::span<const T>(&value, 1)));
		}

		template<typename T>
		static void HashShadowValues(XXH64Hasher& hasher, std::span<const T> values)
		{
			hasher.UpdateU64(values.size());
			hasher.Update(std::as_bytes(values));
		}

		// A signature is never 0, which the cache reserves for "nothing reusable".
		[[nodiscard]] static uint64_t FinishShadowSignature(const XXH64Hasher& hasher)
		{
			return hasher.Digest() | 1U;
		}

		[[nodiscard]] static bool IsFiniteShadowWorld(const glm::mat4& world)
		{
			for (glm::length_t column = 0; column < 4; ++column)
				for (glm::length_t row = 0; row < 4; ++row)
					if (!std::isfinite(world[column][row]))
						return false;
			return true;
		}

		[[nodiscard]] static bool IsOutsideShadow(const Aabb& bounds, const glm::mat4& worldViewProjection, bool depthClamp)
		{
			if (bounds.IsEmpty())
				return false;
			std::array<bool, 6> outside{ true, true, true, true, true, true };
			for (uint32_t index = 0; index < 8; ++index)
			{
				const glm::vec4 clip = worldViewProjection
					* glm::vec4((index & 1U) ? bounds.Max.x : bounds.Min.x, (index & 2U) ? bounds.Max.y : bounds.Min.y,
						(index & 4U) ? bounds.Max.z : bounds.Min.z, 1);
				outside[0] = outside[0] && clip.x < -clip.w;
				outside[1] = outside[1] && clip.x > clip.w;
				outside[2] = outside[2] && clip.y < -clip.w;
				outside[3] = outside[3] && clip.y > clip.w;
				outside[4] = outside[4] && clip.z < 0;
				outside[5] = outside[5] && clip.z > clip.w;
			}
			// Pancake only the near plane; far casters cannot shadow any receiver in the volume.
			return std::ranges::any_of(std::span(outside).first(depthClamp ? 5 : 6), [](bool value)
			{
				return value;
			});
		}

		// M29 (decision 4; the review): the draws (ascending indices) whose box (`boxes`, ComputeShadowCasterBox of each)
		// overlaps `light`'s: the only draws a view inside that box can record. A point light's six faces lie inside its cube
		// of Range; a spot tile's frustum inside the box of its eight corners.
		static void SelectShadowCandidates(std::span<const ShadowBox> boxes, const ShadowBox& light, std::vector<uint32_t>& candidates)
		{
			candidates.clear();
			for (uint32_t index = 0; index < boxes.size(); ++index)
			{
				if (ShadowBoxesOverlap(boxes[index], light))
					candidates.push_back(index);
			}
		}

		// The box of a view's frustum: the eight corners of its clip volume (x and y in [-1, 1], reverse-Z depth in [0, 1]) moved
		// to world space through the inverse view-projection.
		[[nodiscard]] static ShadowBox GetShadowFrustumBox(const glm::mat4& viewProjection)
		{
			const glm::mat4 inverse = glm::inverse(viewProjection);
			glm::vec3 lower(std::numeric_limits<float>::infinity());
			glm::vec3 upper(-std::numeric_limits<float>::infinity());
			for (uint32_t corner = 0; corner < 8; ++corner)
			{
				const glm::vec4 world = inverse
					* glm::vec4((corner & 1U) != 0 ? 1.0f : -1.0f, (corner & 2U) != 0 ? 1.0f : -1.0f, (corner & 4U) != 0 ? 1.0f : 0.0f, 1.0f);
				const glm::vec3 point = glm::vec3(world) / world.w;
				lower = glm::min(lower, point);
				upper = glm::max(upper, point);
			}
			return { .Centre = (lower + upper) * 0.5f, .HalfExtent = (upper - lower) * 0.5f };
		}

		// The candidates `viewProjection` does not cull (IsOutsideShadow), in order: the one culling that a view's signature
		// hashes and RecordView records, so the two agree by construction.
		static void CullShadowView(const ShadowCasterSet& casters, std::span<const uint32_t> candidates, const glm::mat4& viewProjection, bool pancake,
			std::vector<uint32_t>& visible)
		{
			visible.clear();
			for (const uint32_t index : candidates)
			{
				const ShadowCasterDraw& draw = casters.Draws[index];
				if (!IsOutsideShadow(draw.Bounds, viewProjection * draw.World, pancake))
					visible.push_back(index);
			}
		}

		// M29 (decision 4): one shadow view (a cube face or a spot tile) into a map's signature: its view-projection and the
		// signatures of its visible draws (CullShadowView), in draw order.
		static void HashShadowView(XXH64Hasher& hasher, const ShadowCasterSet& casters, const glm::mat4& viewProjection, std::span<const uint32_t> visible)
		{
			HashShadowValue(hasher, viewProjection);
			for (const uint32_t index : visible)
				hasher.UpdateU64(casters.Draws[index].Signature);
		}

	}

	namespace {

		// Pair diagnostic scopes on every error path; recorded counters are updated only after a draw.
		struct ShadowRecordingScope
		{
			RenderRecordingContext& Recording;
			RenderPassCounters Counters{};
			~ShadowRecordingScope() { Recording.EndPass(Counters); }
		};

	}

	struct ShadowPass::State
	{
		GraphicsDevice* Device = nullptr; // back-reference; the device outlives the pass
		std::array<GraphicsPipeline, PipelineCount> Pipelines{};
		nvrhi::SamplerHandle AnisoWrap{};
		// M31 (decision 6): the 1 x 1 white R8 texture the Mask casters' set binds without ShadowRenderInputs::DissolveNoise.
		nvrhi::TextureHandle NeutralNoise{};

		// The Mask casters' material sets (null for Opaque casters), one per draw of `casters`, from the caller's cache.
		[[nodiscard]] Result<std::vector<nvrhi::IBindingSet*>> ResolveMaterialSets(PassBindingCache& bindings, const ShadowCasterSet& casters);
		// One face: the runs of `visible` (the face's CullShadowView, ascending) that are adjacent draws of one batch
		// (`batchOf`: each draw's batch), each one instanced draw. `reflected`: the face's projection is a reflection (a
		// point-shadow face), so the draws use their ReflectedVariant.
		[[nodiscard]] Status RecordView(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ShadowCasterSet& casters,
			std::span<nvrhi::IBindingSet* const> materialSets, nvrhi::IBuffer& instances, nvrhi::ITexture& noise, nvrhi::IFramebuffer& framebuffer,
			const nvrhi::Viewport& viewport, nvrhi::IBuffer& casterConstants, uint32_t slot, std::span<const uint32_t> visible,
			std::span<const uint32_t> batchOf, RenderPassCounters& counters, bool reflected);

		// M29 (the review): CPU buffers every Record reuses, so a steady-state record allocates nothing: the casters' boxes
		// and batches, every draw (a cascade's candidates), a light's candidates, and the visible draws of a cascade or point
		// face and of each spot tile.
		std::vector<ShadowBox> Boxes{};
		std::vector<uint32_t> BatchOf{};
		std::vector<uint32_t> EveryDraw{};
		std::vector<uint32_t> Candidates{};
		std::array<std::vector<uint32_t>, PointShadowFaceCount> FaceVisible{};
		std::array<std::vector<uint32_t>, MaxSpotShadowLights> TileVisible{};
	};

	ShadowPass::ShadowPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ShadowPass::~ShadowPass() = default;

	ShadowBox ComputeShadowCasterBox(const Aabb& bounds, const glm::mat4& world)
	{
		if (bounds.IsEmpty())
			return { .Centre = glm::vec3(world[3]), .HalfExtent = glm::vec3(std::numeric_limits<float>::infinity()) };
		const glm::vec3 centre = (bounds.Min + bounds.Max) * 0.5f;
		const glm::vec3 halfExtent = (bounds.Max - bounds.Min) * 0.5f;
		// Each world axis's reach is the sum of the local half extents along it (the absolute matrix).
		return { .Centre = glm::vec3(world * glm::vec4(centre, 1.0f)),
			.HalfExtent = glm::abs(glm::vec3(world[0])) * halfExtent.x + glm::abs(glm::vec3(world[1])) * halfExtent.y + glm::abs(glm::vec3(world[2])) * halfExtent.z };
	}

	bool ShadowBoxesOverlap(const ShadowBox& caster, const ShadowBox& light)
	{
		const glm::vec3 reach = (caster.HalfExtent + light.HalfExtent) * 1.001f + (glm::abs(caster.Centre) + glm::abs(light.Centre)) * 1e-5f + glm::vec3(1e-4f);
		return glm::all(glm::lessThanEqual(glm::abs(caster.Centre - light.Centre), reach));
	}

	Result<Scope<ShadowPass>> ShadowPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<ShadowPass> pass = CreateScope<ShadowPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		for (uint32_t variant = 0; variant < PipelineCount; ++variant)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = Utils::ShadowLayout(variant);
			if (variant % 3 != 0)
				specification.SharedBindingLayouts = state.Pipelines[variant / 3 * 3].BindingLayouts;
			constexpr uint32_t Stride = sizeof(MeshVertex);
			specification.VertexAttributes = {
				nvrhi::VertexAttributeDesc()
					.setName("POSITION")
					.setFormat(nvrhi::Format::RGB32_FLOAT)
					.setOffset(offsetof(MeshVertex, Position))
					.setElementStride(Stride),
				nvrhi::VertexAttributeDesc()
					.setName("TEXCOORD")
					.setFormat(nvrhi::Format::RG32_FLOAT)
					.setOffset(offsetof(MeshVertex, TexCoord))
					.setElementStride(Stride),
			};
			// M23 (C-MESH2): the attribute stream at locations 2 and 3, which a Mask caster's alpha test reads.
			const std::array<nvrhi::VertexAttributeDesc, 2> stream = Utils::MakeMeshAttributeStreamAttributes();
			specification.VertexAttributes.insert(specification.VertexAttributes.end(), stream.begin(), stream.end());
			specification.Framebuffer.setDepthFormat(ShadowDepthFormat);
			constexpr std::array<nvrhi::RasterCullMode, 3> Modes{ nvrhi::RasterCullMode::Back, nvrhi::RasterCullMode::Front,
				nvrhi::RasterCullMode::None };
			specification.RenderState.rasterState.setCullMode(Modes[variant % 3])
				.setFrontCounterClockwise(true)
				.setDepthClipEnable(true); // pancaking is per fragment in the shader: NVRHI does not expose Vulkan depth clamp
			specification.RenderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true).setDepthFunc(
				nvrhi::ComparisonFunc::GreaterOrEqual);
			ENGINE_TRY_ASSIGN(state.Pipelines[variant], pipelines.CreateGraphicsPipeline(specification));
		}
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap).setMaxAnisotropy(16);
		ENGINE_TRY_ASSIGN(state.AnisoWrap, device.CreateSampler(sampler));
		nvrhi::TextureDesc noise;
		noise.width = 1;
		noise.height = 1;
		noise.format = nvrhi::Format::R8_UNORM;
		noise.isRenderTarget = true;
		noise.initialState = nvrhi::ResourceStates::ShaderResource;
		noise.keepInitialState = true;
		noise.debugName = "ShadowPass.NeutralNoise";
		ENGINE_TRY_ASSIGN(state.NeutralNoise, device.CreateTexture(noise));
		ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle list, device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
		CommandListRecording recording(device, *list);
		list->clearTextureFloat(state.NeutralNoise, nvrhi::AllSubresources, nvrhi::Color(1.0f));
		recording.Execute();
		return pass;
	}

	std::vector<PipelineLayoutDescription> ShadowPass::GetLayoutDescriptions()
	{
		std::vector<PipelineLayoutDescription> result;
		for (uint32_t variant = 0; variant < PipelineCount; ++variant)
			result.push_back(Utils::ShadowLayout(variant));
		return result;
	}

	nvrhi::TextureDesc ShadowPass::GetCascadeTargetDesc(uint32_t shadowMapSize)
	{
		ENGINE_CORE_ASSERT(shadowMapSize >= 256 && shadowMapSize <= 8192 && (shadowMapSize & (shadowMapSize - 1)) == 0,
			"Invalid shadow map size");
		nvrhi::TextureDesc desc;
		desc.width = desc.height = shadowMapSize;
		desc.arraySize = MaxShadowCascades;
		desc.dimension = nvrhi::TextureDimension::Texture2DArray;
		desc.format = ShadowDepthFormat;
		desc.isRenderTarget = true;
		desc.isShaderResource = true;
		desc.initialState = nvrhi::ResourceStates::DepthWrite;
		desc.keepInitialState = true;
		desc.debugName = "DirectionalShadows";
		return desc;
	}

	nvrhi::TextureDesc ShadowPass::GetAtlasTargetDesc()
	{
		nvrhi::TextureDesc desc = GetCascadeTargetDesc(SpotShadowAtlasSize);
		desc.arraySize = 1;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.debugName = "SpotShadows";
		return desc;
	}

	nvrhi::TextureDesc ShadowPass::GetPointTargetDesc(uint32_t faceSize, PointShadowTier tier)
	{
		const bool full = tier == PointShadowTier::Full;
		ENGINE_CORE_ASSERT(faceSize >= (full ? 128U : 64U) && faceSize <= (full ? 2048U : 1024U), "Invalid point shadow face size {}", faceSize);
		nvrhi::TextureDesc desc = GetCascadeTargetDesc(256);
		desc.width = desc.height = faceSize;
		desc.arraySize = PointShadowFaceCount * (full ? MaxFullPointShadowLights : MaxHalfPointShadowLights);
		desc.dimension = nvrhi::TextureDimension::TextureCubeArray;
		desc.debugName = full ? "PointShadowMaps" : "PointShadowMapsHalf";
		return desc;
	}

	nvrhi::BufferDesc ShadowPass::GetCasterConstantsDesc()
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = static_cast<uint64_t>(ShadowCasterSlotCount) * ShadowCasterSlotStride;
		desc.isConstantBuffer = true;
		desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
		desc.keepInitialState = true;
		desc.debugName = "ShadowCasterConstants";
		return desc;
	}

	Result<std::vector<nvrhi::FramebufferHandle>> ShadowPass::CreateFramebuffers(GraphicsDevice& device, nvrhi::ITexture& target)
	{
		const nvrhi::TextureDesc& desc = target.getDesc();
		std::vector<nvrhi::FramebufferHandle> framebuffers;
		framebuffers.reserve(desc.arraySize);
		for (uint32_t slice = 0; slice < desc.arraySize; ++slice)
		{
			ENGINE_TRY_ASSIGN(nvrhi::FramebufferHandle framebuffer,
				device.CreateFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(&target, nvrhi::TextureSubresourceSet(0, 1, slice, 1))));
			framebuffers.push_back(std::move(framebuffer));
		}
		return framebuffers;
	}

	Result<ShadowCasterSet> ShadowPass::PrepareCasters(GpuResourceCache& cache, std::span<const MeshDrawItem> casters, bool instancing,
		std::vector<InstanceRecord>& instances, std::span<const DeformedVertexSource> sources)
	{
		ENGINE_CORE_ASSERT(sources.empty() || sources.size() == casters.size(), "the casters' vertex sources are none or one per caster");
		// The draws in caster order with their keys and records, sorted below.
		struct Candidate
		{
			ShadowCasterDraw Draw{};
			MeshInstanceKey Key{};
			InstanceRecord Record{};
		};
		std::vector<Candidate> candidates;
		for (size_t casterIndex = 0; casterIndex < casters.size(); ++casterIndex)
		{
			const MeshDrawItem& item = casters[casterIndex];
			// M23 (C23.3): a deformed caster's vertices and bounds (SkinningPass.h).
			const DeformedVertexSource source = casterIndex < sources.size() ? sources[casterIndex] : DeformedVertexSource{};
			if (!item.CastShadows || !item.Mesh.IsValid() || source.Skipped)
				continue;
			if (!Utils::IsFiniteShadowWorld(item.World))
				return MakeError(ErrorCode::InvalidArgument, "Shadow caster {} has a non-finite world transform", item.Entity);
			// Cache uploads invalidate borrowed references; own this mesh's handles and value arrays before resolving materials.
			const GpuMesh mesh = cache.GetMesh(item.Mesh);
			if (!mesh.VertexBuffer || !mesh.IndexBuffer || !mesh.AttributeBuffer)
				continue;
			const bool mirrored = glm::determinant(glm::mat3(item.World)) < 0;
			for (uint32_t submeshIndex = 0; submeshIndex < mesh.Submeshes.size(); ++submeshIndex)
			{
				const GpuSubmesh& submesh = mesh.Submeshes[submeshIndex];
				if (submesh.IndexCount == 0)
					continue;
				AssetHandle materialHandle = BuiltinAssetHandles::DefaultMaterial;
				if (submesh.MaterialSlot < item.Materials.size() && item.Materials[submesh.MaterialSlot].IsValid())
					materialHandle = item.Materials[submesh.MaterialSlot];
				else if (submesh.MaterialSlot < mesh.DefaultMaterials.size() && mesh.DefaultMaterials[submesh.MaterialSlot].IsValid())
					materialHandle = mesh.DefaultMaterials[submesh.MaterialSlot];
				const GpuMaterial& material = cache.GetMaterial(materialHandle);
				if (material.AlphaMode == AlphaMode::Blend || !material.Constants || !material.BaseColorMap)
					continue;
				// M31 (decision 5): a dissolving item's Opaque material casts through the Mask variant, its material bound.
				const AlphaMode mode = SelectMeshAlphaMode(material.AlphaMode, item.Parameters, true);
				const bool mask = mode == AlphaMode::Mask;
				Candidate candidate;
				candidate.Draw = { .Vertices = source.Vertices != nullptr ? nvrhi::BufferHandle(source.Vertices) : mesh.VertexBuffer,
					.Attributes = mesh.AttributeBuffer,
					.Indices = mesh.IndexBuffer,
					.IndexOffset = submesh.IndexOffset,
					.IndexCount = submesh.IndexCount,
					.Bounds = source.Vertices != nullptr ? source.Bounds : submesh.Bounds,
					.World = item.World,
					.Variant = Utils::GetPrepassVariant(mode, Utils::SelectCullMode(mirrored, material.DoubleSided)),
					.ReflectedVariant = Utils::GetPrepassVariant(mode, Utils::SelectCullMode(!mirrored, material.DoubleSided)),
					.MaterialConstants = mask ? material.Constants : nvrhi::BufferHandle(),
					.BaseColorMap = mask ? material.BaseColorMap : nvrhi::TextureHandle() };
				// An Opaque caster binds no material, so the Opaque casters of one mesh batch whatever their materials.
				candidate.Key = { .Pipeline = candidate.Draw.Variant,
					.Material = mask ? materialHandle : AssetHandle(),
					.Mesh = item.Mesh,
					.Submesh = submeshIndex,
					.VertexStream = source.VertexStream };
				candidate.Record = MakeMeshInstanceRecord(item);
				// M29 (decision 4): what this draw puts into a depth map. An Opaque caster binds no material, so its material does
				// not count; a deformed caster's vertices are its entity's, posed by its palette and weights.
				XXH64Hasher signature;
				Utils::HashShadowValue(signature, item.World);
				signature.UpdateU64(item.Mesh.GetValue());
				signature.UpdateU64(mesh.Version);
				signature.UpdateU64(submesh.IndexOffset);
				signature.UpdateU64(submesh.IndexCount);
				signature.UpdateU64(candidate.Draw.Variant);
				signature.UpdateU64(candidate.Draw.ReflectedVariant);
				signature.UpdateU64(mask ? materialHandle.GetValue() : 0U);
				signature.UpdateU64(mask ? material.Generation : 0U);
				// M31's parameters (batch 1's landing and its review, finding 2): exactly the instance-record values a Mask
				// caster's fragment shader (Shadow.slang's PSMain) reads under DrawFlagMaterialParameters. The dissolve's amount
				// always; its noise scale only while the amount is above 0 and below 1, the only amounts that sample the noise;
				// the texture-coordinate offset only for a Mask material, whose alpha test samples at it (an Opaque material's
				// dissolve tests no alpha and samples the noise without the offset). A fade and a tint never reach a shadow, and an
				// Opaque caster reads no record value.
				const bool parameters = mask && (candidate.Record.Flags & DrawFlagMaterialParameters) != 0;
				signature.UpdateU64(parameters ? 1U : 0U);
				if (parameters)
				{
					const float dissolve = candidate.Record.DissolveAmount;
					Utils::HashShadowValue(signature, dissolve);
					if (dissolve > 0.0f && dissolve < 1.0f)
						Utils::HashShadowValue(signature, candidate.Record.DissolveNoiseScale);
					if (material.AlphaMode == AlphaMode::Mask)
						Utils::HashShadowValue(signature, candidate.Record.UVOffset);
				}
				signature.UpdateU64(source.Vertices != nullptr ? 1U : 0U);
				if (source.Vertices != nullptr)
				{
					signature.UpdateU64(item.Entity.GetValue());
					Utils::HashShadowValues(signature, std::span<const glm::mat4>(item.JointPalette));
					Utils::HashShadowValues(signature, std::span<const float>(item.MorphWeights));
				}
				candidate.Draw.Signature = Utils::FinishShadowSignature(signature);
				candidates.push_back(std::move(candidate));
			}
		}

		// Equal draws adjacent, ties in caster order (the candidates' index).
		std::vector<uint32_t> order(candidates.size());
		std::iota(order.begin(), order.end(), 0U);
		std::ranges::sort(order, [&candidates](uint32_t left, uint32_t right)
		{
			const MeshInstanceKey& a = candidates[left].Key;
			const MeshInstanceKey& b = candidates[right].Key;
			return std::tie(a.Pipeline, a.Material, a.Mesh, a.Submesh, a.VertexStream, left) < std::tie(b.Pipeline, b.Material, b.Mesh, b.Submesh, b.VertexStream, right);
		});
		ShadowCasterSet set;
		set.Draws.reserve(candidates.size());
		std::vector<MeshInstanceKey> keys;
		keys.reserve(candidates.size());
		instances.reserve(instances.size() + candidates.size());
		for (const uint32_t index : order)
		{
			Candidate& candidate = candidates[index];
			candidate.Draw.Instance = static_cast<uint32_t>(instances.size());
			instances.push_back(candidate.Record);
			keys.push_back(candidate.Key);
			set.Draws.push_back(std::move(candidate.Draw));
		}
		if (instancing)
			set.Batches = GroupMeshInstances(keys);
		else
		{
			set.Batches.reserve(set.Draws.size());
			for (uint32_t index = 0; index < set.Draws.size(); ++index)
				set.Batches.push_back({ .First = index, .Count = 1 });
		}
		return set;
	}

	Result<std::vector<nvrhi::IBindingSet*>> ShadowPass::State::ResolveMaterialSets(PassBindingCache& bindings, const ShadowCasterSet& casters)
	{
		std::vector<nvrhi::IBindingSet*> sets(casters.Draws.size(), nullptr);
		for (size_t index = 0; index < casters.Draws.size(); ++index)
		{
			const ShadowCasterDraw& draw = casters.Draws[index];
			if (draw.MaterialConstants == nullptr)
				continue;
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, draw.MaterialConstants), nvrhi::BindingSetItem::Texture_SRV(0, draw.BaseColorMap) };
			ENGINE_TRY_ASSIGN(sets[index], bindings.GetOrCreate(*Device, desc, *Pipelines[draw.Variant].BindingLayouts[1]));
		}
		return sets;
	}

	Status ShadowPass::State::RecordView(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ShadowCasterSet& casters,
		std::span<nvrhi::IBindingSet* const> materialSets, nvrhi::IBuffer& instances, nvrhi::ITexture& noise, nvrhi::IFramebuffer& framebuffer,
		const nvrhi::Viewport& viewport, nvrhi::IBuffer& casterConstants, uint32_t slot, std::span<const uint32_t> visible,
		std::span<const uint32_t> batchOf, RenderPassCounters& counters, bool reflected)
	{
		// The slot's range of the caller's buffer (NVRHI binds constant-buffer ranges in whole 256-byte units): the same desc
		// every frame, so the caller's cache keeps the sets.
		static_assert(sizeof(ShadowCasterConstants) <= ShadowCasterSlotStride && ShadowCasterSlotStride % 256 == 0);
		const nvrhi::BufferRange range(static_cast<uint64_t>(slot) * ShadowCasterSlotStride, ShadowCasterSlotStride);
		std::array<nvrhi::IBindingSet*, 2> viewSets{};
		for (uint32_t mask = 0; mask < 2; ++mask)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, &casterConstants, range),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(Utils::InstancesRegister, &instances),
				nvrhi::BindingSetItem::PushConstants(Utils::DrawConstantsSlot, sizeof(DrawConstants)) };
			if (mask != 0)
			{
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(Utils::DissolveNoiseRegister, &noise));
				desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(2, AnisoWrap));
			}
			ENGINE_TRY_ASSIGN(viewSets[mask], bindings.GetOrCreate(*Device, desc, *Pipelines[mask * 3].BindingLayouts[0]));
		}
		// One instanced draw of `count` draws from `first`, whose instance records are adjacent. A batch's draws share their key,
		// so they share their variant and its reflected variant.
		const auto recordRun = [this, &commandList, &casters, materialSets, &framebuffer, &viewport, &viewSets, &counters, reflected](uint32_t first, uint32_t count)
		{
			const ShadowCasterDraw& draw = casters.Draws[first];
			const uint32_t variant = reflected ? draw.ReflectedVariant : draw.Variant;
			nvrhi::GraphicsState state;
			state.pipeline = Pipelines[variant].Pipeline;
			state.framebuffer = &framebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = { viewSets[variant / 3] };
			if (materialSets[first] != nullptr)
				state.bindings.push_back(materialSets[first]);
			state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.Vertices).setSlot(0));
			state.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(draw.Attributes).setSlot(Utils::MeshAttributeStreamSlot));
			state.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(draw.Indices).setFormat(nvrhi::Format::R32_UINT));
			commandList.setGraphicsState(state);
			DrawConstants push{};
			push.FirstInstance = draw.Instance;
			commandList.setPushConstants(&push, sizeof(push));
			commandList.drawIndexed(nvrhi::DrawArguments().setVertexCount(draw.IndexCount).setInstanceCount(count).setStartIndexLocation(draw.IndexOffset));
			++counters.DrawCalls;
			counters.Triangles += draw.IndexCount / 3 * count;
		};
		// The runs of the visible draws: adjacent draws of one batch, whose instance records are adjacent. Batches cover the
		// draws in order, so these are the runs each batch's visible draws form, recorded in the same order.
		uint32_t runFirst = 0;
		uint32_t runCount = 0;
		for (const uint32_t index : visible)
		{
			if (runCount != 0 && (index != runFirst + runCount || batchOf[index] != batchOf[runFirst]))
			{
				recordRun(runFirst, runCount);
				runCount = 0;
			}
			ENGINE_CORE_ASSERT(runCount == 0 || casters.Draws[index].Instance == casters.Draws[index - 1].Instance + 1,
				"the instance records of a batch's draws must be adjacent");
			if (runCount == 0)
				runFirst = index;
			++runCount;
		}
		if (runCount != 0)
			recordRun(runFirst, runCount);
		return {};
	}

	Result<ShadowRenderResult> ShadowPass::Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording,
		PassBindingCache& bindings, const ShadowRenderInputs& inputs)
	{
		const bool directional = inputs.Cascades && inputs.Cascades->Count != 0;
		const bool spots = inputs.Spots && !inputs.Spots->Tiles.empty();
		const bool points = inputs.Points && !inputs.Points->Lights.empty();
		if (!directional && !spots && !points)
			return ShadowRenderResult{};
		const ShadowCasterSet noCasters;
		const ShadowCasterSet& casters = inputs.Casters != nullptr ? *inputs.Casters : noCasters;
		ENGINE_CORE_ASSERT(std::isfinite(inputs.DepthBias) && inputs.DepthBias >= 0 && std::isfinite(inputs.NormalBias)
				&& inputs.NormalBias >= 0,
			"Invalid shadow bias");
		ENGINE_CORE_ASSERT(inputs.CasterConstants && inputs.CasterConstants->getDesc().isConstantBuffer
				&& inputs.CasterConstants->getDesc().byteSize >= GetCasterConstantsDesc().byteSize,
			"Shadow caster constants must be a buffer of GetCasterConstantsDesc()");
		ENGINE_CORE_ASSERT(casters.Draws.empty()
				|| (inputs.Instances != nullptr && inputs.Instances->getDesc().structStride == sizeof(InstanceRecord)
					&& inputs.Instances->getDesc().byteSize >= (static_cast<uint64_t>(casters.Draws.back().Instance) + 1) * sizeof(InstanceRecord)),
			"Shadow casters need the view's Instances buffer holding their records");
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::IBindingSet*> materialSets, m_State->ResolveMaterialSets(bindings, casters));
		State& state = *m_State;
		state.Boxes.clear();
		for (const ShadowCasterDraw& draw : casters.Draws)
			state.Boxes.push_back(ComputeShadowCasterBox(draw.Bounds, draw.World));
		state.BatchOf.assign(casters.Draws.size(), 0);
		for (uint32_t batch = 0; batch < casters.Batches.size(); ++batch)
			for (uint32_t index = casters.Batches[batch].First; index < casters.Batches[batch].First + casters.Batches[batch].Count; ++index)
				state.BatchOf[index] = batch;
		state.EveryDraw.resize(casters.Draws.size());
		std::iota(state.EveryDraw.begin(), state.EveryDraw.end(), 0U);
		nvrhi::ITexture& noise = inputs.DissolveNoise != nullptr ? *inputs.DissolveNoise : *state.NeutralNoise;

		// Every slot this record uses, written with one command before any draw: cascades in slots [0, 4), spot tiles from
		// slot MaxShadowCascades, point faces from ShadowCasterFirstPointSlot. Each slot is ShadowCasterSlotStride bytes, so
		// its binding offset is aligned on every device.
		std::array<std::array<std::byte, ShadowCasterSlotStride>, ShadowCasterSlotCount> slots{};
		std::array<ShadowCasterConstants, ShadowCasterSlotCount> constants{};
		uint32_t usedSlots = 0;
		if (directional)
		{
			ENGINE_CORE_ASSERT(inputs.Cascades->Count <= MaxShadowCascades, "Directional shadow cascade count is invalid");
			for (uint32_t index = 0; index < inputs.Cascades->Count; ++index)
			{
				const ShadowCascade& cascade = inputs.Cascades->Cascades[index];
				constants[index].ViewProjection = cascade.ViewProjection;
				constants[index].Bias = glm::vec4(inputs.DepthBias, cascade.TexelWorldSize / (cascade.LightFar - cascade.LightNear),
					m_State->Device->GetInfo().DepthClamp ? 1.0f : 0.0f, 0);
				usedSlots = index + 1;
			}
		}
		if (spots)
		{
			ENGINE_CORE_ASSERT(inputs.Spots->Tiles.size() <= MaxSpotShadowLights, "Spot shadow tile count is invalid");
			for (size_t index = 0; index < inputs.Spots->Tiles.size(); ++index)
			{
				const uint32_t slot = MaxShadowCascades + static_cast<uint32_t>(index);
				constants[slot].ViewProjection = inputs.Spots->Tiles[index].ViewProjection;
				constants[slot].Bias = glm::vec4(1, 0, 0, 0);
				usedSlots = slot + 1;
			}
		}
		if (points)
		{
			ENGINE_CORE_ASSERT(inputs.Points->Lights.size() <= MaxPointShadowLights, "Point shadow light count is invalid");
			for (size_t index = 0; index < inputs.Points->Lights.size(); ++index)
			{
				for (uint32_t face = 0; face < PointShadowFaceCount; ++face)
				{
					// The spot shadows' bias defaults (the slope-scaled factor 1), perspective, so no pancaking.
					const uint32_t slot = ShadowCasterFirstPointSlot + static_cast<uint32_t>(index) * PointShadowFaceCount + face;
					constants[slot].ViewProjection = inputs.Points->Lights[index].FaceViewProjections[face];
					constants[slot].Bias = glm::vec4(1, 0, 0, 0);
					usedSlots = slot + 1;
				}
			}
		}
		for (uint32_t slot = 0; slot < usedSlots; ++slot)
			std::memcpy(slots[slot].data(), &constants[slot], sizeof(ShadowCasterConstants));
		commandList.writeBuffer(inputs.CasterConstants, slots.data(), static_cast<size_t>(usedSlots) * ShadowCasterSlotStride);

		ShadowRenderResult result;
		if (directional)
		{
			ENGINE_CORE_ASSERT(inputs.CascadeTarget && inputs.CascadeFramebuffers.size() >= inputs.Cascades->Count,
				"Directional shadow target or framebuffers are missing");
			const nvrhi::TextureDesc& desc = inputs.CascadeTarget->getDesc();
			ENGINE_CORE_ASSERT(desc.format == ShadowDepthFormat && desc.arraySize == MaxShadowCascades && desc.width == desc.height,
				"Invalid cascade target");
			recording.BeginPass("DirectionalShadows", &commandList);
			ShadowRecordingScope scope{ recording };
			commandList.clearDepthStencilTexture(inputs.CascadeTarget, nvrhi::AllSubresources, true, 0, false, 0);
			for (uint32_t index = 0; index < inputs.Cascades->Count; ++index)
			{
				nvrhi::IFramebuffer* framebuffer = inputs.CascadeFramebuffers[index];
				ENGINE_CORE_ASSERT(framebuffer && framebuffer->getDesc().depthAttachment.texture == inputs.CascadeTarget
						&& framebuffer->getDesc().depthAttachment.subresources.baseArraySlice == index,
					"Cascade framebuffer {} does not attach slice {} of the cascade target", index, index);
				if (!casters.Draws.empty())
				{
					Utils::CullShadowView(casters, state.EveryDraw, constants[index].ViewProjection, constants[index].Bias.z != 0.0f, state.FaceVisible[0]);
					ENGINE_TRY(state.RecordView(commandList, bindings, casters, materialSets, *inputs.Instances, noise, *framebuffer,
						nvrhi::Viewport(static_cast<float>(desc.width), static_cast<float>(desc.height)), *inputs.CasterConstants, index,
						state.FaceVisible[0], state.BatchOf, scope.Counters, false));
				}
			}
			result.DrawCalls += scope.Counters.DrawCalls;
			result.Triangles += scope.Counters.Triangles;
		}
		if (spots)
		{
			ENGINE_CORE_ASSERT(inputs.AtlasTarget && inputs.AtlasFramebuffer
					&& inputs.AtlasFramebuffer->getDesc().depthAttachment.texture == inputs.AtlasTarget,
				"Spot shadow target or framebuffer is missing");
			const nvrhi::TextureDesc& desc = inputs.AtlasTarget->getDesc();
			ENGINE_CORE_ASSERT(desc.format == ShadowDepthFormat && desc.width == SpotShadowAtlasSize && desc.height == SpotShadowAtlasSize
					&& desc.arraySize == 1,
				"Invalid spot shadow target");
			recording.BeginPass("SpotShadows", &commandList);
			ShadowRecordingScope scope{ recording };
			// M29 (decision 4): the atlas is cleared whole (NVRHI clears whole subresources), so it is reused or drawn whole.
			XXH64Hasher signature;
			signature.UpdateU64(desc.width);
			for (size_t index = 0; index < inputs.Spots->Tiles.size(); ++index)
			{
				const SpotShadowTile& tile = inputs.Spots->Tiles[index];
				Utils::SelectShadowCandidates(state.Boxes, Utils::GetShadowFrustumBox(tile.ViewProjection), state.Candidates);
				Utils::CullShadowView(casters, state.Candidates, tile.ViewProjection, false, state.TileVisible[index]);
				signature.UpdateU64(tile.Slot);
				signature.UpdateU64(tile.Size);
				Utils::HashShadowValue(signature, tile.NearClip);
				Utils::HashShadowValue(signature, tile.FarClip);
				Utils::HashShadowView(signature, casters, tile.ViewProjection, state.TileVisible[index]);
			}
			const uint64_t atlas = Utils::FinishShadowSignature(signature);
			const bool cached = inputs.Cache != nullptr && inputs.Cache->Atlas == atlas;
			if (cached)
				result.CachedSpotLights = static_cast<uint32_t>(inputs.Spots->Tiles.size());
			else if (inputs.Cache != nullptr)
				inputs.Cache->Atlas = 0;
			if (!cached)
				commandList.clearDepthStencilTexture(inputs.AtlasTarget, nvrhi::AllSubresources, true, 0, false, 0);
			for (size_t index = 0; index < inputs.Spots->Tiles.size() && !cached; ++index)
			{
				const SpotShadowTile& tile = inputs.Spots->Tiles[index];
				const uint32_t slot = MaxShadowCascades + static_cast<uint32_t>(index);
				const float left = static_cast<float>(tile.X + SpotShadowGuardTexels);
				const float top = static_cast<float>(tile.Y + SpotShadowGuardTexels);
				const float size = static_cast<float>(tile.Size - 2 * SpotShadowGuardTexels);
				if (!casters.Draws.empty())
				{
					ENGINE_TRY(state.RecordView(commandList, bindings, casters, materialSets, *inputs.Instances, noise, *inputs.AtlasFramebuffer,
						nvrhi::Viewport(left, left + size, top, top + size, 0, 1), *inputs.CasterConstants, slot, state.TileVisible[index], state.BatchOf,
						scope.Counters, false));
				}
			}
			if (!cached && inputs.Cache != nullptr)
				inputs.Cache->Atlas = atlas;
			result.DrawCalls += scope.Counters.DrawCalls;
			result.Triangles += scope.Counters.Triangles;
		}
		if (points)
		{
			// M29 (Docs/Decisions/0044-m29-light-limits.md decision 2): each planned light's six faces in its tier's cube array.
			const bool half = std::ranges::any_of(inputs.Points->Lights, [](const PointShadowLight& light)
			{
				return light.Tier == PointShadowTier::Half;
			});
			ENGINE_CORE_ASSERT(inputs.PointTarget && inputs.PointFramebuffers.size() == PointShadowFaceCount * MaxFullPointShadowLights,
				"Point shadow target or framebuffers are missing");
			ENGINE_CORE_ASSERT(!half || (inputs.PointHalfTarget && inputs.PointHalfFramebuffers.size() == PointShadowFaceCount * MaxHalfPointShadowLights),
				"Half-size point shadow target or framebuffers are missing");
			const auto validTarget = [](const nvrhi::ITexture* target, uint32_t lights)
			{
				const nvrhi::TextureDesc& desc = target->getDesc();
				return desc.format == ShadowDepthFormat && desc.dimension == nvrhi::TextureDimension::TextureCubeArray
					&& desc.arraySize == PointShadowFaceCount * lights && desc.width == desc.height;
			};
			ENGINE_CORE_ASSERT(validTarget(inputs.PointTarget, MaxFullPointShadowLights) && (!half || validTarget(inputs.PointHalfTarget, MaxHalfPointShadowLights)),
				"Invalid point shadow target");
			recording.BeginPass("PointShadows", &commandList);
			ShadowRecordingScope scope{ recording };
			for (size_t index = 0; index < inputs.Points->Lights.size(); ++index)
			{
				const PointShadowLight& light = inputs.Points->Lights[index];
				const bool full = light.Tier == PointShadowTier::Full;
				nvrhi::ITexture* target = full ? inputs.PointTarget : inputs.PointHalfTarget;
				const std::span<const nvrhi::FramebufferHandle> framebuffers = full ? inputs.PointFramebuffers : inputs.PointHalfFramebuffers;
				ENGINE_CORE_ASSERT(light.Layer < (full ? MaxFullPointShadowLights : MaxHalfPointShadowLights), "Point shadow light {} is planned at layer group {}",
					index, light.Layer);
				const nvrhi::TextureDesc& desc = target->getDesc();
				// M29 (decision 4): the light's map from the cache when its layer group holds the light's own signature.
				XXH64Hasher signature;
				signature.UpdateU64(std::to_underlying(light.Tier));
				signature.UpdateU64(light.Layer);
				signature.UpdateU64(desc.width);
				Utils::HashShadowValue(signature, light.NearClip);
				Utils::HashShadowValue(signature, light.FarClip);
				// Every face lies inside the light's cube of FarClip (the Range) around it.
				Utils::SelectShadowCandidates(state.Boxes, ShadowBox{ .Centre = light.Position, .HalfExtent = glm::vec3(light.FarClip) }, state.Candidates);
				for (uint32_t face = 0; face < PointShadowFaceCount; ++face)
				{
					Utils::CullShadowView(casters, state.Candidates, light.FaceViewProjections[face], false, state.FaceVisible[face]);
					Utils::HashShadowView(signature, casters, light.FaceViewProjections[face], state.FaceVisible[face]);
				}
				const uint64_t map = Utils::FinishShadowSignature(signature);
				uint64_t* held = nullptr;
				if (inputs.Cache != nullptr)
					held = full ? &inputs.Cache->Full[light.Layer] : &inputs.Cache->Half[light.Layer];
				if (held != nullptr && *held == map)
				{
					++result.CachedPointLights;
					continue;
				}
				if (held != nullptr)
					*held = 0;
				commandList.clearDepthStencilTexture(target, nvrhi::TextureSubresourceSet(0, 1, light.Layer * PointShadowFaceCount, PointShadowFaceCount), true, 0,
					false, 0);
				for (uint32_t face = 0; face < PointShadowFaceCount; ++face)
				{
					const uint32_t layer = light.Layer * PointShadowFaceCount + face;
					nvrhi::IFramebuffer* framebuffer = framebuffers[layer];
					ENGINE_CORE_ASSERT(framebuffer && framebuffer->getDesc().depthAttachment.texture == target
							&& framebuffer->getDesc().depthAttachment.subresources.baseArraySlice == layer,
						"Point shadow framebuffer {} does not attach layer {} of its tier's point shadow target", layer, layer);
					const uint32_t slot = ShadowCasterFirstPointSlot + static_cast<uint32_t>(index) * PointShadowFaceCount + face;
					if (!casters.Draws.empty())
					{
						ENGINE_TRY(state.RecordView(commandList, bindings, casters, materialSets, *inputs.Instances, noise, *framebuffer,
							nvrhi::Viewport(static_cast<float>(desc.width), static_cast<float>(desc.height)), *inputs.CasterConstants, slot,
							state.FaceVisible[face], state.BatchOf, scope.Counters, true));
					}
				}
				if (held != nullptr)
					*held = map;
			}
			result.DrawCalls += scope.Counters.DrawCalls;
			result.Triangles += scope.Counters.Triangles;
		}
		return result;
	}

}
