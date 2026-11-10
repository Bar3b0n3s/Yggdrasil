#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/GltfImporter.h"

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/AssetPipeline/Private/TangentGenerator.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Migrations.h"
#include "Engine/Scene/Prefab.h"

#include <cgltf.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>

// The glTF importer (GltfImporter.h; Architecture §6.4, §7.4; Docs/Decisions/0010-m6-decisions.md decision 15).
//
// cgltf parses the document from memory (cgltf_parse); the importer then supplies every buffer itself (the GLB binary
// chunk, a base64 data URI, or an external file read through ImportContext::ReadDependency after the URI rule), so
// cgltf never opens a file and one URI rule serves Import, ListExternalUris and ListDependencyFiles; the importer's own
// overflow-safe bounds check of every buffer view and accessor (CheckAccessors), then cgltf_validate, check the result
// before any accessor is read. cgltf keeps strings as written in the JSON, so every URI, name and
// extension name is JSON-decoded first (DecodeJsonString): the URI rule applies to the URI's string value, which is also
// how error messages quote it ("as written"). Everything is computed in the glTF's own space first (degenerate
// triangles, generated normals and tangents), and only then placed: Scale multiplies positions (and, for MergeMeshes, the
// node's world matrix transforms the primitive), so normals and tangents never depend on Scale.
//
// Import reads every external buffer, and the external images of the textures its materials bind (none with
// ImportMaterials off; a standalone image is looked up, not read). ListExternalUris lists every external buffer and image,
// which is the closure asset.import copies and the EditorAssetManager turns into dependency metas.
//
// Version 3 (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 25): after the scene's nodes are collected, PlanSkins
// decides everything the skins change before any mesh is read: which skins import, which nodes are joints, each skin's
// Animator entity, the Skeletons (the skins of one Animator entity merged into one, the lead's decision recorded in the
// ADR's stream A notes), each skin's joint map into its Skeleton and its per-joint corrections, which nodes are skinned and
// where every entity is placed. ImportMeshes then reads each mesh once and cooks one variant per skin that uses it,
// ImportSkeletons and ImportAnimations cook the Skeletons and AnimationClips, and BuildPrefab writes the entities in
// canonical order with their Animators and SkinnedMeshes. MergeMeshes, which merges every mesh into one static mesh, skips
// skins, animations and morph targets as version 2 did.

namespace Engine {

	namespace {

		struct CgltfDataDeleter
		{
			void operator()(cgltf_data* data) const { cgltf_free(data); }
		};

		using CgltfDataPointer = std::unique_ptr<cgltf_data, CgltfDataDeleter>;

		// How a material uses an image, which selects the texture sub-asset's key suffix and its TextureUsage.
		enum class ImageUse : uint8_t
		{
			Color,  // "texture:<image>": base colour and emissive (sRGB)
			Linear, // "texture:<image>:linear": metallic-roughness and occlusion
			Normal  // "texture:<image>:normal": normal maps
		};

		// Positions, normals and tangents of a primitive in its node's space, with the triangle list; the attribute
		// vectors have one entry per vertex. TexCoords1 and Colors (M23, MeshData version 2's attribute stream) are empty
		// when the primitive has no TEXCOORD_1 or no COLOR_0.
		struct PrimitiveAttributes
		{
			std::vector<glm::vec3> Positions{};
			std::vector<glm::vec3> Normals{};
			std::vector<glm::vec2> TexCoords{};
			std::vector<glm::vec4> Tangents{};
			std::vector<glm::vec2> TexCoords1{};
			std::vector<glm::vec4> Colors{};
			// M23 (version 3): JOINTS_0 (the skin's joint indices as written) and WEIGHTS_0 (renormalized), empty unless the
			// mesh is read for a skin; and one delta per vertex for each morph target, empty unless its targets import.
			std::vector<std::array<uint16_t, 4>> Joints{};
			std::vector<glm::vec4> Weights{};
			std::vector<std::vector<MeshMorphDelta>> Morphs{};
			std::vector<uint32_t> Indices{};
		};

		// One triangle primitive in the space of its node, ready to be placed into a mesh.
		struct PrimitiveGeometry
		{
			std::vector<MeshVertex> Vertices{};
			// One per vertex when the primitive has TEXCOORD_1 or COLOR_0 (the missing one at its default), else empty.
			std::vector<MeshVertexAttributes> Attributes{};
			// M23 (version 3): the primitive's JOINTS_0 and renormalized WEIGHTS_0 as read (indices into its glTF skin's
			// joints), empty unless read for a skin; Skin, the skin of the mesh variant being cooked (indices into the
			// Skeleton's joints, ApplySkin); one delta per vertex for each imported morph target.
			std::vector<std::array<uint16_t, 4>> Joints{};
			std::vector<glm::vec4> Weights{};
			std::vector<MeshSkinVertex> Skin{};
			std::vector<std::vector<MeshMorphDelta>> Morphs{};
			std::vector<uint32_t> Indices{}; // triangle list, counter-clockwise front faces
		};

		// A node of the default scene, in the prefab's canonical order (depth-first, children in glTF order).
		struct SceneNode
		{
			size_t Node = 0;
			UUID ID{};
			UUID Parent{};                     // the root entity's ID for a root node of the scene
			std::string Name{};                // the entity name
			TransformComponent Transform{};    // the entity's local transform, translation multiplied by Scale
			glm::mat4 World = glm::mat4(1.0f); // relative to the prefab root, without Scale (MergeMeshes, version 3's placement)
		};

		// M23 (version 3): what PlanSkins decided for one glTF skin.
		struct SkinPlan
		{
			bool Imported = false;                  // false: skipped (reported); the nodes it skins import unskinned
			std::vector<size_t> Joints{};           // the glTF node of each of the skin's joints, in skin.joints order
			std::vector<glm::dmat4> InverseBinds{}; // per joint, as the file gives them (without Scale)
			size_t Group = 0;                       // its SkeletonGroup
			std::vector<uint16_t> JointMap{};       // per joint: its index in the group's Skeleton
			// Per joint: the matrix that carries this skin's bind space into its Skeleton's (ApplySkin), nullopt when the
			// Skeleton's inverse bind is the skin's own (no correction).
			std::vector<std::optional<glm::dmat4>> Corrections{};
			size_t FirstRootJoint = 0;   // the first of Joints whose parent is not one of them
			std::vector<size_t> Drawn{}; // the scene nodes drawn with the skin, in canonical order
		};

		// M23 (version 3): one Skeleton: the imported skins whose Animator entity is the same entity, merged.
		struct SkeletonGroup
		{
			std::optional<size_t> AnimatorNode{}; // the glTF node whose entity holds the Animator; nullopt: the root entity
			std::vector<size_t> Skins{};          // ascending; the first names the sub-asset
			std::vector<size_t> Nodes{};          // the glTF node of each Skeleton joint, in canonical order
			SkeletonData Skeleton{};
			AssetHandle Handle{};
			AssetHandle FirstClip{}; // the first imported clip that animates the Skeleton (its Animator's one layer)
		};

		// M23 (version 3): an animation's channels as a clip, before the settings' events are added and it is cooked.
		struct PendingClip
		{
			std::string Key{};
			AnimationClipData Clip{};
			std::vector<size_t> Groups{}; // the SkeletonGroups it animates, ascending
		};

	}

	namespace Utils {

		static constexpr std::array<std::string_view, 3> SupportedRequiredExtensions = { "KHR_materials_emissive_strength",
			"KHR_texture_transform", "KHR_mesh_quantization" };

		// The hint for every rejected URI (the URI rule of GltfImporter.h).
		static constexpr std::string_view UriHint = "a glTF file may reference other files only by relative paths inside its own directory "
													"tree; embed the data in the file or copy the referenced file next to it";

		// Tolerances of the node-matrix decomposition: the bottom row must be (0, 0, 0, 1) and the normalized columns
		// orthogonal (no shear), within these bounds.
		static constexpr float MatrixRowTolerance = 1e-5f;
		static constexpr float MatrixShearTolerance = 1e-3f;

		// The end of every message about a node matrix that has no translation-rotation-scale form.
		static constexpr std::string_view NotDecomposable = "which cannot be decomposed into translation, rotation and scale";

		// The largest count of an accessor without a buffer view (all zeros, or sparse substitutions over zeros). Its count
		// is backed by no bytes of the file, so this bounds what the importer allocates for it; 2^24 elements is far more
		// than any engine mesh. An accessor with a buffer view is bounded by the bytes of its view instead.
		static constexpr size_t MaxUnbackedAccessorCount = size_t(1) << 24;

		// a + b, or nullopt when the sum does not fit in size_t.
		static std::optional<size_t> CheckedAdd(size_t a, size_t b)
		{
			if (a > std::numeric_limits<size_t>::max() - b)
				return std::nullopt;
			return a + b;
		}

		// a * b, or nullopt when the product does not fit in size_t.
		static std::optional<size_t> CheckedMultiply(size_t a, size_t b)
		{
			if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
				return std::nullopt;
			return a * b;
		}

		// The end of `count` elements of `elementSize` bytes each, `stride` bytes apart, starting at byte `offset`:
		// offset + stride * (count - 1) + elementSize, or nullopt when it does not fit in size_t. No elements end at `offset`.
		static std::optional<size_t> GetElementsEnd(size_t offset, size_t stride, size_t count, size_t elementSize)
		{
			if (count == 0)
				return offset;
			const std::optional<size_t> span = CheckedMultiply(stride, count - 1);
			if (!span)
				return std::nullopt;
			const std::optional<size_t> start = CheckedAdd(offset, *span);
			if (!start)
				return std::nullopt;
			return CheckedAdd(*start, elementSize);
		}

		static char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		static bool StartsWithIgnoreAsciiCase(std::string_view text, std::string_view prefix)
		{
			if (text.size() < prefix.size())
				return false;
			for (size_t i = 0; i < prefix.size(); ++i)
			{
				if (ToLowerAscii(text[i]) != ToLowerAscii(prefix[i]))
					return false;
			}
			return true;
		}

		static bool EndsWithIgnoreAsciiCase(std::string_view text, std::string_view suffix)
		{
			return text.size() >= suffix.size() && StartsWithIgnoreAsciiCase(text.substr(text.size() - suffix.size()), suffix);
		}

		// The text of a string cgltf parsed, with its JSON escapes decoded ("\/", "\\", "é"): cgltf keeps every string
		// as written in the JSON and leaves decoding to cgltf_decode_string. A "\u0000" escape stays in the result as a NUL
		// character (which the URI rule rejects as a control character). Empty for a missing string.
		static std::string DecodeJsonString(const char* text)
		{
			if (text == nullptr)
				return {};
			std::string decoded(text);
			decoded.resize(cgltf_decode_string(decoded.data()));
			return decoded;
		}

		// Rule 1 of the URI rule: "data:" in any ASCII case.
		static bool IsDataUri(std::string_view uri)
		{
			return StartsWithIgnoreAsciiCase(uri, "data:");
		}

		static int HexDigitValue(char character)
		{
			if (character >= '0' && character <= '9')
				return character - '0';
			if (character >= 'a' && character <= 'f')
				return character - 'a' + 10;
			if (character >= 'A' && character <= 'F')
				return character - 'A' + 10;
			return -1;
		}

		static std::unexpected<Error> RejectUri(std::string_view uri, std::string_view item, std::string_view reason)
		{
			return std::unexpected(Error(ErrorCode::ImportFailed, std::format("{} has the URI '{}', which is rejected: {}", item, uri, reason))
					.WithHint(std::string(UriHint)));
		}

		// Rules 2 and 3 of the URI rule (GltfImporter.h) for a URI that is not a data URI: the percent-decoded relative
		// path, or ImportFailed naming `item` ("buffers[0]") and the URI as written.
		static Result<std::string> DecodeRelativeUri(std::string_view uri, std::string_view item)
		{
			if (uri.empty())
				return RejectUri(uri, item, "it is empty");

			std::string decoded;
			decoded.reserve(uri.size());
			for (size_t i = 0; i < uri.size(); ++i)
			{
				if (uri[i] != '%')
				{
					decoded.push_back(uri[i]);
					continue;
				}
				const int high = i + 2 < uri.size() ? HexDigitValue(uri[i + 1]) : -1;
				const int low = i + 2 < uri.size() ? HexDigitValue(uri[i + 2]) : -1;
				if (high < 0 || low < 0)
					return RejectUri(uri, item, "a '%' is not followed by two hexadecimal digits");
				const int value = high * 16 + low;
				if (value == 0)
					return RejectUri(uri, item, "it encodes a NUL character (%00)");
				decoded.push_back(static_cast<char>(value));
				i += 2;
			}

			if (!IsValidUtf8(decoded))
				return RejectUri(uri, item, "it is not valid UTF-8 once percent-decoded");
			if (std::ranges::any_of(decoded, [](char character)
			{
				return static_cast<unsigned char>(character) < 0x20;
			}))
				return RejectUri(uri, item, "it contains a control character");
			if (decoded.contains('\\'))
				return RejectUri(uri, item, "it contains a backslash (glTF URIs separate segments with '/')");
			if (decoded.contains(':'))
				return RejectUri(uri, item, "it contains ':' (a drive letter or a scheme such as 'http:'; only relative paths and data: URIs are allowed)");
			if (decoded.starts_with('/'))
				return RejectUri(uri, item, "it is an absolute path");

			size_t start = 0;
			while (start <= decoded.size())
			{
				const size_t end = std::min(decoded.find('/', start), decoded.size());
				const std::string_view segment = std::string_view(decoded).substr(start, end - start);
				if (segment.empty())
					return RejectUri(uri, item, "it has an empty path segment");
				if (segment == ".")
					return RejectUri(uri, item, "it has a '.' segment");
				if (segment == "..")
					return RejectUri(uri, item, "it leaves the directory of the glTF file (a '..' segment)");
				start = end + 1;
			}
			return decoded;
		}

		static int Base64Value(char character)
		{
			if (character >= 'A' && character <= 'Z')
				return character - 'A';
			if (character >= 'a' && character <= 'z')
				return character - 'a' + 26;
			if (character >= '0' && character <= '9')
				return character - '0' + 52;
			if (character == '+')
				return 62;
			if (character == '/')
				return 63;
			return -1;
		}

		// The bytes of a base64 data URI ("data:[<media type>];base64,<data>", RFC 2397 with RFC 4648 base64; padding
		// optional). Errors: ImportFailed naming `item` for another encoding or invalid base64.
		static Result<Buffer> DecodeDataUri(std::string_view uri, std::string_view item)
		{
			const size_t comma = uri.find(',');
			if (comma == std::string_view::npos || !EndsWithIgnoreAsciiCase(uri.substr(0, comma), ";base64"))
				return MakeError(ErrorCode::ImportFailed, "{} has a data URI that is not base64-encoded (\"data:<type>;base64,<data>\")", item);

			std::string_view text = uri.substr(comma + 1);
			size_t padding = 0;
			while (!text.empty() && text.back() == '=' && padding < 2)
			{
				text.remove_suffix(1);
				++padding;
			}
			if (text.size() % 4 == 1 || (padding != 0 && (text.size() + padding) % 4 != 0))
				return MakeError(ErrorCode::ImportFailed, "{} has a data URI whose base64 data has an invalid length", item);

			Buffer bytes;
			bytes.reserve(text.size() / 4 * 3 + 2);
			uint32_t accumulator = 0;
			uint32_t bits = 0;
			for (size_t i = 0; i < text.size(); ++i)
			{
				const int value = Base64Value(text[i]);
				if (value < 0)
					return MakeError(ErrorCode::ImportFailed, "{} has a data URI with an invalid base64 character at offset {}", item, comma + 1 + i);
				accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					bytes.push_back(static_cast<std::byte>((accumulator >> bits) & 0xFFu));
				}
			}
			return bytes;
		}

		static std::string_view DescribeCgltfResult(cgltf_result result)
		{
			switch (result)
			{
				case cgltf_result_success:         return "success";
				case cgltf_result_data_too_short:  return "the data is too short: an accessor, buffer view or index reaches past its data";
				case cgltf_result_unknown_format:  return "the data is neither glTF JSON nor binary glTF (GLB)";
				case cgltf_result_invalid_json:    return "the JSON is malformed";
				case cgltf_result_invalid_gltf:    return "the document violates the glTF 2.0 specification";
				case cgltf_result_invalid_options: return "invalid parser options";
				case cgltf_result_file_not_found:  return "a file was not found";
				case cgltf_result_io_error:        return "an I/O error occurred";
				case cgltf_result_out_of_memory:   return "out of memory";
				case cgltf_result_legacy_gltf:     return "glTF 1.0 is not supported; export the model as glTF 2.0";
				case cgltf_result_max_enum:        break;
			}
			return "an unknown cgltf error";
		}

		static Result<CgltfDataPointer> ParseGltf(std::span<const std::byte> source)
		{
			cgltf_options options{};
			cgltf_data* parsed = nullptr;
			const cgltf_result result = cgltf_parse(&options, source.data(), source.size(), &parsed);
			CgltfDataPointer data(parsed);
			if (result != cgltf_result_success)
				return MakeError(ErrorCode::Parse, "not a glTF 2.0 file cgltf can parse: {}", DescribeCgltfResult(result));
			if (data == nullptr)
				return MakeError(ErrorCode::Parse, "cgltf returned no document");
			return data;
		}

		// An error location naming only the file `path`.
		static ErrorLocation MakeFileLocation(const std::string& path)
		{
			ErrorLocation location;
			location.File = path;
			return location;
		}

		// The project-relative path of a project source ("Assets/Models/Box.gltf"), the full path of any other.
		static std::string GetDisplayPath(const VfsPath& path)
		{
			return path.GetScheme() == "project" ? std::string(path.GetPath()) : path.ToString();
		}

		// The decoded glTF name `name` with every character outside [A-Za-z0-9_-] replaced by one '_' (a UTF-8 sequence counts
		// as one character), for sub-asset keys (GltfImporter.h).
		static std::string SanitizeKeyName(const char* name)
		{
			std::string sanitized;
			for (const char character : DecodeJsonString(name))
			{
				const bool allowed = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' || character == '-';
				if (allowed)
					sanitized.push_back(character);
				else if ((static_cast<unsigned char>(character) & 0xC0u) != 0x80u)
					sanitized.push_back('_');
			}
			return sanitized;
		}

		// "<kind>:<index>[:<name>]".
		static std::string MakeKey(std::string_view kind, size_t index, const char* name)
		{
			const std::string sanitized = SanitizeKeyName(name);
			return sanitized.empty() ? std::format("{}:{}", kind, index) : std::format("{}:{}:{}", kind, index, sanitized);
		}

		// A decoded glTF name usable as an entity or slot name, or `fallback` for a missing, empty or invalid one (not UTF-8,
		// or holding a control character).
		static std::string GetName(const char* name, std::string fallback)
		{
			std::string decoded = DecodeJsonString(name);
			const bool control = std::ranges::any_of(decoded, [](char character)
			{
				return static_cast<unsigned char>(character) < 0x20;
			});
			if (decoded.empty() || control || !IsValidUtf8(decoded))
				return fallback;
			return decoded;
		}

		static std::string_view GetAccessorTypeName(cgltf_type type)
		{
			switch (type)
			{
				case cgltf_type_scalar:   return "SCALAR";
				case cgltf_type_vec2:     return "VEC2";
				case cgltf_type_vec3:     return "VEC3";
				case cgltf_type_vec4:     return "VEC4";
				case cgltf_type_mat2:     return "MAT2";
				case cgltf_type_mat3:     return "MAT3";
				case cgltf_type_mat4:     return "MAT4";
				case cgltf_type_invalid:
				case cgltf_type_max_enum: break;
			}
			return "invalid";
		}

		static std::string_view GetPrimitiveModeName(cgltf_primitive_type type)
		{
			switch (type)
			{
				case cgltf_primitive_type_points:         return "POINTS";
				case cgltf_primitive_type_lines:          return "LINES";
				case cgltf_primitive_type_line_loop:      return "LINE_LOOP";
				case cgltf_primitive_type_line_strip:     return "LINE_STRIP";
				case cgltf_primitive_type_triangles:      return "TRIANGLES";
				case cgltf_primitive_type_triangle_strip: return "TRIANGLE_STRIP";
				case cgltf_primitive_type_triangle_fan:   return "TRIANGLE_FAN";
				case cgltf_primitive_type_invalid:
				case cgltf_primitive_type_max_enum:       break;
			}
			return "invalid";
		}

