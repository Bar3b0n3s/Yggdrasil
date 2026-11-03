#pragma once

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

	// Registry struct "GltfImportSettings" (the §6.4 example's settings).
	struct GltfImportSettings
	{
		float Scale = 1.0f;                  // uniform scale baked into the imported data (see Output); > 0
		bool GenerateMissingTangents = true; // MikkTSpace for primitives without TANGENT (normal-mapped or not)
		bool ImportMaterials = true;         // false: no material sub-assets; every slot uses the Default material
		bool MergeMeshes = false;            // true: every mesh of the scene merged into the one mesh sub-asset "mesh:merged"
		// M23 (C23.3; decision 25): the events of the imported clips, each added to the clip its Animation names, sorted by
		// time in its clip (stable); an event naming no clip of the file, or past its clip's end, fails the import naming it.
		// Registered after MergeMeshes by stream A with version 3, so an earlier .meta reads as no events; unregistered and
		// read by nothing until then.
		std::vector<GltfAnimationEvent> AnimationEvents{};
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
	//   - M23 (version 3, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 25; stream A): skins,
	//     animations and morph targets, which version 2 skips:
	//       - "skin:<index>[:<name>]": one Skeleton per glTF skin (SkeletonData.h): its joints and every node between them and
	//         the skin's Animator entity (the static joints), parents first, each named after its node ("Node <index>" without
	//         a name; a name a joint of the skin already took gets " <index>" appended), with the node's TRS as its rest pose
	//         and the skin's inverse bind matrix (the identity for a node that is not one of skin.joints); Scale is baked into
	//         the rest translations and the inverse binds' translations. Its RootMotionJoint is the skin's first root joint
	//         (the first of skin.joints whose parent is not one of skin.joints), so a rig under an intermediate node moves its
	//         hips, never that node;
	//       - "animation:<index>[:<name>]": one AnimationClip per glTF animation whose channels target a joint of a skin or a
	//         skinned node's morph weights (AnimationClipData.h): a joint channel names its joint, a weights channel the path
	//         of the morphed node's entity from the Animator's entity; translations are scaled by Scale; the clip's events are
	//         the settings' AnimationEvents that name it; a channel targeting another node is skipped with
	//         ASSET_CONTENT_SKIPPED ("animations[0].channels[2]"), and an animation left with no channel is skipped whole. Its
	//         Duration is its largest key time; an animation whose largest key time is above 0 but below
	//         MinAnimationClipDuration (a fuzzed or broken file) is skipped whole with ASSET_CONTENT_SKIPPED ("animations[1]"),
	//         as AnimationClipData's validation refuses it;
	//       - a skinned primitive's JOINTS_0 and WEIGHTS_0 (unsigned byte or short joints; float, or normalized unsigned byte
	//         or short weights, renormalized to sum to 1) become the mesh's skin (MeshData::Skin, its indices remapped to the
	//         Skeleton's joint order, SkinJointCount the skeleton's joint count); a mesh that nodes use with different skins,
	//         or with and without one, is imported once per skin, keyed "mesh:<index>[:<name>]:skin:<skin index>". Its
	//         primitives' morph targets (POSITION, NORMAL and TANGENT deltas; the mesh's extras "targetNames", else
	//         "Target <i>"; its weights as the defaults) become MeshData::MorphTargets; a mesh whose primitives disagree on
	//         their target count keeps none and is reported with ASSET_CONTENT_SKIPPED;
	//       - the prefab: joint nodes become no entity (they are the Skeleton's joints). A skin's Animator entity is the
	//         entity of the nearest node that is not a joint at or above the lowest common ancestor of the skin's root joints
	//         and of the nodes it skins: that ancestor itself when it is not a joint, else its nearest ancestor that is not one
	//         (a skinned node may lie below its own root joint), and the root entity when there is none (the ancestor is the
	//         scene, or every node above it is a joint). That entity gets an Animator with the Skeleton and one layer playing
	//         the first imported clip that animates it (none without one); the skeleton's model space is that entity's. Each
	//         skinned or morphed node's entity gets a SkinnedMesh beside its MeshRenderer, with the mesh's default weights. A
	//         non-joint node below a joint is placed under the Animator's entity at its bind-pose transform relative to it.
	//     The skins and clips reference only sub-assets of their own source, so Import.Dependencies gains nothing for them
	//     (ADR 0028's phase-2 importer-dependency rule holds: every asset outside the source a skin or clip references is
	//     listed, and there is none).
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
	// are dropped. Skins, animations, cameras and lights are skipped (§1.2) with ASSET_CONTENT_SKIPPED (skins and animations
	// only until version 3, above).
	//
	// Warnings (ImportResult::Diagnostics, so they persist in the cache manifest): ASSET_UNSUPPORTED_UV_SET for a texture
	// bound to TEXCOORD_2 or a later set (ignored: meshes have two UV sets), ASSET_TANGENTS_APPROXIMATED, and
	// ASSET_CONTENT_SKIPPED with the skipped item as Subject ("meshes[1].primitives[0]", "skins[0]", "animations[0]",
	// "cameras[0]", "KHR_lights_punctual.lights[0]"). Version 2 no longer produces ASSET_VERTEX_COLORS_IGNORED: COLOR_0 is
	// imported (the code stays, never reused; ProjectValidator::GetRetiredCodes).
	class GltfImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Gltf";
		// M23: version 2 imports TEXCOORD_1 and COLOR_0 (MeshData version 2) and each map's UV set (Material version 2);
		// stream A's version 3 (C23.3, ADR 0028's phase-2 ledger) skins, animations and morph targets.
		static constexpr uint32_t Version = 2;

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

		// Registers GltfImportSettings.
		static void RegisterTypes(TypeRegistry& registry);
	};

}
