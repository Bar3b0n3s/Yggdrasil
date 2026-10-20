#include "EditorPCH.h"
#include "EditorCore/Project/ProjectValidator.h"

#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/AssetMoveCommand.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "EditorCore/Scripting/Private/ScriptValidation.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/SpotShadowAtlas.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SpotLightComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/PhysicsValidation.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>

// The validator (Architecture §13.7, ADR 0008 decision 17; the asset checks of M6, ADR 0010 decision 5; the physics checks
// of M11, ADR 0014 decision 15; the audio checks of M12, ADR 0015 decision 12; the light limit of M8, ADR 0013 decision 7).
// The code list and the load-code mapping (ADR 0006 decision 35) are data the contracts froze.

namespace Engine {

	namespace {

		constexpr std::string_view ScriptCompileErrorCode = "SCRIPT_COMPILE_ERROR";
		constexpr std::string_view ScriptTypeErrorCode = "SCRIPT_TYPE_ERROR";
		constexpr std::string_view ScriptNotBehaviourCode = "SCRIPT_NOT_A_BEHAVIOUR";
		constexpr std::string_view ScriptUnknownOverrideCode = "SCRIPT_UNKNOWN_FIELD_OVERRIDE";
		constexpr std::string_view ScriptFieldMismatchCode = "SCRIPT_FIELD_TYPE_MISMATCH";
		constexpr std::string_view InputUnknownActionCode = "INPUT_UNKNOWN_ACTION";
		constexpr std::string_view TestSuiteInvalidCode = "TEST_SUITE_INVALID";

		constexpr std::array ValidatorCodes = {
			SceneNoPrimaryCameraCode,
			SceneMultiplePrimaryCamerasCode,
			SceneDuplicateUniqueComponentCode,
			SceneNonCanonicalOrderCode,
			SceneInvalidHierarchyCode,
			SceneInconsistentPrefabLinkCode,
			EntityDuplicateIdCode,
			EntityDanglingReferenceCode,
			ComponentFieldOutOfRangeCode,
			ComponentMissingRequirementCode,
			ComponentConflictCode,
			AssetMissingCode,
			AssetTypeMismatchCode,
			AssetImportFailedCode,
			AssetOrphanMetaCode,
			AssetDuplicateHandleCode,
			AssetOrphanDependencyCode,
			AssetTangentsApproximatedCode,
			AssetUnsupportedUvSetCode,
			AssetVertexColorsIgnoredCode,
			AssetContentSkippedCode,
			PathCaseMismatchCode,
			PrefabMissingAssetCode,
			// M11 (Engine/Physics/PhysicsDiagnostics.h, ADR 0014 decision 9), in §13.7 order.
			PhysicsNonconvexDynamicCode,
			PhysicsMixedTriggerCode,
			PhysicsDynamicTriggerCode,
			PhysicsAllDofsLockedCode,
			PhysicsInvalidShapeCode,
			PhysicsAdjacentStaticBodiesCode,
			PhysicsUnknownLayerCode,
			PhysicsNonuniformScaleCode,
			PhysicsDynamicUnderMovingParentCode,
			PhysicsLimitExceededCode,
			ScriptCompileErrorCode,
			ScriptTypeErrorCode,
			ScriptNotBehaviourCode,
			ScriptUnknownOverrideCode,
			ScriptFieldMismatchCode,
			InputUnknownActionCode,
			// M12 (Scene/AudioSystem.h, FindAudioSceneIssues).
			AudioNoListenerCode,
			AudioMultiplePrimaryListenersCode,
			// M8 (Renderer/RenderPrepare.h, ADR 0013 decision 7).
			RenderNoLightingCode,
			RenderLightLimitExceededCode,
			RenderSpotShadowBudgetCode,
			BuildStartSceneMissingCode,
			BuildSceneMissingCode,
			TestSuiteInvalidCode,
			// M20 (Scene/LoadReport.h, Docs/Decisions/0031-m20-contract.md decision 10), appended in landing order.
			PrefabCycleCode,
		};

		// The documentation of every code, in ValidatorCodes order (M14, ADR 0022 decision 10): Docs/Reference/Validation.md, the
		// game-debugging skill's error-code table and gate 6 read it. Severity and AutoFixable state what the checks below
		// report; a code is AutoFixable when project.validate {fix} can fix at least some of its diagnostics.
		constexpr std::array ValidatorCodeInfos = {
			ValidationCodeInfo{ SceneNoPrimaryCameraCode, "Warning", true,
				"A scene that has entities has no Primary camera, so it renders through none.",
				"Set Primary on the camera the scene renders through or add one with entity.create {components: {Camera: {Primary: true}}}; the fix makes a scene's only camera Primary." },
			ValidationCodeInfo{ SceneMultiplePrimaryCamerasCode, "Error", true,
				"Several cameras of one scene are Primary, but a scene renders through exactly one.",
				"Clear Primary on all but one camera; the fix keeps the first Primary camera in scene order." },
			ValidationCodeInfo{ SceneDuplicateUniqueComponentCode, "Error", false,
				"A scene file holds more than one component that is unique per scene (Environment, PostProcess).",
				"Open the scene with scene.open {repair: true}, which keeps the first in canonical order, then scene.save." },
			ValidationCodeInfo{ SceneNonCanonicalOrderCode, "Warning", false,
				"A scene file lists a child entity before its parent, which is not the canonical order.",
				"Open the scene with scene.open {repair: true}, which normalizes the order, then scene.save." },
			ValidationCodeInfo{ SceneInvalidHierarchyCode, "Error", false,
				"A scene file has an entity whose Parent names no entity of the scene or forms a parent cycle.",
				"Open the scene with scene.open {repair: true}, which reparents orphans to the root and cuts cycles, then scene.save." },
			ValidationCodeInfo{ SceneInconsistentPrefabLinkCode, "Error", false,
				"A scene file has a prefab instance member whose PrefabLink does not match its instance root, an OuterPrefabEntityID that "
				"names no enclosing instance, or a nested instance root that records overrides.",
				"Open the scene with scene.open {repair: true}, which unpacks the member, clears the OuterPrefabEntityID or drops the "
				"nested root's overrides, then scene.save, or re-instantiate the prefab." },
			ValidationCodeInfo{ EntityDuplicateIdCode, "Error", false,
				"Two entities of a scene file carry the same ID.",
				"Open the scene with scene.open {repair: true}, which gives the later duplicate a fresh ID, then scene.save." },
			ValidationCodeInfo{ EntityDanglingReferenceCode, "Warning", true,
				"An entity reference field (a component field or a script field override) names no entity of its scene.",
				"Point the reference at an existing entity; the fix in the open scene clears it." },
			ValidationCodeInfo{ ComponentFieldOutOfRangeCode, "Error", false,
				"A component value in a scene file fails the registry's checks: a value out of range, of the wrong type or not finite.",
				"Correct the value in the file, or load it with scene.open {repair: true}, which drops the component, and scene.save." },
			ValidationCodeInfo{ ComponentMissingRequirementCode, "Error", false,
				"A component in a scene file lacks a component it requires on the same entity.",
				"Add the required component to the entity in the file, or load it with scene.open {repair: true}, which drops the component." },
			ValidationCodeInfo{ ComponentConflictCode, "Error", false,
				"A component in a scene file shares its entity with a component it excludes.",
				"Remove one of the two components in the file, or load it with scene.open {repair: true}, which drops the later one." },
			ValidationCodeInfo{ AssetMissingCode, "Error", false,
				"A reference in a scene, material or prefab names an asset handle that no asset has; the type's placeholder stands in.",
				"Restore the asset (edit.undo of its deletion, or from version control) or assign another asset." },
			ValidationCodeInfo{ AssetTypeMismatchCode, "Error", true,
				"A reference names an asset of another type than its field accepts, or a .meta's type or importer disagrees with its source's extension.",
				"Assign an asset of the accepted type; the fix rewrites a mismatching .meta for the importer that takes the source, keeping its handle "
				"(with that importer's default settings)." },
			ValidationCodeInfo{ AssetImportFailedCode, "Error", false,
				"Importing, loading or reading an asset or its .meta failed, or a scene file cannot be loaded at all.",
				"Correct the source file (the message names the failure and its location) or restore it from version control." },
			ValidationCodeInfo{ AssetOrphanMetaCode, "Warning", true,
				"A .meta file has no source file beside it.",
				"Restore the source, or let the fix move the .meta to the trash." },
			ValidationCodeInfo{ AssetDuplicateHandleCode, "Error", true,
				"Several .meta files whose sources exist carry the same asset handle, typically a copied file pair.",
				"Let the fix give every copy but the original a fresh handle." },
			ValidationCodeInfo{ AssetOrphanDependencyCode, "Warning", true,
				"A dependency .meta names an owner asset that is not registered.",
				"Restore the owner asset, or let the fix move the dependency .meta to the trash." },
			ValidationCodeInfo{ AssetTangentsApproximatedCode, "Warning", false,
				"A glTF primitive needs tangents but has no TEXCOORD_0, so its tangents are an arbitrary basis.",
				"Export the model with texture coordinates or with its own tangents." },
			ValidationCodeInfo{ AssetUnsupportedUvSetCode, "Warning", false,
				"A glTF texture is bound to TEXCOORD_1, which the engine ignores because meshes have one UV set.",
				"Bind the texture to TEXCOORD_0 in the model, or bake it into the first UV set." },
			ValidationCodeInfo{ AssetVertexColorsIgnoredCode, "Warning", false,
				"A glTF primitive has vertex colours, which the engine ignores.",
				"Bake the colours into a texture or a material, or remove COLOR_0 from the model." },
			ValidationCodeInfo{ AssetContentSkippedCode, "Warning", false,
				"A glTF file holds content the engine does not import: points or lines, skins, animations, cameras or lights.",
				"Remove the content from the model, or rebuild it with engine components (Camera, lights) in the scene." },
			ValidationCodeInfo{ PathCaseMismatchCode, "Error", true,
				"A .meta or a path matches a file only when letter case is ignored, which resolves differently on other systems.",
				"Use the file's exact spelling; the fix renames a .meta to its source's case." },
			ValidationCodeInfo{ PrefabMissingAssetCode, "Warning", false,
				"A prefab instance names a prefab asset that is not registered; the instance stays fully expanded.",
				"Restore the prefab asset, or unpack the instance (prefab.unpack)." },
			ValidationCodeInfo{ PhysicsNonconvexDynamicCode, "Error", false,
				"A non-convex MeshCollider is on a Dynamic RigidBody, so the body is not created.",
				"Set Convex on the MeshCollider, or make the body Static or Kinematic." },
			ValidationCodeInfo{ PhysicsMixedTriggerCode, "Error", false,
				"The colliders gathered by one RigidBody disagree on IsTrigger, so the body is not created.",
				"Make all of the body's colliders triggers or none, moving triggers to a child entity with its own RigidBody." },
			ValidationCodeInfo{ PhysicsDynamicTriggerCode, "Error", false,
				"A Dynamic RigidBody has only trigger colliders, so the body is not created.",
				"Make the body Kinematic or Static, or clear IsTrigger on its colliders." },
			ValidationCodeInfo{ PhysicsAllDofsLockedCode, "Error", false,
				"A Dynamic RigidBody has all six degrees of freedom locked, so the body is not created.",
				"Unlock at least one axis, or make the body Kinematic or Static." },
			ValidationCodeInfo{ PhysicsInvalidShapeCode, "Error", false,
				"A collider's shape cannot be created (a degenerate hull or mesh, or a scale the shape does not allow), so the body is not created.",
				"Correct the collider's size or scale, or use a mesh with a valid volume." },
			ValidationCodeInfo{ PhysicsAdjacentStaticBodiesCode, "Warning", true,
				"Two static bodies touch or nearly touch, so a rolling body bumps at their seam.",
				"Join them into one static compound; the fix adds a Static RigidBody to their nearest common ancestor." },
			ValidationCodeInfo{ PhysicsUnknownLayerCode, "Error", false,
				"A RigidBody or CharacterController names a layer that Physics.Layers does not declare; it uses layer Default.",
				"Declare the layer in Physics.Layers with project.setSettings, or name a declared layer." },
			ValidationCodeInfo{ PhysicsNonuniformScaleCode, "Warning", false,
				"A sphere or capsule collider is under a non-uniform world scale and uses its largest axis.",
				"Give the entity and its ancestors a uniform scale, or use a box or convex mesh collider." },
			ValidationCodeInfo{ PhysicsDynamicUnderMovingParentCode, "Warning", false,
				"A Dynamic RigidBody is below an ancestor that moves on its own, and it simulates in world space without following it.",
				"Move the dynamic body out from under the moving ancestor, or make it Kinematic." },
			ValidationCodeInfo{ PhysicsLimitExceededCode, "Warning; Error at the limit", false,
				"A scene would create more physics bodies than the world's limit, or more than 90 % of it.",
				"Merge colliders into compound bodies or remove bodies so the count stays below the limit." },
			ValidationCodeInfo{ ScriptCompileErrorCode, "Error", false,
				"A script does not compile, its load-time module code fails, or a require does not resolve.",
				"Correct the script at the reported line (script.check reports the same finding)." },
			ValidationCodeInfo{ ScriptTypeErrorCode, "Error; Warning for a Luau lint warning", false,
				"The strict type checker reports a finding in a script other than a syntax error (a type error, an unsupported type function, a require it cannot resolve); one of Luau's lint warnings (an unused local, two statements on one line) is a Warning.",
				"Correct the script at the reported range (script.check reports the same finding)." },
			ValidationCodeInfo{ ScriptNotBehaviourCode, "Error", false,
				"A Script component names a script that is a Module or a TestSuite rather than a Behaviour.",
				"Assign a script that returns Script.Define." },
			ValidationCodeInfo{ ScriptUnknownOverrideCode, "Error", false,
				"A Script component overrides a field its script does not declare; the override is preserved.",
				"Remove the override, or rename it to a declared field." },
			ValidationCodeInfo{ ScriptFieldMismatchCode, "Error", false,
				"A Script component's field override does not match the declared type or constraints; the declared default is used.",
				"Correct the override to match its declared type and constraints." },
			ValidationCodeInfo{ InputUnknownActionCode, "Error", false,
				"A string literal passed to an Input action function names no action of Input.Actions.",
				"Declare the action in Input.Actions with project.setSettings, or correct the literal." },
			ValidationCodeInfo{ AudioNoListenerCode, "Warning", false,
				"A scene has spatial AudioSources but no primary AudioListener and no primary camera, so they are heard from the origin.",
				"Add an AudioListener with Primary to the camera or the player." },
			ValidationCodeInfo{ AudioMultiplePrimaryListenersCode, "Warning", true,
				"Several active AudioListeners are Primary; the first in canonical order is used.",
				"Clear Primary on all but one AudioListener; the fix clears it on every later one." },
			ValidationCodeInfo{ RenderNoLightingCode, "Warning", false,
				"A scene has non-emissive meshes but no light or ambient illumination, so they render black.",
				"Add a light, or raise the environment's effective illumination." },
			ValidationCodeInfo{ RenderLightLimitExceededCode, "Warning", false,
				"A scene has more than 256 enabled lights, more than one view shades.",
				"Disable or remove lights, or keep fewer of them in view at once." },
			ValidationCodeInfo{ RenderSpotShadowBudgetCode, "Warning", false,
				"A scene has more than 8 shadow-casting spot lights, more than one view shadows.",
				"Turn off shadows on less important spot lights, or keep fewer of them in view at once." },
			ValidationCodeInfo{ BuildStartSceneMissingCode, "Error", false,
				"StartScene names no scene file.",
				"Create the scene with scene.new, or set StartScene to an existing scene with project.setSettings." },
			ValidationCodeInfo{ BuildSceneMissingCode, "Error", true,
				"An Export.BuildScenes entry names no scene file.",
				"Create the scene with scene.new; the fix removes the entry." },
			ValidationCodeInfo{ TestSuiteInvalidCode, "Error", false,
				"A Testing.Suites entry names a script that is not a TestSuite, or a scene that is not a registered Scene asset.",
				"Assign a script that returns Test.Suite, and create the scene or leave Scene empty for an empty test scene." },
			ValidationCodeInfo{ PrefabCycleCode, "Error", false,
				"A prefab nests itself, directly or through other prefabs.",
				"Remove the nested instance that closes the cycle from one of the prefab files, then reimport it." },
		};