		// The TEXCOORD_<n> set a texture view samples: KHR_texture_transform's texCoord override, else textureInfo.texCoord.
		static cgltf_int GetTexCoord(const cgltf_texture_view& view)
		{
			return view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
		}

		// M23 (Material version 2): the UV set of a map imported as `map`: UV1 for a texture on TEXCOORD_1, else UV0 (an
		// empty slot, or a texture on TEXCOORD_2 and up, which the importer ignores with ASSET_UNSUPPORTED_UV_SET).
		static TextureCoordinateSet GetTextureCoordinateSet(const cgltf_texture_view& view, AssetHandle map)
		{
			return map.IsValid() && GetTexCoord(view) == 1 ? TextureCoordinateSet::UV1 : TextureCoordinateSet::UV0;
		}

		// The TEXCOORD_<n> set the normal texture of `primitive`'s material samples, which its generated tangents follow;
		// 0 without a material or a normal texture.
		static cgltf_int GetNormalTexCoord(const cgltf_primitive& primitive)
		{
			if (primitive.material == nullptr || primitive.material->normal_texture.texture == nullptr)
				return 0;
			return GetTexCoord(primitive.material->normal_texture);
		}

		static bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// The normal of the triangle (a, b, c) scaled by twice its area: zero exactly when the triangle is degenerate.
		static glm::vec3 GetAreaNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
		{
			return glm::cross(b - a, c - a);
		}

		// `vector` normalized, or the zero vector when it has no direction (zero, subnormal underflow or not finite).
		static glm::vec3 NormalizeOrZero(const glm::vec3& vector)
		{
			if (!IsFinite(vector))
				return glm::vec3(0.0f);
			const float lengthSquared = glm::dot(vector, vector);
			if (!(lengthSquared > 0.0f) || !std::isfinite(lengthSquared))
				return glm::vec3(0.0f);
			const glm::vec3 normalized = vector / std::sqrt(lengthSquared);
			return IsFinite(normalized) ? normalized : glm::vec3(0.0f);
		}

		// Each component's magnitude raised to MinTransformScaleMagnitude, keeping its sign (a zero becomes positive), so
		// the prefab's Transform satisfies its registry bounds (§5.3) and the world matrix stays invertible.
		static glm::vec3 ClampScaleMagnitude(const glm::vec3& scale)
		{
			glm::vec3 clamped = scale;
			for (int axis = 0; axis < 3; ++axis)
			{
				if (std::fabs(clamped[axis]) < MinTransformScaleMagnitude)
					clamped[axis] = std::signbit(clamped[axis]) ? -MinTransformScaleMagnitude : MinTransformScaleMagnitude;
			}
			return clamped;
		}

		static glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
		{
			glm::mat4 matrix = glm::mat4_cast(rotation);
			matrix[0] *= scale.x;
			matrix[1] *= scale.y;
			matrix[2] *= scale.z;
			matrix[3] = glm::vec4(translation, 1.0f);
			return matrix;
		}

		// Area-weighted vertex normals of `attributes` (the sum of the area normals of every triangle that uses a vertex,
		// normalized). A vertex whose sum cancels out takes the direction of its first triangle, and +Y when even that has
		// none. Only vertices with `replace` set are written.
		static void GenerateNormals(PrimitiveAttributes& attributes, const std::vector<bool>& replace)
		{
			std::vector<glm::vec3> sums(attributes.Positions.size(), glm::vec3(0.0f));
			std::vector<glm::vec3> first(attributes.Positions.size(), glm::vec3(0.0f)); // zero until a triangle gives a direction
			for (size_t corner = 0; corner + 2 < attributes.Indices.size(); corner += 3)
			{
				const uint32_t a = attributes.Indices[corner];
				const uint32_t b = attributes.Indices[corner + 1];
				const uint32_t c = attributes.Indices[corner + 2];
				const glm::vec3 areaNormal = GetAreaNormal(attributes.Positions[a], attributes.Positions[b], attributes.Positions[c]);
				for (const uint32_t vertex : { a, b, c })
				{
					sums[vertex] += areaNormal;
					if (first[vertex] == glm::vec3(0.0f))
						first[vertex] = NormalizeOrZero(areaNormal);
				}
			}
			for (size_t vertex = 0; vertex < attributes.Positions.size(); ++vertex)
			{
				if (!replace[vertex])
					continue;
				glm::vec3 normal = NormalizeOrZero(sums[vertex]);
				if (normal == glm::vec3(0.0f))
					normal = first[vertex];
				attributes.Normals[vertex] = normal != glm::vec3(0.0f) ? normal : glm::vec3(0.0f, 1.0f, 0.0f);
			}
		}

		// Applies per-corner tangents: a vertex keeps the tangent of its first corner, and every corner whose tangent
		// differs (MikkTSpace split it) is redirected to a copy of the vertex with that tangent, appended in corner order.
		// Vertices nothing splits keep their index, so a primitive keeps its vertex order whatever its tangents.
		static void ApplyCornerTangents(PrimitiveAttributes& attributes, const std::vector<glm::vec4>& cornerTangents)
		{
			const size_t originalCount = attributes.Positions.size();
			attributes.Tangents.assign(originalCount, glm::vec4(0.0f));
			std::vector<bool> assigned(originalCount, false);
			// The copies made of each original vertex, as (tangent, index) in creation order.
			std::vector<std::vector<std::pair<glm::vec4, uint32_t>>> copies(originalCount);
			for (size_t corner = 0; corner < attributes.Indices.size(); ++corner)
			{
				const uint32_t vertex = attributes.Indices[corner];
				const glm::vec4& tangent = cornerTangents[corner];
				if (!assigned[vertex])
				{
					assigned[vertex] = true;
					attributes.Tangents[vertex] = tangent;
					continue;
				}
				if (attributes.Tangents[vertex] == tangent)
					continue;
				const auto existing = std::ranges::find_if(copies[vertex], [&tangent](const std::pair<glm::vec4, uint32_t>& copy)
				{
					return copy.first == tangent;
				});
				if (existing != copies[vertex].end())
				{
					attributes.Indices[corner] = existing->second;
					continue;
				}
				const uint32_t copyIndex = static_cast<uint32_t>(attributes.Positions.size());
				attributes.Positions.push_back(attributes.Positions[vertex]);
				attributes.Normals.push_back(attributes.Normals[vertex]);
				attributes.TexCoords.push_back(attributes.TexCoords[vertex]);
				attributes.Tangents.push_back(tangent);
				if (!attributes.TexCoords1.empty())
					attributes.TexCoords1.push_back(attributes.TexCoords1[vertex]);
				if (!attributes.Colors.empty())
					attributes.Colors.push_back(attributes.Colors[vertex]);
				if (!attributes.Joints.empty())
				{
					attributes.Joints.push_back(attributes.Joints[vertex]);
					attributes.Weights.push_back(attributes.Weights[vertex]);
				}
				for (std::vector<MeshMorphDelta>& deltas : attributes.Morphs)
					deltas.push_back(deltas[vertex]);
				copies[vertex].emplace_back(tangent, copyIndex);
				attributes.Indices[corner] = copyIndex;
			}
		}

		// The triangle list of a primitive whose vertices are drawn in the order of `indices` in `type` mode (glTF 2.0
		// §3.7.2.1).
		static std::vector<uint32_t> Triangulate(cgltf_primitive_type type, std::span<const uint32_t> indices)
		{
			std::vector<uint32_t> triangles;
			const size_t count = indices.size();
			if (type == cgltf_primitive_type_triangles)
			{
				triangles.assign(indices.begin(), indices.end());
				return triangles;
			}
			if (count < 3)
				return triangles;
			triangles.reserve((count - 2) * 3);
			for (size_t i = 0; i + 2 < count; ++i)
			{
				if (type == cgltf_primitive_type_triangle_strip)
				{
					// Triangle i is (i, i + 1 + i % 2, i + 2 - i % 2), which keeps the winding of every triangle.
					triangles.push_back(indices[i]);
					triangles.push_back(indices[i + 1 + i % 2]);
					triangles.push_back(indices[i + 2 - i % 2]);
				}
				else
				{
					// Fan triangle i is (i + 1, i + 2, 0).
					triangles.push_back(indices[i + 1]);
					triangles.push_back(indices[i + 2]);
					triangles.push_back(indices[0]);
				}
			}
			return triangles;
		}

		// M23 (version 3): the bound on the candidates MakeUniqueName tries after the name and its index suffix.
		static constexpr size_t MaxUniqueNameAttempts = MaxSkeletonJoints + 2;

		// A name for item `index` (a joint's node, a morph target) that `taken` does not hold and that has 1 to `maxBytes`
		// bytes: `name` itself, then `name` with " <index>" appended, then "<fallback> <index>" and "<fallback> <index> <k>"
		// for k = 2, 3, ...; nullopt when every candidate is taken (only possible for more than MaxSkeletonJoints names).
		static std::optional<std::string> MakeUniqueName(const std::string& name, size_t index, std::string_view fallback, size_t maxBytes,
			const std::vector<std::string>& taken)
		{
			const auto isFree = [&taken, maxBytes](const std::string& candidate)
			{
				return !candidate.empty() && candidate.size() <= maxBytes && std::ranges::find(taken, candidate) == taken.end();
			};
			std::vector<std::string> candidates = { name, std::format("{} {}", name, index), std::format("{} {}", fallback, index) };
			for (const std::string& candidate : candidates)
			{
				if (isFree(candidate))
					return candidate;
			}
			for (size_t attempt = 2; attempt < MaxUniqueNameAttempts + 2; ++attempt)
			{
				std::string candidate = std::format("{} {} {}", fallback, index, attempt);
				if (isFree(candidate))
					return candidate;
			}
			return std::nullopt;
		}

		// Whether every element of `matrix` is finite.
		static bool IsFinite(const glm::dmat4& matrix)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		// The inverse of the affine `matrix`, or nullopt when it has no finite inverse (a zero scale). Its fourth row is set to
		// (0, 0, 0, 1), which the inverse of an affine matrix has exactly.
		static std::optional<glm::dmat4> InvertAffine(const glm::dmat4& matrix)
		{
			const glm::dmat3 linear(matrix);
			const double determinant = glm::determinant(linear);
			if (!(std::fabs(determinant) > 0.0) || !std::isfinite(determinant))
				return std::nullopt;
			const glm::dmat3 inverseLinear = glm::inverse(linear);
			glm::dmat4 inverse(inverseLinear);
			inverse[3] = glm::dvec4(-(inverseLinear * glm::dvec3(matrix[3])), 1.0);
			if (!IsFinite(inverse))
				return std::nullopt;
			return inverse;
		}

		// One vertex of a skin variant moved from its skin's bind space into its Skeleton's by `correction` (ApplySkin): the
		// position as a point; the normal and the tangent direction by the upper 3 x 3, then normalized (the deformation
		// normalizes them after blending, so their deformed directions are the ones the skin's own inverse binds give); and
		// the vertex's morph deltas by the same 3 x 3, the normal and tangent deltas divided by the length their base vector
		// took, so a morphed vertex deforms as it would under its skin's own inverse binds too.
		static void CorrectSkinVertex(const glm::dmat4& correction, MeshVertex& vertex, std::span<std::vector<MeshMorphDelta>> morphs, size_t index)
		{
			const glm::dmat3 linear(correction);
			vertex.Position = glm::vec3(correction * glm::dvec4(glm::dvec3(vertex.Position), 1.0));
			const glm::dvec3 normal = linear * glm::dvec3(vertex.Normal);
			const glm::dvec3 tangent = linear * glm::dvec3(glm::vec3(vertex.Tangent));
			const double normalLength = glm::length(normal);
			const double tangentLength = glm::length(tangent);
			const bool normalValid = normalLength > 0.0 && std::isfinite(normalLength);
			const bool tangentValid = tangentLength > 0.0 && std::isfinite(tangentLength);
			if (normalValid)
				vertex.Normal = glm::vec3(normal / normalLength);
			if (tangentValid)
				vertex.Tangent = glm::vec4(glm::vec3(tangent / tangentLength), vertex.Tangent.w);
			for (std::vector<MeshMorphDelta>& deltas : morphs)
			{
				MeshMorphDelta& delta = deltas[index];
				delta.Position = glm::vec3(linear * glm::dvec3(delta.Position));
				if (normalValid)
					delta.Normal = glm::vec3(linear * glm::dvec3(delta.Normal) / normalLength);
				if (tangentValid)
					delta.Tangent = glm::vec3(linear * glm::dvec3(delta.Tangent) / tangentLength);
			}
		}

