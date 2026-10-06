#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class IFieldSchemaSource;
	class UUIDGenerator;

	// The two documents that share the entity schema (Architecture §6.2, §6.3).
	enum class DocumentKind : uint8_t
	{
		Scene,
		Prefab
	};

	// How a scene or prefab loader treats structural defects (Architecture §6 "Structural pre-validation").
	enum class LoadMode : uint8_t
	{
		Strict, // any structural defect fails the load (play copy, Runtime, export, importers)
		Repair  // deterministic fixes, each reported (scene.open {repair: true}, project.validate {fix})
	};

	struct LoadOptions
	{
		LoadMode Mode = LoadMode::Strict;
		// --strict (CI): unknown keys, fields and components, and unresolvable or mismatching Variant values, are errors
		// instead of warnings (§6 "Strict reader").
		bool StrictUnknowns = false;
		// Script field schemas for ScriptComponent.Fields (M13); may be null (values are then kept with a warning).
		const IFieldSchemaSource* Schemas = nullptr;
		// The generator that supplies fresh IDs for repairs (duplicate or invalid IDs). Required in Repair mode (asserted);
		// fixes are deterministic for a given generator state.
		UUIDGenerator* RepairIdGenerator = nullptr;
		// The document's path for ErrorLocation::File and diagnostics ("Assets/Scenes/Level1.scene"); may be empty.
		std::string SourcePath;
	};

	// A warning or error found while loading, located by JSON pointer within the document (after migration) and, when it
	// concerns one entity, by that entity's ID as written in the file.
	struct LoadDiagnostic
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		std::string Code; // one of the codes below, or a REFLECTION_* code from StructInfo reads
		std::string Message;
		std::string JsonPointer;
		UUID Entity;
	};

	// One deterministic fix applied by a Repair load (§6), with what it removed when it dropped data.
	struct LoadRepair
	{
		std::string Code;        // the code of the defect it fixes
		std::string Description; // "gave the later duplicate of 5d1c9a7e33b04f12 the new ID 77e1a0c4d2b95f01"
		std::string JsonPointer;
		UUID Entity;
		// The dropped or reset component's JSON (extra unique, misplaced and invalid components); null otherwise.
		VariantValue Removed;
	};

	// Everything a load reports besides its result: the file's original version, whether migrations ran, warnings and
	// errors, and the repairs applied. Diagnostics and repairs are in document order.
	struct LoadReport
	{
		uint32_t FileVersion = 0;
		bool Migrated = false;
		std::vector<LoadDiagnostic> Diagnostics;
		std::vector<LoadRepair> Repairs;
	};

	// Diagnostic codes of scene and prefab loading (LoadDiagnostic::Code, LoadRepair::Code). Stable identifiers: automation
	// and the validator report them (§13.3).

	// Repair: the later duplicate gets a fresh ID.
	inline constexpr std::string_view SceneDuplicateIdCode = "SCENE_DUPLICATE_ID";
	// A missing, malformed or zero ID. Repair: a fresh ID.
	inline constexpr std::string_view SceneInvalidIdCode = "SCENE_INVALID_ID";
	// Repair: the orphan is reparented to the root.
	inline constexpr std::string_view SceneDanglingParentCode = "SCENE_DANGLING_PARENT";
	// Repair: the cycle is cut at its first back-edge in file order.
	inline constexpr std::string_view SceneParentCycleCode = "SCENE_PARENT_CYCLE";
	// Warning in both modes: a child listed before its parent, normalized to canonical order.
	inline constexpr std::string_view SceneNonCanonicalOrderCode = "SCENE_NONCANONICAL_ORDER";
	// Repair: the member is unpacked (its PrefabLink removed).
	inline constexpr std::string_view SceneInconsistentPrefabLinkCode = "SCENE_INCONSISTENT_PREFAB_LINK";
	// Repair: every extra after the first in canonical order is dropped.
	inline constexpr std::string_view SceneDuplicateUniqueComponentCode = "SCENE_DUPLICATE_UNIQUE_COMPONENT";
	// A registered component that is entity-level or not Serializable under "Components" ("Name" put there by a hand
	// edit or merge). Repair: dropped. Never added to the entity.
	inline constexpr std::string_view SceneMisplacedComponentCode = "SCENE_MISPLACED_COMPONENT";
	// A known component whose value the registry rejects (wrong JSON type, out of range, non-finite, failed validator).
	// Repair: dropped, or reset to its defaults when it is Required.
	inline constexpr std::string_view SceneInvalidComponentCode = "SCENE_INVALID_COMPONENT";
	// Warning: an unknown component, preserved verbatim.
	inline constexpr std::string_view SceneUnknownComponentCode = "SCENE_UNKNOWN_COMPONENT";
	// Warning: an unknown entity or document key, dropped.
	inline constexpr std::string_view SceneUnknownKeyCode = "SCENE_UNKNOWN_KEY";
	// "Root" missing, naming no entity, or not the only root. Not repairable.
	inline constexpr std::string_view PrefabInvalidRootCode = "PREFAB_INVALID_ROOT";

}