		// The code documentation lists exactly the codes, in order.
		consteval bool CodeInfosMatchCodes()
		{
			if (ValidatorCodeInfos.size() != ValidatorCodes.size())
				return false;
			for (size_t index = 0; index < ValidatorCodes.size(); ++index)
			{
				if (ValidatorCodeInfos[index].Code != ValidatorCodes[index])
					return false;
			}
			return true;
		}
		static_assert(CodeInfosMatchCodes(), "ValidatorCodeInfos must document every code of ValidatorCodes, in the same order");

		// The import diagnostics the asset manager records for an asset (the scan's come from the validator's own scan).
		constexpr std::array ImportDiagnosticCodes = {
			AssetImportFailedCode,
			AssetTangentsApproximatedCode,
			AssetUnsupportedUvSetCode,
			AssetVertexColorsIgnoredCode,
			AssetContentSkippedCode,
		};

		constexpr std::string_view FixLabel = "Fix Validation Issues";
		constexpr std::string_view StructuralHint = "scene.open {repair: true}, then scene.save";
		constexpr std::string_view ComponentHint =
			"correct the value in the file, or load it with scene.open {repair: true} (which drops the component) and scene.save";
		constexpr std::string_view RepairedSuffix = " (repaired: ";

		// Where a diagnostic is, before its id is computed.
		struct DiagnosticSite
		{
			std::string File{};
			std::string Entity{};
			std::string Component{};
			std::string Field{};
			std::string Subject{};
		};

		// One scene the validator checks: the open scene (whose checks may fix it) or a scratch copy of a file. Layers is the
		// project's physics layer table (M11), null when the settings give none (the physics checks are skipped then).
		struct CheckedScene
		{
			const Scene* Target = nullptr;
			std::string File{};
			bool IsOpenScene = false;
			const PhysicsLayerTable* Layers = nullptr;
		};

		// The position of one EntityRef value inside a component, as diagnostics name it (ProjectValidator::MakeDiagnosticId):
		// its top-level field, and the subject that tells apart several problems in one field.
		struct EntityRefSite
		{
			std::string Field{};
			std::string MapKey{}; // the first map key above the value; empty when none
			bool InArray = false; // the value is an array element (or below one)
		};

		struct ScriptReferenceFix
		{
			AssetHandle Script{};
			uint64_t Version = 0;
			std::string Field{};
			std::optional<VariantValue> Stored{};
			VariantValue Effective{};
			std::vector<size_t> Indices{};
			Ref<const ScriptFieldSchemaSource> Schemas{};
		};

		// A diagnostic with the subject its id was made from, which fixes need (ProjectDiagnostic does not carry it), for an
		// asset scan diagnostic the registry's own diagnostic, which AssetRegistry::PlanFix identifies, and for
		// PHYSICS_ADJACENT_STATIC_BODIES the entity its fix gives a Static RigidBody (PhysicsDiagnostic::FixTarget).
		struct CollectedDiagnostic
		{
			ProjectDiagnostic Diagnostic{};
			std::string Subject{};
			std::optional<AssetDiagnostic> Scan{};
			UUID FixTarget{};
			std::optional<ScriptReferenceFix> ScriptReference{};
		};

		// What a validation found: the diagnostics, and the scan of the Assets folder that their asset fixes are planned on.
		struct Collection
		{
			std::vector<CollectedDiagnostic> Diagnostics{};
			std::optional<AssetRegistry> Scanned{};
		};

	}

	namespace Utils {

		// The subject of a dangling reference (ADR 0008 decision 17): the map key in a map field, the dangling UUID in an array
		// field, nothing for a plain field.
		static std::string MakeReferenceSubject(const EntityRefSite& site, UUID id)
		{
			if (!site.MapKey.empty())
				return site.MapKey;
			return site.InArray ? id.ToString() : std::string();
		}

		// The 16 hex digits of a valid UUID, or "" for the invalid one (a diagnostic about no particular entity).
		static std::string FormatOptionalUUID(UUID id)
		{
			return id.IsValid() ? id.ToString() : std::string();
		}

		static CollectedDiagnostic MakeDiagnostic(std::string_view code, DiagnosticSeverity severity, std::string message, const DiagnosticSite& site,
			std::string hint, bool autoFixable)
		{
			CollectedDiagnostic collected;
			collected.Subject = site.Subject;
			ProjectDiagnostic& diagnostic = collected.Diagnostic;
			diagnostic.Id = ProjectValidator::MakeDiagnosticId(code, site.File, site.Entity, site.Component, site.Field, site.Subject);
			diagnostic.Severity = severity;
			diagnostic.Code = std::string(code);
			diagnostic.Message = std::move(message);
			diagnostic.Entity = site.Entity;
			diagnostic.Component = site.Component;
			diagnostic.Field = site.Field;
			diagnostic.File = site.File;
			diagnostic.Hint = std::move(hint);
			diagnostic.AutoFixable = autoFixable;
			return collected;
		}

		// The last segment of a JSON pointer, unescaped ("/Entities/3/Components/Rigid~1Body" -> "Rigid/Body").
		static std::string LastPointerSegment(std::string_view pointer)
		{
			const size_t slash = pointer.rfind('/');
			std::string segment(slash == std::string_view::npos ? pointer : pointer.substr(slash + 1));
			for (size_t found = segment.find("~1"); found != std::string::npos; found = segment.find("~1", found + 1))
				segment.replace(found, 2, "/");
			for (size_t found = segment.find("~0"); found != std::string::npos; found = segment.find("~0", found + 1))
				segment.replace(found, 2, "~");
			return segment;
		}

		// The first segment of a JSON pointer, unescaped ("/Mass/0" -> "Mass"); empty for the root.
		static std::string FirstPointerSegment(std::string_view pointer)
		{
			if (pointer.size() < 2 || pointer.front() != '/')
				return {};
			const size_t end = pointer.find('/', 1);
			return LastPointerSegment(pointer.substr(0, end));
		}

		// A load diagnostic's message without the loader's " (repaired: ...)" note: the validator's scratch load repairs
		// nothing on disk.
		static std::string StripRepairNote(const std::string& message)
		{
			const size_t note = message.find(RepairedSuffix);
			return note == std::string::npos ? message : message.substr(0, note);
		}

		// Calls `visit(site, id)` for every non-null EntityRef value inside `value` (a JSON value of `type`); when it returns
		// true, the value is replaced with null (the fix of ENTITY_DANGLING_REFERENCE). Variant values are skipped: their
		// schemas (script fields) are resolved by their own owners (M13).
		template<typename Visit>
		static void VisitEntityRefs(Json& value, const TypeInfo& type, const EntityRefSite& site, Visit& visit)
		{
			switch (type.GetKind())
			{
				case FieldType::EntityRef:
				{
					if (!value.is_string())
						return;
					const Result<std::string> text = JsonReader(value).ReadString();
					const std::optional<UUID> id = text.has_value() ? UUID::FromString(*text) : std::nullopt;
					if (id.has_value() && id->IsValid() && visit(site, *id))
						value = Json();
					return;
				}
				case FieldType::Array:
				{
					if (!value.is_array() || type.GetElement() == nullptr)
						return;
					EntityRefSite element = site;
					element.InArray = true;
					for (Json& item : value)
						VisitEntityRefs(item, *type.GetElement(), element, visit);
					return;
				}
				case FieldType::Map:
				{
					if (!value.is_object() || type.GetElement() == nullptr)
						return;
					for (auto member = value.begin(); member != value.end(); ++member)
					{
						EntityRefSite element = site;
						if (element.MapKey.empty())
							element.MapKey = member.key();
						VisitEntityRefs(member.value(), *type.GetElement(), element, visit);
					}
					return;
				}
				case FieldType::Struct:
				{
					if (!value.is_object() || type.GetStruct() == nullptr)
						return;
					for (const Scope<FieldInfo>& field : type.GetStruct()->GetFields())
					{
						if (field->IsVirtual() || !field->GetMeta().Serialized)
							continue;
						const auto member = value.find(field->GetName());
						if (member != value.end())
							VisitEntityRefs(member.value(), field->GetType(), site, visit);
					}
					return;
				}
				case FieldType::Bool:
				case FieldType::Int32:
				case FieldType::UInt32:
				case FieldType::Float:
				case FieldType::Vec2:
				case FieldType::Vec3:
				case FieldType::Vec4:
				case FieldType::Quat:
				case FieldType::Color3:
				case FieldType::Color4:
				case FieldType::Bool3:
				case FieldType::String:
				case FieldType::AssetRef:
				case FieldType::Enum:
				case FieldType::Variant:
					return;
			}
		}

		// Calls `visit(site, id)` for every EntityRef value in every top-level field of the component JSON `component`.
		template<typename Visit>
		static void VisitComponentEntityRefs(Json& component, const ComponentInfo& info, Visit& visit)
		{
			for (const Scope<FieldInfo>& field : info.GetFields())
			{
				if (field->IsVirtual() || !field->GetMeta().Serialized)
					continue;
				const auto member = component.find(field->GetName());
				if (member == component.end())
					continue;
				EntityRefSite site;
				site.Field = field->GetName();
				VisitEntityRefs(member.value(), field->GetType(), site, visit);
			}
		}

		// The components whose EntityRef fields name scene entities: stored, non-entity-level and maintained by the user.
		// Hidden components (Prefab, PrefabLink) refer to prefab-local ids and instance roots, which the structural checks
		// own (SCENE_INCONSISTENT_PREFAB_LINK).
		static bool HasSceneReferences(const ComponentInfo& info)
		{
			return info.HasFlag(ComponentFlags::Serializable) && !info.HasFlag(ComponentFlags::EntityLevel) && !info.HasFlag(ComponentFlags::Hidden)
				&& info.GetHostOps() != nullptr;
		}

