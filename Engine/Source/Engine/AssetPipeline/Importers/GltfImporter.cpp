#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/GltfImporter.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
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
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Migrations.h"
#include "Engine/Scene/Prefab.h"

#include <cgltf.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

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
		// vectors have one entry per vertex.
		struct PrimitiveAttributes
		{
			std::vector<glm::vec3> Positions{};
			std::vector<glm::vec3> Normals{};
			std::vector<glm::vec2> TexCoords{};
			std::vector<glm::vec4> Tangents{};
			std::vector<uint32_t> Indices{};
		};

		// One triangle primitive in the space of its node, ready to be placed into a mesh.
		struct PrimitiveGeometry
		{
			std::vector<MeshVertex> Vertices{};
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
			glm::mat4 World = glm::mat4(1.0f); // relative to the prefab root, without Scale (MergeMeshes)
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
			[[nodiscard]] Result<AssetHandle> FindStandaloneTexture(size_t image);
			[[nodiscard]] Result<Buffer> ReadImage(size_t image);

			[[nodiscard]] Result<SceneNode> MakeSceneNode(size_t node, UUID parent, const glm::mat4& parentWorld) const;
			[[nodiscard]] Status CollectSceneNodes();

			[[nodiscard]] Status ImportMeshes();
			[[nodiscard]] Result<PrimitiveGeometry> ReadPrimitive(size_t mesh, size_t primitive);
			[[nodiscard]] Result<std::vector<float>> ReadFloats(const cgltf_accessor& accessor, cgltf_type expected, std::string_view subject,
				std::string_view attribute) const;
			void AppendSubmesh(MeshData& mesh, PrimitiveGeometry geometry, const cgltf_primitive& primitive) const;
			[[nodiscard]] Result<AssetHandle> CookMeshSubAsset(MeshData mesh, std::string key);

			[[nodiscard]] Result<Buffer> BuildPrefab() const;

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
			std::vector<AssetHandle> m_MeshHandles; // per glTF mesh; null without a mesh sub-asset
			AssetHandle m_MergedMesh;               // MergeMeshes
		};

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
			const auto report = [this](std::string_view kind, cgltf_size count, std::string_view description)
			{
				for (cgltf_size i = 0; i < count; ++i)
				{
					std::string subject = std::format("{}[{}]", kind, i);
					std::string message = std::format("{} is {}, which the engine does not import", subject, description);
					AddWarning(AssetContentSkippedCode, std::move(subject), std::move(message), {});
				}
			};
			report("skins", m_Data->skins_count, "a skin");
			report("animations", m_Data->animations_count, "an animation");
			report("cameras", m_Data->cameras_count, "a camera");
			report("KHR_lights_punctual.lights", m_Data->lights_count, "a light");
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

			const cgltf_int texCoord = view.has_transform && view.transform.has_texcoord ? view.transform.texcoord : view.texcoord;
			if (texCoord != 0)
			{
				const std::string materialName = Utils::GetName(m_Data->materials[material].name, std::format("Material {}", material));
				AddWarning(AssetUnsupportedUvSetCode, std::format("materials[{}].{}", material, slot),
					std::format("materials[{}] '{}' binds its {} to TEXCOORD_{}; the vertex format has one UV set, so the texture is ignored", material,
						materialName, slot, texCoord),
					"bake the texture against TEXCOORD_0, or remove it from the material");
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
			TextureImportSettings settings;
			settings.GenerateMips = true;
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

		// An empty geometry (no vertices) for a primitive that is skipped with a diagnostic.
		Result<PrimitiveGeometry> GltfImport::ReadPrimitive(size_t meshIndex, size_t primitiveIndex)
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
			if (primitive.targets_count > 0)
			{
				AddWarning(AssetContentSkippedCode, subject + ".targets",
					std::format("{} has morph targets, which the engine does not import: the base shape is used", subject), {});
			}
			if (cgltf_find_accessor(&primitive, cgltf_attribute_type_color, 0) != nullptr)
			{
				AddWarning(AssetVertexColorsIgnoredCode, subject,
					std::format("{} has COLOR_0 vertex colours, which the engine ignores (the vertex format has no colour)", subject),
					"bake the vertex colours into a base colour texture");
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

			// Tangents: the file's (with its normals), else MikkTSpace, else a perpendicular basis.
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
				if (m_Settings.GenerateMissingTangents && texCoordAccessor != nullptr)
				{
					ENGINE_TRY_ASSIGN(const std::vector<glm::vec4> cornerTangents, WithContext(Utils::GenerateCornerTangents({
																								   .Positions = attributes.Positions,
																								   .Normals = attributes.Normals,
																								   .TexCoords = attributes.TexCoords,
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
			geometry.Indices = std::move(attributes.Indices);
			return geometry;
		}

		void GltfImport::AppendSubmesh(MeshData& mesh, PrimitiveGeometry geometry, const cgltf_primitive& primitive) const
		{
			const uint32_t baseVertex = static_cast<uint32_t>(mesh.Vertices.size());
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

		Status GltfImport::ImportMeshes()
		{
			m_MeshHandles.assign(m_Data->meshes_count, AssetHandle());
			const float scale = m_Settings.Scale;
			if (!m_Settings.MergeMeshes)
			{
				for (cgltf_size meshIndex = 0; meshIndex < m_Data->meshes_count; ++meshIndex)
				{
					const cgltf_mesh& source = m_Data->meshes[meshIndex];
					MeshData mesh;
					for (cgltf_size primitiveIndex = 0; primitiveIndex < source.primitives_count; ++primitiveIndex)
					{
						ENGINE_TRY_ASSIGN(PrimitiveGeometry geometry, ReadPrimitive(meshIndex, primitiveIndex));
						if (geometry.Vertices.empty())
							continue;
						// Scale multiplies positions only: it is uniform, so normals and tangents keep their exact values.
						for (MeshVertex& vertex : geometry.Vertices)
							vertex.Position *= scale;
						AppendSubmesh(mesh, std::move(geometry), source.primitives[primitiveIndex]);
					}
					if (mesh.Submeshes.empty())
						continue;
					ENGINE_TRY_ASSIGN(m_MeshHandles[meshIndex], CookMeshSubAsset(std::move(mesh), Utils::MakeKey("mesh", meshIndex, source.name)));
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
					ENGINE_TRY_ASSIGN(PrimitiveGeometry geometry, ReadPrimitive(meshIndex, primitiveIndex));
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

		Result<Buffer> GltfImport::BuildPrefab() const
		{
			const TypeRegistry& registry = m_Context->GetRegistry();
			const ComponentInfo* transformInfo = registry.FindComponent<TransformComponent>();
			const ComponentInfo* rendererInfo = registry.FindComponent<MeshRendererComponent>();
			if (transformInfo == nullptr || rendererInfo == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry lacks the built-in components Transform and MeshRenderer");

			// The entity objects of the canonical document (SceneSerializer.h): ID, Name, Parent, Active, Tags, then the
			// components in registry order with every field (StructInfo::ToJson). The root has an identity Transform.
			const auto makeEntity = [&registry, transformInfo, rendererInfo](UUID id, UUID parent, const std::string& name,
										const TransformComponent& transform, AssetHandle mesh) -> Result<Json>
			{
				Json components = Json::object();
				for (const ComponentInfo* info : registry.GetComponents())
				{
					if (info == transformInfo)
					{
						ENGINE_TRY_ASSIGN(Json value, info->ToJson(&transform));
						components[info->GetName()] = std::move(value);
					}
					else if (info == rendererInfo && mesh.IsValid())
					{
						MeshRendererComponent renderer;
						renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
						ENGINE_TRY_ASSIGN(Json value, info->ToJson(&renderer));
						components[info->GetName()] = std::move(value);
					}
				}
				Json entity = Json::object();
				entity["ID"] = id.ToString();
				entity["Name"] = name;
				entity["Parent"] = parent.IsValid() ? Json(parent.ToString()) : Json(nullptr);
				entity["Active"] = true;
				entity["Tags"] = Json::array();
				entity["Components"] = std::move(components);
				return entity;
			};

			const UUID rootID = MakeEntityID("root");
			const std::string name(m_Context->GetSourcePath().GetStem());
			Json entities = Json::array();
			bool hasRenderer = m_MergedMesh.IsValid();
			ENGINE_TRY_ASSIGN(Json root, makeEntity(rootID, UUID(), name, TransformComponent{}, m_MergedMesh));
			entities.push_back(std::move(root));
			for (const SceneNode& node : m_SceneNodes)
			{
				AssetHandle mesh;
				if (!m_Settings.MergeMeshes && m_Data->nodes[node.Node].mesh != nullptr)
					mesh = m_MeshHandles[cgltf_mesh_index(m_Data.get(), m_Data->nodes[node.Node].mesh)];
				hasRenderer = hasRenderer || mesh.IsValid();
				ENGINE_TRY_ASSIGN(Json entity, makeEntity(node.ID, node.Parent, node.Name, node.Transform, mesh));
				entities.push_back(std::move(entity));
			}

			Json versions = Json::object();
			for (const ComponentInfo* info : registry.GetComponents())
			{
				if (info == transformInfo || (info == rendererInfo && hasRenderer))
					versions[info->GetName()] = info->GetVersion();
			}

			Json document = Json::object();
			document["Format"] = std::string(Prefab::FormatName);
			document["Version"] = Migrations::CurrentVersion;
			document["Name"] = name;
			document["Root"] = rootID.ToString();
			document["ComponentVersions"] = std::move(versions);
			document["Entities"] = std::move(entities);
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
			ENGINE_TRY(ImportMeshes());
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
		registry.Struct<GltfImportSettings>("GltfImportSettings", "How a glTF 2.0 file (.gltf, .glb) is imported (Architecture §6.4, §7.4).")
			.Field("Scale", &GltfImportSettings::Scale,
				"Uniform scale baked into vertex positions, node translations and bounds (never into the prefab root's Transform).",
				{ .Min = 0.0001, .Max = 10000.0 })
			.Field("GenerateMissingTangents", &GltfImportSettings::GenerateMissingTangents,
				"Generates MikkTSpace tangents for primitives without TANGENT; off gives them an arbitrary perpendicular basis.")
			.Field("ImportMaterials", &GltfImportSettings::ImportMaterials,
				"Imports the glTF materials and their textures as sub-assets; off makes every slot use the Default material.")
			.Field("MergeMeshes", &GltfImportSettings::MergeMeshes,
				"Merges every mesh of the scene, placed by its node, into the one mesh sub-asset 'mesh:merged' drawn by the prefab root.");
	}

}
