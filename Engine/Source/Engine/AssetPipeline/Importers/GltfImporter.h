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

	// Registry struct "GltfImportSettings" (the §6.4 example's settings).
	struct GltfImportSettings
	{
		float Scale = 1.0f;                  // uniform scale baked into the imported data (see Output); > 0
		bool GenerateMissingTangents = true; // MikkTSpace for primitives without TANGENT (normal-mapped or not)
		bool ImportMaterials = true;         // false: no material sub-assets; every slot uses the Default material
		bool MergeMeshes = false;            // true: every mesh of the scene merged into the one mesh sub-asset "mesh:merged"
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
	//     TEXCOORD_0, tangents from the file or else MikkTSpace (Vendor/MikkTSpace) when GenerateMissingTangents, uint32
	//     indices (generated for non-indexed primitives); a primitive needing generated tangents without TEXCOORD_0 gets an
	//     arbitrary perpendicular basis and ASSET_TANGENTS_APPROXIMATED;
	//   - "material:<index>[:<name>]": one Material per glTF material (metallic-roughness factors and textures,
	//     KHR_materials_emissive_strength, KHR_texture_transform scale and offset, alpha mode and cutoff, double-sided);
	//   - "texture:<image index>" (colour data), "texture:<image index>:linear" (metallic-roughness, occlusion) and
	//     "texture:<image index>:normal" (normal maps): Textures through TextureImporter::ImportTextureFromMemory, only for
	//     images that have no Texture meta of their own. An image file with its own Texture meta is referenced by that
	//     standalone handle instead and not imported again ("GltfImporter: an image with its own Texture meta is
	//     referenced, not duplicated"; §6.4); its own settings apply.
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
	// are dropped. Skins, animations, cameras and lights are skipped (§1.2) with ASSET_CONTENT_SKIPPED.
	//
	// Warnings (ImportResult::Diagnostics, so they persist in the cache manifest): ASSET_UNSUPPORTED_UV_SET for a texture
	// bound to TEXCOORD_1 (ignored), ASSET_VERTEX_COLORS_IGNORED for COLOR_0 ("GltfImporter: TEXCOORD_1 and COLOR_0 raise
	// their diagnostics"), ASSET_TANGENTS_APPROXIMATED, and ASSET_CONTENT_SKIPPED with the skipped item as Subject
	// ("meshes[1].primitives[0]", "skins[0]", "animations[0]", "cameras[0]", "KHR_lights_punctual.lights[0]").
	class GltfImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Gltf";
		static constexpr uint32_t Version = 1;

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