		static void CheckCameras(const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const Scene& scene = *checked.Target;
			std::vector<ConstEntity> cameras;
			std::vector<ConstEntity> primaries;
			scene.ForEachCanonical([&cameras, &primaries](ConstEntity entity)
			{
				if (const CameraComponent* camera = entity.TryGetComponent<CameraComponent>())
				{
					cameras.push_back(entity);
					if (camera->Primary)
						primaries.push_back(entity);
				}
			});

			DiagnosticSite site;
			site.File = checked.File;
			site.Component = "Camera";
			site.Field = "Primary";
			if (primaries.size() > 1)
			{
				std::string names;
				for (const ConstEntity primary : primaries)
					names += std::format("{}'{}'", names.empty() ? "" : ", ", scene.GetEntityPath(primary));
				std::string hint = checked.IsOpenScene ? "fix it to keep only the first in scene order Primary, or clear Primary on all but one camera"
													   : "open the scene (scene.open) to fix it, or clear Primary on all but one camera";
				diagnostics.push_back(MakeDiagnostic(SceneMultiplePrimaryCamerasCode, DiagnosticSeverity::Error,
					std::format("{} cameras are Primary ({}); a scene renders through one", primaries.size(), names), site, std::move(hint),
					checked.IsOpenScene));
				return;
			}
			if (!primaries.empty() || scene.GetEntityCount() == 0)
				return;

			if (cameras.empty())
			{
				diagnostics.push_back(MakeDiagnostic(SceneNoPrimaryCameraCode, DiagnosticSeverity::Warning, "the scene has no camera", site,
					"add one with entity.create {components: {Camera: {Primary: true}}}", false));
				return;
			}
			const bool fixable = checked.IsOpenScene && cameras.size() == 1;
			std::string hint = fixable ? "fix it to make the scene's only camera Primary" : "set Primary on the camera the scene renders through";
			diagnostics.push_back(MakeDiagnostic(SceneNoPrimaryCameraCode, DiagnosticSeverity::Warning,
				std::format("none of the scene's {} camera(s) is Primary", cameras.size()), site, std::move(hint), fixable));
		}

		// RENDER_LIGHT_LIMIT_EXCEEDED (M8, §13.7; ProjectValidator.h): the validator cannot know every view, so it counts the
		// scene's effectively enabled lights against what one view shades.
		static void CheckLightLimit(const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const Scene& scene = *checked.Target;
			size_t lights = 0;
			scene.ForEachCanonical([&lights](ConstEntity entity)
			{
				if (entity.HasComponent<HierarchyDisabledTag>())
					return;
				lights += static_cast<size_t>(entity.HasComponent<DirectionalLightComponent>()) + static_cast<size_t>(entity.HasComponent<PointLightComponent>())
					+ static_cast<size_t>(entity.HasComponent<SpotLightComponent>());
			});
			if (lights <= MaxVisibleLights)
				return;
			DiagnosticSite site;
			site.File = checked.File;
			diagnostics.push_back(MakeDiagnostic(RenderLightLimitExceededCode, DiagnosticSeverity::Warning,
				std::format("the scene has {} enabled lights; a view shades at most {}, and leaves out the least important lights it sees beyond that",
					lights, MaxVisibleLights),
				site, "disable or remove lights, or keep fewer of them in view at once", false));
		}

		// The audio checks of §13.7 (FindAudioSceneIssues): AUDIO_MULTIPLE_PRIMARY_LISTENERS on each extra primary listener
		// (fixable in the open scene: its Primary is cleared) and AUDIO_NO_LISTENER on the first spatial AudioSource.
		static void CheckAudio(const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			for (AudioSceneIssue& issue : FindAudioSceneIssues(*checked.Target))
			{
				DiagnosticSite site;
				site.File = checked.File;
				site.Entity = FormatOptionalUUID(issue.Entity);
				const bool listener = issue.Code == AudioMultiplePrimaryListenersCode;
				site.Component = listener ? "AudioListener" : "AudioSource";
				site.Field = listener ? "Primary" : "";
				const bool fixable = issue.AutoFixable && checked.IsOpenScene;
				std::string hint = issue.AutoFixable && !checked.IsOpenScene
					? std::string("open the scene (scene.open) to fix it, or clear Primary on all but one AudioListener")
					: std::move(issue.Hint);
				diagnostics.push_back(MakeDiagnostic(issue.Code, issue.Severity, std::move(issue.Message), site, std::move(hint), fixable));
			}
		}

		static void CheckDanglingReferences(const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const Scene& scene = *checked.Target;
			const TypeRegistry& registry = scene.GetTypeRegistry();
			scene.ForEachCanonical([&](ConstEntity entity)
			{
				for (const ComponentInfo* info : registry.GetComponents())
				{
					if (!HasSceneReferences(*info) || !info->GetHostOps()->Has(entity))
						continue;
					Result<Json> component = ComponentAccess::GetComponentJson(entity, info->GetName());
					if (!component)
						continue;
					const auto visit = [&](const EntityRefSite& site, UUID id)
					{
						if (scene.FindEntityByID(id).IsValid())
							return false;
						DiagnosticSite where;
						where.File = checked.File;
						where.Entity = entity.GetUUID().ToString();
						where.Component = info->GetName();
						where.Field = site.Field;
						where.Subject = MakeReferenceSubject(site, id);
						std::string hint = checked.IsOpenScene ? "fix it to clear the reference, or point it at an existing entity"
															   : "open the scene (scene.open) to fix it, or point it at an existing entity";
						diagnostics.push_back(MakeDiagnostic(EntityDanglingReferenceCode, DiagnosticSeverity::Warning,
							std::format("'{}.{}' of '{}' refers to {}, which is not an entity of the scene", info->GetName(), site.Field,
								scene.GetEntityPath(entity), id.ToString()),
							where, std::move(hint), checked.IsOpenScene));
						return false;
					};
					VisitComponentEntityRefs(*component, *info, visit);
				}
			});
		}

		// The COMPONENT_* code of a component a Repair load rejected (SCENE_INVALID_COMPONENT has no single validator code,
		// ADR 0006 decision 35): its own value fails the registry's checks (out of range), or it conflicts with or misses
		// another component of its entity.
		static void ClassifyInvalidComponent(const Scene& scratch, const LoadDiagnostic& diagnostic, const LoadRepair* repair,
			const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const TypeRegistry& registry = scratch.GetTypeRegistry();
			const std::string name = LastPointerSegment(diagnostic.JsonPointer);
			const ComponentInfo* info = registry.FindComponent(name);
			if (info == nullptr)
				return;

			DiagnosticSite site;
			site.File = checked.File;
			site.Entity = FormatOptionalUUID(diagnostic.Entity);
			site.Component = name;
			const std::string message = StripRepairNote(diagnostic.Message);

			if (repair != nullptr && !repair->Removed.IsNull())
			{
				ObjectPtr object = info->CreateDefault();
				const Status read = info->FromJson(object.get(), JsonReader(repair->Removed.Get()), ReadContext{});
				if (!read)
				{
					const std::vector<ErrorIssue>& issues = read.error().GetIssues();
					site.Field = issues.empty() ? FirstPointerSegment(read.error().GetLocation().JsonPointer.value_or(std::string()))
												: FirstPointerSegment(issues.front().JsonPointer);
					diagnostics.push_back(MakeDiagnostic(ComponentFieldOutOfRangeCode, DiagnosticSeverity::Error, message, site,
						std::string(ComponentHint), false));
					return;
				}
			}

			const ConstEntity entity = repair != nullptr ? scratch.FindEntityByID(repair->Entity) : ConstEntity();
			bool conflict = false;
			if (entity.IsValid())
			{
				for (const ComponentInfo* other : registry.GetComponents())
				{
					if (other == info || other->GetHostOps() == nullptr || !other->GetHostOps()->Has(entity))
						continue;
					const auto excludes = [](const ComponentInfo& component, const ComponentInfo* excluded)
					{
						return std::ranges::find(component.GetExcludes(), excluded) != component.GetExcludes().end();
					};
					conflict = conflict || excludes(*info, other) || excludes(*other, info);
				}
			}
			diagnostics.push_back(MakeDiagnostic(conflict ? ComponentConflictCode : ComponentMissingRequirementCode, DiagnosticSeverity::Error, message,
				site, std::string(ComponentHint), false));
		}

		// The validator diagnostics of a scene file's Repair load: structural codes through MapLoadCode, rejected components
		// through ClassifyInvalidComponent. The subject is the diagnostic's pointer within the file, which tells apart problems
		// of one code at one entity; files that are not open never change through a fix, so it is stable.
		static void MapLoadReport(const Scene& scratch, const LoadReport& report, const CheckedScene& checked,
			std::vector<CollectedDiagnostic>& diagnostics)
		{
			for (const LoadDiagnostic& diagnostic : report.Diagnostics)
			{
				if (diagnostic.Code == SceneInvalidComponentCode)
				{
					const auto repair = std::ranges::find_if(report.Repairs, [&diagnostic](const LoadRepair& candidate)
					{
						return candidate.Code == diagnostic.Code && candidate.JsonPointer == diagnostic.JsonPointer;
					});
					ClassifyInvalidComponent(scratch, diagnostic, repair != report.Repairs.end() ? &*repair : nullptr, checked, diagnostics);
					continue;
				}

				const std::string_view code = ProjectValidator::MapLoadCode(diagnostic.Code);
				if (code.empty())
					continue;
				DiagnosticSite site;
				site.File = checked.File;
				site.Entity = FormatOptionalUUID(diagnostic.Entity);
				if (code == SceneDuplicateUniqueComponentCode)
					site.Component = LastPointerSegment(diagnostic.JsonPointer);
				site.Subject = diagnostic.JsonPointer;
				const DiagnosticSeverity severity = code == SceneNonCanonicalOrderCode ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
				diagnostics.push_back(MakeDiagnostic(code, severity, StripRepairNote(diagnostic.Message), site, std::string(StructuralHint), false));
			}
		}

		// Calls `visit(site, handle, acceptedType)` for every non-null AssetRef value inside `value` (a JSON value of `type`).
		// Variant values are skipped like in VisitEntityRefs.
		template<typename Visit>
		static void VisitAssetRefs(const Json& value, const TypeInfo& type, const EntityRefSite& site, Visit& visit)
		{
			switch (type.GetKind())
			{
				case FieldType::AssetRef:
				{
					if (!value.is_string())
						return;
					const Result<std::string> text = JsonReader(value).ReadString();
					const std::optional<UUID> handle = text.has_value() ? UUID::FromString(*text) : std::nullopt;
					if (handle.has_value() && handle->IsValid())
						visit(site, *handle, std::string_view(type.GetAssetTypeName()));
					return;
				}
				case FieldType::Array:
				{
					if (!value.is_array() || type.GetElement() == nullptr)
						return;
					EntityRefSite element = site;
					element.InArray = true;
					for (const Json& item : value)
						VisitAssetRefs(item, *type.GetElement(), element, visit);
					return;
				}
				case FieldType::Map:
				{
					if (!value.is_object() || type.GetElement() == nullptr)
						return;
					for (auto member = value.begin(); member != value.end(); ++member)
					{
						EntityRefSite element = site;
						if (element.MapKey.empty())
							element.MapKey = member.key();
						VisitAssetRefs(member.value(), *type.GetElement(), element, visit);
					}
					return;
				}
				case FieldType::Struct:
				{
					if (!value.is_object() || type.GetStruct() == nullptr)
						return;
					for (const Scope<FieldInfo>& field : type.GetStruct()->GetFields())
					{
						if (field->IsVirtual() || !field->GetMeta().Serialized)
							continue;
						const auto member = value.find(field->GetName());
						if (member != value.end())
							VisitAssetRefs(member.value(), field->GetType(), site, visit);
					}
					return;
				}
				case FieldType::Bool:
				case FieldType::Int32:
				case FieldType::UInt32:
				case FieldType::Float:
				case FieldType::Vec2:
				case FieldType::Vec3:
				case FieldType::Vec4:
				case FieldType::Quat:
				case FieldType::Color3:
				case FieldType::Color4:
				case FieldType::Bool3:
				case FieldType::String:
				case FieldType::EntityRef:
				case FieldType::Enum:
				case FieldType::Variant:
					return;
			}
		}

		// Calls `visit(site, handle, acceptedType)` for every AssetRef value in the top-level fields `fields` of `object`.
		template<typename Visit>
		static void VisitFieldAssetRefs(const Json& object, std::span<const Scope<FieldInfo>> fields, Visit& visit)
		{
			for (const Scope<FieldInfo>& field : fields)
			{
				if (field->IsVirtual() || !field->GetMeta().Serialized)
					continue;
				const auto member = object.find(field->GetName());
				if (member == object.end())
					continue;
				EntityRefSite site;
				site.Field = field->GetName();
				VisitAssetRefs(member.value(), field->GetType(), site, visit);
			}
		}

		// ASSET_MISSING for a reference no registry knows, ASSET_TYPE_MISMATCH for one of another type than `acceptedType`
		// (empty: any type) or one that cannot be loaded at all (a dependency file). The manager knows the built-ins too.
		// `describe` names the referencing place for the message; it runs only when there is a diagnostic, because an entity
		// path costs a walk over the entity's same-named siblings.
		template<typename Describe>
		static void CheckAssetReference(const EditorContext& editor, AssetHandle handle, std::string_view acceptedType, const DiagnosticSite& site,
			const Describe& describe, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const EditorAssetManager& assets = editor.GetAssets();
			const AssetType actual = assets.GetAssetType(handle);
			const std::string expected = acceptedType.empty() ? std::string("asset") : std::string(acceptedType);
			CollectedDiagnostic collected;
			if (actual == AssetType::None && assets.GetMetadata(handle) == nullptr)
			{
				collected = MakeDiagnostic(AssetMissingCode, DiagnosticSeverity::Error,
					std::format("{} refers to the {} {}, which no asset has", describe(), expected, handle.ToString()), site,
					"restore the asset (edit.undo of its deletion, or from version control), or assign another one", false);
			}
			else if (actual == AssetType::None || (!acceptedType.empty() && AssetTypeToString(actual) != acceptedType))
			{
				const std::string what = actual == AssetType::None ? std::string("a dependency file, which is never loaded on its own")
																   : std::format("a {}", AssetTypeToString(actual));
				collected = MakeDiagnostic(AssetTypeMismatchCode, DiagnosticSeverity::Error,
					std::format("{} refers to {} ({}), but takes a {}", describe(), handle.ToString(), what, expected), site,
					std::format("assign an asset of type {}", expected), false);
			}
			else
			{
				return;
			}
			collected.Diagnostic.Asset = handle.ToString();
			diagnostics.push_back(std::move(collected));
		}

