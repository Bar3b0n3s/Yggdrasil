#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Reflection/ValidationContext.h"

#include <span>
#include <string>
#include <string_view>

// Asset diagnostics (Architecture §7.2 failure policy, §7.3 scan diagnostics, §7.4 importer warnings): recorded by the
// asset managers, shown in the editor, reported through the log (so "_meta" counts them, §13.4) and by project.validate
// (§13.7). Export refuses to run while any diagnostic has severity Error (§7.2).

namespace Engine {

	// The asset codes (§13.7 spelling). The project validator reports every one of them except AssetUploadFailedCode under
	// the same code (EditorCore/Project/ProjectValidator.h).
	//
	// Error: a reference names a handle no registry knows (a component field, a material's texture slot, a prefab's mesh),
	// or a load asked for one. The caller gets the type's placeholder (§7.2). Recorded by AssetManager::GetOrPlaceholder as
	// a reference diagnostic (lifetime in AssetManager.h) and reported from the current references by project.validate.
	inline constexpr std::string_view AssetMissingCode = "ASSET_MISSING";
	// Error: a reference names an asset of another type than the field accepts (a reference diagnostic, like
	// AssetMissingCode), or a .meta's Type or Importer disagrees with the importer its source's extension selects (§7.3
	// "type/importer mismatch", a scan diagnostic).
	inline constexpr std::string_view AssetTypeMismatchCode = "ASSET_TYPE_MISMATCH";
	// Error: importing, cooking or loading an asset failed, or its .meta cannot be read. The last good version stays in use
	// after a failed hot reimport (§7.5); a first import failure yields the placeholder.
	inline constexpr std::string_view AssetImportFailedCode = "ASSET_IMPORT_FAILED";
	// Warning, auto-fixable (the .meta goes to the trash): a .meta whose source file does not exist (§7.3).
	inline constexpr std::string_view AssetOrphanMetaCode = "ASSET_ORPHAN_META";
	// Error, auto-fixable (every .meta but the keeper gets a fresh handle; the keeper rule of AssetRegistry::Scan keeps the
	// handle with the original file, never with a file manager's copy): several .meta files whose sources exist carry the
	// same handle, typically a copy-pasted pair (§7.3).
	inline constexpr std::string_view AssetDuplicateHandleCode = "ASSET_DUPLICATE_HANDLE";
	// Warning, auto-fixable (the dependency meta goes to the trash): a dependency meta (§6.4) whose Owner is not registered
	// (§7.3 "a dependency whose owner is missing").
	inline constexpr std::string_view AssetOrphanDependencyCode = "ASSET_ORPHAN_DEPENDENCY";
	// Warning: a glTF primitive needs generated tangents but has no TEXCOORD_0, so MikkTSpace cannot run and the tangents
	// are an arbitrary basis perpendicular to the normal (Appendix C; MikkTSpace is vendored, ADR 0001).
	inline constexpr std::string_view AssetTangentsApproximatedCode = "ASSET_TANGENTS_APPROXIMATED";
	// Warning: a glTF texture is bound to TEXCOORD_1 (an occlusion map, typically) and is ignored: the vertex format has UV0
	// only (§6.8, §7.4).
	inline constexpr std::string_view AssetUnsupportedUvSetCode = "ASSET_UNSUPPORTED_UV_SET";
	// Warning: a glTF primitive has COLOR_0, which is ignored: the vertex format has no colour (§7.4).
	inline constexpr std::string_view AssetVertexColorsIgnoredCode = "ASSET_VERTEX_COLORS_IGNORED";
	// Warning: glTF content the engine does not import was skipped (§1.2, §7.4): a points or lines primitive, a skin, an
	// animation, a camera or a light. Subject names the item ("meshes[2].primitives[0]", "animations[0]"). Recorded in
	// ImportResult::Diagnostics, so it persists in the cache manifest and reappears on cache hits.
	inline constexpr std::string_view AssetContentSkippedCode = "ASSET_CONTENT_SKIPPED";
	// Error, auto-fixable (a case-only rename of the .meta to its source's spelling): a .meta whose name matches a source
	// file only when letter case is ignored (§4.10 case policy, §7.3 "case mismatch"). A reference whose path matches an
	// asset only ignoring case resolves to nothing and is reported under this code by the validator.
	inline constexpr std::string_view PathCaseMismatchCode = "PATH_CASE_MISMATCH";
	// Error, runtime only (never a project.validate code): GpuResourceCache could not create an asset's GPU mirror (device
	// out of memory, --gpu-inject-fault=oom-texture) and uses the placeholder's mirror instead (§8.14 item 7).
	inline constexpr std::string_view AssetUploadFailedCode = "ASSET_UPLOAD_FAILED";

	// One asset diagnostic. Plain value; thread-compatible.
	struct AssetDiagnostic
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		std::string Code{};    // one of the codes above
		AssetHandle Asset{};   // the asset concerned; null when there is none (a .meta that cannot be read)
		std::string Path{};    // the file concerned: project-relative ("Assets/Models/Track.glb", or its .meta) or an engine path
		std::string Message{}; // "duplicate handle 3c9f2e7a11d04b88 (also in Assets/Copy.png.meta)"
		std::string Hint{};    // empty when there is none
		// Tells apart diagnostics of one code at one path without depending on positions (the other path of a duplicate
		// handle, the offending URI); empty when the path is unique. The validator's diagnostic id uses it.
		std::string Subject{};
		bool AutoFixable = false;

		bool operator==(const AssetDiagnostic&) const = default;
	};

	// The asset codes, in §13.7 order (AssetContentSkippedCode after AssetVertexColorsIgnoredCode, ADR 0010 decision 5)
	// followed by AssetUploadFailedCode.
	[[nodiscard]] std::span<const std::string_view> GetAssetDiagnosticCodes();

	// One line for logs and the console: "<Code> <Path>: <Message>[ (hint: <Hint>)]".
	[[nodiscard]] std::string AssetDiagnosticToString(const AssetDiagnostic& diagnostic);

}
