#pragma once

#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class TypeRegistry;

	// M23 (C23.3, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md decision 25): registry struct "GltfAnimationEvent", one
	// named point of an imported clip's time (glTF has no animation events, so they are import settings). Animation names the
	// clip by its sub-asset key ("animation:0:Walk", see Output); Time is in seconds, within the clip's Duration.
	struct GltfAnimationEvent
	{
		std::string Animation{};
		std::string Name{};
		float Time = 0.0f;
	};

	// M46 (GltfImporter version 4; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3B): registry enum
	// "GltfLodSource", where an imported mesh's further levels of detail come from (MeshData::Lods).
	enum class GltfLodSource : uint8_t
	{
		None,     // no further level: every mesh is its level 0 alone
		FromGltf, // the levels the file authors: MSFT_lod node extensions, else sibling nodes named <name>_LOD<n>
		Generate  // levels meshoptimizer generates from level 0 at LodTriangleRatios, index-only
	};

	// M46: the most further levels the settings give a switch size or a triangle ratio for (MaxMeshLods - 1).
	inline constexpr size_t MaxGltfLodSettings = 4;

	// Registry struct "GltfImportSettings" (the §6.4 example's settings).
	struct GltfImportSettings
	{
		float Scale = 1.0f;                  // uniform scale baked into the imported data (see Output); > 0
		bool GenerateMissingTangents = true; // MikkTSpace for primitives without TANGENT (normal-mapped or not)
		bool ImportMaterials = true;         // false: no material sub-assets; every slot uses the Default material
		bool MergeMeshes = false;            // true: every mesh of the scene merged into the one mesh sub-asset "mesh:merged"
		// M23 (C23.3; decision 25): the events of the imported clips, each added to the clip its Animation names, sorted by
		// time in its clip (stable); an event naming no clip of the file (a skipped animation's included), with a name that
		// is not 1 to MaxAnimationNameBytes bytes of UTF-8, or outside its clip's [0, Duration], fails the import naming it.
		// Registered after MergeMeshes with version 3, so an earlier .meta reads as no events.
		std::vector<GltfAnimationEvent> AnimationEvents{};
		// M46 (version 4; decision 3B), registered after AnimationEvents, so an earlier .meta reads their defaults and no
		// .meta is rewritten. Lods: where the further levels come from. LodScreenSizes: per further level k + 1 (at most
		// MaxGltfLodSettings, each in (0, 1], strictly decreasing), the screen size below which it takes over from level k
		// (an MSFT_lod node's MSFT_screencoverage extras replace them). LodTriangleRatios: per generated level k + 1 (at most
		// MaxGltfLodSettings, each in (0, 1)), its share of level 0's triangles. LodMaxError: meshoptimizer's error bound,
		// relative to the mesh's extent. Wind: the wind block of every material the file imports (Material version 3).
		GltfLodSource Lods = GltfLodSource::None;
		std::vector<float> LodScreenSizes{ 0.5f, 0.25f, 0.125f, 0.0625f };
		std::vector<float> LodTriangleRatios{ 0.5f, 0.25f, 0.125f, 0.0625f };
		float LodMaxError = 0.05f;
		MaterialWind Wind{};
	};

	// glTF 2.0 (.gltf, .glb; Architecture §7.4), parsed with cgltf from memory (cgltf_parse, then an overflow-safe bounds
	// check of every buffer view and accessor and cgltf_validate before any data is read). The importer supplies every buffer and image itself (the .glb binary chunk, data URIs, and external
	// files read through ImportContext::ReadDependency after the URI rule below), so cgltf never opens a file. URIs are
	// JSON-decoded first (cgltf keeps escapes such as "\/" as written); the URI rule applies to the decoded string.
	//
	// Output (keys are the stable glTF identities of §6.4, so a reimport keeps every handle: "GltfImporter: reimport keeps
	// sub-asset handles"):
	//   - main asset: a Prefab mirroring the node hierarchy of the default scene (glTF "scene", else scene 0, else every
	//     root node): a root entity named after the file stem with an identity Transform, one entity per node (named after
	//     the node, or "Node <index>") with its TRS (a matrix is decomposed; a non-decomposable one is ImportFailed), and a
	//     MeshRenderer on nodes with a mesh. Prefab-local entity IDs are Hash64(source handle, "node:<index>") and
	//     Hash64(source handle, "root"), so they are stable across reimports;
	//   - Scale is baked into the data, never into the root's Transform (which every instance overrides implicitly, §5.5,
	//     so a root scale would never reach instances already placed): vertex positions, node translations, and submesh
	//     and mesh bounds are multiplied by Scale; normals, tangents, rotations and node scales are unchanged (the scale is
	//     uniform). Changing Scale therefore changes the prefab's content, and its instances follow when they are updated
	//     (EditorContext::CreatePrefabUpdateCommand);
	//   - "mesh:<index>[:<name>]": one Mesh per glTF mesh, one submesh per triangle primitive (strips and fans converted;
	//     points and lines skipped with ASSET_CONTENT_SKIPPED), positions, normals (generated area-weighted when missing),
	//     TEXCOORD_0, tangents from the file or else MikkTSpace (Vendor/MikkTSpace) when GenerateMissingTangents, over the UV
	//     set the primitive's normal texture samples (TEXCOORD_1 when its material binds the normal texture to it and the
	//     primitive has it, else TEXCOORD_0; glTF 2.0 §3.7.2.1), uint32 indices (generated for non-indexed primitives); a
	//     primitive needing generated tangents without those texture coordinates gets an arbitrary perpendicular basis and
	//     ASSET_TANGENTS_APPROXIMATED. M23 (version 2, MeshData version 2; Docs/Decisions/0034-m23-contract.md decision 9):
	//     TEXCOORD_1 (float, or normalized unsigned byte or short) and COLOR_0 (VEC3 or VEC4, float or normalized unsigned
	//     byte or short; VEC3 takes alpha 1; float channels clamped to [0, 1]) go into the attribute stream
	//     (MeshData::Attributes). A mesh whose primitives have neither gets no stream; in a mesh where some do, the others'
	//     vertices take the defaults, TexCoord1 (0, 0) and opaque white, which draw as a mesh without the stream draws;
	//   - "material:<index>[:<name>]": one Material per glTF material (metallic-roughness factors and textures,
	//     KHR_materials_emissive_strength, KHR_texture_transform scale and offset, alpha mode and cutoff, double-sided; M23,
	//     Material version 2: each bound map's UV set, UV1 for a texture on TEXCOORD_1, else UV0);
	//   - "texture:<image index>" (colour data), "texture:<image index>:linear" (metallic-roughness, occlusion) and
	//     "texture:<image index>:normal" (normal maps): Textures through TextureImporter::ImportTextureFromMemory with mips
	//     and Compression Auto, so each compresses by its usage (BC7 for colour and linear data, BC5 for normal maps; M23,
	//     Docs/Decisions/0034-m23-contract.md decision 13), only for images that have no Texture meta of their own. A
	//     texture artifact cached before TextureData version 2 is outdated, so the cache discards it and the import is
	//     rebuilt (AssetCache::Find, §7.5; decision 20). An image file with its own Texture meta is referenced by that
	//     standalone handle instead and not imported again ("GltfImporter: an image with its own Texture meta is referenced,
	//     not duplicated"; §6.4); its own settings apply.
	//   - M23 (version 3, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 25 and stream A's notes):
	//     skins, animations and morph targets, which version 2 skips (MergeMeshes still skips them, with
	//     ASSET_CONTENT_SKIPPED for each skin, animation and primitive's targets, since its one mesh is static):
	//       - a skin's Animator entity is the entity of the nearest node that is not a joint (of any imported skin) at or above
	//         the lowest common ancestor of the skin's root joints (its joints whose parent is not one of them) and of the
	//         nodes drawn with it: that ancestor itself when it is not a joint, else its nearest ancestor that is not one (a
	//         skinned node may lie below its own root joint), and the root entity when there is none (the ancestor is the
	//         scene, or every node above it is a joint). A skin whose joints are not all in the default scene is skipped with
	//         ASSET_CONTENT_SKIPPED ("skins[2]"), and the nodes drawn with it draw unskinned;
	//       - "skin:<index>[:<name>]": one Skeleton per Animator entity (SkeletonData.h). Skins that resolve to the same
	//         Animator entity (common when an exporter gives each skinned mesh its own skin over shared joints) are merged
	//         into one Skeleton, keyed by the first of them (the lowest skin index); skins that resolve to different entities
	//         stay separate Skeletons. Its joints are the union of its skins' joints and every node between them and the
	//         Animator's node (the static joints), in canonical node order (so parents first), each named after its node
	//         ("Node <index>" without a name; a name a joint of the Skeleton already took gets " <index>" appended), with the
	//         node's TRS as its rest pose. A joint's inverse bind matrix is that of the first of the merged skins that lists
	//         it, and a later skin that shares a joint with the earlier ones is carried into their bind space by the
	//         correction its first shared joint gives (the earlier skin's inverse bind's inverse times its own), which the
	//         joints it adds take too; a static joint's is the identity. Scale is baked into the rest translations and the
	//         inverse binds' translations. Its RootMotionJoint is the first skin's first root joint, so a rig under an
	//         intermediate node moves its hips, never that node;
	//       - a skinned primitive's JOINTS_0 and WEIGHTS_0 (unsigned byte or short joints; float, or normalized unsigned byte
	//         or short weights, renormalized to sum to 1, a vertex without a positive weight bound to its first joint) become
	//         the mesh's skin (MeshData::Skin), its indices mapped into its Skeleton's joint order (SkinJointCount the
	//         Skeleton's joint count). Each skinned mesh keeps its own inverse bind matrices: where its skin's differ from its
	//         Skeleton's (a merged skin bound elsewhere), each vertex is moved into the Skeleton's bind space by its heaviest
	//         joint's correction (its Skeleton inverse bind's inverse times its skin's), its normal and tangent turned with it
	//         and its morph deltas too, so it deforms exactly as its own skin deforms it whenever the skins share one bind
	//         pose, which exporters give. A mesh that nodes draw with different skins, or with and without one, is imported
	//         once per skin, each skinned variant keyed "mesh:<index>[:<name>]:skin:<skin index>" (the unskinned one keeps the
	//         mesh's key, as does the only variant of a mesh drawn one way). A node whose mesh's triangle primitives lack
	//         JOINTS_0 or WEIGHTS_0 draws unskinned, reported ("nodes[3].skin"); more than four influences (JOINTS_1) are
	//         reported and ignored;
	//       - a mesh's morph targets (POSITION, NORMAL and TANGENT deltas, a NORMAL or TANGENT delta only where the file gives
	//         the base attribute; named after the mesh's extras "targetNames", else "Target <i>", a name taken earlier getting
	//         " <i>" appended; the mesh's weights as their defaults) become MeshData::MorphTargets; a mesh of more than
	//         MaxMeshMorphTargets targets keeps none, reported ("meshes[1].targets") (cgltf_validate refuses primitives that
	//         disagree on their target count);
	//       - "animation:<index>[:<name>]": one AnimationClip per glTF animation (AnimationClipData.h): a translation,
	//         rotation or scale channel of a joint names the joint (its name in the first Skeleton holding it), a weights
	//         channel of a node drawn with imported morph targets the path of its entity from the entity of the nearest
	//         Animator at or above it; translations are scaled by Scale and Step and Linear rotations normalized. A channel
	//         of another node, a node's weights no Animator plays, or a target an earlier channel keys, is skipped with
	//         ASSET_CONTENT_SKIPPED ("animations[0].channels[2]"); an animation left with no channel is skipped whole
	//         ("animations[0]"), as is one whose largest key time is above 0 but below MinAnimationClipDuration (a fuzzed or
	//         broken file), which AnimationClipData's validation refuses. Its Duration is its largest key time, and its events
	//         are the settings' AnimationEvents that name it. A sampler whose input is not float key times strictly increasing
	//         from 0 within a day, or whose output does not fit its path, fails the import;
	//       - the prefab: joint nodes become no entity (they are the Skeleton's joints; a mesh on a joint is reported,
	//         "nodes[4].mesh", and not placed). A non-joint node below a joint is placed under the Animator entity of the first
	//         Skeleton holding that joint, at its rest transform relative to it (from the nodes' TRS; a shear non-uniformly
	//         scaled joints make is approximated). A skinned node whose nearest Animator would not be its own Skeleton's is
	//         placed under its own Skeleton's Animator the same way, or, when its own entity holds another Skeleton's
	//         Animator, draws unskinned, reported ("nodes[5].skin"). Each Animator entity gets an Animator with its Skeleton
	//         and one layer playing the first imported clip that animates it (none without one), so one Animator drives every
	//         SkinnedMesh of a merged Skeleton; the Skeleton's model space is that entity's. Each node drawn with a skin or
	//         with imported morph targets gets a SkinnedMesh beside its MeshRenderer, its MorphWeights the node's glTF weights,
	//         else its mesh's, clamped into [-1, 1]. Entities are written in canonical order (§5.1).
	//     The skins and clips reference only sub-assets of their own source, so Import.Dependencies gains nothing for them
	//     (ADR 0028's phase-2 importer-dependency rule holds: every asset outside the source a skin or clip references is
	//     listed, and there is none).
	//   - M46 (version 4, MeshData version 3; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3B):
	//       - _WIND, a glTF application attribute (VEC4 of float, or normalized unsigned byte or short), clamped to [0, 1],
	//         becomes the mesh's wind stream (MeshData::Wind, MeshVertexWind's x branch, y phase, z flutter, w bend mask);
	//         in a mesh where some primitives have it, the others' vertices take (0, 0, 0, 0). The Wind setting becomes
	//         every imported material's wind block;
	//       - with Lods FromGltf, a node's MSFT_lod extension names its further levels by "ids" (nodes), its extras'
	//         MSFT_screencoverage, when it is a strictly decreasing list in (0, 1] of at least one value per level, replacing
	//         LodScreenSizes (level L takes over at its value L - 1); without the extension, sibling nodes named
	//         <name>_LOD<n> (n from 0, contiguous) form one group, whose _LOD0 node is placed as an entity named <name>.
	//         A level is the level node's mesh in its own space (the level node's transform is ignored), with Scale
	//         applied; a level node and its subtree are never placed. The levels belong to level 0's glTF mesh, so every
	//         node that draws it draws them;
	//       - with Lods Generate, every mesh (each skin variant, and "mesh:merged") gets the levels meshoptimizer
	//         simplifies from level 0 at LodTriangleRatios within LodMaxError (AssetPipeline/Private/MeshSimplifier.h),
	//         index-only, each submesh on its own with its borders locked;
	//       - a level k (1-based) switches at LodScreenSizes[k - 1], or its screen coverage. A level is dropped with
	//         ASSET_LOD_INVALID (subject "nodes[<level node>]" for an authored level, "meshes[<i>].lods[<k>]" or
	//         "mesh:merged.lods[<k>]" for a generated one), the import still succeeding, when its submesh count or a
	//         submesh's material differs from level 0's; it is a fifth level or later, or the settings give no switch size
	//         for it; it leaves a gap in the _LOD<n> names; its level node, or its level 0 node, has no mesh; it is an
	//         authored level of a skinned or morphed mesh, or of a mesh an earlier group already gave levels; it is
	//         generated and keeps more than 90 % of the triangles of the level before it, or none; or FromGltf meets
	//         MergeMeshes (the merged mesh has no authored level). ASSET_LOD_INVALID also reports, the import still
	//         succeeding, an MSFT_lod extension that is not an object with an "ids" array of node indices (subject
	//         "nodes[<i>].extensions.MSFT_lod", the group ignored), an id out of range, naming its own node or a node of
	//         another group ("nodes[<i>].extensions.MSFT_lod.ids[<k>]", the id skipped), and an MSFT_screencoverage that
	//         is not such a list ("nodes[<i>].extras.MSFT_screencoverage", LodScreenSizes used). Without levels and _WIND,
	//         every mesh's level 0 is version 3's import byte for byte, and sub-asset keys are unchanged.
	//   Names in keys are the glTF names with every character outside [A-Za-z0-9_-] replaced by '_' (empty: no name part).
	//
	// Buffers and images may be embedded (.glb binary chunk, bufferView images), data URIs, or relative external files. The
	// external files are the source's dependency closure (§6.4, §13.2), read through ImportContext::ReadDependency, which
	// the EditorAssetManager turns into dependency metas (a file that is already another asset's dependency fails the
	// import, AssetMetadata.h). URIs are validated exactly once, in this order, and the same rule serves Import,
	// ListExternalUris, ListDependencyFiles and asset.import's copy:
	//   1. A URI that starts with "data:" (ASCII case-insensitive) is a data URI and never names a file.
	//   2. Every other URI is percent-decoded once (RFC 3986): a '%' not followed by two hex digits, and "%00", are
	//      rejected; the decoded bytes must be valid UTF-8 without control characters (below U+0020).
	//   3. The decoded text must be a relative path inside the source's directory tree: no backslash (glTF URIs separate with
	//      '/'), no ':' anywhere (which rejects drive letters and every scheme), no leading '/', and no empty, "." or ".."
	//      segment.
	// A URI that fails is rejected with ImportFailed naming the URI as written ("GltfImporter: parent-escaping, absolute and
	// http URIs are rejected": "../Outside.bin", "/Models/Box.bin", "C:/Models/Box.bin", "http://example.com/Box.bin", and
	// the encoded forms "%2e%2e/Outside.bin", "..%5COutside.bin" and "%2FModels/Box.bin"). The decoded path is resolved
	// against the source's directory.
	//
	// Rejected with ImportFailed and a precise message: a required extension other than KHR_materials_emissive_strength,
	// KHR_texture_transform and KHR_mesh_quantization, naming it (Draco "KHR_draco_mesh_compression", meshopt
	// "EXT_meshopt_compression", BasisU "KHR_texture_basisu": "GltfImporter: required Draco extension is rejected with a
	// precise message"); a buffer view or accessor (sparse parts included) whose elements do not fit in its data, naming it
	// ("accessors[<i>]"; an accessor without a buffer view may have at most 2^24 elements); cgltf_validate failures; an index
	// out of range, a NaN or infinite attribute value, or a primitive with no non-degenerate triangle. Degenerate triangles
	// are dropped. Cameras and lights are skipped (§1.2) with ASSET_CONTENT_SKIPPED, as are skins and animations with
	// MergeMeshes (version 3 imports them otherwise, above).
	//
	// Warnings (ImportResult::Diagnostics, so they persist in the cache manifest): ASSET_UNSUPPORTED_UV_SET for a texture
	// bound to TEXCOORD_2 or a later set (ignored: meshes have two UV sets), ASSET_TANGENTS_APPROXIMATED, and
	// ASSET_CONTENT_SKIPPED with the skipped item as Subject ("meshes[1].primitives[0]", "skins[0]", "animations[0]",
	// "animations[0].channels[1]", "nodes[2].skin", "cameras[0]", "KHR_lights_punctual.lights[0]"), and (version 4)
	// ASSET_LOD_INVALID for each dropped level (above). Version 2 no longer
	// produces ASSET_VERTEX_COLORS_IGNORED: COLOR_0 is imported (the code stays, never reused;
	// ProjectValidator::GetRetiredCodes).
	class GltfImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Gltf";
		// M23: version 2 imports TEXCOORD_1 and COLOR_0 (MeshData version 2) and each map's UV set (Material version 2);
		// version 3 (C23.3, ADR 0028's phase-2 ledger) skins, animations and morph targets, and the AnimationEvents setting;
		// version 4 (M46) levels of detail, _WIND and the Lods, LodScreenSizes, LodTriangleRatios, LodMaxError and Wind
		// settings (MeshData and Material version 3).
		static constexpr uint32_t Version = 4;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Prefab; }
		// ".gltf", ".glb".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return "GltfImportSettings"; }

		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// ListExternalUris resolved against `sourcePath`'s directory, sorted (the EditorAssetManager's closure resolution).
		[[nodiscard]] Result<std::vector<VfsPath>> ListDependencyFiles(std::span<const std::byte> source, const VfsPath& sourcePath) const override;

		// The external buffers and images of `source` as decoded relative paths ('/'-separated, validated by the URI rule
		// above, without data URIs), in glTF order, each once: exactly the paths Import reads, and what asset.import copies
		// with a .gltf (§13.2 "dependency closure"). Errors: Parse for a file cgltf cannot parse; ImportFailed for a rejected
		// URI, naming it as written.
		[[nodiscard]] static Result<std::vector<std::string>> ListExternalUris(std::span<const std::byte> source);

		// Registers GltfAnimationEvent, GltfLodSource and GltfImportSettings (with a Validate rule for the level settings), and
		// MaterialWind when the registry has no such struct yet (RegisterMaterialWindType).
		static void RegisterTypes(TypeRegistry& registry);
	};

}