		// The asset references of one component (by its JSON), and of a prefab instance its prefab (PREFAB_MISSING_ASSET when
		// it is not registered: the scene still loads, fully expanded, §5.5). `owner` names the component's entity for messages,
		// called only when there is a diagnostic (an entity path walks the entity's same-named siblings, so computing it for
		// every component of a large scene would cost time quadratic in its entities).
		template<typename Owner>
		static void CheckComponentAssets(const EditorContext& editor, const ComponentInfo& info, const Json& component, const DiagnosticSite& at,
			const Owner& owner, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const TypeRegistry& registry = editor.GetTypeRegistry();
			if (&info == registry.FindComponent<PrefabInstanceComponent>())
			{
				const auto prefab = component.find("Prefab");
				const Result<std::string> text = prefab != component.end() && prefab->is_string() ? JsonReader(*prefab).ReadString()
																								  : Result<std::string>(std::string());
				const std::optional<UUID> handle = text.has_value() ? UUID::FromString(*text) : std::nullopt;
				if (!handle.has_value() || !handle->IsValid())
					return;
				DiagnosticSite site = at;
				site.Component = info.GetName();
				site.Field = "Prefab";
				if (editor.GetAssets().GetAssetType(*handle) == AssetType::None && editor.GetAssets().GetMetadata(*handle) == nullptr)
				{
					CollectedDiagnostic collected = MakeDiagnostic(PrefabMissingAssetCode, DiagnosticSeverity::Warning,
						std::format("'{}' is an instance of the prefab {}, which is not registered; it stays as it is, fully expanded", owner(),
							handle->ToString()),
						site, "restore the prefab asset, or unpack the instance (prefab.unpack)", false);
					collected.Diagnostic.Asset = handle->ToString();
					diagnostics.push_back(std::move(collected));
					return;
				}
				const auto describe = [&owner]()
				{
					return std::format("the instance '{}'", owner());
				};
				CheckAssetReference(editor, *handle, AssetTypeToString(AssetType::Prefab), site, describe, diagnostics);
				return;
			}

			const auto visit = [&](const EntityRefSite& reference, AssetHandle handle, std::string_view acceptedType)
			{
				DiagnosticSite site = at;
				site.Component = info.GetName();
				site.Field = reference.Field;
				site.Subject = MakeReferenceSubject(reference, handle);
				const auto describe = [&info, &reference, &owner]()
				{
					return std::format("'{}.{}' of '{}'", info.GetName(), reference.Field, owner());
				};
				CheckAssetReference(editor, handle, acceptedType, site, describe, diagnostics);
			};
			VisitFieldAssetRefs(component, info.GetFields(), visit);
		}

		// The components whose asset references the validator checks: those whose entity references it checks, plus the hidden
		// prefab instance component, whose prefab reference PREFAB_MISSING_ASSET covers.
		static bool HasAssetReferences(const ComponentInfo& info, const TypeRegistry& registry)
		{
			if (&info == registry.FindComponent<PrefabInstanceComponent>())
				return info.GetHostOps() != nullptr;
			return HasSceneReferences(info);
		}

		static void CheckSceneAssets(const EditorContext& editor, const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const Scene& scene = *checked.Target;
			const TypeRegistry& registry = scene.GetTypeRegistry();
			scene.ForEachCanonical([&](ConstEntity entity)
			{
				for (const ComponentInfo* info : registry.GetComponents())
				{
					if (!HasAssetReferences(*info, registry) || !info->GetHostOps()->Has(entity))
						continue;
					const Result<Json> component = ComponentAccess::GetComponentJson(entity, info->GetName());
					if (!component)
						continue;
					DiagnosticSite site;
					site.File = checked.File;
					site.Entity = entity.GetUUID().ToString();
					const auto owner = [&scene, entity]()
					{
						return scene.GetEntityPath(entity);
					};
					CheckComponentAssets(editor, *info, *component, site, owner, diagnostics);
				}
			});
		}

		// The physics codes of the scene (M11, Scene/PhysicsValidation.h). Mesh colliders load their meshes through the asset
		// manager the editor injects into its engine context (EditorContext::Create). Only the open scene's diagnostics are
		// fixable: the fix edits the open scene.
		static void CheckPhysics(const EditorContext& editor, const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			if (checked.Layers == nullptr)
				return;
			for (const PhysicsDiagnostic& physics : ValidateScenePhysics(*checked.Target, *checked.Layers, editor.GetEngine().GetAssetManager()))
			{
				DiagnosticSite site;
				site.File = checked.File;
				site.Entity = FormatOptionalUUID(physics.Entity);
				site.Component = physics.Component;
				site.Field = physics.Field;
				site.Subject = physics.Subject;
				CollectedDiagnostic collected =
					MakeDiagnostic(physics.Code, physics.Severity, physics.Message, site, physics.Hint, physics.AutoFixable && checked.IsOpenScene);
				collected.FixTarget = physics.FixTarget;
				diagnostics.push_back(std::move(collected));
			}
		}

		static Status CheckRenderScene(const EditorContext& editor, const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const auto render = EvaluateRenderSceneValidation(*checked.Target, *editor.GetEngine().GetAssetManager());
			if (!render)
				return std::unexpected(Error(render.error()).WithContext(std::format("validating rendering in scene '{}'", checked.File)));
			DiagnosticSite site;
			site.File = checked.File;
			if (render->NoLighting)
				diagnostics.push_back(MakeDiagnostic(RenderNoLightingCode, DiagnosticSeverity::Warning,
					"the scene has nonemissive meshes but no contributing light or ambient illumination", site,
					"add a light or increase the environment's effective illumination", false));
			if (render->ShadowedSpotLights > MaxSpotShadowLights)
				diagnostics.push_back(MakeDiagnostic(RenderSpotShadowBudgetCode, DiagnosticSeverity::Warning,
					std::format("the scene has {} shadow-casting spot lights; each view shadows at most {}", render->ShadowedSpotLights, MaxSpotShadowLights),
					site, "disable shadows on less important spot lights or keep fewer in view at once", false));
			return {};
		}

		// The resolved descriptor carries the element's AssetFilter at every array depth. Never infer references from
		// UUID-shaped strings or unknown overrides. Indices identify individual occurrences, including repeated handles.
		template<typename Visit>
		static void VisitScriptReferences(const Json& value, const FieldInfo& field, std::vector<size_t>& indices, Visit& visit)
		{
			if (field.GetKind() == FieldType::Array)
			{
				const FieldInfo* element = field.GetType().GetElementSchema();
				if (!value.is_array() || element == nullptr)
					return;
				for (size_t index = 0; index < value.size(); ++index)
				{
					indices.push_back(index);
					VisitScriptReferences(value[index], *element, indices, visit);
					indices.pop_back();
				}
				return;
			}
			if ((field.GetKind() != FieldType::EntityRef && field.GetKind() != FieldType::AssetRef) || !value.is_string())
				return;
			const auto id = JsonReader(value).ReadUUID();
			if (id && id->IsValid())
				visit(field, *id, indices);
		}

		static Status CheckSceneScripts(const EditorContext& editor, const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const EditorScriptService* service = editor.GetScriptService();
			if (service == nullptr)
				return {};
			struct InstanceSnapshot
			{
				UUID Entity{};
				std::string Path{};
				ScriptComponent Component{};
			};
			struct SchemaSnapshot
			{
				AssetRef<ScriptData> Script{};
				Ref<const ScriptFieldSchemaSource> Schemas{};
				uint64_t Version = 0;
			};
			// Loading may drain asset publications. Copy scene values before loading, and pin one immutable schema per
			// handle for the whole pass; fixes retain that same schema rather than resolving a later asset generation.
			std::vector<InstanceSnapshot> instances;
			checked.Target->ForEachCanonical([&checked, &instances](ConstEntity entity)
			{
				if (entity.HasComponent<ScriptComponent>())
					instances.push_back({ entity.GetUUID(), checked.Target->GetEntityPath(entity), entity.GetComponent<ScriptComponent>() });
			});
			std::map<AssetHandle, std::optional<SchemaSnapshot>> snapshots;
			for (const InstanceSnapshot& instance : instances)
			{
				const ScriptComponent& component = instance.Component;
				const AssetHandle handle = component.Script.GetHandle();
				if (!handle.IsValid() || editor.GetAssets().GetAssetType(handle) != AssetType::Script)
					continue; // The ordinary asset reference validator reports missing or mistyped handles.
				auto [snapshot, inserted] = snapshots.try_emplace(handle);
				if (inserted)
				{
					const auto loaded = service->GetFields(handle);
					if (loaded)
					{
						ENGINE_TRY_ASSIGN(auto schemas, ScriptFieldSchemaSource::Create({ { handle, *loaded } }));
						snapshot->second = SchemaSnapshot{ *loaded, std::move(schemas), editor.GetAssets().GetVersion(handle) };
					}
				}
				if (!snapshot->second)
					continue; // Source diagnostics below report compilation/import failures once per source.
				const SchemaSnapshot& schema = *snapshot->second;
				DiagnosticSite site{ .File = checked.File, .Entity = instance.Entity.ToString(), .Component = "Script", .Field = "Script" };
				if (schema.Script->Kind != ScriptKind::Behaviour)
				{
					auto diagnostic = MakeDiagnostic(ScriptNotBehaviourCode, DiagnosticSeverity::Error, "Script requires a Behaviour asset", site,
						"assign a script that returns Script.Define", false);
					diagnostic.Diagnostic.Asset = handle.ToString();
					diagnostics.push_back(std::move(diagnostic));
					continue;
				}
				std::map<std::string, VariantValue> effective;
				for (const ScriptFieldSchema& declaration : schema.Script->Fields)
					effective.emplace(declaration.Name, declaration.DefaultValue);
				for (const auto& [name, value] : component.Fields)
				{
					site.Field = "Fields." + name;
					site.Subject = name;
					const auto descriptor = schema.Schemas->FindField(handle, name);
					if (!descriptor)
					{
						diagnostics.push_back(MakeDiagnostic(ScriptUnknownOverrideCode, DiagnosticSeverity::Error,
							std::format("script declares no field '{}'; the override is preserved", name), site,
							"remove or rename the override to a declared field", false));
						continue;
					}
					ValidationContext validation;
					(*descriptor)->ValidateJson(JsonReader(value.Get()), {}, validation);
					if (!validation.HasErrors())
						effective[name] = value;
					for (const auto& issue : validation.GetIssues())
					{
						site.Subject = name + issue.JsonPointer;
						diagnostics.push_back(MakeDiagnostic(ScriptFieldMismatchCode, DiagnosticSeverity::Error,
							std::format("{}{}: {}; the declared default is used", name, issue.JsonPointer, issue.Message), site,
							"correct the override to match its declared type and constraints", false));
					}
				}
				for (const auto& [name, value] : effective)
				{
					ENGINE_TRY_ASSIGN(const FieldInfo* descriptor, schema.Schemas->FindField(handle, name));
					const auto visit = [&](const FieldInfo& reference, UUID target, const std::vector<size_t>& indices)
					{
						DiagnosticSite where = site;
						where.Field = "Fields." + name;
						for (const size_t index : indices)
							where.Field += std::format("[{}]", index);
						where.Subject = target.ToString();
						const std::string owner = std::format("'Script.{}' of '{}'", where.Field, instance.Path);
						if (reference.GetKind() == FieldType::AssetRef)
						{
							const auto describe = [&owner]()
							{
								return owner;
							};
							CheckAssetReference(editor, target, reference.GetMeta().AssetFilter, where, describe, diagnostics);
							return;
						}
						if (checked.Target->FindEntityByID(target).IsValid())
							return;
						auto diagnostic = MakeDiagnostic(EntityDanglingReferenceCode, DiagnosticSeverity::Warning,
							std::format("{} refers to {}, which is not an entity of the scene", owner, target.ToString()), where,
							checked.IsOpenScene ? "fix it to clear this reference override, or point it at an existing entity"
												: "open the scene (scene.open) to fix it, or point it at an existing entity",
							checked.IsOpenScene);
						const auto stored = component.Fields.find(name);
						diagnostic.ScriptReference = ScriptReferenceFix{ .Script = handle, .Version = schema.Version, .Field = name, .Stored = stored == component.Fields.end() ? std::nullopt : std::optional(stored->second), .Effective = value, .Indices = indices, .Schemas = schema.Schemas };
						diagnostics.push_back(std::move(diagnostic));
					};
					std::vector<size_t> indices;
					VisitScriptReferences(value.Get(), *descriptor, indices, visit);
				}
			}
			return {};
		}

		static Status CheckScene(const EditorContext& editor, const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			CheckCameras(checked, diagnostics);
			CheckAudio(checked, diagnostics);
			CheckDanglingReferences(checked, diagnostics);
			CheckSceneAssets(editor, checked, diagnostics);
			ENGINE_TRY(CheckSceneScripts(editor, checked, diagnostics));
			CheckPhysics(editor, checked, diagnostics);
			CheckLightLimit(checked, diagnostics);
			return CheckRenderScene(editor, checked, diagnostics);
		}