		// `transform` (an entity's world matrix relative to another's, without Scale) as translation, rotation and scale: the
		// columns' lengths are the scale (a negative determinant flips X, as a node matrix's decomposition does), and the
		// rotation is the nearest rotation to the normalized columns (they are orthogonalized first, so a sheared matrix, which
		// non-uniformly scaled joints above a node can make, is approximated rather than refused). Each scale magnitude is at
		// least MinTransformScaleMagnitude.
		static TransformComponent DecomposeApproximately(const glm::mat4& transform)
		{
			TransformComponent result;
			result.Translation = glm::vec3(transform[3]);
			std::array<glm::vec3, 3> columns = { glm::vec3(transform[0]), glm::vec3(transform[1]), glm::vec3(transform[2]) };
			glm::vec3 scale(1.0f);
			for (glm::length_t axis = 0; axis < 3; ++axis)
			{
				const float length = glm::length(columns[static_cast<size_t>(axis)]);
				scale[axis] = std::isfinite(length) ? length : 1.0f;
			}
			if (glm::dot(columns[0], glm::cross(columns[1], columns[2])) < 0.0f)
			{
				scale.x = -scale.x;
				columns[0] = -columns[0];
			}
			// Gram-Schmidt in column order; a column without direction falls back to the axes of the identity.
			const glm::vec3 x = NormalizeOrZero(columns[0]);
			const glm::vec3 axisX = x != glm::vec3(0.0f) ? x : glm::vec3(1.0f, 0.0f, 0.0f);
			glm::vec3 axisY = NormalizeOrZero(columns[1] - axisX * glm::dot(axisX, columns[1]));
			if (axisY == glm::vec3(0.0f))
				axisY = NormalizeOrZero(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), axisX));
			if (axisY == glm::vec3(0.0f))
				axisY = NormalizeOrZero(glm::cross(axisX, glm::vec3(1.0f, 0.0f, 0.0f)));
			const glm::vec3 axisZ = glm::cross(axisX, axisY);
			result.Rotation = glm::normalize(glm::quat_cast(glm::mat3(axisX, axisY, axisZ)));
			result.Scale = ClampScaleMagnitude(scale);
			const glm::vec4 rotation(result.Rotation.x, result.Rotation.y, result.Rotation.z, result.Rotation.w);
			if (!IsFinite(result.Translation) || !std::isfinite(glm::dot(rotation, rotation)))
				return TransformComponent{};
			return result;
		}

	}

	namespace {

		// One import of one glTF source: the parsed document, the buffers it owns while cgltf points into them, and the
		// output under construction. Lives inside GltfImporter::Import only.
		class GltfImport
		{
		public:
			GltfImport(ImportContext& context, const AssetMetadata& metadata, const GltfImportSettings& settings)
				: m_Context(&context), m_Metadata(&metadata), m_Settings(settings), m_DisplayPath(Utils::GetDisplayPath(context.GetSourcePath()))
			{
			}

			[[nodiscard]] Result<ImportResult> Run();
		private:
			[[nodiscard]] Error Fail(std::string message, std::string hint = {}) const;
			void AddWarning(std::string_view code, std::string subject, std::string message, std::string hint);

			[[nodiscard]] Status CheckRequiredExtensions() const;
			[[nodiscard]] Result<VfsPath> ResolveUri(std::string_view uri, std::string_view item) const;
			[[nodiscard]] Status LoadBuffers();
			[[nodiscard]] Status CheckImageUris() const;
			[[nodiscard]] Status CheckAccessors() const;
			void ReportSkippedContent();

			[[nodiscard]] Status ImportMaterials();
			[[nodiscard]] Result<AssetHandle> ImportTextureSlot(const cgltf_texture_view& view, ImageUse use, size_t material, std::string_view slot);
			[[nodiscard]] Result<std::vector<glm::vec4>> ReadColors(const cgltf_accessor& accessor, std::string_view subject) const;
			[[nodiscard]] Result<AssetHandle> FindStandaloneTexture(size_t image);
			[[nodiscard]] Result<Buffer> ReadImage(size_t image);

			[[nodiscard]] Result<SceneNode> MakeSceneNode(size_t node, UUID parent, const glm::mat4& parentWorld) const;
			[[nodiscard]] Status CollectSceneNodes();

			// M23 (version 3): the skins' plan (see the file comment).
			[[nodiscard]] Status PlanSkins();
			[[nodiscard]] Status ReadSkin(size_t skin);
			[[nodiscard]] bool HasSkinAttributes(const cgltf_mesh& mesh) const;
			[[nodiscard]] std::optional<size_t> FindAnimatorNode(size_t skin) const;
			[[nodiscard]] Status BuildSkeleton(SkeletonGroup& group);
			void PlaceEntities();
			void PlaceUnder(size_t position, std::optional<size_t> animatorNode);
			[[nodiscard]] std::optional<size_t> FindEntityAnimatorGroup(size_t position) const;
			[[nodiscard]] std::optional<size_t> FindNodeGroup(std::optional<size_t> animatorNode) const;
			[[nodiscard]] std::optional<size_t> GetScenePosition(size_t node) const;

			[[nodiscard]] Status ImportMeshes();
			[[nodiscard]] Result<PrimitiveGeometry> ReadPrimitive(size_t mesh, size_t primitive, bool readSkin, bool readMorphs);
			[[nodiscard]] Status ReadSkinAttributes(const cgltf_primitive& primitive, std::string_view subject, std::span<const size_t> usedVertices,
				PrimitiveAttributes& attributes);
			[[nodiscard]] Status ReadMorphTargets(const cgltf_primitive& primitive, std::string_view subject, std::span<const size_t> usedVertices,
				PrimitiveAttributes& attributes) const;
			[[nodiscard]] Result<std::vector<float>> ReadFloats(const cgltf_accessor& accessor, cgltf_type expected, std::string_view subject,
				std::string_view attribute) const;
			[[nodiscard]] Status ApplySkin(PrimitiveGeometry& geometry, const SkinPlan& skin, std::string_view subject) const;
			void AppendSubmesh(MeshData& mesh, PrimitiveGeometry geometry, const cgltf_primitive& primitive) const;
			[[nodiscard]] Result<AssetHandle> CookMeshSubAsset(MeshData mesh, std::string key);
			[[nodiscard]] Result<std::vector<MeshMorphTarget>> MakeMorphTargets(size_t mesh) const;

			[[nodiscard]] Status ImportSkeletons();
			[[nodiscard]] Status ImportAnimations();
			[[nodiscard]] Result<std::optional<PendingClip>> ReadAnimation(size_t animation);
			[[nodiscard]] Status AddAnimationEvents(std::vector<PendingClip>& clips) const;
			[[nodiscard]] std::optional<std::string> GetWeightsTarget(size_t node, std::optional<size_t>& group) const;

			[[nodiscard]] Result<Buffer> BuildPrefab() const;
			[[nodiscard]] std::vector<float> GetNodeMorphWeights(size_t node) const;

			AssetHandle AddSubAsset(std::string key, AssetType type, Buffer cooked);
			[[nodiscard]] UUID MakeEntityID(std::string_view key) const;
		private:
			ImportContext* m_Context = nullptr;        // the caller's context; outlives the import
			const AssetMetadata* m_Metadata = nullptr; // the caller's metadata; outlives the import
			GltfImportSettings m_Settings{};
			std::string m_DisplayPath;

			CgltfDataPointer m_Data;
			std::vector<Buffer> m_BufferStorage; // the decoded and read buffers cgltf_buffer::data points into

			std::vector<ImportedArtifact> m_SubAssets;
			std::vector<AssetHandle> m_Dependencies;
			std::vector<AssetDiagnostic> m_Diagnostics;

			std::vector<AssetHandle> m_MaterialHandles;                    // per glTF material; null when not imported
			std::map<std::pair<size_t, ImageUse>, AssetHandle> m_Textures; // texture sub-assets made so far
			std::map<size_t, AssetHandle> m_StandaloneImages;              // FindAsset results per image (null: none)
			std::map<size_t, Buffer> m_ImageBytes;                         // encoded images read so far

			std::vector<SceneNode> m_SceneNodes;
			AssetHandle m_MergedMesh; // MergeMeshes

			// M23 (version 3; all empty with MergeMeshes or without skins).
			std::vector<size_t> m_ScenePositions;                // per glTF node: its position in m_SceneNodes, or NotInScene
			std::vector<SkinPlan> m_Skins;                       // per glTF skin
			std::vector<SkeletonGroup> m_Groups;                 // in order of their first skin
			std::vector<bool> m_IsJoint;                         // per glTF node: a joint of an imported skin (no entity)
			std::vector<std::optional<size_t>> m_NodeSkins;      // per glTF node: the skin it is drawn with
			std::vector<bool> m_HasEntity;                       // per scene position: whether the node is an entity
			std::vector<size_t> m_ParentPositions;               // per scene position: its entity's parent's position, NotInScene: the root
			std::vector<std::optional<size_t>> m_AnimatorGroups; // per glTF node: the SkeletonGroup whose Animator its entity holds
			std::optional<size_t> m_RootAnimatorGroup;           // the SkeletonGroup whose Animator the root entity holds
			// Per Skeleton joint's glTF node: (SkeletonGroup, joint index) of each Skeleton holding it, in group order.
			std::map<size_t, std::vector<std::pair<size_t, size_t>>> m_SkeletonJoints;
			// Per glTF mesh: the sub-asset of each variant, keyed by the skin it is drawn with (NoSkin: none); and how many
			// morph targets it imported (0: none, or more than MaxMeshMorphTargets, skipped).
			std::vector<std::map<size_t, AssetHandle>> m_MeshVariants;
			std::vector<size_t> m_MorphTargetCounts;
		};

		// M23 (version 3): m_ScenePositions' value for a node outside the default scene, and m_MeshVariants' key of the
		// variant without a skin.
		constexpr size_t NotInScene = std::numeric_limits<size_t>::max();
		constexpr size_t NoSkin = std::numeric_limits<size_t>::max();

		Error GltfImport::Fail(std::string message, std::string hint) const
		{
			Error error = Error(ErrorCode::ImportFailed, std::move(message)).WithLocation(Utils::MakeFileLocation(m_DisplayPath));
			if (!hint.empty())
				return std::move(error).WithHint(std::move(hint));
			return error;
		}

		void GltfImport::AddWarning(std::string_view code, std::string subject, std::string message, std::string hint)
		{
			// One diagnostic per code and subject: MergeMeshes visits a mesh once per node that uses it.
			const bool known = std::ranges::any_of(m_Diagnostics, [code, &subject](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Code == code && diagnostic.Subject == subject;
			});
			if (known)
				return;
			m_Diagnostics.push_back(AssetDiagnostic{
				.Severity = DiagnosticSeverity::Warning,
				.Code = std::string(code),
				.Asset = m_Metadata->Handle,
				.Path = m_DisplayPath,
				.Message = std::move(message),
				.Hint = std::move(hint),
				.Subject = std::move(subject),
				.AutoFixable = false,
			});
		}

		Status GltfImport::CheckRequiredExtensions() const
		{
			for (cgltf_size i = 0; i < m_Data->extensions_required_count; ++i)
			{
				const std::string name = Utils::DecodeJsonString(m_Data->extensions_required[i]);
				if (std::ranges::find(Utils::SupportedRequiredExtensions, std::string_view(name)) != Utils::SupportedRequiredExtensions.end())
					continue;
				std::string hint = "export the model without this extension";
				if (name == "KHR_draco_mesh_compression" || name == "EXT_meshopt_compression")
					hint = "export the model without mesh compression";
				else if (name == "KHR_texture_basisu" || name == "EXT_texture_webp")
					hint = "export the textures as PNG or JPEG";
				return std::unexpected(Fail(std::format("the file requires the glTF extension '{}', which the engine does not support "
														"(supported required extensions: KHR_materials_emissive_strength, KHR_texture_transform, KHR_mesh_quantization)",
												name),
					std::move(hint)));
			}
			return {};
		}

		Result<VfsPath> GltfImport::ResolveUri(std::string_view uri, std::string_view item) const
		{
			Result<std::string> decoded = Utils::DecodeRelativeUri(uri, item);
			if (!decoded)
				return std::unexpected(std::move(decoded).error().WithLocation(Utils::MakeFileLocation(m_DisplayPath)));
			Result<VfsPath> path = m_Context->GetSourcePath().GetParent().Join(*decoded);
			if (!path)
			{
				return std::unexpected(Fail(std::format("{} has the URI '{}', whose path the engine does not accept: {}", item, uri,
												path.error().GetMessageText()),
					std::string(Utils::UriHint)));
			}
			return path;
		}

		Status GltfImport::LoadBuffers()
		{
			m_BufferStorage.reserve(m_Data->buffers_count);
			for (cgltf_size i = 0; i < m_Data->buffers_count; ++i)
			{
				cgltf_buffer& buffer = m_Data->buffers[i];
				const std::string item = std::format("buffers[{}]", i);
				if (buffer.uri == nullptr)
				{
					// Only the first buffer of a binary glTF may omit its URI: it is the BIN chunk.
					if (i != 0 || m_Data->bin == nullptr)
						return std::unexpected(Fail(std::format("{} has no URI and is not the binary chunk of a .glb file", item)));
					if (m_Data->bin_size < buffer.size)
					{
						return std::unexpected(Fail(std::format("the binary chunk holds {} bytes, fewer than the {} bytes of {}", m_Data->bin_size,
							buffer.size, item)));
					}
					// cgltf_buffer::data is non-const although cgltf never writes through it.
					buffer.data = const_cast<void*>(m_Data->bin);
					buffer.data_free_method = cgltf_data_free_method_none;
					continue;
				}

				const std::string uri = Utils::DecodeJsonString(buffer.uri);
				Buffer bytes;
				if (Utils::IsDataUri(uri))
				{
					Result<Buffer> decoded = Utils::DecodeDataUri(uri, item);
					if (!decoded)
						return std::unexpected(std::move(decoded).error().WithLocation(Utils::MakeFileLocation(m_DisplayPath)));
					bytes = std::move(*decoded);
				}
				else
				{
					ENGINE_TRY_ASSIGN(const VfsPath path, ResolveUri(uri, item));
					Result<Buffer> read = m_Context->ReadDependency(path);
					if (!read)
					{
						return std::unexpected(std::move(read).error().WithContext(std::format("while reading {} '{}' of '{}'", item,
							path.ToString(), m_DisplayPath)));
					}
					bytes = std::move(*read);
				}
				if (bytes.size() < buffer.size)
					return std::unexpected(Fail(std::format("{} holds {} bytes, fewer than its byteLength {}", item, bytes.size(), buffer.size)));
				m_BufferStorage.push_back(std::move(bytes));
				buffer.data = m_BufferStorage.back().data();
				buffer.data_free_method = cgltf_data_free_method_none;
			}
			return {};
		}

		Status GltfImport::CheckImageUris() const
		{
			for (cgltf_size i = 0; i < m_Data->images_count; ++i)
			{
				const std::string uri = Utils::DecodeJsonString(m_Data->images[i].uri);
				if (m_Data->images[i].uri != nullptr && !Utils::IsDataUri(uri))
					ENGINE_TRY(ResolveUri(uri, std::format("images[{}]", i)));
			}
			return {};
		}

		// cgltf_validate's size checks are not overflow-safe (a count of 2^62 + 1 wraps the required size to a few bytes), and
		// with buffer data attached it scans every index of an accessor: a huge count passes the check and the scan reads far
		// past the buffer. So every buffer view and accessor, sparse parts included, is checked here first with overflow-safe
		// arithmetic against the bytes cgltf reads for it, and an accessor without a buffer view, whose count no bytes back,
		// is bounded by MaxUnbackedAccessorCount. Afterwards every accessor read (cgltf's and the importer's) stays inside
		// its buffer and every vector sized from a count is bounded by the file's bytes.
		Status GltfImport::CheckAccessors() const
		{
			for (cgltf_size i = 0; i < m_Data->buffer_views_count; ++i)
			{
				const cgltf_buffer_view& view = m_Data->buffer_views[i];
				const std::optional<size_t> end = Utils::CheckedAdd(view.offset, view.size);
				if (view.buffer == nullptr || !end || *end > view.buffer->size)
				{
					return std::unexpected(Fail(std::format("bufferViews[{}] (byteOffset {}, byteLength {}) does not fit in its buffer of {} bytes", i,
						view.offset, view.size, view.buffer != nullptr ? view.buffer->size : 0)));
				}
			}

			for (cgltf_size i = 0; i < m_Data->accessors_count; ++i)
			{
				const cgltf_accessor& accessor = m_Data->accessors[i];
				const std::string item = std::format("accessors[{}]", i);
				const size_t elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
				if (elementSize == 0)
					return std::unexpected(Fail(std::format("{} has an invalid type or componentType", item)));

				if (accessor.buffer_view != nullptr)
				{
					// cgltf reads element k at byteOffset + stride * k; the stride is the view's byteStride, else the element size.
					if (accessor.stride < elementSize)
					{
						return std::unexpected(Fail(std::format("{} has elements of {} bytes, more than the byteStride {} of its buffer view", item,
							elementSize, accessor.stride)));
					}
					const std::optional<size_t> end = Utils::GetElementsEnd(accessor.offset, accessor.stride, accessor.count, elementSize);
					if (!end || *end > accessor.buffer_view->size)
					{
						return std::unexpected(Fail(std::format("{} ({} elements of {} bytes from byteOffset {}, {} bytes apart) does not fit in its "
																"buffer view of {} bytes",
							item, accessor.count, elementSize, accessor.offset, accessor.stride, accessor.buffer_view->size)));
					}
				}
				else if (accessor.count > Utils::MaxUnbackedAccessorCount)
				{
					return std::unexpected(Fail(std::format("{} has no buffer view and a count of {}, more than the {} elements the engine accepts "
															"for an accessor without data",
						item, accessor.count, Utils::MaxUnbackedAccessorCount)));
				}

				if (!accessor.is_sparse)
					continue;
				const cgltf_accessor_sparse& sparse = accessor.sparse;
				const size_t indexSize = cgltf_component_size(sparse.indices_component_type);
				const bool unsignedIndices = sparse.indices_component_type == cgltf_component_type_r_8u || sparse.indices_component_type == cgltf_component_type_r_16u || sparse.indices_component_type == cgltf_component_type_r_32u;
				if (!unsignedIndices)
					return std::unexpected(Fail(std::format("{}.sparse.indices has a componentType that is not an unsigned integer", item)));
				if (sparse.count > accessor.count)
				{
					return std::unexpected(Fail(std::format("{}.sparse has a count of {}, more than the accessor's count {}", item, sparse.count,
						accessor.count)));
				}
				// cgltf reads the sparse indices packed and the sparse values `stride` bytes apart, like the base elements.
				const std::optional<size_t> indicesEnd = Utils::GetElementsEnd(sparse.indices_byte_offset, indexSize, sparse.count, indexSize);
				if (sparse.indices_buffer_view == nullptr || !indicesEnd || *indicesEnd > sparse.indices_buffer_view->size)
					return std::unexpected(Fail(std::format("{}.sparse.indices ({} indices) does not fit in its buffer view", item, sparse.count)));
				const std::optional<size_t> valuesEnd = Utils::GetElementsEnd(sparse.values_byte_offset, accessor.stride, sparse.count, elementSize);
				if (sparse.values_buffer_view == nullptr || !valuesEnd || *valuesEnd > sparse.values_buffer_view->size)
					return std::unexpected(Fail(std::format("{}.sparse.values ({} values) does not fit in its buffer view", item, sparse.count)));
			}
			return {};
		}

		void GltfImport::ReportSkippedContent()
		{
			const auto report = [this](std::string_view kind, cgltf_size count, std::string_view description, std::string_view reason)
			{
				for (cgltf_size i = 0; i < count; ++i)
				{
					std::string subject = std::format("{}[{}]", kind, i);
					std::string message = std::format("{} is {}, which {}", subject, description, reason);
					AddWarning(AssetContentSkippedCode, std::move(subject), std::move(message), {});
				}
			};
			// Version 3 imports skins and animations (PlanSkins, ImportAnimations), except with MergeMeshes, whose one mesh is
			// static.
			if (m_Settings.MergeMeshes)
			{
				report("skins", m_Data->skins_count, "a skin", "MergeMeshes does not keep: the merged mesh is static");
				report("animations", m_Data->animations_count, "an animation", "MergeMeshes does not keep: the merged mesh is static");
			}
			report("cameras", m_Data->cameras_count, "a camera", "the engine does not import");
			report("KHR_lights_punctual.lights", m_Data->lights_count, "a light", "the engine does not import");
			for (cgltf_size i = 0; i < m_Data->nodes_count; ++i)
			{
				if (m_Data->nodes[i].has_mesh_gpu_instancing)
				{
					AddWarning(AssetContentSkippedCode, std::format("nodes[{}].EXT_mesh_gpu_instancing", i),
						std::format("nodes[{}] uses EXT_mesh_gpu_instancing, which the engine does not import: the mesh is placed once", i), {});
				}
			}
		}

		Result<Buffer> GltfImport::ReadImage(size_t image)
		{
			if (const auto cached = m_ImageBytes.find(image); cached != m_ImageBytes.end())
				return cached->second;

			const cgltf_image& source = m_Data->images[image];
			const std::string item = std::format("images[{}]", image);
			const std::string uri = Utils::DecodeJsonString(source.uri);
			Buffer bytes;
			if (source.buffer_view != nullptr)
			{
				const uint8_t* data = cgltf_buffer_view_data(source.buffer_view);
				if (data == nullptr)
					return std::unexpected(Fail(std::format("{} has a buffer view without data", item)));
				const std::span<const uint8_t> view(data, source.buffer_view->size);
				const std::span<const std::byte> viewBytes = std::as_bytes(view);
				bytes.assign(viewBytes.begin(), viewBytes.end());
			}
			else if (source.uri != nullptr && Utils::IsDataUri(uri))
			{
				Result<Buffer> decoded = Utils::DecodeDataUri(uri, item);
				if (!decoded)
					return std::unexpected(std::move(decoded).error().WithLocation(Utils::MakeFileLocation(m_DisplayPath)));
				bytes = std::move(*decoded);
			}
			else if (source.uri != nullptr)
			{
				ENGINE_TRY_ASSIGN(const VfsPath path, ResolveUri(uri, item));
				Result<Buffer> read = m_Context->ReadDependency(path);
				if (!read)
				{
					return std::unexpected(std::move(read).error().WithContext(std::format("while reading {} '{}' of '{}'", item, path.ToString(),
						m_DisplayPath)));
				}
				bytes = std::move(*read);
			}
			else
			{
				return std::unexpected(Fail(std::format("{} has neither a URI nor a buffer view", item)));
			}
			m_ImageBytes.emplace(image, bytes);
			return bytes;
		}

		Result<AssetHandle> GltfImport::FindStandaloneTexture(size_t image)
		{
			if (const auto cached = m_StandaloneImages.find(image); cached != m_StandaloneImages.end())
				return cached->second;

			AssetHandle standalone;
			const std::string uri = Utils::DecodeJsonString(m_Data->images[image].uri);
			if (m_Data->images[image].buffer_view == nullptr && m_Data->images[image].uri != nullptr && !Utils::IsDataUri(uri))
			{
				ENGINE_TRY_ASSIGN(const VfsPath path, ResolveUri(uri, std::format("images[{}]", image)));
				const std::optional<ImportAssetLookupEntry> found = m_Context->FindAsset(path);
				if (found.has_value() && found->Kind == AssetMetaKind::Asset && found->Type == AssetType::Texture && found->Handle.IsValid())
					standalone = found->Handle;
			}
			m_StandaloneImages.emplace(image, standalone);
			return standalone;
		}

		Result<AssetHandle> GltfImport::ImportTextureSlot(const cgltf_texture_view& view, ImageUse use, size_t material, std::string_view slot)
		{
			if (view.texture == nullptr)
				return AssetHandle();

			// M23 (GltfImporter version 2): TEXCOORD_0 and TEXCOORD_1 are the mesh's two UV sets (the map's UV set,
			// GetTextureCoordinateSet); a texture bound to a third set is ignored.
			const cgltf_int texCoord = Utils::GetTexCoord(view);
			if (texCoord < 0 || texCoord > 1)
			{
				const std::string materialName = Utils::GetName(m_Data->materials[material].name, std::format("Material {}", material));
				AddWarning(AssetUnsupportedUvSetCode, std::format("materials[{}].{}", material, slot),
					std::format("materials[{}] '{}' binds its {} to TEXCOORD_{}; meshes have the UV sets TEXCOORD_0 and TEXCOORD_1, so the texture "
								"is ignored",
						material, materialName, slot, texCoord),
					"bake the texture against TEXCOORD_0 or TEXCOORD_1, or remove it from the material");
				return AssetHandle();
			}

			const size_t texture = cgltf_texture_index(m_Data.get(), view.texture);
			if (view.texture->image == nullptr)
			{
				AddWarning(AssetContentSkippedCode, std::format("textures[{}]", texture),
					std::format("textures[{}] has no PNG or JPEG image (only an extension image the engine does not decode), so materials[{}] "
								"does not use it",
						texture, material),
					"export the textures as PNG or JPEG");
				return AssetHandle();
			}
			const size_t image = cgltf_image_index(m_Data.get(), view.texture->image);

			ENGINE_TRY_ASSIGN(const AssetHandle standalone, FindStandaloneTexture(image));
			if (standalone.IsValid())
			{
				m_Dependencies.push_back(standalone);
				return standalone;
			}

			if (const auto existing = m_Textures.find({ image, use }); existing != m_Textures.end())
				return existing->second;

			ENGINE_TRY_ASSIGN(const Buffer encoded, ReadImage(image));
			// Compressed by its usage (TextureImporter.h's Compression Auto): sub-assets have no settings of their own.
			TextureImportSettings settings;
			settings.GenerateMips = true;
			settings.Compression = TextureCompression::Auto;
			std::string_view suffix;
			switch (use)
			{
				case ImageUse::Color:
					settings.Usage = TextureUsage::Color;
					break;
				case ImageUse::Linear:
					settings.Usage = TextureUsage::Linear;
					suffix = ":linear";
					break;
				case ImageUse::Normal:
					settings.Usage = TextureUsage::NormalMap;
					suffix = ":normal";
					break;
			}
			const std::string uri = Utils::DecodeJsonString(m_Data->images[image].uri);
			const std::string name = m_Data->images[image].uri != nullptr && !Utils::IsDataUri(uri)
				? std::format("images[{}] '{}' of '{}'", image, uri, m_DisplayPath)
				: std::format("images[{}] of '{}'", image, m_DisplayPath);
			Result<Buffer> cooked = TextureImporter::ImportTextureFromMemory(encoded, settings, name);
			if (!cooked)
				return std::unexpected(std::move(cooked).error().WithContext(std::format("while importing the {} of materials[{}]", slot, material)));
			const AssetHandle handle = AddSubAsset(std::format("texture:{}{}", image, suffix), AssetType::Texture, std::move(*cooked));
			m_Textures.emplace(std::pair(image, use), handle);
			return handle;
		}

		Status GltfImport::ImportMaterials()
		{
			m_MaterialHandles.assign(m_Data->materials_count, AssetHandle());
			if (!m_Settings.ImportMaterials)
				return {};

			for (cgltf_size index = 0; index < m_Data->materials_count; ++index)
			{
				const cgltf_material& source = m_Data->materials[index];
				const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
				MaterialData material;

				ENGINE_TRY_ASSIGN(const AssetHandle baseColorMap, ImportTextureSlot(pbr.base_color_texture, ImageUse::Color, index, "pbrMetallicRoughness.baseColorTexture"));
				ENGINE_TRY_ASSIGN(const AssetHandle metallicRoughnessMap, ImportTextureSlot(pbr.metallic_roughness_texture, ImageUse::Linear, index, "pbrMetallicRoughness.metallicRoughnessTexture"));
				ENGINE_TRY_ASSIGN(const AssetHandle normalMap, ImportTextureSlot(source.normal_texture, ImageUse::Normal, index, "normalTexture"));
				ENGINE_TRY_ASSIGN(const AssetHandle occlusionMap, ImportTextureSlot(source.occlusion_texture, ImageUse::Linear, index, "occlusionTexture"));
				ENGINE_TRY_ASSIGN(const AssetHandle emissiveMap, ImportTextureSlot(source.emissive_texture, ImageUse::Color, index, "emissiveTexture"));
				material.BaseColorMap = TypedAssetHandle<AssetType::Texture>(baseColorMap);
				material.MetallicRoughnessMap = TypedAssetHandle<AssetType::Texture>(metallicRoughnessMap);
				material.NormalMap = TypedAssetHandle<AssetType::Texture>(normalMap);
				material.OcclusionMap = TypedAssetHandle<AssetType::Texture>(occlusionMap);
				material.EmissiveMap = TypedAssetHandle<AssetType::Texture>(emissiveMap);
				// M23 (Material version 2): each bound map samples its textureInfo.texCoord; an empty slot keeps UV0.
				material.BaseColorUVSet = Utils::GetTextureCoordinateSet(pbr.base_color_texture, baseColorMap);
				material.MetallicRoughnessUVSet = Utils::GetTextureCoordinateSet(pbr.metallic_roughness_texture, metallicRoughnessMap);
				material.NormalUVSet = Utils::GetTextureCoordinateSet(source.normal_texture, normalMap);
				material.OcclusionUVSet = Utils::GetTextureCoordinateSet(source.occlusion_texture, occlusionMap);
				material.EmissiveUVSet = Utils::GetTextureCoordinateSet(source.emissive_texture, emissiveMap);

				// Factors: glTF bounds them as the Material registry does, except for the unbounded scales and strength;
				// the clamps keep a slightly out-of-range exporter value loadable. A non-finite value is an error.
				const std::array<float, 13> factors = { pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2],
					pbr.base_color_factor[3], pbr.metallic_factor, pbr.roughness_factor, source.normal_texture.scale, source.occlusion_texture.scale,
					source.emissive_factor[0], source.emissive_factor[1], source.emissive_factor[2], source.emissive_strength.emissive_strength,
					source.alpha_cutoff };
				if (!std::ranges::all_of(factors, [](float value)
				{
					return std::isfinite(value);
				}))
					return std::unexpected(Fail(std::format("materials[{}] has a factor that is not a finite number", index)));
				material.BaseColor = glm::clamp(glm::vec4(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2],
													pbr.base_color_factor[3]),
					0.0f, 1.0f);
				material.Metallic = std::clamp(pbr.metallic_factor, 0.0f, 1.0f);
				material.Roughness = std::clamp(pbr.roughness_factor, 0.0f, 1.0f);
				material.NormalScale = std::max(source.normal_texture.scale, 0.0f);
				material.OcclusionStrength = std::clamp(source.occlusion_texture.scale, 0.0f, 1.0f);
				material.Emissive = glm::clamp(glm::vec3(source.emissive_factor[0], source.emissive_factor[1], source.emissive_factor[2]), 0.0f, 1.0f);
				material.EmissiveStrength = source.has_emissive_strength ? std::max(source.emissive_strength.emissive_strength, 0.0f) : 1.0f;
				switch (source.alpha_mode)
				{
					case cgltf_alpha_mode_mask:
						material.AlphaMode = AlphaMode::Mask;
						break;
					case cgltf_alpha_mode_blend:
						material.AlphaMode = AlphaMode::Blend;
						break;
					case cgltf_alpha_mode_opaque:
					case cgltf_alpha_mode_max_enum:
						material.AlphaMode = AlphaMode::Opaque;
						break;
				}
				material.AlphaCutoff = std::clamp(source.alpha_cutoff, 0.0f, 1.0f);
				material.DoubleSided = source.double_sided != 0;

				// One UV transform per material: the base colour texture's KHR_texture_transform, else the first bound slot's.
				const std::array<std::pair<const cgltf_texture_view*, std::string_view>, 5> views = {
					std::pair(&pbr.base_color_texture, std::string_view("pbrMetallicRoughness.baseColorTexture")),
					std::pair(&pbr.metallic_roughness_texture, std::string_view("pbrMetallicRoughness.metallicRoughnessTexture")),
					std::pair(&source.normal_texture, std::string_view("normalTexture")),
					std::pair(&source.occlusion_texture, std::string_view("occlusionTexture")),
					std::pair(&source.emissive_texture, std::string_view("emissiveTexture")),
				};
				const std::array<AssetHandle, 5> bound = { baseColorMap, metallicRoughnessMap, normalMap, occlusionMap, emissiveMap };
				for (size_t slot = 0; slot < views.size(); ++slot)
				{
					const cgltf_texture_view& view = *views[slot].first;
					if (!bound[slot].IsValid() || !view.has_transform)
						continue;
					const cgltf_texture_transform& transform = view.transform;
					const glm::vec2 scale(transform.scale[0], transform.scale[1]);
					const glm::vec2 offset(transform.offset[0], transform.offset[1]);
					if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(transform.rotation))
					{
						return std::unexpected(Fail(std::format("materials[{}].{} has a KHR_texture_transform value that is not a finite number", index,
							views[slot].second)));
					}
					material.UVScale = scale;
					material.UVOffset = offset;
					if (transform.rotation != 0.0f)
					{
						AddWarning(AssetContentSkippedCode, std::format("materials[{}].{}.KHR_texture_transform.rotation", index, views[slot].second),
							std::format("materials[{}] rotates its texture coordinates (KHR_texture_transform rotation), which the engine does not "
										"support: only the scale and offset are imported",
								index),
							"bake the rotation into the texture or the UVs");
					}
					break;
				}

				Result<Buffer> cooked = CookMaterial(material, m_Context->GetRegistry(), GltfImporter::Version);
				if (!cooked)
					return std::unexpected(std::move(cooked).error().WithContext(std::format("while cooking materials[{}]", index)));
				m_MaterialHandles[index] = AddSubAsset(Utils::MakeKey("material", index, source.name), AssetType::Material, std::move(*cooked));
			}
			return {};
		}

		Result<SceneNode> GltfImport::MakeSceneNode(size_t index, UUID parent, const glm::mat4& parentWorld) const
		{
			const cgltf_node& node = m_Data->nodes[index];
			glm::vec3 translation(node.translation[0], node.translation[1], node.translation[2]);
			glm::quat rotation(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
			glm::vec3 scale(node.scale[0], node.scale[1], node.scale[2]);
			if (node.has_matrix)
			{
				// Decomposes the column-major matrix into translation, rotation and scale (a negative determinant flips X).
				const std::span<const float, 16> m(node.matrix);
				const bool finite = std::ranges::all_of(m, [](float value)
				{
					return std::isfinite(value);
				});
				if (!finite)
					return std::unexpected(Fail(std::format("nodes[{}] has a matrix with a value that is not a finite number", index)));
				const std::array<float, 4> lastRow = { m[3], m[7], m[11], m[15] - 1.0f };
				const bool projective = std::ranges::any_of(lastRow, [](float value)
				{
					return std::fabs(value) > Utils::MatrixRowTolerance;
				});
				if (projective)
				{
					return std::unexpected(Fail(std::format("nodes[{}] has a projective matrix (its last row is not 0, 0, 0, 1), {}", index,
						Utils::NotDecomposable)));
				}
				translation = glm::vec3(m[12], m[13], m[14]);
				std::array<glm::vec3, 3> columns = { glm::vec3(m[0], m[1], m[2]), glm::vec3(m[4], m[5], m[6]), glm::vec3(m[8], m[9], m[10]) };
				for (glm::length_t axis = 0; axis < 3; ++axis)
				{
					glm::vec3& column = columns[static_cast<size_t>(axis)];
					const float length = glm::length(column);
					if (!(length > 0.0f))
						return std::unexpected(Fail(std::format("nodes[{}] has a matrix with a zero scale, {}", index, Utils::NotDecomposable)));
					scale[axis] = length;
					column /= length;
				}
				if (glm::dot(columns[0], glm::cross(columns[1], columns[2])) < 0.0f)
				{
					scale.x = -scale.x;
					columns[0] = -columns[0];
				}
				const std::array<float, 3> cosines = { glm::dot(columns[0], columns[1]), glm::dot(columns[0], columns[2]),
					glm::dot(columns[1], columns[2]) };
				const bool sheared = std::ranges::any_of(cosines, [](float cosine)
				{
					return std::fabs(cosine) > Utils::MatrixShearTolerance;
				});
				if (sheared)
				{
					return std::unexpected(Fail(std::format("nodes[{}] has a matrix with shear, {}", index, Utils::NotDecomposable),
						"apply the transform to the mesh, or use a translation, rotation and scale without shear"));
				}
				rotation = glm::quat_cast(glm::mat3(columns[0], columns[1], columns[2]));
			}

			const glm::vec4 rotationVector(rotation.x, rotation.y, rotation.z, rotation.w);
			if (!Utils::IsFinite(translation) || !Utils::IsFinite(scale) || !std::isfinite(glm::dot(rotationVector, rotationVector)))
				return std::unexpected(Fail(std::format("nodes[{}] has a transform value that is not a finite number", index)));
			const float rotationLength = std::sqrt(glm::dot(rotationVector, rotationVector));
			if (!(rotationLength > 0.0f))
				return std::unexpected(Fail(std::format("nodes[{}] has a zero rotation quaternion", index)));
			rotation = glm::quat(rotation.w / rotationLength, rotation.x / rotationLength, rotation.y / rotationLength, rotation.z / rotationLength);
			scale = Utils::ClampScaleMagnitude(scale);

			SceneNode sceneNode;
			sceneNode.Node = index;
			sceneNode.ID = MakeEntityID(std::format("node:{}", index));
			sceneNode.Parent = parent;
			sceneNode.Name = Utils::GetName(node.name, std::format("Node {}", index));
			sceneNode.Transform.Translation = translation * m_Settings.Scale;
			sceneNode.Transform.Rotation = rotation;
			sceneNode.Transform.Scale = scale;
			sceneNode.World = parentWorld * Utils::ComposeTransform(translation, rotation, scale);
			return sceneNode;
		}

		Status GltfImport::CollectSceneNodes()
		{
			// The default scene: "scene", else scene 0, else every root node in index order.
			std::vector<size_t> roots;
			const cgltf_scene* scene = m_Data->scene != nullptr ? m_Data->scene : (m_Data->scenes_count > 0 ? &m_Data->scenes[0] : nullptr);
			if (scene != nullptr)
			{
				for (cgltf_size i = 0; i < scene->nodes_count; ++i)
					roots.push_back(cgltf_node_index(m_Data.get(), scene->nodes[i]));
			}
			else
			{
				for (cgltf_size i = 0; i < m_Data->nodes_count; ++i)
				{
					if (m_Data->nodes[i].parent == nullptr)
						roots.push_back(i);
				}
			}

			// Depth-first without recursion (hierarchies may be deep): the stack holds (node, position of its parent in
			// m_SceneNodes, or NoParent for a root node of the scene).
			constexpr size_t NoParent = std::numeric_limits<size_t>::max();
			const UUID rootID = MakeEntityID("root");
			std::vector<bool> visited(m_Data->nodes_count, false);
			std::vector<std::pair<size_t, size_t>> stack;
			for (auto root = roots.rbegin(); root != roots.rend(); ++root)
				stack.emplace_back(*root, NoParent);
			while (!stack.empty())
			{
				const auto [node, parentPosition] = stack.back();
				stack.pop_back();
				if (visited[node])
					return std::unexpected(Fail(std::format("nodes[{}] appears more than once in the node hierarchy of the scene", node)));
				visited[node] = true;
				const bool isRoot = parentPosition == NoParent;
				const UUID parentID = isRoot ? rootID : m_SceneNodes[parentPosition].ID;
				const glm::mat4 parentWorld = isRoot ? glm::mat4(1.0f) : m_SceneNodes[parentPosition].World;
				ENGINE_TRY_ASSIGN(SceneNode sceneNode, MakeSceneNode(node, parentID, parentWorld));
				const size_t position = m_SceneNodes.size();
				m_SceneNodes.push_back(std::move(sceneNode));
				const cgltf_node& source = m_Data->nodes[node];
				for (cgltf_size child = source.children_count; child > 0; --child)
					stack.emplace_back(cgltf_node_index(m_Data.get(), source.children[child - 1]), position);
			}
			return {};
		}

		Result<std::vector<float>> GltfImport::ReadFloats(const cgltf_accessor& accessor, cgltf_type expected, std::string_view subject,
			std::string_view attribute) const
		{
			if (accessor.type != expected)
			{
				return std::unexpected(Fail(std::format("{} has a {} accessor of type {}, expected {}", subject, attribute,
					Utils::GetAccessorTypeName(accessor.type), Utils::GetAccessorTypeName(expected))));
			}
			const size_t components = cgltf_num_components(accessor.type);
			std::vector<float> values(accessor.count * components);
			if (cgltf_accessor_unpack_floats(&accessor, values.data(), values.size()) != values.size())
				return std::unexpected(Fail(std::format("the {} accessor of {} cannot be read", attribute, subject)));
			for (size_t i = 0; i < values.size(); ++i)
			{
				if (!std::isfinite(values[i]))
				{
					return std::unexpected(Fail(std::format("{} has a {} value that is not a finite number (element {})", subject, attribute,
						i / components)));
				}
			}
			return values;
		}

		// An empty geometry (no vertices) for a primitive that is skipped with a diagnostic. M23 (version 3): `readSkin` reads
		// JOINTS_0 and WEIGHTS_0 when the primitive has both (for the mesh's skinned variants), `readMorphs` the primitive's
		// morph targets.
		Result<PrimitiveGeometry> GltfImport::ReadPrimitive(size_t meshIndex, size_t primitiveIndex, bool readSkin, bool readMorphs)
		{
			const cgltf_primitive& primitive = m_Data->meshes[meshIndex].primitives[primitiveIndex];
			const std::string subject = std::format("meshes[{}].primitives[{}]", meshIndex, primitiveIndex);
			switch (primitive.type)
			{
				case cgltf_primitive_type_points:
				case cgltf_primitive_type_lines:
				case cgltf_primitive_type_line_loop:
				case cgltf_primitive_type_line_strip:
					AddWarning(AssetContentSkippedCode, subject, std::format("{} is a {} primitive, which the engine does not import", subject, Utils::GetPrimitiveModeName(primitive.type)), {});
					return PrimitiveGeometry();
				case cgltf_primitive_type_triangles:
				case cgltf_primitive_type_triangle_strip:
				case cgltf_primitive_type_triangle_fan:
					break;
				case cgltf_primitive_type_invalid:
				case cgltf_primitive_type_max_enum:
					return std::unexpected(Fail(std::format("{} has an invalid mode", subject)));
			}

			const cgltf_accessor* positionAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
			if (positionAccessor == nullptr)
			{
				AddWarning(AssetContentSkippedCode, subject, std::format("{} has no POSITION attribute, so it is not drawn", subject), {});
				return PrimitiveGeometry();
			}
			if (primitive.targets_count > 0 && m_Settings.MergeMeshes)
			{
				AddWarning(AssetContentSkippedCode, subject + ".targets",
					std::format("{} has morph targets, which MergeMeshes does not keep: the base shape is used", subject), {});
			}

			const size_t vertexCount = positionAccessor->count;
			if (vertexCount > std::numeric_limits<uint32_t>::max())
				return std::unexpected(Fail(std::format("{} has {} vertices, more than a mesh can index", subject, vertexCount)));

			PrimitiveAttributes source;
			ENGINE_TRY_ASSIGN(const std::vector<float> positions, ReadFloats(*positionAccessor, cgltf_type_vec3, subject, "POSITION"));
			source.Positions.resize(vertexCount);
			for (size_t vertex = 0; vertex < vertexCount; ++vertex)
				source.Positions[vertex] = glm::vec3(positions[vertex * 3], positions[vertex * 3 + 1], positions[vertex * 3 + 2]);

			std::vector<uint32_t> indices;
			if (primitive.indices != nullptr)
			{
				const cgltf_accessor& indexAccessor = *primitive.indices;
				indices.resize(indexAccessor.count);
				if (indexAccessor.type != cgltf_type_scalar || cgltf_accessor_unpack_indices(&indexAccessor, indices.data(), sizeof(uint32_t), indices.size()) != indices.size())
				{
					return std::unexpected(Fail(std::format("the indices of {} cannot be read (they must be a non-sparse SCALAR accessor of unsigned "
															"integers)",
						subject)));
				}
				for (size_t i = 0; i < indices.size(); ++i)
				{
					if (indices[i] >= vertexCount)
						return std::unexpected(Fail(std::format("index {} of {} is {}, beyond its {} vertices", i, subject, indices[i], vertexCount)));
				}
			}
			else
			{
				indices.resize(vertexCount);
				std::iota(indices.begin(), indices.end(), 0u);
			}
			if (primitive.type == cgltf_primitive_type_triangles && indices.size() % 3 != 0)
				return std::unexpected(Fail(std::format("{} has {} indices, which is not a multiple of 3 (a triangle list)", subject, indices.size())));

			// Triangle list without degenerate triangles (repeated indices or zero area), then only the vertices it uses,
			// in their original order.
			const std::vector<uint32_t> triangles = Utils::Triangulate(primitive.type, indices);
			std::vector<uint32_t> kept;
			kept.reserve(triangles.size());
			for (size_t corner = 0; corner + 2 < triangles.size(); corner += 3)
			{
				const uint32_t a = triangles[corner];
				const uint32_t b = triangles[corner + 1];
				const uint32_t c = triangles[corner + 2];
				if (a == b || b == c || a == c)
					continue;
				if (Utils::GetAreaNormal(source.Positions[a], source.Positions[b], source.Positions[c]) == glm::vec3(0.0f))
					continue;
				kept.insert(kept.end(), { a, b, c });
			}
			if (kept.empty())
				return std::unexpected(Fail(std::format("{} has no non-degenerate triangle", subject)));

			std::vector<uint32_t> remap(vertexCount, std::numeric_limits<uint32_t>::max());
			for (const uint32_t vertex : kept)
				remap[vertex] = 0;
			std::vector<size_t> usedVertices;
			for (size_t vertex = 0; vertex < vertexCount; ++vertex)
			{
				if (remap[vertex] != 0)
					continue;
				remap[vertex] = static_cast<uint32_t>(usedVertices.size());
				usedVertices.push_back(vertex);
			}

			PrimitiveAttributes attributes;
			attributes.Indices.reserve(kept.size());
			for (const uint32_t vertex : kept)
				attributes.Indices.push_back(remap[vertex]);
			attributes.Positions.reserve(usedVertices.size());
			for (const size_t vertex : usedVertices)
				attributes.Positions.push_back(source.Positions[vertex]);

			// Normals: the file's, normalized; generated (area-weighted) when missing or for a vertex whose normal has no
			// direction. The file's tangents are ignored without its normals (glTF 2.0 §3.7.2.1).
			const cgltf_accessor* normalAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
			attributes.Normals.assign(usedVertices.size(), glm::vec3(0.0f, 1.0f, 0.0f));
			std::vector<bool> generateNormal(usedVertices.size(), true);
			if (normalAccessor != nullptr)
			{
				ENGINE_TRY_ASSIGN(const std::vector<float> normals, ReadFloats(*normalAccessor, cgltf_type_vec3, subject, "NORMAL"));
				for (size_t vertex = 0; vertex < usedVertices.size(); ++vertex)
				{
					const size_t sourceVertex = usedVertices[vertex];
					const glm::vec3 normal = Utils::NormalizeOrZero(
						glm::vec3(normals[sourceVertex * 3], normals[sourceVertex * 3 + 1], normals[sourceVertex * 3 + 2]));
					if (normal != glm::vec3(0.0f))
					{
						attributes.Normals[vertex] = normal;
						generateNormal[vertex] = false;
					}
				}
			}
			if (std::ranges::any_of(generateNormal, [](bool generate)
			{
				return generate;
			}))
				Utils::GenerateNormals(attributes, generateNormal);

			const cgltf_accessor* texCoordAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
			attributes.TexCoords.assign(usedVertices.size(), glm::vec2(0.0f));
			if (texCoordAccessor != nullptr)
			{
				ENGINE_TRY_ASSIGN(const std::vector<float> texCoords, ReadFloats(*texCoordAccessor, cgltf_type_vec2, subject, "TEXCOORD_0"));
				for (size_t vertex = 0; vertex < usedVertices.size(); ++vertex)
					attributes.TexCoords[vertex] = glm::vec2(texCoords[usedVertices[vertex] * 2], texCoords[usedVertices[vertex] * 2 + 1]);
			}

			// M23 (GltfImporter version 2): the attribute stream's second UV set and vertex colour (MeshVertexAttributes).
			const cgltf_accessor* texCoord1Accessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 1);
			if (texCoord1Accessor != nullptr)
			{
				ENGINE_TRY_ASSIGN(const std::vector<float> texCoords, ReadFloats(*texCoord1Accessor, cgltf_type_vec2, subject, "TEXCOORD_1"));
				attributes.TexCoords1.resize(usedVertices.size());
				for (size_t vertex = 0; vertex < usedVertices.size(); ++vertex)
					attributes.TexCoords1[vertex] = glm::vec2(texCoords[usedVertices[vertex] * 2], texCoords[usedVertices[vertex] * 2 + 1]);
			}
			const cgltf_accessor* colorAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_color, 0);
			if (colorAccessor != nullptr)
			{
				ENGINE_TRY_ASSIGN(const std::vector<glm::vec4> colors, ReadColors(*colorAccessor, subject));
				attributes.Colors.resize(usedVertices.size());
				for (size_t vertex = 0; vertex < usedVertices.size(); ++vertex)
					attributes.Colors[vertex] = colors[usedVertices[vertex]];
			}

			// M23 (version 3): the skin's influences and the morph targets' deltas, per used vertex.
			if (readSkin)
				ENGINE_TRY(ReadSkinAttributes(primitive, subject, usedVertices, attributes));
			if (readMorphs)
			{
				ENGINE_TRY(ReadMorphTargets(primitive, subject, usedVertices, attributes));
				// A NORMAL delta displaces the file's normals (glTF 2.0 3.7.2.2): generated normals take none.
				if (normalAccessor == nullptr)
				{
					for (std::vector<MeshMorphDelta>& deltas : attributes.Morphs)
					{
						for (MeshMorphDelta& delta : deltas)
							delta.Normal = glm::vec3(0.0f);
					}
				}
			}

			// Tangents: the file's (with its normals), else MikkTSpace over the UV set the primitive's normal texture samples
			// (glTF 2.0 §3.7.2.1; TEXCOORD_0 unless its material binds the normal texture to TEXCOORD_1 and the primitive has
			// it), else a perpendicular basis.
			const bool tangentsFromTexCoord1 = Utils::GetNormalTexCoord(primitive) == 1 && texCoord1Accessor != nullptr;
			const cgltf_accessor* tangentTexCoords = tangentsFromTexCoord1 ? texCoord1Accessor : texCoordAccessor;
			bool haveTangents = false;
			const cgltf_accessor* tangentAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_tangent, 0);
			if (tangentAccessor != nullptr && normalAccessor != nullptr)
			{
				ENGINE_TRY_ASSIGN(const std::vector<float> tangents, ReadFloats(*tangentAccessor, cgltf_type_vec4, subject, "TANGENT"));
				attributes.Tangents.resize(usedVertices.size());
				haveTangents = true;
				for (size_t vertex = 0; vertex < usedVertices.size() && haveTangents; ++vertex)
				{
					const size_t sourceVertex = usedVertices[vertex];
					const glm::vec3 direction = Utils::NormalizeOrZero(
						glm::vec3(tangents[sourceVertex * 4], tangents[sourceVertex * 4 + 1], tangents[sourceVertex * 4 + 2]));
					// A tangent without direction makes the file's set unusable; it is generated instead.
					haveTangents = direction != glm::vec3(0.0f);
					if (haveTangents)
						attributes.Tangents[vertex] = glm::vec4(direction, tangents[sourceVertex * 4 + 3] < 0.0f ? -1.0f : 1.0f);
				}
			}
			if (!haveTangents)
			{
				// A TANGENT delta displaces the file's tangents: generated tangents take none.
				for (std::vector<MeshMorphDelta>& deltas : attributes.Morphs)
				{
					for (MeshMorphDelta& delta : deltas)
						delta.Tangent = glm::vec3(0.0f);
				}
				if (m_Settings.GenerateMissingTangents && tangentTexCoords != nullptr)
				{
					ENGINE_TRY_ASSIGN(const std::vector<glm::vec4> cornerTangents, WithContext(Utils::GenerateCornerTangents({
																								   .Positions = attributes.Positions,
																								   .Normals = attributes.Normals,
																								   .TexCoords = tangentsFromTexCoord1 ? attributes.TexCoords1 : attributes.TexCoords,
																								   .Indices = attributes.Indices,
																							   }),
																					   std::format("while generating the tangents of {} in '{}'", subject, m_DisplayPath)));
					Utils::ApplyCornerTangents(attributes, cornerTangents);
				}
				else
				{
					attributes.Tangents.resize(attributes.Positions.size());
					for (size_t vertex = 0; vertex < attributes.Positions.size(); ++vertex)
						attributes.Tangents[vertex] = Utils::MakePerpendicularTangent(attributes.Normals[vertex]);
					if (m_Settings.GenerateMissingTangents)
					{
						AddWarning(AssetTangentsApproximatedCode, subject,
							std::format("{} has no TANGENT and no TEXCOORD_0, so its tangents are an arbitrary basis perpendicular to the normal", subject),
							"export the model with UVs or with tangents");
					}
				}
			}

			if (attributes.Positions.size() > std::numeric_limits<uint32_t>::max())
				return std::unexpected(Fail(std::format("{} has more vertices than a mesh can index once its tangents are split", subject)));
			PrimitiveGeometry geometry;
			geometry.Vertices.resize(attributes.Positions.size());
			for (size_t vertex = 0; vertex < attributes.Positions.size(); ++vertex)
			{
				geometry.Vertices[vertex] = MeshVertex{
					.Position = attributes.Positions[vertex],
					.Normal = attributes.Normals[vertex],
					.Tangent = attributes.Tangents[vertex],
					.TexCoord = attributes.TexCoords[vertex],
				};
			}
			if (!attributes.TexCoords1.empty() || !attributes.Colors.empty())
			{
				geometry.Attributes.resize(attributes.Positions.size());
				for (size_t vertex = 0; vertex < attributes.Positions.size(); ++vertex)
				{
					if (!attributes.TexCoords1.empty())
						geometry.Attributes[vertex].TexCoord1 = attributes.TexCoords1[vertex];
					if (!attributes.Colors.empty())
						geometry.Attributes[vertex].Color = attributes.Colors[vertex];
				}
			}
			geometry.Joints = std::move(attributes.Joints);
			geometry.Weights = std::move(attributes.Weights);
			geometry.Morphs = std::move(attributes.Morphs);
			geometry.Indices = std::move(attributes.Indices);
			return geometry;
		}

		// JOINTS_0 (VEC4 unsigned byte or short, the skin's joint indices) and WEIGHTS_0 (VEC4 float, or normalized unsigned
		// byte or short), when the primitive has both; each vertex's weights are renormalized to sum to 1, and a vertex
		// without a positive weight is bound to its first joint. The joint indices are checked against the skin in ApplySkin.
		Status GltfImport::ReadSkinAttributes(const cgltf_primitive& primitive, std::string_view subject, std::span<const size_t> usedVertices,
			PrimitiveAttributes& attributes)
		{
			const cgltf_accessor* jointAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 0);
			const cgltf_accessor* weightAccessor = cgltf_find_accessor(&primitive, cgltf_attribute_type_weights, 0);
			if (jointAccessor == nullptr || weightAccessor == nullptr)
				return {};
			if (jointAccessor->component_type != cgltf_component_type_r_8u && jointAccessor->component_type != cgltf_component_type_r_16u)
				return std::unexpected(Fail(std::format("{} has a JOINTS_0 accessor that is not unsigned bytes or shorts", subject)));
			if (jointAccessor->normalized)
				return std::unexpected(Fail(std::format("{} has a normalized JOINTS_0 accessor; joint indices are plain integers", subject)));
			const cgltf_component_type weightType = weightAccessor->component_type;
			const bool weightsSupported = weightType == cgltf_component_type_r_32f
				|| (weightAccessor->normalized && (weightType == cgltf_component_type_r_8u || weightType == cgltf_component_type_r_16u));
			if (!weightsSupported)
				return std::unexpected(Fail(std::format("{} has a WEIGHTS_0 accessor that is neither float nor normalized unsigned bytes or shorts", subject)));
			ENGINE_TRY_ASSIGN(const std::vector<float> joints, ReadFloats(*jointAccessor, cgltf_type_vec4, subject, "JOINTS_0"));
			ENGINE_TRY_ASSIGN(const std::vector<float> weights, ReadFloats(*weightAccessor, cgltf_type_vec4, subject, "WEIGHTS_0"));
			if (cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 1) != nullptr)
			{
				AddWarning(AssetContentSkippedCode, std::string(subject) + ".JOINTS_1",
					std::format("{} has more than four joint influences (JOINTS_1 and WEIGHTS_1), which the engine does not import: only the first four "
								"deform the vertex",
						subject),
					"limit the skin to four influences per vertex when exporting");
			}

			attributes.Joints.resize(usedVertices.size());
			attributes.Weights.resize(usedVertices.size());
			for (size_t vertex = 0; vertex < usedVertices.size(); ++vertex)
			{
				const size_t source = usedVertices[vertex];
				glm::vec4 influence(0.0f);
				double sum = 0.0;
				for (glm::length_t component = 0; component < 4; ++component)
				{
					attributes.Joints[vertex][static_cast<size_t>(component)] = static_cast<uint16_t>(joints[source * 4 + static_cast<size_t>(component)]);
					const float weight = weights[source * 4 + static_cast<size_t>(component)];
					if (weight < 0.0f)
						return std::unexpected(Fail(std::format("{} has a negative WEIGHTS_0 value (element {})", subject, source)));
					influence[component] = weight;
					sum += weight;
				}
				if (sum > 0.0)
				{
					for (glm::length_t component = 0; component < 4; ++component)
						influence[component] = static_cast<float>(influence[component] / sum);
				}
				else
				{
					influence = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
				}
				attributes.Weights[vertex] = influence;
			}
			return {};
		}

		// The POSITION, NORMAL and TANGENT deltas of each of the primitive's morph targets (VEC3; float, or normalized integers
		// with KHR_mesh_quantization), one per used vertex; a target without an attribute has zero deltas for it.
		Status GltfImport::ReadMorphTargets(const cgltf_primitive& primitive, std::string_view subject, std::span<const size_t> usedVertices,
			PrimitiveAttributes& attributes) const
		{
			attributes.Morphs.assign(primitive.targets_count, std::vector<MeshMorphDelta>(usedVertices.size()));
			for (cgltf_size target = 0; target < primitive.targets_count; ++target)
			{
				const cgltf_morph_target& morph = primitive.targets[target];
				for (cgltf_size attribute = 0; attribute < morph.attributes_count; ++attribute)
				{
					const cgltf_attribute& source = morph.attributes[attribute];
					if (source.index != 0 || source.data == nullptr)
						continue;
					glm::vec3 MeshMorphDelta::* member = nullptr;
					std::string_view name;
					switch (source.type)
					{
						case cgltf_attribute_type_position:
							member = &MeshMorphDelta::Position;
							name = "POSITION";
							break;
						case cgltf_attribute_type_normal:
							member = &MeshMorphDelta::Normal;
							name = "NORMAL";
							break;
						case cgltf_attribute_type_tangent:
							member = &MeshMorphDelta::Tangent;
							name = "TANGENT";
							break;
						default:
							break;
					}
					if (member == nullptr)
						continue;
					const std::string targetSubject = std::format("{}.targets[{}]", subject, target);
					ENGINE_TRY_ASSIGN(const std::vector<float> deltas, ReadFloats(*source.data, cgltf_type_vec3, targetSubject, name));
					for (size_t vertex = 0; vertex < usedVertices.size(); ++vertex)
					{
						const size_t element = usedVertices[vertex] * 3;
						attributes.Morphs[target][vertex].*member = glm::vec3(deltas[element], deltas[element + 1], deltas[element + 2]);
					}
				}
			}
			return {};
		}

		// COLOR_0 as RGBA: a VEC3 accessor takes alpha 1 (glTF 2.0 §3.7.2.1), float or normalized unsigned byte or short
		// (cgltf_accessor_unpack_floats reads normalized integers as [0, 1]). A float channel outside [0, 1] is clamped into
		// it, as the material factors are.
		Result<std::vector<glm::vec4>> GltfImport::ReadColors(const cgltf_accessor& accessor, std::string_view subject) const
		{
			if (accessor.type != cgltf_type_vec3 && accessor.type != cgltf_type_vec4)
			{
				return std::unexpected(Fail(std::format("{} has a COLOR_0 accessor of type {}, expected VEC3 or VEC4", subject,
					Utils::GetAccessorTypeName(accessor.type))));
			}
			const bool rgb = accessor.type == cgltf_type_vec3;
			ENGINE_TRY_ASSIGN(const std::vector<float> values, ReadFloats(accessor, accessor.type, subject, "COLOR_0"));
			const size_t components = rgb ? 3 : 4;
			std::vector<glm::vec4> colors(accessor.count);
			for (size_t vertex = 0; vertex < colors.size(); ++vertex)
			{
				const float* channels = values.data() + vertex * components;
				const glm::vec4 color(channels[0], channels[1], channels[2], rgb ? 1.0f : channels[3]);
				colors[vertex] = glm::clamp(color, 0.0f, 1.0f);
			}
			return colors;
		}

		// Turns `geometry`'s JOINTS_0 and WEIGHTS_0 into the skin of a mesh variant drawn with `skin` (Skin, whose indices are
		// the Skeleton's joints, SkinPlan::JointMap) and moves each vertex from the skin's bind space into its Skeleton's by
		// the correction of its heaviest joint (the first of equal weights; SkinPlan::Corrections), so the variant deforms as
		// the skin's own inverse bind matrices deform it. Errors: ImportFailed for a weighted joint index beyond the skin's
		// joints, or for a primitive without the attributes (which PlanSkins rules out).
		Status GltfImport::ApplySkin(PrimitiveGeometry& geometry, const SkinPlan& skin, std::string_view subject) const
		{
			if (geometry.Joints.size() != geometry.Vertices.size() || geometry.Weights.size() != geometry.Vertices.size())
				return std::unexpected(Fail(std::format("{} has no JOINTS_0 and WEIGHTS_0, which the skin it is drawn with needs", subject)));
			geometry.Skin.resize(geometry.Vertices.size());
			for (size_t vertex = 0; vertex < geometry.Vertices.size(); ++vertex)
			{
				const std::array<uint16_t, 4>& joints = geometry.Joints[vertex];
				const glm::vec4& weights = geometry.Weights[vertex];
				MeshSkinVertex& skinned = geometry.Skin[vertex];
				skinned.Weights = weights;
				size_t heaviest = 0;
				for (size_t influence = 0; influence < joints.size(); ++influence)
				{
					const float weight = weights[static_cast<glm::length_t>(influence)];
					const uint16_t joint = joints[influence];
					if (weight > 0.0f && joint >= skin.Joints.size())
					{
						return std::unexpected(Fail(std::format("{} weights joint {} of its skin, which has {} joints (JOINTS_0)", subject, joint,
							skin.Joints.size())));
					}
					skinned.Joints[influence] = joint < skin.JointMap.size() ? skin.JointMap[joint] : uint16_t{ 0 };
					if (weight > weights[static_cast<glm::length_t>(heaviest)])
						heaviest = influence;
				}
				const std::optional<glm::dmat4>& correction = skin.Corrections[joints[heaviest]];
				if (correction.has_value())
					Utils::CorrectSkinVertex(*correction, geometry.Vertices[vertex], geometry.Morphs, vertex);
			}
			geometry.Joints.clear();
			geometry.Weights.clear();
			return {};
		}

		void GltfImport::AppendSubmesh(MeshData& mesh, PrimitiveGeometry geometry, const cgltf_primitive& primitive) const
		{
			const uint32_t baseVertex = static_cast<uint32_t>(mesh.Vertices.size());
			// The mesh has the attribute stream once one of its primitives has it: the vertices of the others take the
			// defaults (TexCoord1 (0, 0), opaque white), which draw them as a mesh without the stream draws. Until then the
			// stream is empty, so resizing it to the vertices so far gives the earlier primitives their defaults.
			if (!geometry.Attributes.empty() || !mesh.Attributes.empty())
			{
				mesh.Attributes.resize(mesh.Vertices.size());
				if (geometry.Attributes.empty())
					mesh.Attributes.resize(mesh.Vertices.size() + geometry.Vertices.size());
				else
					mesh.Attributes.insert(mesh.Attributes.end(), geometry.Attributes.begin(), geometry.Attributes.end());
			}
			// M23 (version 3): a skinned variant's primitives all have a skin (PlanSkins), and every primitive of a mesh has
			// the same morph targets (cgltf_validate), whose MeshMorphTarget the caller made.
			mesh.Skin.insert(mesh.Skin.end(), geometry.Skin.begin(), geometry.Skin.end());
			for (size_t target = 0; target < geometry.Morphs.size() && target < mesh.MorphTargets.size(); ++target)
			{
				std::vector<MeshMorphDelta>& deltas = mesh.MorphTargets[target].Deltas;
				deltas.insert(deltas.end(), geometry.Morphs[target].begin(), geometry.Morphs[target].end());
			}
			MeshSubmesh submesh;
			submesh.IndexOffset = static_cast<uint32_t>(mesh.Indices.size());
			submesh.IndexCount = static_cast<uint32_t>(geometry.Indices.size());
			submesh.MaterialSlot = static_cast<uint32_t>(mesh.Slots.size());
			for (const MeshVertex& vertex : geometry.Vertices)
				submesh.Bounds.Extend(vertex.Position);
			for (const uint32_t index : geometry.Indices)
				mesh.Indices.push_back(baseVertex + index);
			mesh.Vertices.insert(mesh.Vertices.end(), geometry.Vertices.begin(), geometry.Vertices.end());
			mesh.Submeshes.push_back(submesh);

			MeshMaterialSlot slot;
			if (primitive.material != nullptr)
			{
				const size_t material = cgltf_material_index(m_Data.get(), primitive.material);
				slot.Name = Utils::GetName(primitive.material->name, std::format("Material {}", material));
				slot.DefaultMaterial = m_MaterialHandles[material];
			}
			else
			{
				slot.Name = "Default";
			}
			mesh.Slots.push_back(std::move(slot));
		}

		Result<AssetHandle> GltfImport::CookMeshSubAsset(MeshData mesh, std::string key)
		{
			for (const MeshVertex& vertex : mesh.Vertices)
				mesh.Bounds.Extend(vertex.Position);
			if (mesh.Vertices.size() > std::numeric_limits<uint32_t>::max() || mesh.Indices.size() > std::numeric_limits<uint32_t>::max())
				return std::unexpected(Fail(std::format("the mesh '{}' has more vertices or indices than a mesh can hold", key)));
			const Status valid = ValidateMeshData(mesh);
			if (!valid)
				return std::unexpected(Error(valid.error()).WithContext(std::format("while cooking the mesh '{}' of '{}'", key, m_DisplayPath)));
			return AddSubAsset(std::move(key), AssetType::Mesh, CookMesh(mesh, GltfImporter::Version));
		}

		// M23 (version 3): the mesh's morph targets without their deltas: each named after the mesh's extras "targetNames"
		// ("Target <index>" without one; a name an earlier target took gets " <index>" appended), its default weight the
		// mesh's weights' (0 without them).
		Result<std::vector<MeshMorphTarget>> GltfImport::MakeMorphTargets(size_t meshIndex) const
		{
			const cgltf_mesh& source = m_Data->meshes[meshIndex];
			const size_t count = source.primitives_count > 0 ? source.primitives[0].targets_count : 0;
			std::vector<MeshMorphTarget> targets(count);
			std::vector<std::string> taken;
			for (size_t target = 0; target < count; ++target)
			{
				const char* name = target < source.target_names_count && source.target_names != nullptr ? source.target_names[target] : nullptr;
				const std::string fallback = std::format("Target {}", target);
				const std::optional<std::string> unique = Utils::MakeUniqueName(Utils::GetName(name, fallback), target, "Target",
					MaxMeshMorphTargetNameBytes, taken);
				if (!unique.has_value())
					return std::unexpected(Fail(std::format("meshes[{}] has no unique name left for its morph target {}", meshIndex, target)));
				taken.push_back(*unique);
				targets[target].Name = *unique;
				if (source.weights != nullptr && source.weights_count == count)
				{
					if (!std::isfinite(source.weights[target]))
						return std::unexpected(Fail(std::format("meshes[{}] has a morph weight that is not a finite number (weights[{}])", meshIndex, target)));
					targets[target].DefaultWeight = source.weights[target];
				}
			}
			return targets;
		}

		Status GltfImport::ImportMeshes()
		{
			m_MeshVariants.assign(m_Data->meshes_count, {});
			m_MorphTargetCounts.assign(m_Data->meshes_count, 0);
			const float scale = m_Settings.Scale;
			if (!m_Settings.MergeMeshes)
			{
				// The variants each mesh is cooked in (M23, version 3): one per skin an entity draws it with, and the one without
				// a skin for an entity that draws it unskinned, or when no entity draws it (every mesh imports).
				std::vector<std::vector<size_t>> variants(m_Data->meshes_count);
				for (size_t position = 0; position < m_SceneNodes.size(); ++position)
				{
					const size_t node = m_SceneNodes[position].Node;
					const cgltf_mesh* drawn = m_Data->nodes[node].mesh;
					if (drawn == nullptr || !m_HasEntity[position])
						continue;
					const size_t variant = m_NodeSkins[node].value_or(NoSkin);
					std::vector<size_t>& list = variants[cgltf_mesh_index(m_Data.get(), drawn)];
					if (std::ranges::find(list, variant) == list.end())
						list.push_back(variant);
				}

				for (cgltf_size meshIndex = 0; meshIndex < m_Data->meshes_count; ++meshIndex)
				{
					const cgltf_mesh& source = m_Data->meshes[meshIndex];
					std::vector<size_t>& list = variants[meshIndex];
					if (list.empty())
						list.push_back(NoSkin);
					std::ranges::sort(list); // the skins in index order, then the variant without one

					const size_t targetCount = source.primitives_count > 0 ? source.primitives[0].targets_count : 0;
					const bool readMorphs = targetCount > 0 && targetCount <= MaxMeshMorphTargets;
					if (targetCount > MaxMeshMorphTargets)
					{
						AddWarning(AssetContentSkippedCode, std::format("meshes[{}].targets", meshIndex),
							std::format("meshes[{}] has {} morph targets, more than the {} a mesh may have, so none is imported: the base shape is used",
								meshIndex, targetCount, MaxMeshMorphTargets),
							std::format("export the mesh with at most {} morph targets", MaxMeshMorphTargets));
					}
					const bool readSkin = list.front() != NoSkin;

					std::vector<std::pair<size_t, PrimitiveGeometry>> geometries;
					for (cgltf_size primitiveIndex = 0; primitiveIndex < source.primitives_count; ++primitiveIndex)
					{
						ENGINE_TRY_ASSIGN(PrimitiveGeometry geometry, ReadPrimitive(meshIndex, primitiveIndex, readSkin, readMorphs));
						if (!geometry.Vertices.empty())
							geometries.emplace_back(primitiveIndex, std::move(geometry));
					}
					if (geometries.empty())
						continue;
					std::vector<MeshMorphTarget> targets;
					if (readMorphs)
					{
						ENGINE_TRY_ASSIGN(targets, MakeMorphTargets(meshIndex));
						m_MorphTargetCounts[meshIndex] = targetCount;
					}

					const std::string key = Utils::MakeKey("mesh", meshIndex, source.name);
					for (const size_t variant : list)
					{
						MeshData mesh;
						mesh.MorphTargets = targets;
						for (const auto& [primitiveIndex, read] : geometries)
						{
							PrimitiveGeometry geometry = read;
							if (variant != NoSkin)
							{
								const std::string subject = std::format("meshes[{}].primitives[{}]", meshIndex, primitiveIndex);
								ENGINE_TRY(ApplySkin(geometry, m_Skins[variant], subject));
							}
							// Scale multiplies positions (and their morph deltas) only: it is uniform, so normals and tangents keep
							// their exact values.
							for (MeshVertex& vertex : geometry.Vertices)
								vertex.Position *= scale;
							for (std::vector<MeshMorphDelta>& deltas : geometry.Morphs)
							{
								for (MeshMorphDelta& delta : deltas)
									delta.Position *= scale;
							}
							AppendSubmesh(mesh, std::move(geometry), source.primitives[primitiveIndex]);
						}
						if (variant != NoSkin)
							mesh.SkinJointCount = static_cast<uint32_t>(m_Groups[m_Skins[variant].Group].Nodes.size());
						// One variant keeps the mesh's key; with several, each skinned variant names its skin.
						std::string variantKey = list.size() > 1 && variant != NoSkin ? std::format("{}:skin:{}", key, variant) : key;
						ENGINE_TRY_ASSIGN(const AssetHandle handle, CookMeshSubAsset(std::move(mesh), std::move(variantKey)));
						m_MeshVariants[meshIndex].emplace(variant, handle);
					}
				}
				return {};
			}

			// MergeMeshes: every mesh instance of the default scene, placed by its node's world matrix, in canonical order.
			MeshData merged;
			for (const SceneNode& node : m_SceneNodes)
			{
				const cgltf_mesh* source = m_Data->nodes[node.Node].mesh;
				if (source == nullptr)
					continue;
				const size_t meshIndex = cgltf_mesh_index(m_Data.get(), source);
				const glm::mat3 linear(node.World);
				const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
				const bool mirrored = glm::determinant(linear) < 0.0f;
				for (cgltf_size primitiveIndex = 0; primitiveIndex < source->primitives_count; ++primitiveIndex)
				{
					ENGINE_TRY_ASSIGN(PrimitiveGeometry geometry, ReadPrimitive(meshIndex, primitiveIndex, false, false));
					if (geometry.Vertices.empty())
						continue;
					for (MeshVertex& vertex : geometry.Vertices)
					{
						vertex.Position = glm::vec3(node.World * glm::vec4(vertex.Position, 1.0f)) * scale;
						const glm::vec3 normal = Utils::NormalizeOrZero(normalMatrix * vertex.Normal);
						vertex.Normal = normal != glm::vec3(0.0f) ? normal : glm::vec3(0.0f, 1.0f, 0.0f);
						const glm::vec3 tangent = Utils::NormalizeOrZero(linear * glm::vec3(vertex.Tangent));
						vertex.Tangent = tangent != glm::vec3(0.0f) ? glm::vec4(tangent, mirrored ? -vertex.Tangent.w : vertex.Tangent.w)
																	: Utils::MakePerpendicularTangent(vertex.Normal);
					}
					// A mirroring transform turns counter-clockwise triangles clockwise; swapping two corners restores them.
					if (mirrored)
					{
						for (size_t corner = 0; corner + 2 < geometry.Indices.size(); corner += 3)
							std::swap(geometry.Indices[corner + 1], geometry.Indices[corner + 2]);
					}
					AppendSubmesh(merged, std::move(geometry), source->primitives[primitiveIndex]);
				}
			}
			if (!merged.Submeshes.empty())
			{
				ENGINE_TRY_ASSIGN(m_MergedMesh, CookMeshSubAsset(std::move(merged), "mesh:merged"));
			}
			return {};
		}

		std::optional<size_t> GltfImport::GetScenePosition(size_t node) const
		{
			if (node >= m_ScenePositions.size() || m_ScenePositions[node] == NotInScene)
				return std::nullopt;
			return m_ScenePositions[node];
		}

		std::optional<size_t> GltfImport::FindNodeGroup(std::optional<size_t> animatorNode) const
		{
			if (!animatorNode.has_value())
				return m_RootAnimatorGroup;
			return *animatorNode < m_AnimatorGroups.size() ? m_AnimatorGroups[*animatorNode] : std::nullopt;
		}

		// Reads skin `skinIndex`'s joints and inverse bind matrices into its SkinPlan; a skin whose joints are not all in the
		// default scene is skipped with ASSET_CONTENT_SKIPPED. Errors: ImportFailed for a skin without joints or with more than
		// MaxSkeletonJoints, a joint listed twice, and inverse bind matrices that are not MAT4 floats, too few, or projective.
		Status GltfImport::ReadSkin(size_t skinIndex)
		{
			const cgltf_skin& skin = m_Data->skins[skinIndex];
			SkinPlan& plan = m_Skins[skinIndex];
			const std::string subject = std::format("skins[{}]", skinIndex);
			if (skin.joints_count == 0)
				return std::unexpected(Fail(std::format("{} has no joints", subject)));
			if (skin.joints_count > MaxSkeletonJoints)
			{
				return std::unexpected(Fail(std::format("{} has {} joints, more than the {} a skeleton may have", subject, skin.joints_count, MaxSkeletonJoints),
					std::format("export the skeleton with at most {} joints", MaxSkeletonJoints)));
			}
			std::vector<size_t> joints;
			joints.reserve(skin.joints_count);
			for (cgltf_size joint = 0; joint < skin.joints_count; ++joint)
			{
				const size_t node = cgltf_node_index(m_Data.get(), skin.joints[joint]);
				if (std::ranges::find(joints, node) != joints.end())
					return std::unexpected(Fail(std::format("{} lists nodes[{}] twice among its joints", subject, node)));
				joints.push_back(node);
			}
			for (const size_t node : joints)
			{
				if (!GetScenePosition(node).has_value())
				{
					AddWarning(AssetContentSkippedCode, subject,
						std::format("{} has the joint nodes[{}], which is not in the default scene, so the skin is not imported: the meshes it skins draw "
									"unskinned",
							subject, node),
						"put the skeleton in the scene the file shows by default");
					return {};
				}
			}

			std::vector<glm::dmat4> inverseBinds(joints.size(), glm::dmat4(1.0));
			if (skin.inverse_bind_matrices != nullptr)
			{
				const cgltf_accessor& accessor = *skin.inverse_bind_matrices;
				if (accessor.component_type != cgltf_component_type_r_32f)
					return std::unexpected(Fail(std::format("{} has inverse bind matrices that are not floats", subject)));
				if (accessor.count < joints.size())
				{
					return std::unexpected(Fail(std::format("{} has {} inverse bind matrices for its {} joints", subject,
						accessor.count, joints.size())));
				}
				ENGINE_TRY_ASSIGN(const std::vector<float> values, ReadFloats(accessor, cgltf_type_mat4, subject, "inverseBindMatrices"));
				for (size_t joint = 0; joint < joints.size(); ++joint)
				{
					const float* m = values.data() + joint * 16;
					const std::array<float, 4> lastRow = { m[3], m[7], m[11], m[15] - 1.0f };
					const bool projective = std::ranges::any_of(lastRow, [](float value)
					{
						return std::fabs(value) > Utils::MatrixRowTolerance;
					});
					if (projective)
					{
						return std::unexpected(Fail(std::format("{} has a projective inverse bind matrix for joint {} (its last row is not 0, 0, 0, 1)", subject,
							joint)));
					}
					glm::dmat4 matrix(1.0);
					for (glm::length_t column = 0; column < 4; ++column)
					{
						for (glm::length_t row = 0; row < 3; ++row)
							matrix[column][row] = m[static_cast<size_t>(column) * 4 + static_cast<size_t>(row)];
					}
					inverseBinds[joint] = matrix;
				}
			}

			plan.Imported = true;
			plan.Joints = std::move(joints);
			plan.InverseBinds = std::move(inverseBinds);
			plan.JointMap.assign(plan.Joints.size(), 0);
			plan.Corrections.assign(plan.Joints.size(), std::nullopt);
			for (size_t joint = 0; joint < plan.Joints.size(); ++joint)
			{
				const cgltf_node* parent = m_Data->nodes[plan.Joints[joint]].parent;
				if (parent == nullptr || std::ranges::find(plan.Joints, cgltf_node_index(m_Data.get(), parent)) == plan.Joints.end())
				{
					plan.FirstRootJoint = joint;
					break;
				}
			}
			return {};
		}

		// Whether every triangle primitive of `mesh` that has POSITION also has JOINTS_0 and WEIGHTS_0 (at least one does).
		bool GltfImport::HasSkinAttributes(const cgltf_mesh& mesh) const
		{
			bool any = false;
			for (cgltf_size index = 0; index < mesh.primitives_count; ++index)
			{
				const cgltf_primitive& primitive = mesh.primitives[index];
				const bool triangles = primitive.type == cgltf_primitive_type_triangles || primitive.type == cgltf_primitive_type_triangle_strip
					|| primitive.type == cgltf_primitive_type_triangle_fan;
				if (!triangles || cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0) == nullptr)
					continue;
				const bool skinned = cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 0) != nullptr
					&& cgltf_find_accessor(&primitive, cgltf_attribute_type_weights, 0) != nullptr;
				if (!skinned)
					return false;
				any = true;
			}
			return any;
		}

		// The node whose entity holds skin `skinIndex`'s Animator (GltfImporter.h): the nearest node that is not a joint at or
		// above the lowest common ancestor of the skin's root joints and the nodes drawn with it; nullopt for the root entity.
		std::optional<size_t> GltfImport::FindAnimatorNode(size_t skinIndex) const
		{
			const SkinPlan& plan = m_Skins[skinIndex];
			std::vector<size_t> members;
			for (const size_t joint : plan.Joints)
			{
				const cgltf_node* parent = m_Data->nodes[joint].parent;
				if (parent == nullptr || std::ranges::find(plan.Joints, cgltf_node_index(m_Data.get(), parent)) == plan.Joints.end())
					members.push_back(joint);
			}
			members.insert(members.end(), plan.Drawn.begin(), plan.Drawn.end());

			// The path from the scene to a node, root first; the common prefix of every member's path ends at their lowest
			// common ancestor (none: the scene).
			const auto pathTo = [this](size_t node)
			{
				std::vector<size_t> path;
				for (const cgltf_node* current = &m_Data->nodes[node]; current != nullptr; current = current->parent)
					path.push_back(cgltf_node_index(m_Data.get(), current));
				std::ranges::reverse(path);
				return path;
			};
			std::vector<size_t> common = pathTo(members.front());
			for (size_t member = 1; member < members.size(); ++member)
			{
				const std::vector<size_t> path = pathTo(members[member]);
				size_t shared = 0;
				while (shared < common.size() && shared < path.size() && common[shared] == path[shared])
					++shared;
				common.resize(shared);
			}
			while (!common.empty() && m_IsJoint[common.back()])
				common.pop_back();
			if (common.empty())
				return std::nullopt;
			return common.back();
		}

		// Builds `group`'s Skeleton (GltfImporter.h): its joints are its skins' joints and every node between them and the
		// Animator's node, in canonical order, each with its node's rest transform; a skin's joint keeps its inverse bind
		// matrix where it is the first skin of the group to list the joint, carried by that skin's correction into the
		// Skeleton's bind space; every other node of the Skeleton is a static joint with the identity. Fills each skin's
		// JointMap and Corrections. Errors: ImportFailed for more than MaxSkeletonJoints joints or inverse bind matrices that
		// cannot be inverted where skins must be reconciled.
		Status GltfImport::BuildSkeleton(SkeletonGroup& group)
		{
			const std::string subject = std::format("the skeleton of skins[{}]", group.Skins.front());
			std::set<size_t> members;
			for (const size_t skin : group.Skins)
			{
				for (const size_t joint : m_Skins[skin].Joints)
				{
					for (const cgltf_node* current = &m_Data->nodes[joint]; current != nullptr; current = current->parent)
					{
						const size_t node = cgltf_node_index(m_Data.get(), current);
						if (group.AnimatorNode == node || !members.insert(node).second)
							break;
						if (members.size() > MaxSkeletonJoints)
						{
							return std::unexpected(Fail(std::format("{} holds more than {} joints with the nodes between them and its Animator's entity, the "
																	"most a skeleton may have",
															subject, MaxSkeletonJoints),
								"export the skeleton with fewer joints"));
						}
					}
				}
			}
			group.Nodes.assign(members.begin(), members.end());
			std::ranges::sort(group.Nodes, {}, [this](size_t node)
			{
				return m_ScenePositions[node];
			});
			std::map<size_t, size_t> jointOfNode;
			for (size_t joint = 0; joint < group.Nodes.size(); ++joint)
				jointOfNode.emplace(group.Nodes[joint], joint);

			// The inverse bind matrices, skin by skin (the lead's decision on merged skins, ADR 0034's stream A notes): a skin
			// that shares a joint with the earlier skins is carried into their bind space by the correction its first shared
			// joint gives, so the joints it adds keep the earlier skins' bind space.
			std::map<size_t, glm::dmat4> merged;
			for (const size_t skin : group.Skins)
			{
				SkinPlan& plan = m_Skins[skin];
				std::optional<glm::dmat4> inverseCorrection;
				for (size_t joint = 0; joint < plan.Joints.size(); ++joint)
				{
					const auto shared = merged.find(plan.Joints[joint]);
					if (shared == merged.end())
						continue;
					if (shared->second != plan.InverseBinds[joint])
					{
						const std::optional<glm::dmat4> inverse = Utils::InvertAffine(shared->second);
						const std::optional<glm::dmat4> correction = inverse.has_value() ? std::optional(*inverse * plan.InverseBinds[joint]) : std::nullopt;
						inverseCorrection = correction.has_value() ? Utils::InvertAffine(*correction) : std::nullopt;
						if (!inverseCorrection.has_value())
						{
							return std::unexpected(Fail(std::format("skins[{}] and an earlier skin of the same Animator give nodes[{}] inverse bind matrices that "
																	"cannot be reconciled (one cannot be inverted)",
								skin, plan.Joints[joint])));
						}
					}
					break;
				}
				for (size_t joint = 0; joint < plan.Joints.size(); ++joint)
				{
					const size_t node = plan.Joints[joint];
					if (!merged.contains(node))
					{
						merged.emplace(node,
							inverseCorrection.has_value() ? plan.InverseBinds[joint] * *inverseCorrection : plan.InverseBinds[joint]);
					}
					const glm::dmat4& skeletonInverseBind = merged.at(node);
					plan.JointMap[joint] = static_cast<uint16_t>(jointOfNode.at(node));
					if (skeletonInverseBind == plan.InverseBinds[joint])
					{
						plan.Corrections[joint] = std::nullopt;
						continue;
					}
					const std::optional<glm::dmat4> inverse = Utils::InvertAffine(skeletonInverseBind);
					if (!inverse.has_value())
					{
						return std::unexpected(Fail(std::format("skins[{}] and an earlier skin of the same Animator give nodes[{}] inverse bind matrices that "
																"cannot be reconciled (one cannot be inverted)",
							skin, node)));
					}
					plan.Corrections[joint] = *inverse * plan.InverseBinds[joint];
				}
			}

			std::vector<std::string> taken;
			taken.reserve(group.Nodes.size());
			for (size_t joint = 0; joint < group.Nodes.size(); ++joint)
			{
				const size_t node = group.Nodes[joint];
				const SceneNode& sceneNode = m_SceneNodes[m_ScenePositions[node]];
				const std::optional<std::string> name = Utils::MakeUniqueName(sceneNode.Name, node, "Node", MaxSkeletonJointNameBytes, taken);
				if (!name.has_value())
					return std::unexpected(Fail(std::format("{} has no unique name left for nodes[{}]", subject, node)));
				taken.push_back(*name);

				SkeletonJoint skeletonJoint;
				skeletonJoint.Name = *name;
				const cgltf_node* parent = m_Data->nodes[node].parent;
				const auto parentJoint = parent != nullptr ? jointOfNode.find(cgltf_node_index(m_Data.get(), parent)) : jointOfNode.end();
				skeletonJoint.Parent = parentJoint == jointOfNode.end() ? SkeletonRootParent : static_cast<int32_t>(parentJoint->second);
				skeletonJoint.Translation = sceneNode.Transform.Translation;
				skeletonJoint.Rotation = sceneNode.Transform.Rotation;
				skeletonJoint.Scale = sceneNode.Transform.Scale;
				if (const auto inverseBind = merged.find(node); inverseBind != merged.end())
				{
					// Scale is baked as into the meshes: S * InverseBind * S^-1 scales the translation only.
					glm::dmat4 scaled = inverseBind->second;
					scaled[3] = glm::dvec4(glm::dvec3(scaled[3]) * static_cast<double>(m_Settings.Scale), 1.0);
					skeletonJoint.InverseBind = glm::mat4(scaled);
					for (glm::length_t column = 0; column < 3; ++column)
						skeletonJoint.InverseBind[column][3] = 0.0f;
					skeletonJoint.InverseBind[3][3] = 1.0f;
				}
				group.Skeleton.Joints.push_back(std::move(skeletonJoint));
			}
			const SkinPlan& first = m_Skins[group.Skins.front()];
			group.Skeleton.RootMotionJoint = static_cast<uint32_t>(jointOfNode.at(first.Joints[first.FirstRootJoint]));
			const Status valid = ValidateSkeletonData(group.Skeleton);
			if (!valid)
				return std::unexpected(Fail(std::format("{} is not a valid skeleton: {}", subject, valid.error().GetMessageText())));
			return {};
		}

		// The SkeletonGroup of the nearest Animator on the entity at `position` or an ancestor of it (the Animator its
		// SkinnedMesh follows), or nullopt without one.
		std::optional<size_t> GltfImport::FindEntityAnimatorGroup(size_t position) const
		{
			size_t current = position;
			while (current != NotInScene)
			{
				if (const std::optional<size_t> group = FindNodeGroup(m_SceneNodes[current].Node); group.has_value())
					return group;
				current = m_ParentPositions[current];
			}
			return FindNodeGroup(std::nullopt);
		}

		// Places the entity at `position` under the entity of `animatorNode` (nullopt: the root entity) at its rest transform
		// relative to it.
		void GltfImport::PlaceUnder(size_t position, std::optional<size_t> animatorNode)
		{
			SceneNode& sceneNode = m_SceneNodes[position];
			glm::dmat4 animatorWorld(1.0);
			size_t parentPosition = NotInScene;
			UUID parentID = MakeEntityID("root");
			if (animatorNode.has_value())
			{
				parentPosition = m_ScenePositions[*animatorNode];
				animatorWorld = glm::dmat4(m_SceneNodes[parentPosition].World);
				parentID = m_SceneNodes[parentPosition].ID;
			}
			const std::optional<glm::dmat4> inverse = Utils::InvertAffine(animatorWorld);
			const glm::dmat4 relative = inverse.value_or(glm::dmat4(1.0)) * glm::dmat4(sceneNode.World);
			TransformComponent transform = Utils::DecomposeApproximately(glm::mat4(relative));
			transform.Translation *= m_Settings.Scale;
			sceneNode.Transform = transform;
			sceneNode.Parent = parentID;
			m_ParentPositions[position] = parentPosition;
		}

		// Decides each entity's place (GltfImporter.h): joints are no entity (a mesh on a joint is reported and not placed); a
		// node below a joint goes under the Animator of the first Skeleton holding that joint; and a skinned node whose
		// nearest Animator is not its skin's goes under its skin's (or, when its own entity holds another skin's Animator,
		// draws unskinned, reported).
		void GltfImport::PlaceEntities()
		{
			for (size_t position = 0; position < m_SceneNodes.size(); ++position)
			{
				const size_t node = m_SceneNodes[position].Node;
				const cgltf_node& source = m_Data->nodes[node];
				const std::string name = m_SceneNodes[position].Name;
				if (m_IsJoint[node])
				{
					m_HasEntity[position] = false;
					if (source.mesh != nullptr)
					{
						AddWarning(AssetContentSkippedCode, std::format("nodes[{}].mesh", node),
							std::format("nodes[{}] '{}' is a joint of a skin and draws meshes[{}]; joints become the Skeleton's joints, not entities, so the "
										"mesh is not placed",
								node, name, cgltf_mesh_index(m_Data.get(), source.mesh)),
							"parent the mesh to a node that is not a joint");
					}
					continue;
				}
				if (source.parent != nullptr && m_IsJoint[cgltf_node_index(m_Data.get(), source.parent)])
				{
					const auto holders = m_SkeletonJoints.find(cgltf_node_index(m_Data.get(), source.parent));
					if (holders != m_SkeletonJoints.end() && !holders->second.empty())
						PlaceUnder(position, m_Groups[holders->second.front().first].AnimatorNode);
				}

				const std::optional<size_t> skin = m_NodeSkins[node];
				if (!skin.has_value())
					continue;
				const size_t group = m_Skins[*skin].Group;
				const std::optional<size_t> nearest = FindEntityAnimatorGroup(position);
				if (nearest == group)
					continue;
				const std::optional<size_t> own = FindNodeGroup(node);
				if (own.has_value())
				{
					m_NodeSkins[node] = std::nullopt;
					AddWarning(AssetContentSkippedCode, std::format("nodes[{}].skin", node),
						std::format("nodes[{}] '{}' is drawn with skins[{}] but holds the Animator of skins[{}]; a SkinnedMesh follows the Animator of its own "
									"entity first, so the mesh draws unskinned",
							node, name, *skin, m_Groups[*own].Skins.front()),
						"move the mesh to a node of its own");
					continue;
				}
				PlaceUnder(position, m_Groups[group].AnimatorNode);
			}
		}

		Status GltfImport::PlanSkins()
		{
			m_ScenePositions.assign(m_Data->nodes_count, NotInScene);
			for (size_t position = 0; position < m_SceneNodes.size(); ++position)
				m_ScenePositions[m_SceneNodes[position].Node] = position;
			m_IsJoint.assign(m_Data->nodes_count, false);
			m_NodeSkins.assign(m_Data->nodes_count, std::nullopt);
			m_AnimatorGroups.assign(m_Data->nodes_count, std::nullopt);
			m_HasEntity.assign(m_SceneNodes.size(), true);
			const UUID rootID = MakeEntityID("root");
			m_ParentPositions.assign(m_SceneNodes.size(), NotInScene);
			for (size_t position = 0; position < m_SceneNodes.size(); ++position)
			{
				const cgltf_node* parent = m_Data->nodes[m_SceneNodes[position].Node].parent;
				if (m_SceneNodes[position].Parent != rootID && parent != nullptr)
					m_ParentPositions[position] = m_ScenePositions[cgltf_node_index(m_Data.get(), parent)];
			}
			if (m_Settings.MergeMeshes || m_Data->skins_count == 0)
				return {};

			m_Skins.resize(m_Data->skins_count);
			for (cgltf_size skin = 0; skin < m_Data->skins_count; ++skin)
				ENGINE_TRY(ReadSkin(skin));
			for (const SkinPlan& plan : m_Skins)
			{
				for (const size_t joint : plan.Joints)
					m_IsJoint[joint] = true;
			}

			for (const SceneNode& sceneNode : m_SceneNodes)
			{
				const cgltf_node& node = m_Data->nodes[sceneNode.Node];
				if (node.skin == nullptr || node.mesh == nullptr || m_IsJoint[sceneNode.Node])
					continue;
				const size_t skin = cgltf_skin_index(m_Data.get(), node.skin);
				if (!m_Skins[skin].Imported)
					continue;
				if (!HasSkinAttributes(*node.mesh))
				{
					AddWarning(AssetContentSkippedCode, std::format("nodes[{}].skin", sceneNode.Node),
						std::format("nodes[{}] '{}' is drawn with skins[{}], but its mesh's primitives lack JOINTS_0 or WEIGHTS_0, so it draws unskinned",
							sceneNode.Node, sceneNode.Name, skin),
						"export the mesh with its skin weights");
					continue;
				}
				m_NodeSkins[sceneNode.Node] = skin;
				m_Skins[skin].Drawn.push_back(sceneNode.Node);
			}

			// One Skeleton per Animator entity: the skins that resolve to the same entity are merged, in skin order.
			for (size_t skin = 0; skin < m_Skins.size(); ++skin)
			{
				if (!m_Skins[skin].Imported)
					continue;
				const std::optional<size_t> animatorNode = FindAnimatorNode(skin);
				std::optional<size_t> group = FindNodeGroup(animatorNode);
				if (!group.has_value())
				{
					group = m_Groups.size();
					m_Groups.push_back(SkeletonGroup{ .AnimatorNode = animatorNode });
					if (animatorNode.has_value())
						m_AnimatorGroups[*animatorNode] = group;
					else
						m_RootAnimatorGroup = group;
				}
				m_Groups[*group].Skins.push_back(skin);
				m_Skins[skin].Group = *group;
			}
			for (size_t group = 0; group < m_Groups.size(); ++group)
			{
				ENGINE_TRY(BuildSkeleton(m_Groups[group]));
				for (size_t joint = 0; joint < m_Groups[group].Nodes.size(); ++joint)
					m_SkeletonJoints[m_Groups[group].Nodes[joint]].emplace_back(group, joint);
			}
			PlaceEntities();
			return {};
		}

		Status GltfImport::ImportSkeletons()
		{
			for (SkeletonGroup& group : m_Groups)
			{
				const size_t first = group.Skins.front();
				group.Handle = AddSubAsset(Utils::MakeKey("skin", first, m_Data->skins[first].name), AssetType::Skeleton,
					CookSkeleton(group.Skeleton, GltfImporter::Version));
			}
			return {};
		}

		// The Target of a Weights channel that animates `node`'s morph weights: the path of its entity from the entity of the
		// nearest Animator at or above it ("" for that entity itself), whose SkeletonGroup goes to `group`; nullopt when the
		// node draws no imported morph targets, has no Animator at or above it, or its path cannot be written (an entity name
		// holding '/', or longer than MaxAnimationNameBytes).
		std::optional<std::string> GltfImport::GetWeightsTarget(size_t node, std::optional<size_t>& group) const
		{
			const std::optional<size_t> position = GetScenePosition(node);
			const cgltf_mesh* mesh = m_Data->nodes[node].mesh;
			if (!position.has_value() || !m_HasEntity[*position] || mesh == nullptr)
				return std::nullopt;
			const size_t meshIndex = cgltf_mesh_index(m_Data.get(), mesh);
			if (m_MorphTargetCounts[meshIndex] == 0 || m_MeshVariants[meshIndex].empty())
				return std::nullopt;
			group = FindEntityAnimatorGroup(*position);
			if (!group.has_value())
				return std::nullopt;
			const std::optional<size_t> animatorNode = m_Groups[*group].AnimatorNode;
			std::vector<std::string_view> names;
			for (size_t current = *position; current != NotInScene && m_SceneNodes[current].Node != animatorNode;
				current = m_ParentPositions[current])
			{
				if (m_SceneNodes[current].Name.contains('/'))
					return std::nullopt;
				names.push_back(m_SceneNodes[current].Name);
			}
			std::string path;
			for (auto name = names.rbegin(); name != names.rend(); ++name)
			{
				if (!path.empty())
					path.push_back('/');
				path += *name;
			}
			if (path.size() > MaxAnimationNameBytes)
				return std::nullopt;
			return path;
		}

		// Animation `animationIndex` as a clip (GltfImporter.h), or nullopt when it is skipped (reported): no channel left, or
		// shorter than MinAnimationClipDuration. Errors: ImportFailed for a sampler whose input is not SCALAR floats, strictly
		// increasing from 0 within a day, or whose output does not match its path, or for more channels, keys or floats than a
		// clip may hold.
		Result<std::optional<PendingClip>> GltfImport::ReadAnimation(size_t animationIndex)
		{
			const cgltf_animation& animation = m_Data->animations[animationIndex];
			const std::string subject = std::format("animations[{}]", animationIndex);
			const std::string animationName = Utils::GetName(animation.name, std::format("Animation {}", animationIndex));
			if (animation.channels_count > MaxAnimationChannels)
			{
				return std::unexpected(Fail(std::format("{} '{}' has {} channels, more than the {} a clip may have", subject, animationName, animation.channels_count,
					MaxAnimationChannels)));
			}
			PendingClip pending;
			pending.Key = Utils::MakeKey("animation", animationIndex, animation.name);
			std::vector<std::pair<std::string, std::string>> skipped; // (subject, message) of each channel skipped
			std::set<std::pair<std::string, AnimationPath>> keyed;
			uint64_t floats = 0;
			for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; ++channelIndex)
			{
				const cgltf_animation_channel& channel = animation.channels[channelIndex];
				const std::string channelSubject = std::format("{}.channels[{}]", subject, channelIndex);
				if (channel.target_node == nullptr)
				{
					skipped.emplace_back(channelSubject, std::format("{} targets no node, so it is not imported", channelSubject));
					continue;
				}
				const size_t node = cgltf_node_index(m_Data.get(), channel.target_node);
				const std::string nodeName = Utils::GetName(channel.target_node->name, std::format("Node {}", node));
				AnimationChannel output;
				std::vector<size_t> groups;
				switch (channel.target_path)
				{
					case cgltf_animation_path_type_translation:
					case cgltf_animation_path_type_rotation:
					case cgltf_animation_path_type_scale:
					{
						const auto holders = node < m_IsJoint.size() && m_IsJoint[node] ? m_SkeletonJoints.find(node) : m_SkeletonJoints.end();
						if (holders == m_SkeletonJoints.end() || holders->second.empty())
						{
							skipped.emplace_back(channelSubject, std::format("{} animates nodes[{}] '{}', which is not a joint of a skin; the engine imports only "
																			 "skeletal and morph animation",
																	 channelSubject, node, nodeName));
							continue;
						}
						output.Target = m_Groups[holders->second.front().first].Skeleton.Joints[holders->second.front().second].Name;
						for (const std::pair<size_t, size_t>& holder : holders->second)
							groups.push_back(holder.first);
						output.Path = channel.target_path == cgltf_animation_path_type_translation ? AnimationPath::Translation
							: channel.target_path == cgltf_animation_path_type_rotation            ? AnimationPath::Rotation
																								   : AnimationPath::Scale;
						break;
					}
					case cgltf_animation_path_type_weights:
					{
						std::optional<size_t> group;
						std::optional<std::string> target = GetWeightsTarget(node, group);
						if (!target.has_value() || !group.has_value())
						{
							skipped.emplace_back(channelSubject, std::format("{} animates the morph weights of nodes[{}] '{}', which no Animator plays: only a node "
																			 "with imported morph targets at or below a skin's Animator is animated",
																	 channelSubject, node, nodeName));
							continue;
						}
						output.Target = std::move(*target);
						output.Path = AnimationPath::Weights;
						output.WeightCount = static_cast<uint32_t>(m_MorphTargetCounts[cgltf_mesh_index(m_Data.get(), channel.target_node->mesh)]);
						groups.push_back(*group);
						break;
					}
					case cgltf_animation_path_type_invalid:
					case cgltf_animation_path_type_max_enum:
						skipped.emplace_back(channelSubject, std::format("{} has a target path the engine does not import", channelSubject));
						continue;
				}
				if (!keyed.emplace(output.Target, output.Path).second)
				{
					skipped.emplace_back(channelSubject,
						std::format("{} keys what an earlier channel of the animation keys, so it is not imported", channelSubject));
					continue;
				}

				const cgltf_animation_sampler& sampler = *channel.sampler;
				const std::string samplerSubject = std::format("{}.samplers[{}]", subject, cgltf_animation_sampler_index(&animation, &sampler));
				if (sampler.input->component_type != cgltf_component_type_r_32f)
					return std::unexpected(Fail(std::format("{} has an input that is not float key times", samplerSubject)));
				ENGINE_TRY_ASSIGN(std::vector<float> times, ReadFloats(*sampler.input, cgltf_type_scalar, samplerSubject, "input"));
				if (times.empty() || times.size() > MaxAnimationChannelKeys)
				{
					return std::unexpected(Fail(std::format("{} has {} keys, not 1 to the {} a clip channel may hold", samplerSubject, times.size(),
						MaxAnimationChannelKeys)));
				}
				for (size_t key = 0; key < times.size(); ++key)
				{
					if (times[key] < 0.0f || times[key] > MaxAnimationClipDuration || (key > 0 && !(times[key] > times[key - 1])))
					{
						return std::unexpected(Fail(std::format("{}'s key times are not strictly increasing from 0 to at most a day (key {} is {} s)", samplerSubject,
							key, times[key])));
					}
				}
				switch (sampler.interpolation)
				{
					case cgltf_interpolation_type_step:
						output.Interpolation = AnimationInterpolation::Step;
						break;
					case cgltf_interpolation_type_cubic_spline:
						output.Interpolation = AnimationInterpolation::CubicSpline;
						break;
					case cgltf_interpolation_type_linear:
					case cgltf_interpolation_type_max_enum:
						output.Interpolation = AnimationInterpolation::Linear;
						break;
				}
				const cgltf_type outputType = output.Path == AnimationPath::Rotation ? cgltf_type_vec4
					: output.Path == AnimationPath::Weights                          ? cgltf_type_scalar
																					 : cgltf_type_vec3;
				ENGINE_TRY_ASSIGN(std::vector<float> values, ReadFloats(*sampler.output, outputType, samplerSubject, "output"));
				const uint64_t keyFloats = GetAnimationKeyFloatCount(output);
				if (values.size() != times.size() * keyFloats)
				{
					return std::unexpected(Fail(std::format("{} has {} output values for its {} keys of {} values", samplerSubject, values.size(), times.size(),
						keyFloats)));
				}
				floats += times.size() + values.size();
				if (floats > MaxAnimationClipFloats)
				{
					return std::unexpected(Fail(std::format("{} '{}' keys more than the {} values a clip may hold", subject,
						animationName, MaxAnimationClipFloats)));
				}
				if (output.Path == AnimationPath::Translation)
				{
					for (float& value : values)
						value *= m_Settings.Scale;
				}
				if (output.Path == AnimationPath::Rotation && output.Interpolation != AnimationInterpolation::CubicSpline)
				{
					for (size_t key = 0; key < times.size(); ++key)
					{
						float* rotation = values.data() + key * 4;
						const double length = std::sqrt(static_cast<double>(rotation[0]) * rotation[0] + static_cast<double>(rotation[1]) * rotation[1]
							+ static_cast<double>(rotation[2]) * rotation[2] + static_cast<double>(rotation[3]) * rotation[3]);
						if (!(length > 0.0) || !std::isfinite(length))
							return std::unexpected(Fail(std::format("{} has a rotation key of zero length (key {})", samplerSubject, key)));
						for (size_t component = 0; component < 4; ++component)
							rotation[component] = static_cast<float>(rotation[component] / length);
					}
				}
				for (const float value : values)
				{
					if (!std::isfinite(value))
						return std::unexpected(Fail(std::format("{} has an output value that is not a finite number once imported", samplerSubject)));
				}
				output.Times = std::move(times);
				output.Values = std::move(values);
				pending.Clip.Channels.push_back(std::move(output));
				for (const size_t group : groups)
				{
					if (std::ranges::find(pending.Groups, group) == pending.Groups.end())
						pending.Groups.push_back(group);
				}
			}

			if (pending.Clip.Channels.empty())
			{
				AddWarning(AssetContentSkippedCode, subject,
					std::format("{} '{}' animates no joint of a skin and no morph weights an Animator plays, so it is not imported (the engine imports only "
								"skeletal and morph animation)",
						subject, animationName),
					{});
				return std::optional<PendingClip>();
			}
			float duration = 0.0f;
			for (const AnimationChannel& channel : pending.Clip.Channels)
				duration = std::max(duration, channel.Times.back());
			if (duration > 0.0f && duration < MinAnimationClipDuration)
			{
				AddWarning(AssetContentSkippedCode, subject,
					std::format("{} '{}' lasts {} s, shorter than the {} s a clip that is not a single pose lasts, so it is not imported", subject,
						animationName, duration, MinAnimationClipDuration),
					"key the animation over at least a millisecond");
				return std::optional<PendingClip>();
			}
			pending.Clip.Duration = duration;
			for (auto& [channelSubject, message] : skipped)
				AddWarning(AssetContentSkippedCode, std::move(channelSubject), std::move(message), {});
			std::ranges::sort(pending.Groups);
			return std::optional<PendingClip>(std::move(pending));
		}

		// Adds the settings' AnimationEvents to the clips they name, each clip's sorted by time (stable). Errors: ImportFailed
		// naming the event for a clip key the file does not produce, a name that is not 1 to MaxAnimationNameBytes bytes of
		// UTF-8, a time outside the clip, or more than MaxAnimationEvents events in one clip.
		Status GltfImport::AddAnimationEvents(std::vector<PendingClip>& clips) const
		{
			for (size_t index = 0; index < m_Settings.AnimationEvents.size(); ++index)
			{
				const GltfAnimationEvent& event = m_Settings.AnimationEvents[index];
				const std::string subject = std::format("the import setting AnimationEvents[{}] '{}'", index, event.Name);
				const auto clip = std::ranges::find(clips, event.Animation, &PendingClip::Key);
				if (clip == clips.end())
				{
					std::string available;
					for (const PendingClip& candidate : clips)
						available += available.empty() ? candidate.Key : ", " + candidate.Key;
					return std::unexpected(Fail(std::format("{} names the clip '{}', which is not an animation clip of the file", subject, event.Animation),
						available.empty() ? std::string("the file imports no animation clip; remove the event")
										  : std::format("name one of the file's clips: {}", available)));
				}
				if (event.Name.empty() || event.Name.size() > MaxAnimationNameBytes || !IsValidUtf8(event.Name))
				{
					return std::unexpected(Fail(std::format("{} has a name that is not 1 to {} bytes of valid UTF-8", subject,
						MaxAnimationNameBytes)));
				}
				if (!std::isfinite(event.Time) || event.Time < 0.0f || event.Time > clip->Clip.Duration)
				{
					return std::unexpected(Fail(std::format("{} is at {} s, outside its clip '{}' of {} s", subject, event.Time, event.Animation,
						clip->Clip.Duration)));
				}
				if (clip->Clip.Events.size() >= MaxAnimationEvents)
					return std::unexpected(Fail(std::format("{} is beyond the {} events a clip may have", subject, MaxAnimationEvents)));
				clip->Clip.Events.push_back(AnimationEvent{ .Name = event.Name, .Time = event.Time });
			}
			for (PendingClip& clip : clips)
				std::ranges::stable_sort(clip.Clip.Events, {}, &AnimationEvent::Time);
			return {};
		}

		Status GltfImport::ImportAnimations()
		{
			std::vector<PendingClip> clips;
			if (!m_Settings.MergeMeshes)
			{
				for (cgltf_size animation = 0; animation < m_Data->animations_count; ++animation)
				{
					ENGINE_TRY_ASSIGN(std::optional<PendingClip> clip, ReadAnimation(animation));
					if (clip.has_value())
						clips.push_back(std::move(*clip));
				}
			}
			ENGINE_TRY(AddAnimationEvents(clips));
			for (PendingClip& pending : clips)
			{
				const Status valid = ValidateAnimationClipData(pending.Clip);
				if (!valid)
				{
					return std::unexpected(Fail(std::format("the clip '{}' is not a valid animation clip: {}", pending.Key,
						valid.error().GetMessageText())));
				}
				const AssetHandle handle = AddSubAsset(pending.Key, AssetType::AnimationClip, CookAnimationClip(pending.Clip, GltfImporter::Version));
				for (const size_t group : pending.Groups)
				{
					if (!m_Groups[group].FirstClip.IsValid())
						m_Groups[group].FirstClip = handle;
				}
			}
			return {};
		}

		// The SkinnedMesh weights of `node`: the node's glTF weights, else its mesh's, each clamped into [-1, 1] (0 for a
		// non-finite one); empty when its mesh imported no morph targets.
		std::vector<float> GltfImport::GetNodeMorphWeights(size_t node) const
		{
			const cgltf_node& source = m_Data->nodes[node];
			if (source.mesh == nullptr)
				return {};
			const size_t count = m_MorphTargetCounts[cgltf_mesh_index(m_Data.get(), source.mesh)];
			if (count == 0)
				return {};
			const cgltf_float* weights = nullptr;
			if (source.weights != nullptr && source.weights_count == count)
				weights = source.weights;
			else if (source.mesh->weights != nullptr && source.mesh->weights_count == count)
				weights = source.mesh->weights;
			std::vector<float> result(count, 0.0f);
			for (size_t target = 0; target < count && weights != nullptr; ++target)
				result[target] = std::isfinite(weights[target]) ? std::clamp(weights[target], -1.0f, 1.0f) : 0.0f;
			return result;
		}

		Result<Buffer> GltfImport::BuildPrefab() const
		{
			const TypeRegistry& registry = m_Context->GetRegistry();
			const ComponentInfo* transformInfo = registry.FindComponent<TransformComponent>();
			const ComponentInfo* rendererInfo = registry.FindComponent<MeshRendererComponent>();
			const ComponentInfo* animatorInfo = registry.FindComponent<AnimatorComponent>();
			const ComponentInfo* skinnedInfo = registry.FindComponent<SkinnedMeshComponent>();
			if (transformInfo == nullptr || rendererInfo == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry lacks the built-in components Transform and MeshRenderer");

			// One entity of the document, before it is written.
			struct PrefabEntity
			{
				UUID ID{};
				UUID Parent{};
				std::string Name{};
				TransformComponent Transform{};
				AssetHandle Mesh{};
				std::optional<AnimatorComponent> Animator{};
				std::optional<SkinnedMeshComponent> Skinned{};
			};
			std::vector<PrefabEntity> entities;
			const auto makeAnimator = [this](std::optional<size_t> animatorNode) -> std::optional<AnimatorComponent>
			{
				const std::optional<size_t> group = FindNodeGroup(animatorNode);
				if (!group.has_value())
					return std::nullopt;
				const SkeletonGroup& skeleton = m_Groups[*group];
				AnimatorComponent animator;
				animator.Skeleton = TypedAssetHandle<AssetType::Skeleton>(skeleton.Handle);
				if (skeleton.FirstClip.IsValid())
					animator.Layers.push_back(AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(skeleton.FirstClip) });
				return animator;
			};

			const UUID rootID = MakeEntityID("root");
			const std::string name(m_Context->GetSourcePath().GetStem());
			entities.push_back(PrefabEntity{ .ID = rootID, .Name = name, .Mesh = m_MergedMesh, .Animator = makeAnimator(std::nullopt) });
			for (size_t position = 0; position < m_SceneNodes.size(); ++position)
			{
				if (!m_HasEntity[position])
					continue;
				const SceneNode& node = m_SceneNodes[position];
				PrefabEntity entity{ .ID = node.ID, .Parent = node.Parent, .Name = node.Name, .Transform = node.Transform };
				entity.Animator = makeAnimator(node.Node);
				const cgltf_mesh* drawn = m_Data->nodes[node.Node].mesh;
				if (!m_Settings.MergeMeshes && drawn != nullptr)
				{
					const size_t meshIndex = cgltf_mesh_index(m_Data.get(), drawn);
					const size_t variant = m_NodeSkins[node.Node].value_or(NoSkin);
					if (const auto handle = m_MeshVariants[meshIndex].find(variant); handle != m_MeshVariants[meshIndex].end())
						entity.Mesh = handle->second;
					if (entity.Mesh.IsValid() && (variant != NoSkin || m_MorphTargetCounts[meshIndex] > 0))
						entity.Skinned = SkinnedMeshComponent{ .MorphWeights = GetNodeMorphWeights(node.Node) };
				}
				entities.push_back(std::move(entity));
			}
			const bool animated = std::ranges::any_of(entities, [](const PrefabEntity& entity)
			{
				return entity.Animator.has_value() || entity.Skinned.has_value();
			});
			if (animated && (animatorInfo == nullptr || skinnedInfo == nullptr))
				return MakeError(ErrorCode::InvalidState, "the type registry lacks the built-in components Animator and SkinnedMesh");

			// Canonical order (§5.1): the root, then depth first with each entity's children in the order of the scene's
			// nodes, which a placement under an Animator's entity can change from the glTF hierarchy's.
			std::map<UUID, std::vector<size_t>> children;
			for (size_t index = 1; index < entities.size(); ++index)
				children[entities[index].Parent].push_back(index);
			std::vector<size_t> order;
			order.reserve(entities.size());
			std::vector<size_t> stack = { 0 };
			while (!stack.empty())
			{
				const size_t index = stack.back();
				stack.pop_back();
				order.push_back(index);
				if (const auto found = children.find(entities[index].ID); found != children.end())
				{
					for (auto child = found->second.rbegin(); child != found->second.rend(); ++child)
						stack.push_back(*child);
				}
			}
			if (order.size() != entities.size())
				return MakeError(ErrorCode::InvalidState, "the prefab of '{}' placed {} of its {} entities under no entity of the prefab", m_DisplayPath,
					entities.size() - order.size(), entities.size());

			// The entity objects of the canonical document (SceneSerializer.h): ID, Name, Parent, Active, Tags, then the
			// components in registry order with every field (StructInfo::ToJson). The root has an identity Transform.
			Json documentEntities = Json::array();
			bool hasRenderer = false;
			bool hasAnimator = false;
			bool hasSkinned = false;
			for (const size_t index : order)
			{
				const PrefabEntity& entity = entities[index];
				Json components = Json::object();
				for (const ComponentInfo* info : registry.GetComponents())
				{
					std::optional<Json> value;
					if (info == transformInfo)
					{
						ENGINE_TRY_ASSIGN(value, info->ToJson(&entity.Transform));
					}
					else if (info == rendererInfo && entity.Mesh.IsValid())
					{
						MeshRendererComponent renderer;
						renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(entity.Mesh);
						ENGINE_TRY_ASSIGN(value, info->ToJson(&renderer));
						hasRenderer = true;
					}
					else if (info == animatorInfo && entity.Animator.has_value())
					{
						ENGINE_TRY_ASSIGN(value, info->ToJson(&*entity.Animator));
						hasAnimator = true;
					}
					else if (info == skinnedInfo && entity.Skinned.has_value())
					{
						ENGINE_TRY_ASSIGN(value, info->ToJson(&*entity.Skinned));
						hasSkinned = true;
					}
					if (value.has_value())
						components[info->GetName()] = std::move(*value);
				}
				Json object = Json::object();
				object["ID"] = entity.ID.ToString();
				object["Name"] = entity.Name;
				object["Parent"] = entity.Parent.IsValid() ? Json(entity.Parent.ToString()) : Json(nullptr);
				object["Active"] = true;
				object["Tags"] = Json::array();
				object["Components"] = std::move(components);
				documentEntities.push_back(std::move(object));
			}

			Json versions = Json::object();
			for (const ComponentInfo* info : registry.GetComponents())
			{
				const bool used = info == transformInfo || (info == rendererInfo && hasRenderer) || (info == animatorInfo && hasAnimator)
					|| (info == skinnedInfo && hasSkinned);
				if (used)
					versions[info->GetName()] = info->GetVersion();
			}

			Json document = Json::object();
			document["Format"] = std::string(Prefab::FormatName);
			document["Version"] = Migrations::CurrentVersion;
			document["Name"] = name;
			document["Root"] = rootID.ToString();
			document["ComponentVersions"] = std::move(versions);
			document["Entities"] = std::move(documentEntities);
			return WithContext(CookDocument(AssetType::Prefab, document, GltfImporter::Version),
				std::format("while cooking the prefab of '{}'", m_DisplayPath));
		}

		AssetHandle GltfImport::AddSubAsset(std::string key, AssetType type, Buffer cooked)
		{
			const AssetHandle handle = DeriveSubAssetHandle(m_Metadata->Handle, key);
			ENGINE_CORE_ASSERT(std::ranges::none_of(m_SubAssets, [&key](const ImportedArtifact& artifact)
			{
				return artifact.SubAssetKey == key;
			}),
				"Sub-asset key '{}' is produced twice", key);
			m_SubAssets.push_back(ImportedArtifact{ .Handle = handle, .Type = type, .SubAssetKey = std::move(key), .Cooked = std::move(cooked) });
			return handle;
		}

		UUID GltfImport::MakeEntityID(std::string_view key) const
		{
			return UUID(Hash64(m_Metadata->Handle.GetValue(), key));
		}

		Result<ImportResult> GltfImport::Run()
		{
			Result<CgltfDataPointer> parsed = Utils::ParseGltf(m_Context->GetSourceBytes());
			if (!parsed)
				return std::unexpected(std::move(parsed).error().WithLocation(Utils::MakeFileLocation(m_DisplayPath)));
			m_Data = std::move(*parsed);

			ENGINE_TRY(CheckRequiredExtensions());
			ENGINE_TRY(LoadBuffers());
			ENGINE_TRY(CheckImageUris());
			ENGINE_TRY(CheckAccessors());
			const cgltf_result validation = cgltf_validate(m_Data.get());
			if (validation != cgltf_result_success)
				return std::unexpected(Fail(std::format("the file is not valid glTF 2.0: {}", Utils::DescribeCgltfResult(validation))));

			ReportSkippedContent();
			ENGINE_TRY(ImportMaterials());
			ENGINE_TRY(CollectSceneNodes());
			ENGINE_TRY(PlanSkins());
			ENGINE_TRY(ImportMeshes());
			ENGINE_TRY(ImportSkeletons());
			ENGINE_TRY(ImportAnimations());
			ENGINE_TRY_ASSIGN(Buffer prefab, BuildPrefab());

			ImportResult result;
			result.Artifacts.reserve(m_SubAssets.size() + 1);
			result.Artifacts.push_back(ImportedArtifact{ .Handle = m_Metadata->Handle, .Type = AssetType::Prefab, .SubAssetKey = {}, .Cooked = std::move(prefab) });
			std::vector<ImportedArtifact> subAssets = std::move(m_SubAssets);
			std::ranges::sort(subAssets, {}, &ImportedArtifact::SubAssetKey);
			for (ImportedArtifact& artifact : subAssets)
				result.Artifacts.push_back(std::move(artifact));
			result.Dependencies = std::move(m_Dependencies);
			std::ranges::sort(result.Dependencies);
			const auto [first, last] = std::ranges::unique(result.Dependencies);
			result.Dependencies.erase(first, last);
			result.Diagnostics = std::move(m_Diagnostics);
			return result;
		}

	}

	namespace Utils {

		static Result<GltfImportSettings> ReadSettings(const ImportContext& context)
		{
			const StructInfo* info = context.GetRegistry().FindStruct<GltfImportSettings>();
			if (info == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no 'GltfImportSettings' (GltfImporter::RegisterTypes was not called)");
			GltfImportSettings settings;
			const Json& json = context.GetSettings();
			if (!json.is_null())
				ENGINE_TRY(WithContext(info->FromJson(&settings, JsonReader(json), ReadContext{ .Schemas = nullptr, .Strict = true, .Diagnostics = nullptr }),
					"while reading the glTF import settings"));
			return settings;
		}

	}

	std::span<const std::string_view> GltfImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 2> Extensions = { ".gltf", ".glb" };
		return Extensions;
	}

	Result<ImportResult> GltfImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		ENGINE_TRY_ASSIGN(const GltfImportSettings settings, Utils::ReadSettings(context));
		GltfImport import(context, metadata, settings);
		return import.Run();
	}

	Result<std::vector<VfsPath>> GltfImporter::ListDependencyFiles(std::span<const std::byte> source, const VfsPath& sourcePath) const
	{
		ENGINE_TRY_ASSIGN(const std::vector<std::string> uris, ListExternalUris(source));
		std::vector<VfsPath> files;
		files.reserve(uris.size());
		for (const std::string& uri : uris)
		{
			Result<VfsPath> path = sourcePath.GetParent().Join(uri);
			if (!path)
			{
				return MakeError(ErrorCode::ImportFailed, "'{}' references the file '{}', whose path the engine does not accept: {}", sourcePath.ToString(),
					uri, path.error().GetMessageText());
			}
			files.push_back(std::move(*path));
		}
		std::ranges::sort(files);
		const auto [first, last] = std::ranges::unique(files);
		files.erase(first, last);
		return files;
	}

	Result<std::vector<std::string>> GltfImporter::ListExternalUris(std::span<const std::byte> source)
	{
		ENGINE_TRY_ASSIGN(const CgltfDataPointer data, Utils::ParseGltf(source));
		std::vector<std::string> uris;
		const auto add = [&uris](const char* text, std::string_view item) -> Status
		{
			const std::string uri = Utils::DecodeJsonString(text);
			if (text == nullptr || Utils::IsDataUri(uri))
				return {};
			ENGINE_TRY_ASSIGN(std::string decoded, Utils::DecodeRelativeUri(uri, item));
			if (std::ranges::find(uris, decoded) == uris.end())
				uris.push_back(std::move(decoded));
			return {};
		};
		for (cgltf_size i = 0; i < data->buffers_count; ++i)
			ENGINE_TRY(add(data->buffers[i].uri, std::format("buffers[{}]", i)));
		for (cgltf_size i = 0; i < data->images_count; ++i)
			ENGINE_TRY(add(data->images[i].uri, std::format("images[{}]", i)));
		return uris;
	}

	void GltfImporter::RegisterTypes(TypeRegistry& registry)
	{
		registry.Struct<GltfAnimationEvent>("GltfAnimationEvent", "A named event at a point of an imported animation clip's time (glTF has no animation events).")
			.Field("Animation", &GltfAnimationEvent::Animation, "The clip's sub-asset key, as the glTF's meta lists it (\"animation:0:Walk\").")
			.Field("Name", &GltfAnimationEvent::Name, "The event's name, which OnAnimationEvent receives.")
			.Field("Time", &GltfAnimationEvent::Time, "When the event fires, in seconds from the clip's start; at most the clip's duration.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxAnimationClipDuration), .Unit = "s" });

		registry.Struct<GltfImportSettings>("GltfImportSettings", "How a glTF 2.0 file (.gltf, .glb) is imported (Architecture §6.4, §7.4).")
			.Field("Scale", &GltfImportSettings::Scale,
				"Uniform scale baked into vertex positions, node translations and bounds (never into the prefab root's Transform).",
				{ .Min = 0.0001, .Max = 10000.0 })
			.Field("GenerateMissingTangents", &GltfImportSettings::GenerateMissingTangents,
				"Generates MikkTSpace tangents for primitives without TANGENT; off gives them an arbitrary perpendicular basis.")
			.Field("ImportMaterials", &GltfImportSettings::ImportMaterials,
				"Imports the glTF materials and their textures as sub-assets; off makes every slot use the Default material.")
			.Field("MergeMeshes", &GltfImportSettings::MergeMeshes,
				"Merges every mesh of the scene, placed by its node, into the one mesh sub-asset 'mesh:merged' drawn by the prefab root.")
			.Field("AnimationEvents", &GltfImportSettings::AnimationEvents,
				"Named events added to the imported animation clips, each to the clip it names; an event naming no clip of the file, or past its clip's end, "
				"fails the import.");
	}

}
