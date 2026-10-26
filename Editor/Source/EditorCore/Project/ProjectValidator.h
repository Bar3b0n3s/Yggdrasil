#pragma once

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Renderer/RenderPrepare.h"
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
// these codes through MapLoadCode (ADR 0006 decision 35). M6 adds the asset codes of Engine/Asset/AssetDiagnostic.h, under
// the same constants (every one but AssetUploadFailedCode, a runtime-only code): the registry's scan diagnostics with
// their fixes (AssetRegistry::PlanFix applied as AssetEditCommand or AssetMoveCommand inside the fix transaction), the
// asset manager's import diagnostics, ASSET_MISSING and ASSET_TYPE_MISMATCH for asset references in scene components,
// materials and prefabs, PATH_CASE_MISMATCH, and PREFAB_MISSING_ASSET for an instance whose prefab asset is not registered.
// M8 adds RENDER_LIGHT_LIMIT_EXCEEDED. M11 adds the physics codes of Engine/Physics/PhysicsDiagnostics.h:
// Scene/PhysicsValidation.h's ValidateScenePhysics runs on the open scene and on every scratch scene, with the layer table
// of the project's PhysicsSettings, each diagnostic keeping its entity, component, field and subject (so its id);
// PHYSICS_ADJACENT_STATIC_BODIES of the open scene is fixed by adding a Static RigidBody to the two bodies' nearest common
// ancestor (PhysicsDiagnostic::FixTarget), inside the one undoable fix command. M12 adds AUDIO_NO_LISTENER and
// AUDIO_MULTIPLE_PRIMARY_LISTENERS (Scene/AudioSystem.h's FindAudioSceneIssues); the extra primary listeners of the open
// scene are fixed by clearing their Primary. M9 adds RENDER_NO_LIGHTING (Scene/RenderAnnotations.h) and
// RENDER_SPOT_SHADOW_BUDGET (Renderer/SpotShadowAtlas.h). M13 adds the script codes (SCRIPT_COMPILE_ERROR,
// SCRIPT_TYPE_ERROR, SCRIPT_NOT_A_BEHAVIOUR, SCRIPT_UNKNOWN_FIELD_OVERRIDE, SCRIPT_FIELD_TYPE_MISMATCH),
// INPUT_UNKNOWN_ACTION and TEST_SUITE_INVALID. M19 adds SCRIPT_COMPUTED_ASSET_LOAD (Engine/Scripting/ScriptAssetLoads.h:
// with Export.PruneUnreferenced on, each computed Assets.Load of a script below Assets/ that Export.Exclude leaves in, a
// testing export's excluded test content unchecked; Docs/Decisions/0030-m19-contract.md decision 21). M20 adds
// PREFAB_CYCLE (Scene/LoadReport.h) on every prefab file that nests itself, directly or through other prefabs
// (Docs/Decisions/0031-m20-contract.md decision 8). GetCodes lists every code in §13.7 order, later milestones' codes
// appended in landing order.

namespace Engine {

	class EditorContext;
	class TypeRegistry;
	class VfsPath;

	// The documentation of one validation code (M14: Docs/Reference/Validation.md, the game-debugging skill's error-code
	// table and gate 6 read it; Docs/Decisions/0022-m14-contract.md decision 10). Static text, valid for the process.
	struct ValidationCodeInfo
	{
		std::string_view Code{};        // "SCENE_NO_PRIMARY_CAMERA"
		std::string_view Severity{};    // "Error", "Warning", "Warning; Error at the limit" or "Error; Warning for a Luau lint warning" (§13.7)
		bool AutoFixable = false;       // project.validate {fix} can fix at least some of its diagnostics
		std::string_view Description{}; // one sentence: what the code reports
		std::string_view Fix{};         // one sentence: how an author or agent fixes it
	};

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
	// (A scene file under Assets/ that cannot be loaded at all is an Error, not fixable, reported under AssetImportFailedCode
	// of Engine/Asset/AssetDiagnostic.h: scenes are assets, and that is the code M6's importers report for every asset
	// type.)

	// Warning, not fixable (M6): a prefab instance root whose PrefabInstanceComponent names a prefab asset that is not
	// registered (§5.5: the scene still loads, fully expanded).
	inline constexpr std::string_view PrefabMissingAssetCode = "PREFAB_MISSING_ASSET";

	// (Warning, not fixable, M8, §13.7: "more than 256 visible lights": a scene whose effectively enabled DirectionalLight,
	// PointLight and SpotLight components number more than MaxVisibleLights is reported under RenderLightLimitExceededCode
	// of Engine/Renderer/RenderPrepare.h, which the renderer logs too. The validator cannot know every view, so it counts
	// the scene's lights; the renderer culls per view and logs the code once per renderer when a view really drops lights,
	// the least important ones (RenderPrepare.h's CullLights). Reported on the scene file, without an entity.)