		// Loads the scene file `path` into a scratch scene in Repair mode and checks it; a file that cannot be loaded at all is
		// ASSET_IMPORT_FAILED. `layers` as CheckedScene::Layers.
		static Status CheckSceneFile(const EditorContext& editor, const VfsPath& path, const PhysicsLayerTable* layers,
			std::vector<CollectedDiagnostic>& diagnostics)
		{
			UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
			SceneSpecification specification;
			specification.Name = std::string(path.GetStem());
			specification.Registry = &editor.GetTypeRegistry();
			specification.IdGenerator = &scratchIds;
			const Scope<Scene> scratch = Scene::Create(specification);

			LoadOptions options;
			options.Mode = LoadMode::Repair;
			options.RepairIdGenerator = &scratchIds;
			options.SourcePath = std::string(path.GetPath());
			LoadReport report;
			CheckedScene checked{ scratch.get(), std::string(path.GetPath()), false, layers };
			const Status loaded = SceneSerializer::LoadFromFile(*scratch, editor.GetVfs(), path, options, report);
			if (!loaded)
			{
				DiagnosticSite site;
				site.File = checked.File;
				diagnostics.push_back(MakeDiagnostic(AssetImportFailedCode, DiagnosticSeverity::Error,
					std::format("the scene file cannot be loaded: {}", loaded.error().ToString()), site,
					"correct the file by hand or restore it from version control; scene.open reports where loading fails", false));
				return {};
			}
			MapLoadReport(*scratch, report, checked, diagnostics);
			return CheckScene(editor, checked, diagnostics);
		}

		// The project file's name, relative to the project root ("Tetris.eproj").
		static std::string GetProjectFileName(const EditorContext& editor)
		{
			return FileSystem::PathToUtf8(editor.GetProject().GetProjectFile().filename());
		}

		// Whether the project-relative `path` names an existing file (an invalid path names none).
		static bool ProjectFileExists(const EditorContext& editor, std::string_view path)
		{
			const Result<VfsPath> file = VfsPath::Create("project", path);
			if (!file)
				return false;
			const Result<FileInfo> info = editor.GetVfs().GetInfo(*file);
			return info.has_value() && !info->IsDirectory;
		}

		// The registered main asset of the project-relative `path`, or null (an invalid path names none).
		static const AssetRecord* FindRegisteredAsset(const EditorContext& editor, std::string_view path)
		{
			const Result<VfsPath> file = VfsPath::Create("project", path);
			const AssetRecord* record = file ? editor.GetAssets().GetRegistry().FindBySourcePath(*file) : nullptr;
			return record != nullptr && record->Metadata.Kind == AssetMetaKind::Asset ? record : nullptr;
		}

		// TEST_SUITE_INVALID (ProjectValidator::CheckTestSuites, the rule Validate and testing exports share; M13, tightened in
		// M15 by Docs/Decisions/0024-m15-contract.md decision 6): per Testing.Suites entry, in entry order, a Script that is not
		// a registered TestSuite script, then a non-empty Scene that is not a registered Scene asset (a testing export packages
		// the scene, so a file the registry does not know is not enough).
		static void CheckTestSuiteSettings(const EditorContext& editor, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const ProjectSettings& settings = editor.GetProject().GetSettings();
			DiagnosticSite site;
			site.File = GetProjectFileName(editor);
			for (size_t index = 0; index < settings.Testing.Suites.size(); ++index)
			{
				const TestSuiteSettings& suite = settings.Testing.Suites[index];
				site.Field = std::format("Testing.Suites[{}]", index);
				site.Subject = std::to_string(index);
				const AssetRecord* script = FindRegisteredAsset(editor, suite.Script);
				bool validScript = false;
				if (script != nullptr && script->Metadata.Type == AssetType::Script && editor.GetScriptService() != nullptr)
				{
					const auto fields = editor.GetScriptService()->GetFields(script->Metadata.Handle);
					validScript = fields && (*fields)->Kind == ScriptKind::TestSuite;
				}
				if (!validScript)
				{
					diagnostics.push_back(MakeDiagnostic(TestSuiteInvalidCode, DiagnosticSeverity::Error,
						std::format("{} does not name a valid TestSuite script: '{}'", site.Field, suite.Script), site,
						"assign a script that returns Test.Suite", false));
				}
				if (suite.Scene.empty())
					continue;
				const AssetRecord* scene = FindRegisteredAsset(editor, suite.Scene);
				if (scene == nullptr || scene->Metadata.Type != AssetType::Scene)
				{
					site.Subject += "/Scene";
					diagnostics.push_back(MakeDiagnostic(TestSuiteInvalidCode, DiagnosticSeverity::Error,
						std::format("{} names '{}', which is not a registered scene", site.Field, suite.Scene), site,
						"create the scene (scene.new) or use an empty Scene for an empty test scene", false));
				}
			}
		}

		static void CheckSettings(const EditorContext& editor, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const ProjectSettings& settings = editor.GetProject().GetSettings();
			DiagnosticSite site;
			site.File = GetProjectFileName(editor);

			if (!settings.StartScene.empty() && !ProjectFileExists(editor, settings.StartScene))
			{
				site.Field = "StartScene";
				diagnostics.push_back(MakeDiagnostic(BuildStartSceneMissingCode, DiagnosticSeverity::Error,
					std::format("StartScene '{}' names no scene file", settings.StartScene), site,
					std::format("create it with scene.new {{path: \"{}\"}}, or set StartScene to an existing scene with project.setSettings",
						settings.StartScene),
					false));
			}

			site.Field = "Export.BuildScenes";
			std::set<std::string> reported;
			for (const std::string& scene : settings.Export.BuildScenes)
			{
				if (ProjectFileExists(editor, scene) || !reported.insert(scene).second)
					continue;
				site.Subject = scene;
				diagnostics.push_back(MakeDiagnostic(BuildSceneMissingCode, DiagnosticSeverity::Error,
					std::format("Export.BuildScenes names '{}', which is not a scene file", scene), site,
					"fix it to remove the entry, or create the scene with scene.new", true));
			}
			CheckTestSuiteSettings(editor, diagnostics);
		}

		// Every .scene file under project://Assets, sorted. Errors: the VFS errors other than a missing Assets/ directory.
		static Result<std::vector<VfsPath>> ListSceneFiles(const EditorContext& editor)
		{
			ENGINE_TRY_ASSIGN(const VfsPath assets, VfsPath::Create("project", "Assets"));
			const Result<std::vector<VfsEntry>> entries = editor.GetVfs().List(assets, true);
			if (!entries)
			{
				if (entries.error().GetCode() == ErrorCode::NotFound)
					return std::vector<VfsPath>();
				return std::unexpected(Error(entries.error()).WithContext("while listing the project's scene files"));
			}
			std::vector<VfsPath> scenes;
			for (const VfsEntry& entry : *entries)
			{
				if (!entry.Info.IsDirectory && entry.Path.GetExtension() == ".scene")
					scenes.push_back(entry.Path);
			}
			return scenes;
		}

		// The validator's own scan of the Assets folder (a copy of the editor's registry, so the keeper rule sees the registered
		// paths, scanned now: the report describes the files as they are), its diagnostics under their codes. nullopt without
		// an Assets folder.
		static Result<std::optional<AssetRegistry>> ScanAssets(const EditorContext& editor, std::vector<CollectedDiagnostic>& diagnostics)
		{
			ENGINE_TRY_ASSIGN(const VfsPath assets, VfsPath::Create("project", "Assets"));
			if (!editor.GetVfs().Exists(assets))
				return std::optional<AssetRegistry>();
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			AssetRegistry scanned = editor.GetAssets().GetRegistry();
			ENGINE_TRY_ASSIGN(const AssetScanResult scan, scanned.Scan(editor.GetVfs(), assets, importers.Describe()));
			for (const AssetDiagnostic& diagnostic : scan.Diagnostics)
			{
				DiagnosticSite site;
				site.File = diagnostic.Path;
				site.Subject = diagnostic.Subject;
				CollectedDiagnostic collected = MakeDiagnostic(diagnostic.Code, diagnostic.Severity, diagnostic.Message, site, diagnostic.Hint,
					diagnostic.AutoFixable);
				collected.Diagnostic.Asset = FormatOptionalUUID(diagnostic.Asset);
				collected.Scan = diagnostic;
				diagnostics.push_back(std::move(collected));
			}
			return std::optional<AssetRegistry>(std::move(scanned));
		}

		// The import diagnostics the asset manager holds: failed imports and the importers' warnings (§7.4).
		static void CollectImportDiagnostics(const EditorContext& editor, std::vector<CollectedDiagnostic>& diagnostics)
		{
			for (const AssetDiagnostic& diagnostic : editor.GetAssets().GetDiagnostics())
			{
				// An import diagnostic names its asset; the scan's (unreadable .meta files) come from ScanAssets.
				if (!diagnostic.Asset.IsValid()
					|| std::ranges::find(ImportDiagnosticCodes, std::string_view(diagnostic.Code)) == ImportDiagnosticCodes.end())
				{
					continue;
				}
				DiagnosticSite site;
				site.File = diagnostic.Path;
				site.Subject = diagnostic.Subject.empty() ? diagnostic.Asset.ToString() : diagnostic.Subject;
				CollectedDiagnostic collected = MakeDiagnostic(diagnostic.Code, diagnostic.Severity, diagnostic.Message, site, diagnostic.Hint, false);
				collected.Diagnostic.Asset = diagnostic.Asset.ToString();
				diagnostics.push_back(std::move(collected));
			}
		}

		// The asset references in the project's material and prefab files: the registered main assets of the Material and
		// Prefab importers (a glTF's materials reference its own textures and are checked by its import).
		static void CheckAssetFiles(const EditorContext& editor, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const TypeRegistry& registry = editor.GetTypeRegistry();
			for (const AssetRecord* record : editor.GetAssets().GetRegistry().GetRecords())
			{
				const AssetMetadata& metadata = record->Metadata;
				const bool isMaterial = metadata.Kind == AssetMetaKind::Asset && metadata.Importer == "Material";
				const bool isPrefab = metadata.Kind == AssetMetaKind::Asset && metadata.Importer == "Prefab";
				if (!isMaterial && !isPrefab)
					continue;
				const Result<std::string> text = editor.GetVfs().ReadText(record->SourcePath);
				// An unreadable file is the import's to report (ASSET_IMPORT_FAILED).
				if (!text.has_value())
					continue;
				const Result<Json> document = JsonReader::Parse(*text);
				if (!document.has_value() || !document->is_object())
					continue;
				const std::string file(record->SourcePath.GetPath());
				if (isMaterial)
				{
					const StructInfo* material = registry.FindStruct("Material");
					if (material == nullptr)
						continue;
					const auto visit = [&](const EntityRefSite& reference, AssetHandle handle, std::string_view acceptedType)
					{
						DiagnosticSite site;
						site.File = file;
						site.Field = reference.Field;
						site.Subject = MakeReferenceSubject(reference, handle);
						const auto describe = [&reference]()
						{
							return std::format("'{}' of the material", reference.Field);
						};
						CheckAssetReference(editor, handle, acceptedType, site, describe, diagnostics);
					};
					VisitFieldAssetRefs(*document, material->GetFields(), visit);
					continue;
				}

				const auto entities = document->find("Entities");
				if (entities == document->end() || !entities->is_array())
					continue;
				for (const Json& entity : *entities)
				{
					if (!entity.is_object())
						continue;
					const auto id = entity.find("ID");
					const auto components = entity.find("Components");
					if (id == entity.end() || !id->is_string() || components == entity.end() || !components->is_object())
						continue;
					DiagnosticSite site;
					site.File = file;
					site.Entity = JsonReader(*id).ReadString().value_or(std::string());
					for (auto component = components->begin(); component != components->end(); ++component)
					{
						const ComponentInfo* info = registry.FindComponent(component.key());
						if (info == nullptr || !component.value().is_object())
							continue;
						const auto owner = [&site]()
						{
							return std::format("entity {} of the prefab", site.Entity);
						};
						CheckComponentAssets(editor, *info, component.value(), site, owner, diagnostics);
					}
				}
			}
		}

