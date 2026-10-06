#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/LoadReport.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	class ConstEntity;
	class Entity;
	class Scene;
	class VirtualFileSystem;

	// Scene files (Architecture §6, §6.2) and the one scene-copy path (§1.3: the play copy, the exported Runtime and undo all
	// go through it).
	//
	// Document layout, written by the canonical JsonWriter (UTF-8, LF, tabs):
	//     { "Format": "Scene", "Version": 1, "Name": <string>, "Seed": <uint32>,
	//       "ComponentVersions": { <registry name>: <version>, ... },
	//       "Entities": [ { "ID": <hex>, "Name": <string>, "Parent": <hex or null>, "Active": <bool>, "Tags": [<string>...],
	//                       "Components": { <registry name>: { <fields> }, ... } }, ... ] }
	// Entities in canonical order (§5.1). "Components" holds every Serializable, non-EntityLevel component in registry
	// order, every serialized field written with default values included, then the entity's unknown components verbatim
	// (UnknownComponentsComponent). "ComponentVersions" lists every component type present in the document, known ones in
	// registry order with their registered versions, then unknown ones as read. Floats in shortest round-trip form; UUIDs
	// as 16-digit hex, null for none; enums by name; Map keys sorted (§5.4, §6).
	//
	// Loading: header check (Format, Version 0..1; newer is UnsupportedVersion naming both versions), migration
	// (Migrations), structural pre-validation and repair (StructuralValidator, which also removes misplaced entity-level
	// components), then entity creation in canonical order: each component read with StructInfo::FromJson (unknown fields
	// warn; values validated; Variant values resolved or preserved), unknown components preserved with
	// SCENE_UNKNOWN_COMPONENT, unknown keys dropped with SCENE_UNKNOWN_KEY, Children rebuilt from file order. With
	// LoadOptions::StrictUnknowns those warnings are errors. A known component whose value the registry rejects (wrong JSON
	// type, out of range, non-finite, failed validator) fails a Strict load with a located Validation error; a Repair load
	// drops it instead, or resets it to its defaults when it is Required (Transform), records SCENE_INVALID_COMPONENT as a
	// warning and a LoadRepair whose Removed holds the rejected JSON, and continues; when that drops an instance root's
	// Prefab component, the instance's members are unpacked with SCENE_INCONSISTENT_PREFAB_LINK, so a repaired scene always
	// loads strictly again. A failed load leaves the target scene empty. Load then save is byte-identical for every
	// canonical file (§6), and serializing a copy gives the same bytes and state hash.
	//
	// Static functions only. Scene access is main-thread (§4.11); the JSON functions are otherwise pure.
	class SceneSerializer
	{
	public:
		static constexpr std::string_view FormatName = "Scene";

		// The canonical document of `scene`. Errors: Validation for a value that cannot be written (a non-finite float set
		// directly by a runtime system), located at its pointer.
		[[nodiscard]] static Result<Json> ToJson(const Scene& scene);

		// ToJson written by JsonWriter in `style` (Pretty for files, Minified for cooked payloads and the state hash).
		[[nodiscard]] static Result<std::string> SaveToString(const Scene& scene, JsonStyle style = JsonStyle::Pretty);

		// Loads `document` into `scene`, which must be empty (asserted); the scene's name and seed are set from the
		// document. `report` receives the file version, migrations, diagnostics and repairs. Errors: Validation (located,
		// with issues) for a malformed header or document, and in Strict mode for an invalid component value or a
		// structural defect (an unrepairable one in Repair mode too); UnsupportedVersion; on error the scene is left
		// empty.
		[[nodiscard]] static Status FromJson(Scene& scene, const Json& document, const LoadOptions& options, LoadReport& report);

		// JsonReader::Parse then FromJson. Errors: Parse for invalid JSON (with line and column), and as FromJson.
		[[nodiscard]] static Status LoadFromString(Scene& scene, std::string_view text, const LoadOptions& options, LoadReport& report);

		// SaveToString(Pretty) written atomically to `path` through `vfs` (§4.10). Errors: as SaveToString, plus the VFS
		// write errors.
		[[nodiscard]] static Status SaveToFile(const Scene& scene, VirtualFileSystem& vfs, const VfsPath& path);

		// Reads `path` through `vfs` (UTF-8 validated) and LoadFromString; options.SourcePath defaults to the path's text.
		// Errors: the VFS read errors (NotFound, Io, Validation for a case mismatch), and as LoadFromString.
		[[nodiscard]] static Status LoadFromFile(Scene& scene, const VirtualFileSystem& vfs, const VfsPath& path, const LoadOptions& options,
			LoadReport& report);

		// The canonical JSON of one entity, exactly as it appears in a scene document's "Entities" (undo snapshots and the
		// change tracker, §12.3; prefab creation; the clipboard). Read-only: it takes a ConstEntity, so a const Scene's
		// handles work and an Entity converts. Errors: as ToJson.
		[[nodiscard]] static Result<Json> EntityToJson(ConstEntity entity);

		// Creates one entity from its canonical JSON (the inverse of EntityToJson) under the entity its "Parent" names (which
		// must exist in `scene`, or be null) at `siblingIndex` (the end when absent). The ID must be valid and unused in the
		// scene. Components are read as in FromJson. Errors: Validation (located, with issues) for an invalid entity object,
		// a used or invalid ID, or a missing parent; nothing is created then.
		[[nodiscard]] static Result<Entity> EntityFromJson(Scene& scene, const JsonReader& entity, std::optional<uint32_t> siblingIndex,
			const LoadOptions& options, LoadReport& report);

		// Replaces the name, active state, tags and components of an existing entity with those of its canonical JSON
		// (undo and redo of a modification, §12.3): components absent from `json` are removed (Required ones excepted),
		// present ones added or overwritten. The ID and parent in `json` must match the entity's (asserted; hierarchy moves
		// go through Scene::SetParent). Atomic: everything is validated first. Errors: Validation (located).
		[[nodiscard]] static Status ApplyEntityJson(Entity entity, const JsonReader& json, const LoadOptions& options, LoadReport& report);
	};

}
