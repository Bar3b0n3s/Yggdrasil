#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/LoadReport.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The project validator (Architecture §13.7 "project.validate"): diagnostics with stable ids and selective, undoable
// fixes. The editor's DiagnosticsPanel (M10) and automation use the same functions.
//
// Codes. §13.7 lists every code of the finished engine; each milestone adds the codes its subsystems can detect. The M4
// validator reports the scene, entity, component and build codes below. Load diagnostics of scene files are reported under
// these codes through MapLoadCode (ADR 0006 decision 35); asset, physics, script, input, audio, render and test codes
// arrive with M6, M11, M13, M12, M8/M9 and M13.

namespace Engine {

	class EditorContext;
	class TypeRegistry;

	// The codes the M4 validator reports (§13.7 spelling), with their severity and whether a fix exists.
	// Warning, fixable when the scene has exactly one camera (it becomes Primary).
	inline constexpr std::string_view SceneNoPrimaryCameraCode = "SCENE_NO_PRIMARY_CAMERA";
	// Error, fixable: every Primary camera after the first in canonical order is cleared.
	inline constexpr std::string_view SceneMultiplePrimaryCamerasCode = "SCENE_MULTIPLE_PRIMARY_CAMERAS";
	// The structural codes, found in scene files (the open scene cannot hold them: every write path rejects them). Not
	// fixable by project.validate in M4; the hint names scene.open {repair: true} followed by scene.save. Three of them are
	// spelled like their load codes and reuse those constants (Scene/LoadReport.h, ADR 0006 decision 35):
	// SceneDuplicateUniqueComponentCode, SceneNonCanonicalOrderCode (a warning) and SceneInconsistentPrefabLinkCode.
	inline constexpr std::string_view SceneInvalidHierarchyCode = "SCENE_INVALID_HIERARCHY";
	inline constexpr std::string_view EntityDuplicateIdCode = "ENTITY_DUPLICATE_ID";
	// Warning, fixable: an EntityRef field naming no entity of the scene is set to null.
	inline constexpr std::string_view EntityDanglingReferenceCode = "ENTITY_DANGLING_REFERENCE";
	// Errors found in scene files (in-memory values are validated on every write); not fixable in M4 (repair loading).
	inline constexpr std::string_view ComponentFieldOutOfRangeCode = "COMPONENT_FIELD_OUT_OF_RANGE";
	inline constexpr std::string_view ComponentMissingRequirementCode = "COMPONENT_MISSING_REQUIREMENT";
	inline constexpr std::string_view ComponentConflictCode = "COMPONENT_CONFLICT";
	// Error, not fixable: StartScene names no existing scene file (empty StartScene is fine until export).
	inline constexpr std::string_view BuildStartSceneMissingCode = "BUILD_START_SCENE_MISSING";
	// Error, fixable: the Export.BuildScenes entry is removed (a ProjectSettingsCommand).
	inline constexpr std::string_view BuildSceneMissingCode = "BUILD_SCENE_MISSING";
	// Error, not fixable: a scene file under Assets/ cannot be loaded at all. Scenes are assets, and M6's importers report
	// the same code for every asset type.
	inline constexpr std::string_view AssetImportFailedCode = "ASSET_IMPORT_FAILED";

	// Registry enum "ValidationScope" (project.validate {scope}).
	enum class ValidationScope : uint8_t
	{
		Project, // the settings, every scene file under Assets/ (the open scene's in-memory state instead of its file)
		Scene    // the open scene only
	};

	// Registry struct "ProjectDiagnostic" (§13.7: {id, severity, code, message, entity?, component?, field?, asset?, file?,
	// line?, hint, autoFixable}). Optional members are "" (or 0 for line) when absent. JSON keys as the conventions of
	// MethodRegistry.h.
	struct ProjectDiagnostic
	{
		// Stable and unique within a report: MakeDiagnosticId of the code, the location and the subject, so the same problem
		// keeps its id across runs, edits elsewhere and the order in which files are scanned, and fixing one problem never
		// changes another's id (fix: [ids] relies on both).
		std::string Id{};
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		std::string Code{};
		std::string Message{};
		std::string Entity{};    // 16 hex digits
		std::string Component{}; // registry name
		std::string Field{};
		std::string Asset{}; // asset handle (M6)
		std::string File{};  // project-relative path
		uint32_t Line = 0;   // 1-based, for text files (scripts, M13)
		std::string Hint{};
		bool AutoFixable = false;
	};

