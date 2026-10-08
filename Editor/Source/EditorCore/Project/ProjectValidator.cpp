#include "EditorPCH.h"
#include "EditorCore/Project/ProjectValidator.h"

#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/AssetMoveCommand.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
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
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <tuple>

// The validator (Architecture §13.7, ADR 0008 decision 17; the asset checks of M6, ADR 0010 decision 5). The code list and
// the load-code mapping (ADR 0006 decision 35) are data the contracts froze.

namespace Engine {

	namespace {

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
			BuildStartSceneMissingCode,
			BuildSceneMissingCode,
		};

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

		// One scene the validator checks: the open scene (whose checks may fix it) or a scratch copy of a file.
		struct CheckedScene
		{
			const Scene* Target = nullptr;
			std::string File{};
			bool IsOpenScene = false;
		};

		// The position of one EntityRef value inside a component, as diagnostics name it (ProjectValidator::MakeDiagnosticId):
		// its top-level field, and the subject that tells apart several problems in one field.
		struct EntityRefSite
		{
			std::string Field{};
			std::string MapKey{}; // the first map key above the value; empty when none
			bool InArray = false; // the value is an array element (or below one)
		};

		// A diagnostic with the subject its id was made from, which fixes need (ProjectDiagnostic does not carry it), and for
		// an asset scan diagnostic the registry's own diagnostic, which AssetRegistry::PlanFix identifies.
		struct CollectedDiagnostic
		{
			ProjectDiagnostic Diagnostic{};
			std::string Subject{};
			std::optional<AssetDiagnostic> Scan{};
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
		static void CheckAssetReference(const EditorContext& editor, AssetHandle handle, std::string_view acceptedType, const DiagnosticSite& site,
			std::string_view where, std::vector<CollectedDiagnostic>& diagnostics)
		{
			const EditorAssetManager& assets = editor.GetAssets();
			const AssetType actual = assets.GetAssetType(handle);
			const std::string expected = acceptedType.empty() ? std::string("asset") : std::string(acceptedType);
			CollectedDiagnostic collected;
			if (actual == AssetType::None && assets.GetMetadata(handle) == nullptr)
			{
				collected = MakeDiagnostic(AssetMissingCode, DiagnosticSeverity::Error,
					std::format("{} refers to the {} {}, which no asset has", where, expected, handle.ToString()), site,
					"restore the asset (edit.undo of its deletion, or from version control), or assign another one", false);
			}
			else if (actual == AssetType::None || (!acceptedType.empty() && AssetTypeToString(actual) != acceptedType))
			{
				const std::string what = actual == AssetType::None ? std::string("a dependency file, which is never loaded on its own")
																   : std::format("a {}", AssetTypeToString(actual));
				collected = MakeDiagnostic(AssetTypeMismatchCode, DiagnosticSeverity::Error,
					std::format("{} refers to {} ({}), but takes a {}", where, handle.ToString(), what, expected), site,
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
		// it is not registered: the scene still loads, fully expanded, §5.5).
		static void CheckComponentAssets(const EditorContext& editor, const ComponentInfo& info, const Json& component, const DiagnosticSite& at,
			std::string_view owner, std::vector<CollectedDiagnostic>& diagnostics)
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
						std::format("'{}' is an instance of the prefab {}, which is not registered; it stays as it is, fully expanded", owner,
							handle->ToString()),
						site, "restore the prefab asset, or unpack the instance (prefab.unpack)", false);
					collected.Diagnostic.Asset = handle->ToString();
					diagnostics.push_back(std::move(collected));
					return;
				}
				CheckAssetReference(editor, *handle, AssetTypeToString(AssetType::Prefab), site, std::format("the instance '{}'", owner), diagnostics);
				return;
			}

			const auto visit = [&](const EntityRefSite& reference, AssetHandle handle, std::string_view acceptedType)
			{
				DiagnosticSite site = at;
				site.Component = info.GetName();
				site.Field = reference.Field;
				site.Subject = MakeReferenceSubject(reference, handle);
				CheckAssetReference(editor, handle, acceptedType, site, std::format("'{}.{}' of '{}'", info.GetName(), reference.Field, owner),
					diagnostics);
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
					CheckComponentAssets(editor, *info, *component, site, scene.GetEntityPath(entity), diagnostics);
				}
			});
		}

		static void CheckScene(const EditorContext& editor, const CheckedScene& checked, std::vector<CollectedDiagnostic>& diagnostics)
		{
			CheckCameras(checked, diagnostics);
			CheckDanglingReferences(checked, diagnostics);
			CheckSceneAssets(editor, checked, diagnostics);
		}

		// Loads the scene file `path` into a scratch scene in Repair mode and checks it; a file that cannot be loaded at all is
		// ASSET_IMPORT_FAILED.
		static void CheckSceneFile(const EditorContext& editor, const VfsPath& path, std::vector<CollectedDiagnostic>& diagnostics)
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
			CheckedScene checked{ scratch.get(), std::string(path.GetPath()), false };
			const Status loaded = SceneSerializer::LoadFromFile(*scratch, editor.GetVfs(), path, options, report);
			if (!loaded)
			{
				DiagnosticSite site;
				site.File = checked.File;
				diagnostics.push_back(MakeDiagnostic(AssetImportFailedCode, DiagnosticSeverity::Error,
					std::format("the scene file cannot be loaded: {}", loaded.error().ToString()), site,
					"correct the file by hand or restore it from version control; scene.open reports where loading fails", false));
				return;
			}
			MapLoadReport(*scratch, report, checked, diagnostics);
			CheckScene(editor, checked, diagnostics);
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
						CheckAssetReference(editor, handle, acceptedType, site, std::format("'{}' of the material", reference.Field), diagnostics);
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
						CheckComponentAssets(editor, *info, component.value(), site, std::format("entity {} of the prefab", site.Entity), diagnostics);
					}
				}
			}
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
			const std::optional<VfsPath>& openPath = context.GetScenePath();
			if (context.HasScene())
			{
				const CheckedScene open{ &context.GetScene(), openPath.has_value() ? std::string(openPath->GetPath()) : std::string(), true };
				CheckScene(context, open, diagnostics);
			}

			if (scope == ValidationScope::Project)
			{
				CheckSettings(context, diagnostics);
				ENGINE_TRY_ASSIGN(collection.Scanned, ScanAssets(context, diagnostics));
				CollectImportDiagnostics(context, diagnostics);
				CheckAssetFiles(context, diagnostics);
				ENGINE_TRY_ASSIGN(const std::vector<VfsPath> scenes, ListSceneFiles(context));
				for (const VfsPath& scene : scenes)
				{
					// The open scene's in-memory state replaces its file (it was checked above).
					if (context.HasScene() && openPath.has_value() && *openPath == scene)
						continue;
					CheckSceneFile(context, scene, diagnostics);
				}
			}
			SortDiagnostics(diagnostics);
			return collection;
		}

		// The scene fixes (cameras, dangling references) of the selected diagnostics, as one SceneEdit of the open scene.
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