		// PREFAB_CYCLE (M20, Docs/Decisions/0031-m20-contract.md decision 8): every prefab file whose document nests itself,
		// directly or through other prefabs. The nesting graph is read from the files: the Prefab component of each instance
		// in a prefab's document names the prefab it nests. One Error on each prefab on a cycle.
		static void CheckPrefabNesting(const EditorContext& editor, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const TypeRegistry& registry = editor.GetTypeRegistry();
			const ComponentInfo* instanceInfo = registry.FindComponent<PrefabInstanceComponent>();
			if (instanceInfo == nullptr)
				return;
			std::map<AssetHandle, std::set<AssetHandle>> nests; // prefab -> the prefabs its document nests
			std::map<AssetHandle, std::string> files;
			for (const AssetRecord* record : editor.GetAssets().GetRegistry().GetRecords())
			{
				const AssetMetadata& metadata = record->Metadata;
				if (metadata.Kind != AssetMetaKind::Asset || metadata.Importer != "Prefab")
					continue;
				files.emplace(metadata.Handle, std::string(record->SourcePath.GetPath()));
				// An unreadable or malformed file is the import's to report (ASSET_IMPORT_FAILED).
				const Result<std::string> text = editor.GetVfs().ReadText(record->SourcePath);
				if (!text.has_value())
					continue;
				const Result<Json> document = JsonReader::Parse(*text);
				if (!document.has_value() || !document->is_object())
					continue;
				const auto entities = document->find("Entities");
				if (entities == document->end() || !entities->is_array())
					continue;
				std::set<AssetHandle>& nested = nests[metadata.Handle];
				for (const Json& entity : *entities)
				{
					const auto components = entity.is_object() ? entity.find("Components") : entity.end();
					if (!entity.is_object() || components == entity.end() || !components->is_object())
						continue;
					const auto instance = components->find(instanceInfo->GetName());
					if (instance == components->end() || !instance->is_object())
						continue;
					const auto prefab = instance->find("Prefab");
					if (prefab == instance->end())
						continue;
					if (const Result<UUID> handle = JsonReader(*prefab).ReadUUID(); handle && handle->IsValid())
						nested.insert(*handle);
				}
			}

			for (const auto& [prefab, file] : files)
			{
				// On a cycle when it reaches itself through the prefabs it nests.
				std::set<AssetHandle> seen;
				std::vector<AssetHandle> pending{ prefab };
				bool cycle = false;
				while (!pending.empty() && !cycle)
				{
					const auto found = nests.find(pending.back());
					pending.pop_back();
					if (found == nests.end())
						continue;
					for (const AssetHandle next : found->second)
					{
						cycle = cycle || next == prefab;
						if (seen.insert(next).second)
							pending.push_back(next);
					}
				}
				if (!cycle)
					continue;
				DiagnosticSite site;
				site.File = file;
				site.Subject = prefab.ToString();
				CollectedDiagnostic collected = MakeDiagnostic(PrefabCycleCode, DiagnosticSeverity::Error,
					"the prefab nests itself, directly or through other prefabs, so a change of it could never finish propagating", site,
					"remove the nested instance that closes the cycle from one of the prefab files", false);
				collected.Diagnostic.Asset = prefab.ToString();
				diagnostics.push_back(std::move(collected));
			}
		}

		constexpr std::string_view EntitiesPointer = "/Entities/";
		constexpr std::string_view ScriptFieldsPointer = "/Components/Script/Fields/";

		// The entity index of a pointer "/Entities/<i>/Components/Script/Fields/<name>[/...]" (MapScriptFieldLoadCode), or
		// nullopt for any other pointer.
		static std::optional<size_t> ParseScriptFieldEntityIndex(std::string_view pointer)
		{
			const size_t fields = pointer.find(ScriptFieldsPointer);
			if (!pointer.starts_with(EntitiesPointer) || fields == std::string_view::npos || fields <= EntitiesPointer.size())
				return std::nullopt;
			const std::string_view digits = pointer.substr(EntitiesPointer.size(), fields - EntitiesPointer.size());
			size_t index = 0;
			const auto [end, parsed] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
			if (parsed != std::errc() || end != digits.data() + digits.size())
				return std::nullopt;
			return index;
		}

		// The Script asset handle the Script component of entity `index` of a scene or prefab document names, or the invalid
		// handle when it names none.
		static AssetHandle FindEntityScriptHandle(const Json& document, size_t index)
		{
			if (!document.is_object())
				return {};
			const auto entities = document.find("Entities");
			if (entities == document.end() || !entities->is_array() || index >= entities->size())
				return {};
			const Json& entity = (*entities)[index];
			const auto components = entity.is_object() ? entity.find("Components") : entity.end();
			if (!entity.is_object() || components == entity.end() || !components->is_object())
				return {};
			const auto script = components->find("Script");
			if (script == components->end() || !script->is_object())
				return {};
			const auto handle = script->find("Script");
			return handle != script->end() && handle->is_string() ? JsonReader(*handle).ReadUUID().value_or(AssetHandle()) : AssetHandle();
		}

		// Whether the Script.Fields values of an entity whose script is `handle` resolve against declared fields: `handle`
		// names a registered Script asset whose fields load and which is a Behaviour. Validate checks only those values
		// (CheckSceneScripts, CheckPrefabScriptFields); the asset reference, SCRIPT_NOT_A_BEHAVIOUR and script source checks
		// report the others' scripts, never their values.
		static bool HasDeclaredScriptFields(const EditorContext& editor, AssetHandle handle)
		{
			const EditorScriptService* service = editor.GetScriptService();
			if (service == nullptr || !handle.IsValid() || editor.GetAssets().GetAssetType(handle) != AssetType::Script)
				return false;
			const Result<AssetRef<ScriptData>> declared = service->GetFields(handle);
			return declared.has_value() && (*declared)->Kind == ScriptKind::Behaviour;
		}

		// The Script component field values of the project's prefab files, under the codes and at the place a scene's values
		// get (CheckSceneScripts): SCRIPT_UNKNOWN_FIELD_OVERRIDE or SCRIPT_FIELD_TYPE_MISMATCH on the prefab file and entity,
		// component "Script", field "Fields.<name>". They are found as the strict open and the exporter find them
		// (ProjectValidator::FindScriptFieldProblems), so the three name the same code for the same entity (ADR 0026, friction
		// S1-04). An entity whose script is not a registered Behaviour with loadable fields is left out there, as
		// CheckSceneScripts leaves it: the asset reference, SCRIPT_NOT_A_BEHAVIOUR and script source checks report it.
		static Status CheckPrefabScriptFields(const EditorContext& editor, std::vector<CollectedDiagnostic>& diagnostics)
		{
			if (editor.GetScriptService() == nullptr)
				return {};
			for (const AssetRecord* record : editor.GetAssets().GetRegistry().GetRecords())
			{
				const AssetMetadata& metadata = record->Metadata;
				if (metadata.Kind != AssetMetaKind::Asset || metadata.Importer != "Prefab")
					continue;
				ENGINE_TRY_ASSIGN(const std::vector<LoadDiagnostic> problems, ProjectValidator::FindScriptFieldProblems(editor, record->SourcePath));
				const std::string file(record->SourcePath.GetPath());
				for (const LoadDiagnostic& problem : problems)
				{
					// "/Entities/<i>/Components/Script/Fields/<name>[/<inside the value>]" (MapScriptFieldLoadCode).
					const std::string_view pointer = problem.JsonPointer;
					const size_t fields = pointer.find(ScriptFieldsPointer);
					if (fields == std::string_view::npos)
						continue;
					const std::string_view value = pointer.substr(fields + ScriptFieldsPointer.size() - 1); // "/<name>[/...]"
					const std::string name = FirstPointerSegment(value);
					if (name.empty())
						continue;
					const size_t nameEnd = value.find('/', 1);
					const std::string_view inside = nameEnd == std::string_view::npos ? std::string_view() : value.substr(nameEnd);

					DiagnosticSite site{ .File = file, .Entity = FormatOptionalUUID(problem.Entity), .Component = "Script", .Field = "Fields." + name, .Subject = name };
					if (problem.Code == ScriptUnknownOverrideCode)
					{
						diagnostics.push_back(MakeDiagnostic(ScriptUnknownOverrideCode, DiagnosticSeverity::Error,
							std::format("script declares no field '{}'; the prefab's value is preserved", name), site,
							"remove or rename the value to a declared field", false));
						continue;
					}
					site.Subject = name + std::string(inside);
					diagnostics.push_back(MakeDiagnostic(ScriptFieldMismatchCode, DiagnosticSeverity::Error,
						std::format("{}{}: {}; the declared default is used", name, inside, problem.Message), site,
						"correct the value to match its declared type and constraints", false));
				}
			}
			return {};
		}

		// Sorts `diagnostics` (by file, entity, code, component, field, id) and drops repeats of one id: the same problem found
		// twice (two load diagnostics of one code at one pointer) is one diagnostic, so ids are unique within a report.
		static void SortDiagnostics(std::vector<CollectedDiagnostic>& diagnostics)
		{
			std::sort(diagnostics.begin(), diagnostics.end(), [](const CollectedDiagnostic& left, const CollectedDiagnostic& right)
			{
				const ProjectDiagnostic& a = left.Diagnostic;
				const ProjectDiagnostic& b = right.Diagnostic;
				return std::tie(a.File, a.Entity, a.Code, a.Component, a.Field, a.Id) < std::tie(b.File, b.Entity, b.Code, b.Component, b.Field, b.Id);
			});
			const auto repeats = std::unique(diagnostics.begin(), diagnostics.end(), [](const CollectedDiagnostic& left, const CollectedDiagnostic& right)
			{
				return left.Diagnostic.Id == right.Diagnostic.Id;
			});
			diagnostics.erase(repeats, diagnostics.end());

			std::set<std::string> ids;
			for (const CollectedDiagnostic& collected : diagnostics)
				ENGINE_ASSERT(ids.insert(collected.Diagnostic.Id).second, "Diagnostic id '{}' appears twice in one report", collected.Diagnostic.Id);
		}

		static ValidationReport MakeReport(const std::vector<CollectedDiagnostic>& diagnostics)
		{
			ValidationReport report;
			report.Diagnostics.reserve(diagnostics.size());
			for (const CollectedDiagnostic& collected : diagnostics)
			{
				if (collected.Diagnostic.Severity == DiagnosticSeverity::Error)
					++report.ErrorCount;
				else
					++report.WarningCount;
				report.Diagnostics.push_back(collected.Diagnostic);
			}
			return report;
		}

		static Status CheckScriptSources(const EditorContext& editor, ValidationScope scope, std::vector<CollectedDiagnostic>& diagnostics)
		{
			std::vector<VfsPath> paths;
			if (scope == ValidationScope::Project)
			{
				ENGINE_TRY_ASSIGN(const VfsPath root, VfsPath::Create("project", "Assets"));
				if (!editor.GetVfs().Exists(root))
					return {};
				ENGINE_TRY_ASSIGN(const auto entries, editor.GetVfs().List(root, true));
				for (const auto& entry : entries)
				{
					if (!entry.Info.IsDirectory && entry.Path.GetExtension() == ".luau")
						paths.push_back(entry.Path);
				}
			}
			else if (editor.HasScene())
			{
				editor.GetScene().ForEachCanonical([&editor, &paths](ConstEntity entity)
				{
					if (!entity.HasComponent<ScriptComponent>())
						return;
					const auto* record = editor.GetAssets().GetRegistry().Find(entity.GetComponent<ScriptComponent>().Script.GetHandle());
					if (record != nullptr && record->Metadata.Type == AssetType::Script)
						paths.push_back(record->SourcePath);
				});
			}
			if (paths.empty())
				return {};
			std::ranges::sort(paths);
			paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
			const EditorScriptService* service = editor.GetScriptService();
			if (service == nullptr)
				return MakeError(ErrorCode::InvalidState, "script validation service is unavailable");
			ENGINE_TRY_ASSIGN(const auto checked, service->Check(paths));
			// The checker's syntax errors (SCRIPT_COMPILE_ERROR) by file and line: ValidateScriptSource finds the same ones.
			std::set<std::pair<std::string, uint32_t>> checkerCompileErrors;
			for (const auto& diagnostic : checked.Diagnostics)
			{
				// The checker's own code: SCRIPT_COMPILE_ERROR for syntax errors, SCRIPT_TYPE_ERROR for type findings.
				const bool compile = diagnostic.Code == ScriptCompileErrorCode;
				if (compile)
					checkerCompileErrors.emplace(diagnostic.File, diagnostic.Line);
				DiagnosticSite site{ .File = diagnostic.File, .Subject = std::format("{}:{}:{}:{}:{}", diagnostic.Line, diagnostic.Column, diagnostic.EndLine, diagnostic.EndColumn, diagnostic.Message) };
				auto finding = MakeDiagnostic(compile ? ScriptCompileErrorCode : ScriptTypeErrorCode, diagnostic.Severity, diagnostic.Message, site,
					std::format("correct the script at {}:{} (range ends at {}:{})", diagnostic.Line, diagnostic.Column, diagnostic.EndLine, diagnostic.EndColumn), false);
				finding.Diagnostic.Line = diagnostic.Line;
				diagnostics.push_back(std::move(finding));
			}
			std::vector<std::string> actions;
			for (const auto& entry : editor.GetProject().GetSettings().Input.Actions)
				actions.push_back(entry.first);
			for (const VfsPath& path : paths)
			{
				ENGINE_TRY_ASSIGN(const std::string source, service->Read(path));
				const auto sourceFindings = Detail::ValidateScriptSource(path.GetPath(), source, actions);
				bool syntaxFailure = false;
				for (const auto& diagnostic : sourceFindings)
				{
					syntaxFailure |= diagnostic.Code == ScriptCompileErrorCode;
					// One syntax error is one finding: the checker already reported it at this line.
					if (diagnostic.Code == ScriptCompileErrorCode && checkerCompileErrors.contains({ diagnostic.File, diagnostic.Line }))
						continue;
					DiagnosticSite site{ .File = diagnostic.File, .Subject = std::format("{}:{}:{}", diagnostic.Line, diagnostic.Column, diagnostic.Message) };
					auto finding = MakeDiagnostic(diagnostic.Code, diagnostic.Severity, diagnostic.Message, site,
						diagnostic.Code == InputUnknownActionCode ? "declare the action in Input.Actions or correct the literal" : "correct the Luau syntax", false);
					finding.Diagnostic.Line = diagnostic.Line;
					diagnostics.push_back(std::move(finding));
				}
				const auto* record = editor.GetAssets().GetRegistry().FindBySourcePath(path);
				if (!syntaxFailure && record != nullptr)
				{
					const AssetHandle handle = record->Metadata.Handle;
					const auto fields = service->GetFields(record->Metadata.Handle);
					std::string failure = fields ? std::string() : fields.error().GetMessageText();
					// Load may legitimately return the last good artifact after a failed reimport. That does not make the
					// latest source valid: report the attempt's diagnostic separately from the preserved asset.
					for (const AssetDiagnostic& attempt : editor.GetAssets().GetDiagnostics())
					{
						if (attempt.Asset == handle && attempt.Code == AssetImportFailedCode)
							failure = attempt.Message;
					}
					if (!failure.empty())
					{
						DiagnosticSite site{ .File = std::string(path.GetPath()) };
						diagnostics.push_back(MakeDiagnostic(ScriptCompileErrorCode, DiagnosticSeverity::Error, std::move(failure), site,
							"correct the load-time script or its required modules", false));
					}
				}
			}
			return {};
		}