	struct ValidationReport
	{
		std::vector<ProjectDiagnostic> Diagnostics{}; // sorted by (file, entity, code, component, field, id); ids are unique
		uint32_t ErrorCount = 0;
		uint32_t WarningCount = 0;
	};

	// Which fixes to apply (project.validate {fix}).
	struct FixSelection
	{
		bool All = false;                      // fix: true, every auto-fixable diagnostic
		std::vector<std::string> IdsOrCodes{}; // fix: [...]: diagnostic ids and/or codes
	};

	struct FixReport
	{
		std::vector<std::string> Fixed{}; // ids of the diagnostics fixed, in report order
		uint64_t UndoIndex = 0;           // the one undoable command holding every fix (0 when nothing was fixed or dry run)
		ValidationReport After{};         // the report after the fixes
	};

	// Static functions only; main thread (they read the open scene).
	class ProjectValidator
	{
	public:
		ProjectValidator() = delete;

		// Validates `scope`. Scene files are loaded in Repair mode into scratch scenes (the open scene's file is replaced by
		// its in-memory state), their load diagnostics and repairs mapped through MapLoadCode, and the scene checks run on
		// each. A scene file that cannot be loaded at all (Parse, UnsupportedVersion, an unrepairable structural defect) is
		// an Error under AssetImportFailedCode with the load error as message. Errors: InvalidState without an open project,
		// or for ValidationScope::Scene without an open scene.
		[[nodiscard]] static Result<ValidationReport> Validate(const EditorContext& context, ValidationScope scope);

		// Validates, then fixes the selected auto-fixable diagnostics as one undoable command (an EditorTransaction holding a
		// SceneEditCommand and/or a ProjectSettingsCommand, §13.7 "either way the fixes form one undoable command"), and
		// reports what was fixed and the report afterwards. Inside an open transaction (an op project.validate {fix} of
		// edit.batch) the fixes join it and become part of its one undo step, FixReport::UndoIndex then 0. A selected id that
		// matches no diagnostic, or a code that this build does not know, is InvalidArgument; selected diagnostics that are
		// not auto-fixable are left alone (and stay in the report). Errors: as Validate; InvalidArgument; the commands'
		// errors (nothing changed then).
		[[nodiscard]] static Result<FixReport> Fix(EditorContext& context, ValidationScope scope, const FixSelection& selection);

		// The stable id of a diagnostic: the code, '-', and the first 12 hex digits of XXH64 over
		// "<code>|<file>|<entity>|<component>|<field>|<subject>" ("SCENE_MULTIPLE_PRIMARY_CAMERAS-3f2a9c1b0d4e"). `subject`
		// tells apart problems of one code at one location, and never depends on positions that a fix elsewhere shifts: the
		// missing scene path for BUILD_SCENE_MISSING, the dangling UUID (16 hex digits) or the map key for
		// ENTITY_DANGLING_REFERENCE in an array or map field; empty when the location is unique. The validator asserts that
		// a report's ids are unique.
		[[nodiscard]] static std::string MakeDiagnosticId(std::string_view code, std::string_view file, std::string_view entity,
			std::string_view component, std::string_view field, std::string_view subject = {});

		// The codes this build reports, in §13.7 order.
		[[nodiscard]] static std::span<const std::string_view> GetCodes();

		// The validator code of a scene-load diagnostic code (ADR 0006 decision 35); empty for load codes the validator does
		// not report (unknown keys and components, which stay load warnings).
		[[nodiscard]] static std::string_view MapLoadCode(std::string_view loadCode);
	};

	// Registers ValidationScope and ProjectDiagnostic (DiagnosticSeverity comes from RegisterAutomationCommonTypes).
	void RegisterProjectValidatorTypes(TypeRegistry& registry);

}
