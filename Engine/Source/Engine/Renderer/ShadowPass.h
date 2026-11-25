#pragma once

#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/MeshWind.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/PointShadowMaps.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStaticDraws.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Engine/Renderer/SkinningPass.h"
#include "Engine/Renderer/SpotShadowAtlas.h"
#include "Shared/InstanceRecord.h"

#include <glm/mat4x4.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
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
		nvrhi::BufferHandle Wind{};       // M46: vertex binding 3, the mesh's wind stream (GpuMesh::WindBuffer)
		nvrhi::BufferHandle Indices{};
		uint32_t IndexOffset = 0; // M46: of the caster's level (ShadowCasterView)
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
		// GpuMaterial::Generation, and a deformed caster's entity, joint palette and morph weights (PrepareCasters). M46: and its
		// level, and for a wind caster its wind response and the render's wind (so a map holding one re-renders every frame).
		uint64_t Signature = 0;
	};

	// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3E and 3G): the view whose shadows the casters
	// draw. Camera: the view's camera, by whose distance each caster draws its level (SelectViewShadowLod: the view's level
	// plus its ShadowLodOffset, never faded) and beyond whose CullDistance it casts nothing; null draws level 0 of every
	// caster. Wind: the render's wind (Renderer/MeshWind.h), null without a Wind in the snapshot: a caster whose material has
	// wind enabled then gets DrawFlagWind and its WindResponse, and the wind enters its signature.
	struct ShadowCasterView
	{
		const CameraData* Camera = nullptr; // borrowed for PrepareCasters only
		const WindFrame* Wind = nullptr;    // borrowed for PrepareCasters only
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

	// M29 (Docs/Decisions/0044-m29-light-limits.md decision 4; the review): a world-space box of the shadow pass's culling,
	// which ShadowPass::Record tests before any view's frustum: for each shadowed light (a point light's cube of Range, a spot
	// tile's frustum's box), the caster draws whose box overlaps the light's are the only ones its views cull further.
	struct ShadowBox
	{
		glm::vec3 Centre = glm::vec3(0.0f);
		glm::vec3 HalfExtent = glm::vec3(0.0f);
	};

	// The box of a caster draw's local `bounds` moved by its `world` matrix: the box of the moved corners, so it holds every
	// moved point of the bounds whatever the matrix's rotation, scale or shear. Empty bounds, which no shadow view culls,
	// give an infinite box at the world matrix's origin.
	[[nodiscard]] ShadowBox ComputeShadowCasterBox(const Aabb& bounds, const glm::mat4& world);
	// Whether a caster's box may reach a light's: overlapping on every axis, with a margin against rounding of a thousandth of
	// the half extents, a hundred-thousandth of the centres' coordinates and a tenth of a millimetre, so a caster it rejects
	// lies wholly outside and no view inside the light's box records it.
	[[nodiscard]] bool ShadowBoxesOverlap(const ShadowBox& caster, const ShadowBox& light);

	// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): what the static casters of one key draw with (RenderStaticDrawKey:
	// the caster variant, a Mask caster's or (M46) a wind caster's material or null, the mesh and the submesh): ShadowCasterDraw's members a key
	// determines, resolved when a static cell is prepared and shared by the cells of the renderer that hold the key.
	struct StaticShadowKey
	{
		RenderStaticDrawKey Key{};
		nvrhi::BufferHandle Vertices{};
		nvrhi::BufferHandle Attributes{};
		nvrhi::BufferHandle Indices{};
		uint32_t IndexOffset = 0;
		uint32_t IndexCount = 0;
		uint32_t Variant = 0;
		uint32_t ReflectedVariant = 0;
		nvrhi::BufferHandle MaterialConstants{};
		nvrhi::TextureHandle BaseColorMap{};
		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E and its W46.4 record): the mesh's wind
		// stream (vertex binding 3), and whether the key's material has wind, with its WindResponse (Renderer/MeshWind.h): a
		// static record serves every submesh of its item, so the key's draws carry the response (DrawConstantsWind) in a
		// render whose snapshot has a Wind (StaticShadowSet::Wind).
		nvrhi::BufferHandle Wind{};
		bool WindCaster = false;
		uint32_t WindResponse = 0;
	};

	// M46 (W46.4): a StaticShadowCaster's key and M29 signature at one of its further levels; Key is NoStaticShadowKey where the
	// submesh has no indices at that level (it casts nothing there).
	inline constexpr uint32_t NoStaticShadowKey = std::numeric_limits<uint32_t>::max();
	struct StaticShadowLevel
	{
		uint32_t Key = NoStaticShadowKey;
		uint64_t Signature = 0;
	};

	// M43: one static caster of a prepared cell: its key (an index into the renderer's StaticShadowKey table), its item's index
	// in the cell (its record is the cell's first record plus it), its submesh's local bounds, its world matrix, its box
	// (ComputeShadowCasterBox) and its M29 signature (PrepareCasters' recipe), all fixed for the cell's version.
	struct StaticShadowCaster
	{
		uint32_t Key = 0;
		uint32_t Item = 0;
		Aabb Bounds{};
		glm::mat4 World = glm::mat4(1.0f);
		ShadowBox Box{};
		uint64_t Signature = 0;
		// M46 (W46.4): what chooses the caster's level in a view (SelectViewShadowLod with the view's camera, as a dynamic caster's:
		// its mesh's switch sizes, LodCount of them, its mesh's level 0 bounds and its item's RenderDistance), its key and
		// signature at each further level (Levels[level - 1]; Key and Signature above are level 0's), and whether it is a wind
		// caster, whose face hashes the render's wind.
		std::array<float, MaxMeshLods - 1> LodScreenSizes{};
		uint32_t LodCount = 0;
		Aabb MeshBounds{};
		MeshDrawDistance Distance{};
		std::array<StaticShadowLevel, MaxMeshLods - 1> Levels{};
		bool Wind = false;
	};

	// M43: one static cell as the shadow faces cull it: the box of its casters' boxes (which each face inflates by
	// InflateStaticCellBox before it classifies it), Unbounded when a caster has empty bounds (which no face culls, so neither
	// is the cell), its first record in the static record buffer this render (GpuResourceCache::AcquireStaticRecords) and its
	// casters in item order, borrowed for the render.
	struct StaticShadowCell
	{
		Aabb Box{};
		bool Unbounded = false;
		uint32_t FirstRecord = 0;
		std::span<const StaticShadowCaster> Casters{};
	};

	// M43: one indexed draw of a face: the face's Count visible static casters of key Key, whose records' indices are
	// InstanceIndices[First, First + Count).
	struct StaticShadowDraw
	{
		uint32_t Key = 0;
		uint32_t First = 0;
		uint32_t Count = 0;
	};

	// M43: a face's static draws, Draws[FirstDraw, FirstDraw + DrawCount) of its StaticShadowSet, and the signature of its
	// visible static casters (FinishShadowSignature of their signatures in draw order; 0 without one, and for a cascade, whose
	// maps M29's cache does not hold), which M29's cache hashes after the face's dynamic draws.
	struct StaticShadowFace
	{
		uint32_t FirstDraw = 0;
		uint32_t DrawCount = 0;
		uint64_t Signature = 0;
	};

	// M43: the static casters' draws of every face of a view's shadow plan (ShadowPass::PrepareStaticFaces): the cascades in
	// order, the spot tiles in the atlas's order, each planned point light's six faces.
	struct StaticShadowSet
	{
		std::vector<StaticShadowDraw> Draws{};
		// M46 (W46.4): whether the render has a Wind, so the draws of a wind caster's key carry its WindResponse.
		bool Wind = false;
		std::array<StaticShadowFace, MaxShadowCascades> Cascades{};
		std::array<StaticShadowFace, MaxSpotShadowLights> Spots{};
		std::array<std::array<StaticShadowFace, PointShadowFaceCount>, MaxPointShadowLights> Points{};
	};

	// M43: ShadowPass::PrepareStaticFaces' working buckets: for each StaticShadowKey, the records and M29 signatures of the
	// face's visible casters of that key, and the keys the face touched, in the order they first appear. Keys and Touched are
	// empty between calls; the renderer keeps them from render to render, so an unchanged frame's faces reuse their storage.
	struct StaticShadowBuckets
	{
		std::vector<std::vector<std::pair<uint32_t, uint64_t>>> Keys{}; // per key: (record, signature)
		std::vector<uint32_t> Touched{};
		bool Wind = false; // M46: the face's visible static casters hold a wind caster
		// M46 (the W46.4 review, finding 3): each caster's level in the render, chosen once for a cell a face looks at
		// (PrepareStaticFaces' view camera is every face's): CellLevels[cell] is the cell's first entry in Levels
		// (NoStaticCellLevels until then), Levels[i] its i-th caster's level or NoStaticShadowLevel beyond its CullDistance.
		// Reset by each PrepareStaticFaces.
		std::vector<uint32_t> CellLevels{};
		std::vector<uint8_t> Levels{};
	};
	inline constexpr uint32_t NoStaticCellLevels = std::numeric_limits<uint32_t>::max();
	inline constexpr uint8_t NoStaticShadowLevel = std::numeric_limits<uint8_t>::max();

	// M43: one static caster draw of an item at one level (ShadowPass::AppendStaticItemCasters): its draw, by PrepareCasters'
	// rule (Instance unset), and its key; M46 (W46.4): whether its material has wind, and its WindResponse.
	struct StaticCasterDraw
	{
		ShadowCasterDraw Draw{};
		RenderStaticDrawKey Key{};
		bool Wind = false;
		uint32_t WindResponse = 0;
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
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 2): the Half tier's cubes (GetPointTargetDesc(Half)); may be
		// non-null while no planned light is Half, and is read only for Half lights (then required).
		nvrhi::ITexture* PointHalfTarget = nullptr;
		std::span<const nvrhi::FramebufferHandle> PointHalfFramebuffers{}; // CreateFramebuffers(PointHalfTarget), one per layer
		// M29 (decision 4): the view's static shadow cache, read and written by Record; null draws every map.
		ShadowMapCache* Cache = nullptr;
		// M31 (Docs/Decisions/0046-m31-material-parameters.md decision 6): the dissolve noise the Mask casters sample for an
		// instance that dissolves (BuiltinAssetHandles::DissolveNoiseTexture's mirror); null binds the pass's own neutral
		// white texture, under which only an amount of 1 dissolves anything.
		nvrhi::ITexture* DissolveNoise = nullptr;
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the view's static casters per face (PrepareStaticFaces),
		// their keys, and what their draws read: the static records (t21, GpuResourceCache::GetStaticRecordBuffer) and the
		// view's InstanceIndices (t22) holding the faces' indices; borrowed for Record only. Null draws no static caster; the
		// sets bind Instances in a null buffer's place (nothing reads it then).
		const StaticShadowSet* StaticDraws = nullptr;
		std::span<const StaticShadowKey> StaticKeys{};
		nvrhi::IBuffer* StaticInstances = nullptr;
		nvrhi::IBuffer* InstanceIndices = nullptr;
		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G): the renderer's WindConstants, bound at
		// b8 of both variants' set 0 for the wind casters' displacement; null binds the pass's own calm constants, under which
		// nothing moves.
		nvrhi::IBuffer* Wind = nullptr;
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
	// x back/front/none cull, matching M8 alpha test, mirrored determinant and DoubleSided (M31: the Mask variants also clip a
	// dissolving instance by its noise, Docs/Decisions/0046-m31-material-parameters.md decision 5). Blend does not cast. Reverse-Z
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
		// indices whose material is Opaque, or Mask with its constants and base colour map: Blend does not cast; M31: an
		// Opaque material's draw of a dissolving item takes the Mask variant with its material, SelectMeshAlphaMode), sorted so
		// that equal draws are adjacent: by (variant, Mask material, mesh, submesh), then caster order (a depth-only pass's
		// result does not depend on its order, and an Opaque caster binds no material). Appends one instance record per draw
		// to `instances` (MakeMeshInstanceRecord's, RenderPrepare.h: {World, PreviousWorld or World, PickId, the mirroring
		// flag}), the draws' records in their order, and groups the draws into batches (one per draw when `instancing` is
		// false). Main thread; the caller writes `instances` into the buffer Record reads. Errors: InvalidArgument for a
		// caster with a non-finite world matrix (nothing is appended).
		// M23 (C23.3, decision 27): `sources`, empty or one per caster, are the vertices each caster's draws bind
		// (SkinningPass.h): a deformed caster's draws bind its deformed vertices, are culled by its deformed bounds and key
		// its VertexStream, so they batch with no other caster's; a Skipped caster casts nothing.
		// M46 (decisions 3E and 3G): `view` chooses each caster's level and its wind (ShadowCasterView); the draws' keys gain
		// the level, so two levels of one submesh never batch together.
		[[nodiscard]] static Result<ShadowCasterSet> PrepareCasters(GpuResourceCache& cache, std::span<const MeshDrawItem> casters, bool instancing,
			std::vector<InstanceRecord>& instances, std::span<const DeformedVertexSource> sources = {}, const ShadowCasterView& view = {});

		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the caster draws of static item `item` (a finite world
		// matrix), by PrepareCasters' rule and in its submesh order, appended to `draws` with their keys (RenderStaticDrawKey: the
		// variant, a Mask caster's or (M46) a wind caster's material or null, the mesh and the submesh; M46: and `level`) and their M29 signatures,
		// Instance unset: a static cell's casters, prepared once per cell version and level. Nothing for an item that does not
		// cast, or at a level beyond its mesh's last. M46 (W46.4): the draws of `level` (0 to the mesh's last), each with its
		// material's wind (a static record holds none), whose signature hashes the response but not the render's wind (its
		// face's does, PrepareStaticFaces). Main thread (the cache's mirrors).
		static void AppendStaticItemCasters(GpuResourceCache& cache, const MeshDrawItem& item, uint32_t level, std::vector<StaticCasterDraw>& draws);

		// M43: the static casters' draws of every face of the plan (`cascades`, `spots`, `points`; null for none), `keyCount` the
		// size of the renderer's StaticShadowKey table. Each face selects the cells, then their casters, exactly as Record
		// selects the dynamic casters: a spot tile's and a point light's by their boxes (ShadowBoxesOverlap) first, then every
		// face by its frustum (with the directional maps' pancake when `depthClamp`), a cell whose inflated box is outside
		// skipped whole and one inside taken whole (ClassifyStaticCellLight, ClassifyStaticCellShadow); its visible casters'
		// record indices are appended to `indices` grouped by key, in the order the keys first appear in the cells (cell
		// order, then item order) and, within a key, in cell order then item order: one StaticShadowDraw per key and face. Only
		// a spot tile's and a point face's signatures are hashed (M29's cache holds no cascade). `buckets` is the caller's
		// working storage, grown to `keyCount` keys and left empty. Pure but for `buckets` and `indices`.
		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E and its W46.4 record): `view` chooses each
		// visible caster's level as PrepareCasters does (its camera: the view's level plus the caster's ShadowLodOffset, nothing
		// beyond its CullDistance; null draws level 0), once per render for the cells the faces look at (a caster without
		// levels or CullDistance takes level 0 unmeasured), its draws keyed and signed by that level; and a face whose visible
		// casters hold a wind caster hashes `view`'s wind (its phases change every frame), which also sets the set's Wind.
		[[nodiscard]] static StaticShadowSet PrepareStaticFaces(std::span<const StaticShadowCell> cells, size_t keyCount, const ShadowCascadeSet* cascades,
			const SpotShadowAtlas* spots, const PointShadowSet* points, bool depthClamp, StaticShadowBuckets& buckets, std::vector<InstanceIndex>& indices,
			const ShadowCasterView& view = {});

		// Culls the prepared casters per face and records shadow passes only (M43: each face's dynamic draws as before, then its
		// static draws, one indexed draw each of DrawConstantsStaticInstances). All inputs borrowed for this call;
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