		// The project's physics layer table (PhysicsSettings), which the physics checks resolve layer names against; nullopt,
		// logged, when the settings give none (a loaded project's always do: its settings passed the same rules).
		static std::optional<PhysicsLayerTable> MakePhysicsLayers(const EditorContext& context)
		{
			const PhysicsSettings& physics = context.GetProject().GetSettings().Physics;
			Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(physics.Layers, physics.Collisions);
			if (!layers)
			{
				ENGINE_WARN("Project validator: the physics checks are skipped, the project's physics layers give no layer table: {}", layers.error().ToString());
				return std::nullopt;
			}
			return std::move(*layers);
		}

		// Every diagnostic of `scope`, sorted, with its subject, and the asset scan their fixes are planned on
		// (ProjectValidator::Validate's work).
		static Result<Collection> CollectDiagnostics(const EditorContext& context, ValidationScope scope)
		{
			if (!context.HasProject())
				return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
			if (scope == ValidationScope::Scene && !context.HasScene())
			{
				return std::unexpected(
					Error(ErrorCode::InvalidState, "no scene open to validate").WithHint("open one with scene.open, or validate the scope \"project\""));
			}

			Collection collection;
			std::vector<CollectedDiagnostic>& diagnostics = collection.Diagnostics;
			ENGINE_TRY(CheckScriptSources(context, scope, diagnostics));
			const std::optional<PhysicsLayerTable> layers = MakePhysicsLayers(context);
			const PhysicsLayerTable* physicsLayers = layers.has_value() ? &*layers : nullptr;
			const std::optional<VfsPath>& openPath = context.GetScenePath();
			if (context.HasScene())
			{
				const CheckedScene open{ &context.GetScene(), openPath.has_value() ? std::string(openPath->GetPath()) : std::string(), true, physicsLayers };
				ENGINE_TRY(CheckScene(context, open, diagnostics));
			}

			if (scope == ValidationScope::Project)
			{
				CheckSettings(context, diagnostics);
				ENGINE_TRY_ASSIGN(collection.Scanned, ScanAssets(context, diagnostics));
				CollectImportDiagnostics(context, diagnostics);
				CheckAssetFiles(context, diagnostics);
				CheckPrefabNesting(context, diagnostics);
				ENGINE_TRY(CheckPrefabScriptFields(context, diagnostics));
				ENGINE_TRY_ASSIGN(const std::vector<VfsPath> scenes, ListSceneFiles(context));
				for (const VfsPath& scene : scenes)
				{
					// The open scene's in-memory state replaces its file (it was checked above).
					if (context.HasScene() && openPath.has_value() && *openPath == scene)
						continue;
					ENGINE_TRY(CheckSceneFile(context, scene, physicsLayers, diagnostics));
				}
			}
			SortDiagnostics(diagnostics);
			return collection;
		}

		static Status FixScriptReferences(EditorContext& editor, Entity entity, const std::vector<const CollectedDiagnostic*>& fixes)
		{
			if (!entity.HasComponent<ScriptComponent>())
				return MakeError(ErrorCode::Conflict, "script component changed after validation");
			ScriptComponent component = entity.GetComponent<ScriptComponent>();
			std::map<std::string, Json> replacements;
			for (const CollectedDiagnostic* collected : fixes)
			{
				const ScriptReferenceFix& fix = *collected->ScriptReference;
				if (component.Script.GetHandle() != fix.Script || editor.GetAssets().GetVersion(fix.Script) != fix.Version)
					return MakeError(ErrorCode::Conflict, "script schema changed after validation");
				const auto stored = component.Fields.find(fix.Field);
				if (fix.Stored ? stored == component.Fields.end() || stored->second != *fix.Stored : stored != component.Fields.end())
					return MakeError(ErrorCode::Conflict, "script override changed after validation");
				auto [replacement, inserted] = replacements.try_emplace(fix.Field, fix.Effective.Get());
				static_cast<void>(inserted);
				Json* value = &replacement->second;
				for (const size_t index : fix.Indices)
				{
					if (!value->is_array() || index >= value->size())
						return MakeError(ErrorCode::Conflict, "script reference shape changed after validation");
					value = &(*value)[index];
				}
				*value = nullptr;
			}
			const ScriptReferenceFix& first = *fixes.front()->ScriptReference;
			for (auto& [name, value] : replacements)
			{
				ENGINE_TRY_ASSIGN(const FieldInfo* field, first.Schemas->FindField(first.Script, name));
				ValidationContext validation;
				field->ValidateJson(JsonReader(value), {}, validation);
				ENGINE_TRY(validation.ToStatus("script reference fix"));
				component.Fields.insert_or_assign(name, VariantValue(std::move(value)));
			}
			// Validate the changed fields through their pinned schema, then publish one tracked patch. Revalidating the
			// whole component would reject unrelated legacy overrides that the script contract requires us to preserve.
			entity.Patch<ScriptComponent>([&component](ScriptComponent& target)
			{
				target.Fields = std::move(component.Fields);
			});
			return {};
		}

		// The scene fixes (cameras, audio listeners, adjacent static bodies, dangling references) of the selected diagnostics,
		// as one SceneEdit of the open scene.
		static Status FixOpenScene(EditorContext& editor, const std::vector<const CollectedDiagnostic*>& selected)
		{
			const bool hasSceneFix = std::ranges::any_of(selected, [](const CollectedDiagnostic* collected)
			{
				return collected->Diagnostic.Code != BuildSceneMissingCode && !collected->Scan.has_value();
			});
			if (!hasSceneFix)
				return {};
			ENGINE_ASSERT(editor.HasScene(), "A scene fix needs the open scene");

			Scene& scene = editor.GetScene();
			SceneEdit edit(editor, std::string(FixLabel));
			for (const CollectedDiagnostic* collected : selected)
			{
				const std::string& code = collected->Diagnostic.Code;
				if (code != SceneMultiplePrimaryCamerasCode && code != SceneNoPrimaryCameraCode)
					continue;
				std::vector<Entity> cameras;
				scene.ForEachCanonical([&cameras](Entity entity)
				{
					if (entity.HasComponent<CameraComponent>())
						cameras.push_back(entity);
				});
				// Several Primary cameras: the first in canonical order stays Primary. No Primary camera: the only one becomes it
				// (the diagnostic is auto-fixable only then).
				bool keptPrimary = false;
				for (const Entity camera : cameras)
				{
					const bool primary = camera.GetComponent<CameraComponent>().Primary;
					const bool wanted = code == SceneNoPrimaryCameraCode ? cameras.size() == 1 : (primary && !keptPrimary);
					keptPrimary = keptPrimary || wanted;
					if (primary == wanted)
						continue;
					Json patch = Json::object();
					patch["Primary"] = wanted;
					ENGINE_TRY(ComponentAccess::PatchComponentJson(camera, "Camera", patch));
				}
			}

			// Several primary audio listeners: each diagnostic names one after the first, whose Primary is cleared.
			for (const CollectedDiagnostic* collected : selected)
			{
				if (collected->Diagnostic.Code != AudioMultiplePrimaryListenersCode)
					continue;
				const std::optional<UUID> id = UUID::FromString(collected->Diagnostic.Entity);
				const Entity listener = id.has_value() ? scene.FindEntityByID(*id) : Entity();
				if (!listener.IsValid() || !listener.HasComponent<AudioListenerComponent>() || !listener.GetComponent<AudioListenerComponent>().Primary)
					continue;
				Json patch = Json::object();
				patch["Primary"] = false;
				ENGINE_TRY(ComponentAccess::PatchComponentJson(listener, "AudioListener", patch));
			}

			// Adjacent implicit static bodies (M11): a Static RigidBody on each fix target joins the bodies below it into one
			// static compound. A target below another selected target is left alone: the upper one's RigidBody gathers its
			// pieces too, which makes one compound instead of two.
			std::set<UUID> targets;
			for (const CollectedDiagnostic* collected : selected)
			{
				if (collected->Diagnostic.Code == PhysicsAdjacentStaticBodiesCode && collected->FixTarget.IsValid())
					targets.insert(collected->FixTarget);
			}
			for (const UUID target : targets)
			{
				const Entity entity = scene.FindEntityByID(target);
				if (!entity.IsValid() || entity.HasComponent<RigidBodyComponent>())
					continue;
				bool belowAnotherTarget = false;
				for (Entity ancestor = entity.GetParent(); ancestor.IsValid() && !belowAnotherTarget; ancestor = ancestor.GetParent())
					belowAnotherTarget = targets.contains(ancestor.GetUUID());
				if (belowAnotherTarget)
					continue;
				Json body = Json::object();
				body["Type"] = "Static";
				ENGINE_TRY(ComponentAccess::AddComponent(entity, "RigidBody", &body));
			}

			// Dangling references: every selected one of a component is cleared in one write of that component.
			std::map<std::pair<UUID, std::string>, std::vector<const CollectedDiagnostic*>> references;
			for (const CollectedDiagnostic* collected : selected)
			{
				if (collected->Diagnostic.Code != EntityDanglingReferenceCode)
					continue;
				const std::optional<UUID> id = UUID::FromString(collected->Diagnostic.Entity);
				if (id.has_value())
					references[{ *id, collected->Diagnostic.Component }].push_back(collected);
			}
			for (const auto& [key, fixes] : references)
			{
				const Entity entity = scene.FindEntityByID(key.first);
				const ComponentInfo* info = editor.GetTypeRegistry().FindComponent(key.second);
				if (!entity.IsValid() || info == nullptr)
					continue;
				if (fixes.front()->ScriptReference)
				{
					ENGINE_TRY(FixScriptReferences(editor, entity, fixes));
					continue;
				}
				ENGINE_TRY_ASSIGN(Json component, ComponentAccess::GetComponentJson(entity, key.second));
				const auto visit = [&scene, &fixes](const EntityRefSite& site, UUID id)
				{
					if (scene.FindEntityByID(id).IsValid())
						return false;
					const std::string subject = MakeReferenceSubject(site, id);
					return std::ranges::any_of(fixes, [&site, &subject](const CollectedDiagnostic* fix)
					{
						return fix->Diagnostic.Field == site.Field && fix->Subject == subject;
					});
				};
				VisitComponentEntityRefs(component, *info, visit);
				ENGINE_TRY(ComponentAccess::SetComponentJson(entity, key.second, component));
			}
			ENGINE_TRY(edit.Commit());
			return {};
		}

		// The settings fix of the selected BUILD_SCENE_MISSING diagnostics: one ProjectSettingsCommand removing their entries.
		static Status FixSettings(EditorContext& editor, const std::vector<const CollectedDiagnostic*>& selected)
		{
			std::set<std::string> removed;
			for (const CollectedDiagnostic* collected : selected)
			{
				if (collected->Diagnostic.Code == BuildSceneMissingCode)
					removed.insert(collected->Subject);
			}
			if (removed.empty())
				return {};

			Json scenes = Json::array();
			for (const std::string& scene : editor.GetProject().GetSettings().Export.BuildScenes)
			{
				if (!removed.contains(scene))
					scenes.push_back(scene);
			}
			Json patch = Json::object();
			patch["Export"] = Json::object();
			patch["Export"]["BuildScenes"] = std::move(scenes);
			ENGINE_TRY_ASSIGN(Scope<ProjectSettingsCommand> command, ProjectSettingsCommand::CreateFromPatch(editor, patch, std::string(FixLabel)));
			ENGINE_TRY(editor.Execute(std::move(command)));
			return {};
		}

		// The trash folder of one .meta moved there by a fix (§12.3: Library/Trash/<handle>, or the first free -<n> variant).
		static Result<VfsPath> MakeTrashDirectory(const EditorContext& editor, AssetHandle handle)
		{
			const std::string name = handle.IsValid() ? handle.ToString() : std::string("meta");
			for (uint32_t index = 1;; ++index)
			{
				const std::string entry = index == 1 ? name : std::format("{}-{}", name, index);
				ENGINE_TRY_ASSIGN(VfsPath directory, VfsPath::Create("project", std::format("Library/Trash/{}", entry)));
				if (!editor.GetVfs().Exists(directory))
					return directory;
			}
		}

		// A RewriteImporter fix's .meta text with its null Settings replaced by the new importer's complete defaults (§6.4:
		// a .meta stores every settings field), for an importer that has settings.
		static Result<std::string> CompleteRewrittenMeta(const EditorContext& editor, std::string_view planned)
		{
			ENGINE_TRY_ASSIGN(AssetMetadata metadata, ParseAssetMetadata(planned));
			Result<VariantValue> defaults = editor.GetAssets().MergeImportSettings(metadata.Importer, VariantValue(), Json());
			if (defaults.has_value())
				metadata.Settings = std::move(*defaults);
			else if (defaults.error().GetCode() != ErrorCode::InvalidArgument) // InvalidArgument: an importer without settings
				return std::unexpected(std::move(defaults).error());
			return SerializeAssetMetadata(metadata);
		}

