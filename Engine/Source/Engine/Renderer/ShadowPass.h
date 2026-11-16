#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/PointShadowMaps.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/SkinningPass.h"
#include "Engine/Renderer/SpotShadowAtlas.h"
#include "Shared/InstanceRecord.h"

#include <glm/mat4x4.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Engine {

	class GpuResourceCache;
	class GraphicsDevice;

	// The caster-constant slots of one record: one per cascade (slots [0, 4)), then one per spot tile (from slot 4), then one per
	// point-shadow face (M23; M29's 16 spots and 16 point lights: from slot 20, the plan's light i's faces at 20 + 6 i in
	// PointShadowFaceCount order), 116 in all. A slot is 256 bytes, the largest minUniformBufferOffsetAlignment Vulkan allows,
	// so every slot offset is a valid binding offset.
	inline constexpr uint32_t ShadowCasterFirstPointSlot = MaxShadowCascades + MaxSpotShadowLights;
	inline constexpr uint32_t ShadowCasterSlotCount = ShadowCasterFirstPointSlot + MaxPointShadowLights * PointShadowFaceCount;
	inline constexpr uint32_t ShadowCasterSlotStride = 256;

	// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): one submesh draw of a view's shadow casters,
	// resolved once per render by ShadowPass::PrepareCasters, which every face of the view's shadow maps culls against its
	// own frustum. It holds its mesh's buffers and a Mask caster's material objects for the render.
	struct ShadowCasterDraw
	{
		nvrhi::BufferHandle Vertices{};
		nvrhi::BufferHandle Attributes{}; // M23 (C-MESH2): vertex binding 1, the mesh's attribute stream (GpuMesh::AttributeBuffer)
		nvrhi::BufferHandle Indices{};
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		Aabb Bounds{};                     // the submesh's local bounds
		glm::mat4 World = glm::mat4(1.0f); // the caster's world matrix, also in its instance record
		uint32_t Variant = 0;              // the caster pipeline: {Opaque, Mask} x {CullBack, CullFront, CullNone}
		// The variant under a reflecting projection (the point-shadow faces, M23 R-PLS): the front and back cull modes swap,
		// since the reflection reverses every triangle's winding. A function of Variant, so the draws of a batch share it.
		uint32_t ReflectedVariant = 0;
		// A Mask caster's alpha test (its material's constants and base colour map); null for an Opaque caster.
		nvrhi::BufferHandle MaterialConstants{};
		nvrhi::TextureHandle BaseColorMap{};
		uint32_t Instance = 0; // its instance record's index in the view's Instances buffer
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 4): XXH64 of everything the draw puts into a depth map: its
		// world matrix, mesh handle and GpuMesh::Version, submesh range, variants, a Mask caster's material handle and
		// GpuMaterial::Generation, and a deformed caster's entity, joint palette and morph weights (PrepareCasters).
		uint64_t Signature = 0;
	};

	// The view's shadow casters (ShadowPass::PrepareCasters).
	struct ShadowCasterSet
	{
		std::vector<ShadowCasterDraw> Draws{};
		// The instanced draws: maximal runs of Draws with equal keys (GroupMeshInstances), or one per draw without instancing
		// (RenderQualitySettings::Instancing false). Each face records, of every batch, the maximal runs of the draws its
		// frustum does not cull, each run one instanced draw of adjacent instance records.
		std::vector<MeshInstanceBatch> Batches{};
	};

	// M29 (Docs/Decisions/0044-m29-light-limits.md decision 4): the static shadow cache, per-view renderer state that
	// ShadowPass::Record reads and writes: the signature of what each point-shadow layer group of either tier and the spot atlas
	// hold (0: nothing reusable). A point light's signature hashes its tier, layer group, face size, clip planes and each face's
	// view-projection with the signatures of the caster draws that face records; the atlas's hashes every tile's slot,
	// view-projection and clip planes with its draws. Record skips a point light whose layer group holds its own signature, and
	// the whole atlas when it holds its own, counting them as cached, and stores a map's signature once it is drawn. The owner
	// clears it (Clear) whenever the maps may no longer hold what the signatures say: a render that does not continue its
	// view's temporal history, a new or re-acquired shadow target, a shader reload and a device loss.
	struct ShadowMapCache
	{
		std::array<uint64_t, MaxFullPointShadowLights> Full{};
		std::array<uint64_t, MaxHalfPointShadowLights> Half{};
		uint64_t Atlas = 0;

		void Clear() { *this = {}; }
	};

	struct ShadowRenderInputs
	{
		// M23: the view's casters, prepared by PrepareCasters from the full snapshot list (not camera-culled: off-screen
		// casters must survive), and the view's Instances structured buffer that holds their instance records; borrowed for
		// Record only. Null casters draw nothing.
		const ShadowCasterSet* Casters = nullptr;
		nvrhi::IBuffer* Instances = nullptr;
		const ShadowCascadeSet* Cascades = nullptr; // borrowed for Record only; null means no directional shadows
		const SpotShadowAtlas* Spots = nullptr;     // borrowed for Record only
		nvrhi::ITexture* CascadeTarget = nullptr;   // D32 array, ShadowMapSize square, 4 slices
		nvrhi::ITexture* AtlasTarget = nullptr;     // D32, 4096 square
		// Per-view objects the caller keeps with its targets (ADR 0017: per-view bindings and targets belong to the caller), so
		// a steady-state record creates no GPU object: CreateFramebuffers of each target and a GetCasterConstantsDesc buffer.
		std::span<const nvrhi::FramebufferHandle> CascadeFramebuffers{}; // one per CascadeTarget slice, in slice order
		nvrhi::IFramebuffer* AtlasFramebuffer = nullptr;                 // the AtlasTarget's framebuffer
		nvrhi::IBuffer* CasterConstants = nullptr;                       // rewritten by every Record
		float DepthBias = 1.0f;                                          // directional light's texel-scaled factors
		float NormalBias = 1.0f;
		// M23 (R-PLS, Docs/Decisions/0034-m23-contract.md decision 3): the point lights' cube faces, drawn with the caster
		// pipelines of the reflected winding (every face projection is a reflection, PointShadowMaps.h) and the spot shadows'
		// bias defaults. Null or empty Lights means no point shadows.
		const PointShadowSet* Points = nullptr;                        // borrowed for Record only
		nvrhi::ITexture* PointTarget = nullptr;                        // GetPointTargetDesc(Full), the Full tier's cubes
		std::span<const nvrhi::FramebufferHandle> PointFramebuffers{}; // CreateFramebuffers(PointTarget), one per layer
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 2): the Half tier's cubes (GetPointTargetDesc(Half)); null
		// while no planned light is Half.
		nvrhi::ITexture* PointHalfTarget = nullptr;
		std::span<const nvrhi::FramebufferHandle> PointHalfFramebuffers{}; // CreateFramebuffers(PointHalfTarget), one per layer
		// M29 (decision 4): the view's static shadow cache, read and written by Record; null draws every map.
		ShadowMapCache* Cache = nullptr;
	};

	struct ShadowRenderResult
	{
		uint32_t DrawCalls = 0;
		uint32_t Triangles = 0;
		// M29 (decision 4): the planned point lights and spot lights whose maps Record reused from the cache instead of drawing.
		uint32_t CachedPointLights = 0;
		uint32_t CachedSpotLights = 0;
	};

	// Shared immutable pipelines; per-view targets, framebuffers, caster constants, instance buffer and binding cache supplied
	// by the caller. Main thread, no stored spans. M23 (R-INST): every caster draw is instanced (Shadow.slang reads its world
	// matrix from its instance record), and RenderPassStats::DrawCalls counts the instanced draws. Six variants: Opaque/Mask
	// x back/front/none cull, matching M8 alpha test, mirrored determinant and DoubleSided. Blend does not cast. Reverse-Z
	// clear 0/GreaterOrEqual. Directional casters nearer the light than the map's near plane are pancaked per fragment
	// (GraphicsDeviceInfo::DepthClamp, no device feature: Shadow.slang clamps the exactly interpolated depth);
	// GraphicsSpecification::DisableDepthClamp forces the extended-near fallback, which clips them. Both paths are tested.
	class ShadowPass
	{
	public:
		static constexpr uint32_t PipelineCount = 6;
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ShadowPass;
		};
		explicit ShadowPass(ConstructionKey key);
		~ShadowPass();
		ShadowPass(const ShadowPass&) = delete;
		ShadowPass& operator=(const ShadowPass&) = delete;

		// device back-reference outlives pass; PipelineFactory used only during creation. Gpu/compile/reflection errors
		// propagate; startup caller maps creation failure to FatalError(OutOfMemory) per §8.14.
		[[nodiscard]] static Result<Scope<ShadowPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] static nvrhi::TextureDesc GetCascadeTargetDesc(uint32_t shadowMapSize);
		[[nodiscard]] static nvrhi::TextureDesc GetAtlasTargetDesc();
		// M23: the PointShadowMaps cube array, PointShadowFaceCount x MaxFullPointShadowLights layers of `faceSize` (a
		// GetPointShadowFaceSize) texels in the shadows' depth format; M29: for PointShadowTier::Half, the PointShadowMapsHalf
		// cube array, PointShadowFaceCount x MaxHalfPointShadowLights layers of `faceSize` (a GetPointShadowHalfFaceSize).
		[[nodiscard]] static nvrhi::TextureDesc GetPointTargetDesc(uint32_t faceSize, PointShadowTier tier = PointShadowTier::Full);
		// The per-view caster-constant buffer ShadowRenderInputs::CasterConstants takes.
		[[nodiscard]] static nvrhi::BufferDesc GetCasterConstantsDesc();
		// One depth-only framebuffer per array slice of `target` (a cascade, atlas or point-shadow target). Gpu errors propagate.
		[[nodiscard]] static Result<std::vector<nvrhi::FramebufferHandle>> CreateFramebuffers(GraphicsDevice& device, nvrhi::ITexture& target);

		// M23 (C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): resolves `casters` through the cache's mirrors
		// into the submesh draws that cast (the items with CastShadows and a mesh whose buffers exist; each submesh with
		// indices whose material is Opaque, or Mask with its constants and base colour map: Blend does not cast), sorted so
		// that equal draws are adjacent: by (variant, Mask material, mesh, submesh), then caster order (a depth-only pass's
		// result does not depend on its order, and an Opaque caster binds no material). Appends one instance record per draw
		// to `instances` (MakeMeshInstanceRecord's, RenderPrepare.h: {World, PreviousWorld or World, PickId, the mirroring
		// flag}), the draws' records in their order, and groups the draws into batches (one per draw when `instancing` is
		// false). Main thread; the caller writes `instances` into the buffer Record reads. Errors: InvalidArgument for a
		// caster with a non-finite world matrix (nothing is appended).
		// M23 (C23.3, decision 27): `sources`, empty or one per caster, are the vertices each caster's draws bind
		// (SkinningPass.h): a deformed caster's draws bind its deformed vertices, are culled by its deformed bounds and key
		// its VertexStream, so they batch with no other caster's; a Skipped caster casts nothing.
		[[nodiscard]] static Result<ShadowCasterSet> PrepareCasters(GpuResourceCache& cache, std::span<const MeshDrawItem> casters, bool instancing,
			std::vector<InstanceRecord>& instances, std::span<const DeformedVertexSource> sources = {});

		// Culls the prepared casters per face and records shadow passes only. All inputs borrowed for this call;
		// commandList open, targets match descriptions and Casters' records are in Instances (asserted). Gpu errors from
		// bindings; malformed external light/settings data must have been rejected by the pure planning functions.
		[[nodiscard]] Result<ShadowRenderResult> Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings,
			const ShadowRenderInputs& inputs);
		// Record opens/closes DirectionalShadows, SpotShadows and PointShadows (M23).
		// Borrow recording only for this call; disabled work has no row, errors still close scopes and report recorded counters.
	private:
		struct State;
		Scope<State> m_State;
	};

}