	// Registry enum "ValidationScope" (project.validate {scope}).
	enum class ValidationScope : uint8_t
	{
		Project, // the settings, every scene file under Assets/ (each open scene's in-memory state instead of its file)
		Scene    // the addressed open scene only (EditorContext.h)
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

		// Validates `scope`. Scene files are loaded in Repair mode into scratch scenes (the file of every open scene is
		// replaced by its in-memory state; only the addressed scene's diagnostics are auto-fixable, since the fixes edit it
		// alone: Docs/Decisions/0031-m20-contract.md decision 16), their load diagnostics and repairs mapped through
		// MapLoadCode, and the scene checks run on each. A scene file that cannot be loaded at all (Parse, UnsupportedVersion, an unrepairable structural defect) is
		// an Error under AssetImportFailedCode with the load error as message. Errors: InvalidState without an open project,
		// or for ValidationScope::Scene without an open scene; InvalidArgument when nonfinite CPU asset or environment
		// data prevents render illumination classification. Render lighting and spot-shadow warnings never have fixes.
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

		// M14: one entry per code of GetCodes, in the same order, each with a non-empty severity, description and fix.
		[[nodiscard]] static std::span<const ValidationCodeInfo> GetCodeInfos();

		// The validator code of a scene-load diagnostic code (ADR 0006 decision 35); empty for load codes the validator does
		// not report (unknown keys and components, which stay load warnings).
		[[nodiscard]] static std::string_view MapLoadCode(std::string_view loadCode);

		// The validator code ADR 0006 decision 35 gives a load diagnostic of a Script slot's Fields value (located below
		// "/Components/Script/Scripts/<slot>/Fields/" of an entity): SCRIPT_UNKNOWN_FIELD_OVERRIDE for
		// REFLECTION_VARIANT_UNRESOLVED (no declared field checks the value), SCRIPT_FIELD_TYPE_MISMATCH for
		// REFLECTION_VARIANT_MISMATCH; empty for any other. Validate reports these problems from every slot of the Script
		// components of each scene, a prefab instance's members included, and of each prefab file (with the project scope;
		// found through FindScriptFieldProblems) under the same codes (file, entity, component "Script", field
		// "Scripts[<slot>].Fields.<name>"); the exporter and the strict open name the same code for the same
		// entity and field, by JSON pointer, so the three agree (ADR 0026). The mapping reads only the code and the pointer:
		// which values validate checks at all is FindScriptFieldProblems' filter.
		[[nodiscard]] static std::string_view MapScriptFieldLoadCode(std::string_view loadCode, std::string_view jsonPointer);

		// The Script component Fields problems of the scene or prefab document `path` (project://), each as its load diagnostic
		// with the code MapScriptFieldLoadCode gives it, in document order: the document read as the strict reader reads it
		// (LoadMode::Strict with the editor's script field schemas) but without --strict's promotion, so a document that
		// --strict rejects for these values still names each of them. Only the values of an entity whose script is a
		// registered Script asset that loads as a Behaviour are named, the values validate checks: a missing or mistyped
		// script, one that fails to import and a Module leave every value unresolved, and validate reports the script
		// (ASSET_MISSING, the script's source diagnostics, SCRIPT_NOT_A_BEHAVIOUR) instead of the values (M16 review R24).
		// Empty for a document that does not load at all (its load error says why). The strict open and the exporter's strict
		// load use it to name what project.validate names.
		// Errors: InvalidState without an open project; those of EditorContext::GetScriptSchemaSnapshot.
		[[nodiscard]] static Result<std::vector<LoadDiagnostic>> FindScriptFieldProblems(const EditorContext& context, const VfsPath& path);

		// M15 (Docs/Decisions/0024-m15-contract.md decision 6): the TEST_SUITE_INVALID diagnostics of the open project's
		// Testing.Suites, the one rule Validate and the exporter's testing validation (TestingExport.h) share. Entry i is
		// invalid when its Script is not a registered Script asset whose kind is TestSuite (Field "Testing.Suites[i]",
		// subject "<i>"), or when its non-empty Scene is not a registered Scene asset (subject "<i>/Scene"; M15 tightens the
		// file-exists rule of M13, since a testing export packages the scene). Sorted by entry, the script's before the
		// scene's. Errors: InvalidState without an open project.
		[[nodiscard]] static Result<std::vector<ProjectDiagnostic>> CheckTestSuites(const EditorContext& context);
	};

	// Registers ValidationScope and ProjectDiagnostic (DiagnosticSeverity comes from RegisterAutomationCommonTypes).
	void RegisterProjectValidatorTypes(TypeRegistry& registry);

}