		// The asset scan fixes of the selected diagnostics (AssetRegistry::PlanFix on the validator's scan), each as an asset
		// command: a fresh handle written into the copy's .meta, an orphan .meta moved to the trash, a .meta renamed to its
		// source's spelling, a .meta rewritten for the importer that takes its source.
		static Status FixAssets(EditorContext& editor, const std::optional<AssetRegistry>& scanned, const std::vector<const CollectedDiagnostic*>& selected)
		{
			for (const CollectedDiagnostic* collected : selected)
			{
				if (!collected->Scan.has_value())
					continue;
				ENGINE_ASSERT(scanned.has_value(), "An asset scan fix needs the validator's scan");
				const AssetHandle fresh = collected->Scan->Code == AssetDuplicateHandleCode ? editor.GetIdGenerator().Next() : AssetHandle();
				ENGINE_TRY_ASSIGN(const AssetScanFix fix, scanned->PlanFix(*collected->Scan, fresh));
				switch (fix.Kind)
				{
					case AssetScanFixKind::AssignNewHandle:
					{
						ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command,
							AssetEditCommand::CreateForWrite(editor, fix.MetaPath, AsBytes(fix.NewMetaText), std::string(FixLabel)));
						ENGINE_TRY(editor.Execute(std::move(command)));
						break;
					}
					case AssetScanFixKind::TrashMeta:
					{
						ENGINE_TRY_ASSIGN(const VfsPath trash, MakeTrashDirectory(editor, collected->Scan->Asset));
						ENGINE_TRY_ASSIGN(VfsPath destination, trash.Join(fix.MetaPath.GetPath()));
						std::vector<AssetFileMove> moves = { { .From = fix.MetaPath, .To = std::move(destination) } };
						ENGINE_TRY(editor.Execute(CreateScope<AssetMoveCommand>(std::string(FixLabel), std::move(moves))));
						break;
					}
					case AssetScanFixKind::RenameMetaToSourceCase:
					{
						std::vector<AssetFileMove> moves = { { .From = fix.MetaPath, .To = fix.NewMetaPath } };
						ENGINE_TRY(editor.Execute(CreateScope<AssetMoveCommand>(std::string(FixLabel), std::move(moves))));
						break;
					}
					case AssetScanFixKind::RewriteImporter:
					{
						ENGINE_TRY_ASSIGN(const std::string text, CompleteRewrittenMeta(editor, fix.NewMetaText));
						ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command,
							AssetEditCommand::CreateForWrite(editor, fix.MetaPath, AsBytes(text), std::string(FixLabel)));
						ENGINE_TRY(editor.Execute(std::move(command)));
						break;
					}
				}
			}
			return {};
		}

	}

	Result<ValidationReport> ProjectValidator::Validate(const EditorContext& context, ValidationScope scope)
	{
		ENGINE_TRY_ASSIGN(const Collection collection, Utils::CollectDiagnostics(context, scope));
		return Utils::MakeReport(collection.Diagnostics);
	}

	Result<FixReport> ProjectValidator::Fix(EditorContext& context, ValidationScope scope, const FixSelection& selection)
	{
		ENGINE_TRY_ASSIGN(const Collection collection, Utils::CollectDiagnostics(context, scope));
		const std::vector<CollectedDiagnostic>& diagnostics = collection.Diagnostics;

		const std::span<const std::string_view> codes = GetCodes();
		std::set<std::string> selectedIds;
		for (size_t index = 0; index < selection.IdsOrCodes.size(); ++index)
		{
			const std::string& item = selection.IdsOrCodes[index];
			const bool isCode = std::ranges::find(codes, std::string_view(item)) != codes.end();
			bool matched = false;
			for (const CollectedDiagnostic& collected : diagnostics)
			{
				if (collected.Diagnostic.Id == item || (isCode && collected.Diagnostic.Code == item))
				{
					selectedIds.insert(collected.Diagnostic.Id);
					matched = true;
				}
			}
			if (!matched && !isCode)
			{
				const std::string pointer = std::format("/fix/{}", index);
				ErrorLocation location;
				location.JsonPointer = pointer;
				ErrorIssue issue;
				issue.JsonPointer = pointer;
				issue.Message = std::format("'{}' is neither the id of a current diagnostic nor a validator code", item);
				issue.Hint = "validate again for the current ids";
				return std::unexpected(Error(ErrorCode::InvalidArgument, issue.Message)
						.WithHint("validate again for the current ids; the codes are listed in the generated reference")
						.WithLocation(std::move(location))
						.WithIssue(std::move(issue)));
			}
		}

		std::vector<const CollectedDiagnostic*> selected;
		for (const CollectedDiagnostic& collected : diagnostics)
		{
			if (collected.Diagnostic.AutoFixable && (selection.All || selectedIds.contains(collected.Diagnostic.Id)))
				selected.push_back(&collected);
		}
		if (selected.empty())
		{
			FixReport unchanged;
			unchanged.After = Utils::MakeReport(diagnostics);
			return unchanged;
		}

		EditorTransaction transaction(context, std::string(FixLabel));
		Status applied = Utils::FixOpenScene(context, selected);
		if (applied)
			applied = Utils::FixSettings(context, selected);
		if (applied)
			applied = Utils::FixAssets(context, collection.Scanned, selected);
		if (!applied)
		{
			Error error = std::move(applied).error();
			const Status rolledBack = transaction.Rollback();
			if (!rolledBack)
				error = Error(error).WithContext(std::format("rolling the fixes back failed too: {}", rolledBack.error().ToString()));
			return std::unexpected(std::move(error));
		}

		FixReport fixed;
		fixed.UndoIndex = transaction.Commit();
		for (const CollectedDiagnostic* collected : selected)
			fixed.Fixed.push_back(collected->Diagnostic.Id);
		// The asset manager's registry follows the fixed files at once: its scan diagnostics gate play and export (§7.2).
		const bool fixedAssets = std::ranges::any_of(selected, [](const CollectedDiagnostic* collected)
		{
			return collected->Scan.has_value();
		});
		if (fixedAssets && context.GetAssets().HasProject())
		{
			if (Result<AssetRefreshReport> refreshed = context.GetAssets().Refresh(); !refreshed.has_value())
				ENGINE_WARN("Project validator: refreshing the assets after the fixes failed: {}", refreshed.error().ToString());
		}
		ENGINE_TRY_ASSIGN(fixed.After, Validate(context, scope));
		return fixed;
	}

	std::string ProjectValidator::MakeDiagnosticId(std::string_view code, std::string_view file, std::string_view entity, std::string_view component,
		std::string_view field, std::string_view subject)
	{
		const std::string key = std::format("{}|{}|{}|{}|{}|{}", code, file, entity, component, field, subject);
		const std::string digest = std::format("{:016x}", XXH64(key));
		return std::format("{}-{}", code, std::string_view(digest).substr(0, 12));
	}

	std::span<const std::string_view> ProjectValidator::GetCodes()
	{
		return ValidatorCodes;
	}

	std::span<const ValidationCodeInfo> ProjectValidator::GetCodeInfos()
	{
		return ValidatorCodeInfos;
	}

	std::string_view ProjectValidator::MapLoadCode(std::string_view loadCode)
	{
		// ADR 0006 decision 35. SCENE_INVALID_COMPONENT has no single validator code: the validator runs its own registry
		// checks for the COMPONENT_* codes instead of splitting it by cause.
		if (loadCode == SceneDuplicateIdCode)
			return EntityDuplicateIdCode;
		if (loadCode == SceneDanglingParentCode || loadCode == SceneParentCycleCode)
			return SceneInvalidHierarchyCode;
		if (loadCode == SceneDuplicateUniqueComponentCode)
			return SceneDuplicateUniqueComponentCode;
		if (loadCode == SceneNonCanonicalOrderCode)
			return SceneNonCanonicalOrderCode;
		if (loadCode == SceneInconsistentPrefabLinkCode)
			return SceneInconsistentPrefabLinkCode;
		return {};
	}

	std::string_view ProjectValidator::MapScriptFieldLoadCode(std::string_view loadCode, std::string_view jsonPointer)
	{
		// "/Entities/<i>/Components/Script/Fields/<name>[/...]": a value of an entity's own Script component. An override's
		// value (".../Components/Prefab/Overrides/<k>/Value/...") is not one: the member it applies to reports it.
		constexpr std::string_view FieldsPointer = "/Components/Script/Fields/";
		const size_t fields = jsonPointer.find(FieldsPointer);
		if (fields == std::string_view::npos || !jsonPointer.starts_with("/Entities/")
			|| jsonPointer.substr(0, fields).find('/', std::string_view("/Entities/").size()) != std::string_view::npos)
			return {};
		if (loadCode == VariantUnresolvedCode)
			return ScriptUnknownOverrideCode;
		if (loadCode == VariantSchemaMismatchCode)
			return ScriptFieldMismatchCode;
		return {};
	}

	Result<std::vector<LoadDiagnostic>> ProjectValidator::FindScriptFieldProblems(const EditorContext& context, const VfsPath& path)
	{
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
		ENGINE_TRY_ASSIGN(const Ref<const ScriptFieldSchemaSource> schemas, context.GetScriptSchemaSnapshot());
		LoadOptions options;
		options.Mode = LoadMode::Strict;
		options.Schemas = schemas.get();
		options.SourcePath = std::string(path.GetPath());
		LoadReport report;
		Status loaded;
		if (path.GetExtension() == ".prefab")
		{
			if (Result<Prefab> prefab = Prefab::LoadFromFile(context.GetVfs(), path, context.GetTypeRegistry(), options, report); !prefab)
				loaded = std::unexpected(std::move(prefab).error());
		}
		else
		{
			UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
			const Scope<Scene> scratch = Scene::Create({ .Name = std::string(path.GetStem()), .Seed = 0, .Registry = &context.GetTypeRegistry(), .IdGenerator = &scratchIds, .Runtime = false });
			loaded = SceneSerializer::LoadFromFile(*scratch, context.GetVfs(), path, options, report);
		}
		std::vector<LoadDiagnostic> problems;
		if (!loaded)
			return problems;
		// The document, read once a diagnostic maps, for the script of the entity each one locates.
		std::optional<Json> document;
		for (LoadDiagnostic& diagnostic : report.Diagnostics)
		{
			const std::string_view code = MapScriptFieldLoadCode(diagnostic.Code, diagnostic.JsonPointer);
			if (code.empty())
				continue;
			if (!document.has_value())
			{
				const Result<std::string> text = context.GetVfs().ReadText(path);
				document = text.has_value() ? JsonReader::Parse(*text).value_or(Json()) : Json();
			}
			// Validate checks only the values of a registered Behaviour whose fields load: a missing or mistyped script, one
			// that fails to import or a Module leaves every value unresolved, and validate reports the script instead
			// (CheckSceneScripts), so neither code is named here.
			const std::optional<size_t> entity = Utils::ParseScriptFieldEntityIndex(diagnostic.JsonPointer);
			if (!entity.has_value() || !Utils::HasDeclaredScriptFields(context, Utils::FindEntityScriptHandle(*document, *entity)))
				continue;
			diagnostic.Code = std::string(code);
			problems.push_back(std::move(diagnostic));
		}
		return problems;
	}

	Result<std::vector<ProjectDiagnostic>> ProjectValidator::CheckTestSuites(const EditorContext& context)
	{
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "no project open; call project.create or project.open");
		std::vector<CollectedDiagnostic> collected;
		Utils::CheckTestSuiteSettings(context, collected);
		std::vector<ProjectDiagnostic> diagnostics;
		diagnostics.reserve(collected.size());
		for (CollectedDiagnostic& entry : collected)
			diagnostics.push_back(std::move(entry.Diagnostic));
		return diagnostics;
	}

	void RegisterProjectValidatorTypes(TypeRegistry& registry)
	{
		registry.Enum<ValidationScope>("ValidationScope", "What project.validate checks.")
			.Entry(ValidationScope::Project, "Project", "The settings and every scene file under Assets/ (the open scene as it is in memory).")
			.Entry(ValidationScope::Scene, "Scene", "The open scene only.");

		registry.Struct<ProjectDiagnostic>("ProjectDiagnostic", "One problem the project validator found (§13.7).")
			.Field("id", &ProjectDiagnostic::Id, "Stable id of the problem: the code and 12 hex digits of its location; project.validate {fix} selects by it.")
			.Field("severity", &ProjectDiagnostic::Severity, "Warning or Error.")
			.Field("code", &ProjectDiagnostic::Code, "The diagnostic code, such as \"SCENE_MULTIPLE_PRIMARY_CAMERAS\".")
			.Field("message", &ProjectDiagnostic::Message, "What is wrong.")
			.Field("entity", &ProjectDiagnostic::Entity, "The entity's id (16 hex digits); empty when the problem is not about one entity.")
			.Field("component", &ProjectDiagnostic::Component, "The component's registry name; empty when none.")
			.Field("field", &ProjectDiagnostic::Field, "The field or setting; empty when none.")
			.Field("asset", &ProjectDiagnostic::Asset, "The asset handle; empty when none.")
			.Field("file", &ProjectDiagnostic::File, "The project-relative file; empty for an unsaved scene.")
			.Field("line", &ProjectDiagnostic::Line, "The 1-based line in a text file; 0 when none.")
			.Field("hint", &ProjectDiagnostic::Hint, "How to fix it.")
			.Field("autoFixable", &ProjectDiagnostic::AutoFixable, "Whether project.validate {fix} can fix it.");
	}

}
