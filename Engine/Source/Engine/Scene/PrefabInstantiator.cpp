#include "EnginePCH.h"
#include "Engine/Scene/PrefabInstantiator.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Private/EntityDocument.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		constexpr std::string_view ComponentsKey = "Components";
		constexpr std::array<std::string_view, 3> EntityKeyNames = { "Name", "Active", "Tags" };

		// A mapping between prefab-local IDs and instance IDs, in either direction. Lookup only, never iterated to produce
		// output (§5.1).
		using IdMap = std::unordered_map<UUID, UUID>;

		// Rewrites the entity references inside canonical entity and component JSON through an IdMap: "ID", "Parent", every
		// EntityRef value of a registered component (nested structs, arrays and maps included) and every Variant value whose
		// resolved schema contains EntityRefs (Entity-kind script fields through IFieldSchemaSource, §5.5). A reference the
		// map does not contain (an entity outside the instance, a value that cannot be resolved) is kept as it is.
		//
		// Mapping instance IDs back to prefab-local IDs is ambiguous for a reference the map does not contain whose UUID is a
		// prefab-local ID: kept as it is, it would name a prefab entity, and the next update would point it at the instance's
		// own member. Such references are appended to `ambiguous` when the caller passes the prefab's IDs as `reserved`.
		class EntityRefRemapper
		{
		public:
			EntityRefRemapper(const TypeRegistry& registry, const IFieldSchemaSource* schemas, const IdMap& map)
				: m_Registry(registry), m_Schemas(schemas), m_Map(map)
			{
			}

			// Also reports every reference the map does not contain but `reserved` does to `ambiguous`, which the caller owns
			// and reads after each remap (both outlive the remapper).
			EntityRefRemapper(const TypeRegistry& registry, const IFieldSchemaSource* schemas, const IdMap& map,
				const std::unordered_set<UUID>& reserved, std::vector<UUID>& ambiguous)
				: m_Registry(registry), m_Schemas(schemas), m_Map(map), m_Reserved(&reserved), m_Ambiguous(&ambiguous)
			{
			}

			// "ID", "Parent" and every registered component of the entity object `entity`.
			void RemapEntity(Json& entity) const
			{
				if (const auto id = entity.find("ID"); id != entity.end())
					RemapUUID(*id);
				if (const auto parent = entity.find("Parent"); parent != entity.end())
					RemapUUID(*parent);
				RemapComponents(entity);
			}

			// Every registered component of the entity object `entity`.
			void RemapComponents(Json& entity) const
			{
				const auto components = entity.find(ComponentsKey);
				if (components == entity.end() || !components->is_object())
					return;
				for (auto component = components->begin(); component != components->end(); ++component)
				{
					if (const ComponentInfo* info = m_Registry.FindComponent(component.key()))
						RemapStruct(*component, *info);
				}
			}

			// Every serialized stored field of the object `object` of `type`.
			void RemapStruct(Json& object, const StructInfo& type) const
			{
				if (!object.is_object())
					return;
				for (const Scope<FieldInfo>& field : type.GetFields())
				{
					if (!field->IsStored() || !field->GetMeta().Serialized)
						continue;
					if (const auto member = object.find(field->GetName()); member != object.end())
						RemapValue(*member, field->GetType(), *field, &type, &object, {});
				}
			}

			// The value `value` of `field`, a field of `ownerType` whose JSON object is `ownerJson` (Variant resolution reads
			// the members declared before the field from it).
			void RemapField(Json& value, const FieldInfo& field, const StructInfo& ownerType, const Json& ownerJson) const
			{
				RemapValue(value, field.GetType(), field, &ownerType, &ownerJson, {});
			}
		private:
			void RemapUUID(Json& value) const
			{
				if (!value.is_string())
					return;
				const Result<UUID> id = JsonReader(value).ReadUUID();
				if (!id)
					return;
				if (const auto mapped = m_Map.find(*id); mapped != m_Map.end())
					value = mapped->second.ToString();
				else if (m_Reserved != nullptr && m_Reserved->contains(*id))
					m_Ambiguous->push_back(*id);
			}

			void RemapValue(Json& value, const TypeInfo& type, const FieldInfo& field, const StructInfo* ownerType, const Json* ownerJson,
				std::string_view key) const
			{
				switch (type.GetKind())
				{
					case FieldType::EntityRef:
						RemapUUID(value);
						return;
					case FieldType::Struct:
					{
						if (type.GetStruct() != nullptr)
							RemapStruct(value, *type.GetStruct());
						return;
					}
					case FieldType::Array:
					{
						if (!value.is_array() || type.GetElement() == nullptr)
							return;
						for (Json& element : value)
							RemapValue(element, *type.GetElement(), field, ownerType, ownerJson, {});
						return;
					}
					case FieldType::Map:
					{
						if (!value.is_object() || type.GetElement() == nullptr)
							return;
						for (auto element = value.begin(); element != value.end(); ++element)
							RemapValue(*element, *type.GetElement(), field, ownerType, ownerJson, element.key());
						return;
					}
					case FieldType::Variant:
					{
						if (field.GetResolver() == nullptr || ownerJson == nullptr)
							return; // free-form JSON has no references to follow
						const JsonReader owner(*ownerJson);
						ResolveContext context;
						context.Registry = &m_Registry;
						context.OwnerType = ownerType;
						context.OwnerJson = &owner;
						context.Key = key;
						context.Schemas = m_Schemas;
						const Result<const FieldInfo*> resolved = field.ResolveVariant(context);
						if (!resolved || *resolved == &field)
							return; // unresolvable: kept verbatim, as every reader keeps it
						const FieldInfo& schema = **resolved;
						// A struct schema descends with the nested object as the owner (ResolveContext); RemapValue's Struct case
						// does exactly that.
						RemapValue(value, schema.GetType(), schema, ownerType, ownerJson, {});
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
						return; // no entity references
				}
				ENGINE_CORE_ASSERT(false, "Unknown FieldType {}", std::to_underlying(type.GetKind()));
			}
		private:
			const TypeRegistry& m_Registry;
			const IFieldSchemaSource* m_Schemas = nullptr;
			const IdMap& m_Map;
			const std::unordered_set<UUID>* m_Reserved = nullptr; // lookup only
			std::vector<UUID>* m_Ambiguous = nullptr;             // the caller's output
		};

		// The members of one instance: the root and every entity of its subtree whose PrefabLink names it, in canonical
		// order (the root first). Entities without such a link are user children (or members of other instances).
		struct InstanceMembers
		{
			std::vector<Entity> Members;
			IdMap ToPrefab;               // member ID -> prefab-local ID; the root maps to the prefab's root
			std::unordered_set<UUID> IDs; // lookup only
		};

		// One entity of the rebuilt instance: the prefab entity, its member ID, its parent in the instance and its JSON.
		struct MemberTarget
		{
			UUID PrefabID;
			UUID ID;
			UUID Parent;
			Json Object;
		};

	}

	namespace Utils {

		static Status RequireInstanceRoot(Entity entity)
		{
			if (!entity.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "the prefab instance root is not a valid entity");
			if (!entity.HasComponent<PrefabInstanceComponent>())
			{
				return MakeError(ErrorCode::InvalidArgument, "Entity '{}' ({}) is not a prefab instance root (it has no Prefab component)",
					entity.GetName(), entity.GetUUID());
			}
			return {};
		}

		static Status RequirePrefab(const Prefab& prefab)
		{
			if (prefab.IsEmpty())
				return MakeError(ErrorCode::InvalidArgument, "an empty prefab has no entities to instantiate");
			return {};
		}

		// The prefab-local ID of every entity of `prefab`.
		static std::unordered_set<UUID> CollectPrefabIDs(const Prefab& prefab)
		{
			const std::vector<UUID> ids = prefab.GetEntityIDs();
			return std::unordered_set<UUID>(ids.begin(), ids.end());
		}

		// The "ComponentVersions" of `prefab`'s document when one of its entities preserves an unknown component, whose
		// members carry the prefab's version (ADR 0006 decision 34); an empty object otherwise, so that the common case does
		// not copy the document.
		static Result<Json> GetUnknownComponentVersions(const TypeRegistry& registry, const Prefab& prefab)
		{
			const Json& entities = prefab.GetEntities();
			const bool hasUnknown = std::any_of(entities.begin(), entities.end(), [&registry](const Json& entity)
			{
				const auto components = entity.find(ComponentsKey);
				if (components == entity.end() || !components->is_object())
					return false;
				for (auto component = components->begin(); component != components->end(); ++component)
				{
					if (registry.FindComponent(component.key()) == nullptr)
						return true;
				}
				return false;
			});
			if (!hasUnknown)
				return Json::object();
			ENGINE_TRY_ASSIGN(Json document, prefab.ToJson());
			const auto versions = document.find("ComponentVersions");
			if (versions == document.end() || !versions->is_object())
				return Json::object();
			return std::move(*versions);
		}

		// The error for the reference to `target`, an entity outside the instance whose ID is also a prefab-local ID, found
		// at `pointer` in member `member` (EntityRefRemapper: storing it in prefab space would redirect it).
		static Error MakeAmbiguousReferenceError(UUID member, std::string pointer, UUID target)
		{
			ErrorLocation location;
			location.Entity = member;
			location.JsonPointer = std::move(pointer);
			return Error(ErrorCode::InvalidState,
				std::format("the reference to entity {} cannot be stored in prefab space: that entity is outside the instance, but {} is "
							"also the ID of a prefab entity, which names the instance's own member",
					target, target))
				.WithLocation(std::move(location))
				.WithHint("reference the instance's own member instead, or unpack the instance");
		}

		// True for a Map of Variants (Script.Fields): its keys are authored independently (§11.2), so an override records and
		// applies only the keys that differ, as an RFC 7386 patch whose null removes a key.
		static bool IsKeyedVariantMap(const FieldInfo& field)
		{
			const TypeInfo* element = field.GetType().GetElement();
			return field.GetKind() == FieldType::Map && element != nullptr && element->GetKind() == FieldType::Variant;
		}

		// The keys of the map `actual` whose values differ from those of `expected`, in sorted order as canonical maps are
		// written, with `actual`'s values and null for the keys `actual` lacks. A value that is not an object (never
		// canonical) differs as a whole.
		static Json DiffKeyedVariantMap(const Json& expected, const Json& actual)
		{
			if (!expected.is_object() || !actual.is_object())
				return actual;
			std::vector<std::string> keys;
			keys.reserve(expected.size() + actual.size());
			for (auto entry = expected.begin(); entry != expected.end(); ++entry)
				keys.push_back(entry.key());
			for (auto entry = actual.begin(); entry != actual.end(); ++entry)
				keys.push_back(entry.key());
			std::sort(keys.begin(), keys.end());
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

			Json patch = Json::object();
			for (const std::string& key : keys)
			{
				const auto value = actual.find(key);
				const auto previous = expected.find(key);
				if (value == actual.end())
					patch[key] = nullptr;
				else if (previous == expected.end() || *previous != *value)
					patch[key] = *value;
			}
			return patch;
		}

		// The ID `map` gives `id`; the invalid UUID when it has none.
		static UUID Lookup(const IdMap& map, UUID id)
		{
			const auto found = map.find(id);
			return found != map.end() ? found->second : UUID();
		}

		// The member ID of every prefab entity in the instance rooted at `rootID`.
		static IdMap MakeInstanceMap(const Prefab& prefab, UUID rootID)
		{
			IdMap map;
			for (const UUID id : prefab.GetEntityIDs())
				map.emplace(id, id == prefab.GetRootID() ? rootID : PrefabInstantiator::DeriveInstanceID(rootID, id));
			return map;
		}

		static InstanceMembers CollectMembers(Entity root, UUID rootPrefabID)
		{
			Scene& scene = *root.GetScene();
			InstanceMembers members;
			std::vector<Entity> stack{ root };
			while (!stack.empty())
			{
				const Entity entity = stack.back();
				stack.pop_back();
				if (entity == root)
				{
					members.Members.push_back(entity);
					members.ToPrefab.emplace(entity.GetUUID(), rootPrefabID);
					members.IDs.insert(entity.GetUUID());
				}
				else if (const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>();
					link != nullptr && link->InstanceRoot == root.GetUUID())
				{
					members.Members.push_back(entity);
					members.ToPrefab.emplace(entity.GetUUID(), link->PrefabEntityID);
					members.IDs.insert(entity.GetUUID());
				}

				const std::span<const UUID> children = entity.GetChildren();
				for (auto child = children.rbegin(); child != children.rend(); ++child)
				{
					const Entity handle = scene.FindEntityByID(*child);
					if (handle.IsValid())
						stack.push_back(handle);
				}
			}
			return members;
		}

		// The member of `object` named `key`, or JSON null.
		static const Json& GetMember(const Json& object, std::string_view key)
		{
			static const Json Null; // immutable after its thread-safe initialization
			const auto member = object.find(key);
			return member != object.end() ? *member : Null;
		}

		// Components a prefab override can target on a member: written under "Components" and not engine-maintained
		// (Prefab, PrefabLink). The instance root's Transform is an implicit override (§5.5) and never recorded.
		static bool IsOverridable(const ComponentInfo& info, bool isRoot, const ComponentInfo* transform)
		{
			if (!info.HasFlag(ComponentFlags::Serializable) || info.HasFlag(ComponentFlags::EntityLevel) || info.HasFlag(ComponentFlags::Hidden))
				return false;
			return info.GetHostOps() != nullptr && !(isRoot && &info == transform);
		}

		// The JSON of prefab entity `source` (prefab-local ID `prefabID`) as a member of the instance rooted at `rootID`, in
		// instance space: IDs and internal references through `toInstance`, and the member's PrefabLink. A root's "Parent"
		// stays null for the caller to set.
		static Result<Json> MakeMemberJson(const Json& source, UUID prefabID, UUID rootID, const EntityRefRemapper& toInstance,
			const ComponentInfo& linkInfo)
		{
			Json member = source;
			toInstance.RemapEntity(member);
			const PrefabLinkComponent link{ prefabID, rootID };
			ENGINE_TRY_ASSIGN(Json linkJson, linkInfo.ToJson(&link));
			member[ComponentsKey][linkInfo.GetName()] = std::move(linkJson);
			return member;
		}

		// Reads `json` into a fresh object of `type` (validated as a file read is, unresolvable Variant values kept) and
		// returns its canonical JSON.
		static Result<Json> Canonicalize(const StructInfo& type, const Json& json, const IFieldSchemaSource* schemas)
		{
			const ObjectPtr object = type.CreateDefault();
			ReadContext context;
			context.Schemas = schemas;
			ENGINE_TRY(type.FromJson(object.get(), JsonReader(json), context));
			return type.ToJson(object.get());
		}

		static bool IsLess(const PrefabOverride& left, const PrefabOverride& right)
		{
			if (left.PrefabEntityID != right.PrefabEntityID)
				return left.PrefabEntityID < right.PrefabEntityID;
			if (left.Component != right.Component)
				return left.Component < right.Component;
			if (left.Kind != right.Kind)
				return left.Kind < right.Kind;
			return left.Field < right.Field;
		}

		static bool AreEqual(const PrefabOverride& left, const PrefabOverride& right)
		{
			return left.PrefabEntityID == right.PrefabEntityID && left.Kind == right.Kind && left.Component == right.Component
				&& left.Field == right.Field && left.Value == right.Value;
		}

		// The registered name of `kind` ("AddComponent"), as files and diagnostics spell it.
		static std::string_view GetKindName(const TypeRegistry& registry, PrefabOverrideKind kind)
		{
			const EnumInfo* kinds = registry.FindEnum<PrefabOverrideKind>();
			const EnumEntry* entry = kinds != nullptr ? kinds->FindByValue(std::to_underlying(kind)) : nullptr;
			ENGINE_CORE_ASSERT(entry != nullptr, "Unknown PrefabOverrideKind {}", std::to_underlying(kind));
			return entry != nullptr ? std::string_view(entry->Name) : std::string_view("unknown");
		}

		// The overrides of one member: its entity keys (the root's Name excepted), the components it added or removed, and
		// every serialized field whose JSON differs from the instance-space prefab entity `expected` (only the differing keys
		// of a Map of Variants). Unknown components are not overridable: they have no schema and follow the prefab. Values
		// are mapped to prefab space through `toPrefab`, which reports ambiguous references to `ambiguous`. Errors:
		// InvalidState for an ambiguous reference (MakeAmbiguousReferenceError), located at the member.
		static Status DiffMember(const TypeRegistry& registry, UUID memberID, UUID prefabID, bool isRoot, const Json& expected,
			const Json& actual, const EntityRefRemapper& toPrefab, std::vector<UUID>& ambiguous, std::vector<PrefabOverride>& overrides)
		{
			const auto record = [&overrides, prefabID](PrefabOverrideKind kind, std::string component, std::string field, Json value)
			{
				overrides.push_back(PrefabOverride{ prefabID, kind, std::move(component), std::move(field), VariantValue(std::move(value)) });
			};
			const auto checkMapped = [&ambiguous, memberID](std::string pointer) -> Status
			{
				if (ambiguous.empty())
					return {};
				return std::unexpected(MakeAmbiguousReferenceError(memberID, std::move(pointer), ambiguous.front()));
			};

			for (const std::string_view key : EntityKeyNames)
			{
				if (isRoot && key == "Name")
					continue;
				const Json& value = GetMember(actual, key);
				if (value != GetMember(expected, key))
					record(PrefabOverrideKind::EntityKey, {}, std::string(key), value);
			}

			const ComponentInfo* transform = registry.FindComponent<TransformComponent>();
			const Json& actualComponents = GetMember(actual, ComponentsKey);
			const Json& expectedComponents = GetMember(expected, ComponentsKey);
			for (const ComponentInfo* info : registry.GetComponents())
			{
				if (!IsOverridable(*info, isRoot, transform))
					continue;
				const std::string& name = info->GetName();
				const auto present = actualComponents.find(name);
				const auto original = expectedComponents.find(name);
				const bool isPresent = present != actualComponents.end();
				const bool isOriginal = original != expectedComponents.end();
				if (!isPresent && isOriginal)
				{
					record(PrefabOverrideKind::RemoveComponent, name, {}, Json());
				}
				else if (isPresent && !isOriginal)
				{
					Json value = *present;
					toPrefab.RemapStruct(value, *info);
					ENGINE_TRY(checkMapped(JsonReader::AppendPointer("/Components", name)));
					record(PrefabOverrideKind::AddComponent, name, {}, std::move(value));
				}
				else if (isPresent)
				{
					for (const Scope<FieldInfo>& field : info->GetFields())
					{
						if (!field->IsStored() || !field->GetMeta().Serialized)
							continue;
						const Json& value = GetMember(*present, field->GetName());
						const Json& prefabValue = GetMember(*original, field->GetName());
						if (value == prefabValue)
							continue;
						Json mapped = IsKeyedVariantMap(*field) ? DiffKeyedVariantMap(prefabValue, value) : value;
						toPrefab.RemapField(mapped, *field, *info, *present);
						ENGINE_TRY(checkMapped(JsonReader::AppendPointer(JsonReader::AppendPointer("/Components", name), field->GetName())));
						record(PrefabOverrideKind::Field, name, field->GetName(), std::move(mapped));
					}
				}
			}
			return {};
		}

		// The UniquePerScene components the instance could hold once built: those under some target's "Components" and those
		// an AddComponent override names. Only these need a scan of the scene, so instantiating a prefab without one (the
		// common case: scripts spawning pieces at run time) never walks the scene.
		static std::vector<const ComponentInfo*> CollectUniqueCandidates(const TypeRegistry& registry, std::span<const MemberTarget> targets,
			std::span<const PrefabOverride> overrides)
		{
			std::vector<const ComponentInfo*> candidates;
			for (const ComponentInfo* info : registry.GetComponents())
			{
				if (!info->HasFlag(ComponentFlags::UniquePerScene) || info->GetHostOps() == nullptr)
					continue;
				const bool held = std::any_of(targets.begin(), targets.end(), [info](const MemberTarget& target)
				{
					return GetMember(target.Object, ComponentsKey).contains(info->GetName());
				});
				const bool added = std::any_of(overrides.begin(), overrides.end(), [info](const PrefabOverride& prefabOverride)
				{
					return prefabOverride.Kind == PrefabOverrideKind::AddComponent && prefabOverride.Component == info->GetName();
				});
				if (held || added)
					candidates.push_back(info);
			}
			return candidates;
		}

		// The `candidates` (UniquePerScene components) held by entities of `scene` that are not members of the instance.
		static std::unordered_set<const ComponentInfo*> CollectUniqueComponentsOutside(const Scene& scene,
			std::span<const ComponentInfo* const> candidates, const std::unordered_set<UUID>& members)
		{
			std::unordered_set<const ComponentInfo*> taken; // lookup only
			if (candidates.empty())
				return taken;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				if (members.contains(id) || !entity.IsValid())
					continue;
				for (const ComponentInfo* info : candidates)
				{
					if (!taken.contains(info) && info->GetHostOps()->Has(entity))
						taken.insert(info);
				}
				if (taken.size() == candidates.size())
					break;
			}
			return taken;
		}

		// Fails when a UniquePerScene component appears on two of `targets` or on one of them and an entity outside the
		// instance (`taken`): the prefab cannot be placed in this scene.
		static Status CheckUniqueComponents(const TypeRegistry& registry, std::span<const MemberTarget> targets,
			const std::unordered_set<const ComponentInfo*>& taken)
		{
			for (const ComponentInfo* info : registry.GetComponents())
			{
				if (!info->HasFlag(ComponentFlags::UniquePerScene))
					continue;
				const std::ptrdiff_t count = std::count_if(targets.begin(), targets.end(), [info](const MemberTarget& target)
				{
					return GetMember(target.Object, ComponentsKey).contains(info->GetName());
				});
				if (count > 1 || (count == 1 && taken.contains(info)))
				{
					return MakeError(ErrorCode::Validation, "'{}' is unique per scene, and the prefab would add another one to the scene",
						info->GetName());
				}
			}
			return {};
		}

		// Applies the prefab overrides to the instance's member JSON, each validated against the registry on its own (as a
		// file read validates it: unresolvable Variant values are kept). An override that no longer matches, or whose value
		// is no longer valid there, is dropped with a PREFAB_STALE_OVERRIDE warning. Returns the overrides that were applied,
		// sorted. RemoveComponent overrides go first so that an added component may replace one the prefab excludes it with.
		class OverrideApplier
		{
		public:
			OverrideApplier(const TypeRegistry& registry, const PrefabOptions& options, const EntityRefRemapper& toInstance,
				std::vector<MemberTarget>& targets, const std::unordered_set<const ComponentInfo*>& taken, UUID prefabRoot, UUID instanceRoot,
				LoadReport& report)
				: m_Registry(registry),
				  m_Options(options),
				  m_ToInstance(toInstance),
				  m_Targets(targets),
				  m_Taken(taken),
				  m_PrefabRoot(prefabRoot),
				  m_InstanceRoot(instanceRoot),
				  m_Report(report)
			{
				for (size_t index = 0; index < targets.size(); ++index)
					m_TargetIndex.emplace(targets[index].PrefabID, index);
				m_Transform = registry.FindComponent<TransformComponent>();
			}

			[[nodiscard]] std::vector<PrefabOverride> Apply(std::span<const PrefabOverride> overrides)
			{
				std::vector<PrefabOverride> applied;
				for (const PrefabOverrideKind phase :
					{ PrefabOverrideKind::RemoveComponent, PrefabOverrideKind::AddComponent, PrefabOverrideKind::Field, PrefabOverrideKind::EntityKey })
				{
					for (size_t index = 0; index < overrides.size(); ++index)
					{
						const PrefabOverride& prefabOverride = overrides[index];
						if (prefabOverride.Kind != phase)
							continue;
						const std::string problem = ApplyOne(prefabOverride);
						if (problem.empty())
						{
							applied.push_back(prefabOverride);
							continue;
						}
						std::string target = prefabOverride.Field;
						if (prefabOverride.Kind != PrefabOverrideKind::EntityKey)
						{
							target = prefabOverride.Field.empty() ? prefabOverride.Component
																  : std::format("{}.{}", prefabOverride.Component, prefabOverride.Field);
						}
						m_Report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Warning, std::string(PrefabStaleOverrideCode),
							std::format("dropped the {} override of '{}' on prefab entity {}: {}", GetKindName(m_Registry, prefabOverride.Kind),
								target, prefabOverride.PrefabEntityID, problem),
							std::format("/Components/Prefab/Overrides/{}", index), m_InstanceRoot });
					}
				}
				std::sort(applied.begin(), applied.end(), &IsLess);
				return applied;
			}
		private:
			// Applies one override; the reason it does not apply otherwise.
			[[nodiscard]] std::string ApplyOne(const PrefabOverride& prefabOverride)
			{
				const auto found = m_TargetIndex.find(prefabOverride.PrefabEntityID);
				if (found == m_TargetIndex.end())
					return "the prefab no longer has that entity";
				MemberTarget& target = m_Targets[found->second];
				const bool isRoot = target.PrefabID == m_PrefabRoot;

				if (prefabOverride.Kind == PrefabOverrideKind::EntityKey)
					return ApplyEntityKey(prefabOverride, target, isRoot);

				const ComponentInfo* info = m_Registry.FindComponent(prefabOverride.Component);
				if (info == nullptr)
					return std::format("'{}' is not a registered component", prefabOverride.Component);
				if (!IsOverridable(*info, isRoot, m_Transform))
				{
					return isRoot && info == m_Transform ? std::string("the instance root's Transform is an implicit override")
														 : std::format("'{}' is not a component overrides can change", info->GetName());
				}

				Json& components = target.Object[ComponentsKey];
				switch (prefabOverride.Kind)
				{
					case PrefabOverrideKind::RemoveComponent:
						return RemoveComponent(*info, components);
					case PrefabOverrideKind::AddComponent:
						return AddComponent(*info, prefabOverride, target);
					case PrefabOverrideKind::Field:
						return SetField(*info, prefabOverride, components);
					case PrefabOverrideKind::EntityKey:
						break; // applied above, before the component lookup
				}
				ENGINE_CORE_ASSERT(false, "PrefabOverrideKind {} reached the component overrides", std::to_underlying(prefabOverride.Kind));
				return "unknown override kind";
			}

			[[nodiscard]] std::string RemoveComponent(const ComponentInfo& info, Json& components) const
			{
				if (!components.contains(info.GetName()))
					return std::format("the prefab entity no longer has '{}'", info.GetName());
				if (info.HasFlag(ComponentFlags::Required) || !info.HasFlag(ComponentFlags::Removable))
					return std::format("'{}' cannot be removed", info.GetName());
				for (auto present = components.begin(); present != components.end(); ++present)
				{
					const ComponentInfo* other = m_Registry.FindComponent(present.key());
					if (other != nullptr && other != &info && std::ranges::find(other->GetRequires(), &info) != other->GetRequires().end())
						return std::format("'{}' is required by '{}'", info.GetName(), other->GetName());
				}
				components.erase(info.GetName());
				return {};
			}

			[[nodiscard]] std::string AddComponent(const ComponentInfo& info, const PrefabOverride& prefabOverride, MemberTarget& target) const
			{
				Json& components = target.Object[ComponentsKey];
				for (const ComponentInfo* required : info.GetRequires())
				{
					if (!components.contains(required->GetName()))
						return std::format("'{}' requires '{}'", info.GetName(), required->GetName());
				}
				for (auto present = components.begin(); present != components.end(); ++present)
				{
					const ComponentInfo* other = m_Registry.FindComponent(present.key());
					if (other == nullptr || other == &info)
						continue;
					if (std::ranges::find(info.GetExcludes(), other) != info.GetExcludes().end()
						|| std::ranges::find(other->GetExcludes(), &info) != other->GetExcludes().end())
						return std::format("'{}' cannot be combined with '{}'", info.GetName(), other->GetName());
				}
				if (info.HasFlag(ComponentFlags::UniquePerScene))
				{
					const bool elsewhere = m_Taken.contains(&info)
						|| std::any_of(m_Targets.begin(), m_Targets.end(), [&info, &target](const MemberTarget& other)
					{
						return &other != &target && GetMember(other.Object, ComponentsKey).contains(info.GetName());
					});
					if (elsewhere)
						return std::format("'{}' is unique per scene and another entity has one", info.GetName());
				}

				Json value = prefabOverride.Value.Get();
				m_ToInstance.RemapStruct(value, info);
				Result<Json> canonical = Canonicalize(info, value, m_Options.Schemas);
				if (!canonical)
					return canonical.error().ToString();
				components[info.GetName()] = std::move(*canonical);
				return {};
			}

			[[nodiscard]] std::string SetField(const ComponentInfo& info, const PrefabOverride& prefabOverride, Json& components) const
			{
				const FieldInfo* field = info.FindField(prefabOverride.Field);
				if (field == nullptr || !field->IsStored() || !field->GetMeta().Serialized)
					return std::format("'{}' has no serialized field '{}'", info.GetName(), prefabOverride.Field);
				const auto component = components.find(info.GetName());
				if (component == components.end())
					return std::format("the prefab entity no longer has '{}'", info.GetName());

				Json candidate = *component;
				Json value = prefabOverride.Value.Get();
				m_ToInstance.RemapField(value, *field, info, candidate);
				if (IsKeyedVariantMap(*field) && value.is_object())
				{
					// The keys the override names, each replaced whole (a Variant's shape belongs to its schema) or removed by null.
					Json& map = candidate[field->GetName()];
					if (!map.is_object())
						map = Json::object();
					for (auto entry = value.begin(); entry != value.end(); ++entry)
					{
						if (entry->is_null())
							map.erase(entry.key());
						else
							map[entry.key()] = std::move(*entry);
					}
				}
				else
				{
					candidate[field->GetName()] = std::move(value);
				}
				Result<Json> canonical = Canonicalize(info, candidate, m_Options.Schemas);
				if (!canonical)
					return canonical.error().ToString();
				*component = std::move(*canonical);
				return {};
			}

			[[nodiscard]] std::string ApplyEntityKey(const PrefabOverride& prefabOverride, MemberTarget& target, bool isRoot) const
			{
				if (std::ranges::find(EntityKeyNames, prefabOverride.Field) == EntityKeyNames.end())
					return std::format("'{}' is not an entity key (Name, Active or Tags)", prefabOverride.Field);
				if (isRoot && prefabOverride.Field == "Name")
					return "the instance root's Name is an implicit override";
				const StructInfo* keys = m_Registry.FindStruct<PrefabEntityKeys>();
				ENGINE_CORE_ASSERT(keys != nullptr, "PrefabEntityKeys is a built-in struct");
				if (keys == nullptr)
					return "the entity keys are not registered";

				Json object = Json::object();
				object[prefabOverride.Field] = prefabOverride.Value.Get();
				Result<Json> canonical = Canonicalize(*keys, object, m_Options.Schemas);
				if (!canonical)
					return canonical.error().ToString();
				target.Object[prefabOverride.Field] = GetMember(*canonical, prefabOverride.Field);
				return {};
			}
		private:
			const TypeRegistry& m_Registry;
			const PrefabOptions& m_Options;
			const EntityRefRemapper& m_ToInstance;
			std::vector<MemberTarget>& m_Targets;
			const std::unordered_set<const ComponentInfo*>& m_Taken;
			std::unordered_map<UUID, size_t> m_TargetIndex; // prefab-local ID -> index in m_Targets; lookup only
			const ComponentInfo* m_Transform = nullptr;
			UUID m_PrefabRoot;
			UUID m_InstanceRoot;
			LoadReport& m_Report;
		};

		// Puts the children of `parent` that are members of the rebuilt instance in prefab order, `members`. Each member
		// takes the place of a member already there, so user children keep their positions among them, and an unchanged
		// instance is not touched at all.
		static Status OrderMemberChildren(Scene& scene, Entity parent, std::span<const UUID> members)
		{
			const std::unordered_set<UUID> memberSet(members.begin(), members.end()); // lookup only
			const std::vector<UUID> current(parent.GetChildren().begin(), parent.GetChildren().end());
			std::vector<UUID> desired;
			desired.reserve(current.size());
			size_t next = 0;
			for (const UUID child : current)
			{
				if (!memberSet.contains(child))
					desired.push_back(child);
				else if (next < members.size())
					desired.push_back(members[next++]);
			}
			for (; next < members.size(); ++next)
				desired.push_back(members[next]);

			for (size_t index = 0; index < desired.size(); ++index)
			{
				if (index < parent.GetChildren().size() && parent.GetChildren()[index] == desired[index])
					continue;
				const Entity child = scene.FindEntityByID(desired[index]);
				ENGINE_TRY(scene.SetParent(child, parent, static_cast<uint32_t>(index), false));
			}
			return {};
		}

		// The body of UpdateInstance and Revert: the instance rebuilt as `prefab` plus `overrides` (§5.5 "Update"). Every check
		// that can fail runs before the scene changes.
		static Status RebuildInstance(Scene& scene, Entity root, const Prefab& prefab, std::span<const PrefabOverride> overrides,
			const PrefabOptions& options, LoadReport& report)
		{
			ENGINE_TRY(RequireInstanceRoot(root));
			if (root.GetScene() != &scene)
			{
				return MakeError(ErrorCode::InvalidArgument, "Entity '{}' ({}) is an instance root of another scene", root.GetName(),
					root.GetUUID());
			}
			ENGINE_TRY(RequirePrefab(prefab));
			const Scene& constScene = scene;
			const TypeRegistry& registry = scene.GetTypeRegistry();
			const ComponentInfo* linkInfo = registry.FindComponent<PrefabLinkComponent>();
			const ComponentInfo* instanceInfo = registry.FindComponent<PrefabInstanceComponent>();
			const ComponentInfo* transformInfo = registry.FindComponent<TransformComponent>();
			ENGINE_CORE_ASSERT(linkInfo != nullptr && instanceInfo != nullptr && transformInfo != nullptr, "The prefab components are built in");
			if (linkInfo == nullptr || instanceInfo == nullptr || transformInfo == nullptr)
				return MakeError(ErrorCode::InvalidState, "the prefab components are not registered");
			ENGINE_TRY_ASSIGN(const Json unknownVersions, GetUnknownComponentVersions(registry, prefab));

			const UUID rootID = root.GetUUID();
			const InstanceMembers members = CollectMembers(root, prefab.GetRootID());
			const IdMap toInstanceMap = MakeInstanceMap(prefab, rootID);
			const EntityRefRemapper toInstance(registry, options.Schemas, toInstanceMap);

			// Member IDs: the derived ID of every prefab entity is free or already a member of this instance.
			std::unordered_set<UUID> targetIDs; // lookup only
			for (const UUID prefabID : prefab.GetEntityIDs())
			{
				const UUID id = Lookup(toInstanceMap, prefabID);
				const ConstEntity holder = constScene.FindEntityByID(id);
				if (!id.IsValid() || !targetIDs.insert(id).second || (holder.IsValid() && !members.IDs.contains(id)))
				{
					return MakeError(ErrorCode::InvalidState,
						"the member ID {} of prefab entity {} is already used in the scene outside the instance", id, prefabID);
				}
			}

			// The rebuilt members' JSON in prefab order: the prefab in instance space, the root keeping its Name, Transform
			// and Parent (implicit overrides), then the overrides.
			const Entity rootParent = root.GetParent();
			std::vector<MemberTarget> targets;
			targets.reserve(prefab.GetEntities().size());
			for (const Json& source : prefab.GetEntities())
			{
				MemberTarget target;
				target.PrefabID = ReadEntityUUID(source, "ID");
				target.ID = Lookup(toInstanceMap, target.PrefabID);
				ENGINE_TRY_ASSIGN(target.Object, MakeMemberJson(source, target.PrefabID, rootID, toInstance, *linkInfo));
				if (target.PrefabID == prefab.GetRootID())
				{
					target.Object["Parent"] = rootParent.IsValid() ? Json(rootParent.GetUUID().ToString()) : Json(nullptr);
					target.Object["Name"] = root.GetName();
					ENGINE_TRY_ASSIGN(target.Object[ComponentsKey][transformInfo->GetName()],
						transformInfo->ToJson(&root.GetComponent<TransformComponent>()));
				}
				target.Parent = ReadEntityUUID(target.Object, "Parent");
				targets.push_back(std::move(target));
			}

			// The overrides go first, so that a unique component the instance removed (RemoveComponent) and the scene now has
			// elsewhere does not block the update. Their warnings join `report` once nothing can fail any more.
			const std::unordered_set<const ComponentInfo*> taken =
				CollectUniqueComponentsOutside(constScene, CollectUniqueCandidates(registry, targets, overrides), members.IDs);
			LoadReport overrideReport;
			OverrideApplier applier(registry, options, toInstance, targets, taken, prefab.GetRootID(), rootID, overrideReport);
			PrefabInstanceComponent instance;
			instance.Prefab = root.GetComponent<PrefabInstanceComponent>().Prefab;
			instance.Overrides = applier.Apply(overrides);
			ENGINE_TRY(CheckUniqueComponents(registry, targets, taken));
			ENGINE_CORE_ASSERT(targets.front().PrefabID == prefab.GetRootID(), "Canonical order starts with the prefab's root");
			ENGINE_TRY_ASSIGN(targets.front().Object[ComponentsKey][instanceInfo->GetName()], instanceInfo->ToJson(&instance));
			report.Diagnostics.insert(report.Diagnostics.end(), std::make_move_iterator(overrideReport.Diagnostics.begin()),
				std::make_move_iterator(overrideReport.Diagnostics.end()));

			// Nothing below fails for a validated prefab: every ID, parent and unique component was checked above.
			std::unordered_set<UUID> doomed; // lookup only: members whose prefab entity is gone
			for (const Entity member : members.Members)
			{
				if (!targetIDs.contains(member.GetUUID()))
					doomed.insert(member.GetUUID());
			}
			const auto nearestSurvivor = [&doomed](Entity entity)
			{
				Entity ancestor = entity.GetParent();
				while (ancestor.IsValid() && doomed.contains(ancestor.GetUUID()))
					ancestor = ancestor.GetParent();
				return ancestor;
			};
			for (const Entity member : members.Members)
			{
				if (!doomed.contains(member.GetUUID()))
					continue;
				// User children (and surviving members) of a vanishing member move up to its nearest surviving ancestor.
				const std::vector<UUID> children(member.GetChildren().begin(), member.GetChildren().end());
				for (const UUID childID : children)
				{
					const Entity child = scene.FindEntityByID(childID);
					if (child.IsValid() && !doomed.contains(childID))
						ENGINE_TRY(scene.SetParent(child, nearestSurvivor(member), std::nullopt, false));
				}
			}
			// Destroying a doomed member destroys its doomed member descendants with it (their user children moved up above), so
			// a later one in canonical order may be gone already.
			for (const Entity member : members.Members)
			{
				if (member.IsValid() && doomed.contains(member.GetUUID()))
					scene.DestroyEntity(member);
			}

			// A unique component that moves to another member leaves its old holder first.
			for (const MemberTarget& target : targets)
			{
				const Entity member = scene.FindEntityByID(target.ID);
				if (!member.IsValid())
					continue;
				for (const ComponentInfo* info : registry.GetComponents())
				{
					if (info->HasFlag(ComponentFlags::UniquePerScene) && info->GetHostOps() != nullptr && info->GetHostOps()->Has(member)
						&& !GetMember(target.Object, ComponentsKey).contains(info->GetName()))
						info->GetHostOps()->Remove(member);
				}
			}

			LoadOptions loadOptions;
			loadOptions.Schemas = options.Schemas;
			std::unordered_map<UUID, std::vector<UUID>> memberChildren; // parent member ID -> member children in prefab order
			for (const MemberTarget& target : targets)
			{
				const JsonReader reader(target.Object);
				const Entity parent = target.Parent.IsValid() ? scene.FindEntityByID(target.Parent) : Entity();
				if (const Entity member = scene.FindEntityByID(target.ID); member.IsValid())
				{
					const Entity currentParent = member.GetParent();
					const UUID currentParentID = currentParent.IsValid() ? currentParent.GetUUID() : UUID();
					if (currentParentID != target.Parent)
						ENGINE_TRY(scene.SetParent(member, parent, std::nullopt, false));
					ENGINE_TRY(ApplyEntityJson(member, reader, &unknownVersions, loadOptions, report));
				}
				else
				{
					ENGINE_TRY(EntityFromJson(scene, reader, std::nullopt, &unknownVersions, loadOptions, report));
				}
				if (target.ID != rootID)
					memberChildren[target.Parent].push_back(target.ID);
			}
			for (const MemberTarget& target : targets)
			{
				const auto children = memberChildren.find(target.ID);
				if (children != memberChildren.end())
					ENGINE_TRY(OrderMemberChildren(scene, scene.FindEntityByID(target.ID), children->second));
			}
			return {};
		}

	}

	UUID PrefabInstantiator::DeriveInstanceID(UUID instanceRootID, UUID prefabEntityID)
	{
		return UUID(Hash64(instanceRootID.GetValue(), prefabEntityID.GetValue()));
	}

	Result<Entity> PrefabInstantiator::Instantiate(Scene& scene, const Prefab& prefab, const PrefabInstantiateOptions& instance,
		const PrefabOptions& options, LoadReport& report)
	{
		ENGINE_TRY(Utils::RequirePrefab(prefab));
		if (!instance.RootID.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "a prefab instance needs a valid root ID");
		if (instance.Parent.IsValid() && instance.Parent.GetScene() != &scene)
			return MakeError(ErrorCode::InvalidArgument, "the parent of a prefab instance must be an entity of the same scene");
		const Scene& constScene = scene;
		if (constScene.FindEntityByID(instance.RootID).IsValid())
			return MakeError(ErrorCode::AlreadyExists, "the root ID {} of the prefab instance is already used in the scene", instance.RootID);
		const TypeRegistry& registry = scene.GetTypeRegistry();
		const ComponentInfo* linkInfo = registry.FindComponent<PrefabLinkComponent>();
		const ComponentInfo* instanceInfo = registry.FindComponent<PrefabInstanceComponent>();
		const ComponentInfo* transformInfo = registry.FindComponent<TransformComponent>();
		ENGINE_CORE_ASSERT(linkInfo != nullptr && instanceInfo != nullptr && transformInfo != nullptr, "The prefab components are built in");
		if (linkInfo == nullptr || instanceInfo == nullptr || transformInfo == nullptr)
			return MakeError(ErrorCode::InvalidState, "the prefab components are not registered");

		// IDs: a pure function of the root, never re-hashed (ADR 0006 decision 18), so a taken derived ID is an inconsistent
		// scene rather than the caller's choice.
		const IdMap toInstanceMap = Utils::MakeInstanceMap(prefab, instance.RootID);
		std::unordered_set<UUID> ids; // lookup only
		for (const UUID prefabID : prefab.GetEntityIDs())
		{
			const UUID id = Utils::Lookup(toInstanceMap, prefabID);
			if (!id.IsValid() || !ids.insert(id).second || constScene.FindEntityByID(id).IsValid())
				return MakeError(ErrorCode::InvalidState, "the member ID {} of prefab entity {} is already used in the scene", id, prefabID);
		}
		ENGINE_TRY_ASSIGN(const Json unknownVersions, Utils::GetUnknownComponentVersions(registry, prefab));

		Json rootTransform;
		if (instance.RootTransform)
		{
			ENGINE_TRY_ASSIGN(const Json written, WithContext(transformInfo->ToJson(&*instance.RootTransform), "in the instance's root transform"));
			ENGINE_TRY_ASSIGN(rootTransform, WithContext(Utils::Canonicalize(*transformInfo, written, nullptr), "in the instance's root transform"));
		}

		const EntityRefRemapper toInstance(registry, options.Schemas, toInstanceMap);
		std::vector<MemberTarget> targets;
		targets.reserve(prefab.GetEntities().size());
		for (const Json& source : prefab.GetEntities())
		{
			MemberTarget target;
			target.PrefabID = Utils::ReadEntityUUID(source, "ID");
			target.ID = Utils::Lookup(toInstanceMap, target.PrefabID);
			ENGINE_TRY_ASSIGN(target.Object, Utils::MakeMemberJson(source, target.PrefabID, instance.RootID, toInstance, *linkInfo));
			if (target.PrefabID == prefab.GetRootID())
			{
				target.Object["Parent"] = instance.Parent.IsValid() ? Json(instance.Parent.GetUUID().ToString()) : Json(nullptr);
				if (instance.RootTransform)
					target.Object[ComponentsKey][transformInfo->GetName()] = rootTransform;
				PrefabInstanceComponent component;
				component.Prefab = TypedAssetHandle<AssetType::Prefab>(instance.PrefabHandle);
				ENGINE_TRY_ASSIGN(target.Object[ComponentsKey][instanceInfo->GetName()], instanceInfo->ToJson(&component));
			}
			targets.push_back(std::move(target));
		}
		const std::unordered_set<const ComponentInfo*> taken =
			Utils::CollectUniqueComponentsOutside(constScene, Utils::CollectUniqueCandidates(registry, targets, {}), {});
		ENGINE_TRY(Utils::CheckUniqueComponents(registry, targets, taken));

		// Creation in canonical order (parents first). The prefab was validated when it was created, so a failure here is
		// unexpected; the partial instance is removed again.
		LoadOptions loadOptions;
		loadOptions.Schemas = options.Schemas;
		Entity root;
		for (const MemberTarget& target : targets)
		{
			const bool isRoot = target.ID == instance.RootID;
			Result<Entity> created = Utils::EntityFromJson(scene, JsonReader(target.Object), isRoot ? instance.SiblingIndex : std::nullopt,
				&unknownVersions, loadOptions, report);
			if (!created)
			{
				if (root.IsValid())
					scene.DestroyEntity(root);
				return std::unexpected(std::move(created).error());
			}
			if (isRoot)
				root = *created;
		}
		return root;
	}

	Status PrefabInstantiator::UpdateInstance(Scene& scene, Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options,
		LoadReport& report)
	{
		ENGINE_TRY(Utils::RequireInstanceRoot(instanceRoot));
		const std::vector<PrefabOverride> overrides = instanceRoot.GetComponent<PrefabInstanceComponent>().Overrides;
		return Utils::RebuildInstance(scene, instanceRoot, prefab, overrides, options, report);
	}

	Result<std::vector<PrefabOverride>> PrefabInstantiator::ComputeOverrides(Entity instanceRoot, const Prefab& prefab,
		const PrefabOptions& options)
	{
		ENGINE_TRY(Utils::RequireInstanceRoot(instanceRoot));
		ENGINE_TRY(Utils::RequirePrefab(prefab));
		const TypeRegistry& registry = instanceRoot.GetScene()->GetTypeRegistry();
		const ComponentInfo* linkInfo = registry.FindComponent<PrefabLinkComponent>();
		ENGINE_CORE_ASSERT(linkInfo != nullptr, "PrefabLink is built in");
		if (linkInfo == nullptr)
			return MakeError(ErrorCode::InvalidState, "the prefab components are not registered");

		const UUID rootID = instanceRoot.GetUUID();
		const InstanceMembers members = Utils::CollectMembers(instanceRoot, prefab.GetRootID());
		const IdMap toInstanceMap = Utils::MakeInstanceMap(prefab, rootID);
		const EntityRefRemapper toInstance(registry, options.Schemas, toInstanceMap);
		const std::unordered_set<UUID> prefabIDs = Utils::CollectPrefabIDs(prefab);
		std::vector<UUID> ambiguous;
		const EntityRefRemapper toPrefab(registry, options.Schemas, members.ToPrefab, prefabIDs, ambiguous);

		std::vector<PrefabOverride> overrides;
		for (const Entity member : members.Members)
		{
			const UUID prefabID = Utils::Lookup(members.ToPrefab, member.GetUUID());
			const Json* source = prefab.FindEntity(prefabID);
			if (source == nullptr)
				continue; // a member of a prefab entity that no longer exists: the next update removes it
			ENGINE_TRY_ASSIGN(const Json expected, Utils::MakeMemberJson(*source, prefabID, rootID, toInstance, *linkInfo));
			ENGINE_TRY_ASSIGN(const Json actual, SceneSerializer::EntityToJson(member));
			ENGINE_TRY(Utils::DiffMember(registry, member.GetUUID(), prefabID, member == instanceRoot, expected, actual, toPrefab, ambiguous,
				overrides));
		}
		std::sort(overrides.begin(), overrides.end(), &Utils::IsLess);
		return overrides;
	}

	Status PrefabInstantiator::RefreshOverrides(Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options)
	{
		ENGINE_TRY_ASSIGN(std::vector<PrefabOverride> overrides, ComputeOverrides(instanceRoot, prefab, options));
		const std::vector<PrefabOverride>& current = instanceRoot.GetComponent<PrefabInstanceComponent>().Overrides;
		if (std::equal(current.begin(), current.end(), overrides.begin(), overrides.end(), &Utils::AreEqual))
			return {};
		instanceRoot.Patch<PrefabInstanceComponent>([&overrides](PrefabInstanceComponent& component)
		{
			component.Overrides = std::move(overrides);
		});
		return {};
	}

	Status PrefabInstantiator::Revert(Scene& scene, Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options,
		LoadReport& report)
	{
		return Utils::RebuildInstance(scene, instanceRoot, prefab, {}, options, report);
	}

	Status PrefabInstantiator::Unpack(Entity instanceRoot)
	{
		ENGINE_TRY(Utils::RequireInstanceRoot(instanceRoot));
		const InstanceMembers members = Utils::CollectMembers(instanceRoot, UUID());
		for (const Entity member : members.Members)
		{
			if (member.HasComponent<PrefabLinkComponent>())
				member.RemoveComponent<PrefabLinkComponent>();
		}
		instanceRoot.RemoveComponent<PrefabInstanceComponent>();
		return {};
	}

	Result<Prefab> PrefabInstantiator::ApplyOverrides(Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options)
	{
		ENGINE_TRY(Utils::RequireInstanceRoot(instanceRoot));
		ENGINE_TRY(Utils::RequirePrefab(prefab));
		const TypeRegistry& registry = instanceRoot.GetScene()->GetTypeRegistry();
		const ComponentInfo* linkInfo = registry.FindComponent<PrefabLinkComponent>();
		const ComponentInfo* instanceInfo = registry.FindComponent<PrefabInstanceComponent>();
		const ComponentInfo* transformInfo = registry.FindComponent<TransformComponent>();
		ENGINE_CORE_ASSERT(linkInfo != nullptr && instanceInfo != nullptr && transformInfo != nullptr, "The prefab components are built in");
		if (linkInfo == nullptr || instanceInfo == nullptr || transformInfo == nullptr)
			return MakeError(ErrorCode::InvalidState, "the prefab components are not registered");

		const InstanceMembers members = Utils::CollectMembers(instanceRoot, prefab.GetRootID());
		const std::unordered_set<UUID> prefabIDs = Utils::CollectPrefabIDs(prefab);
		std::vector<UUID> ambiguous;
		const EntityRefRemapper toPrefab(registry, options.Schemas, members.ToPrefab, prefabIDs, ambiguous);
		const Json* prefabRoot = prefab.FindEntity(prefab.GetRootID());
		ENGINE_CORE_ASSERT(prefabRoot != nullptr, "A prefab contains its root");

		Json entities = Json::array();
		std::vector<std::pair<std::string, uint32_t>> unknownVersions;
		std::unordered_set<UUID> written; // lookup only: prefab entities already written
		for (const Entity member : members.Members)
		{
			const UUID prefabID = Utils::Lookup(members.ToPrefab, member.GetUUID());
			if (!written.insert(prefabID).second)
				continue; // a second member claiming the same prefab entity (a hand edit): the first in canonical order wins

			// The ID and parent are set from the member table, and the link components go (they hold prefab-local IDs
			// already); only the other components' references go through the remapper.
			ENGINE_TRY_ASSIGN(Json json, SceneSerializer::EntityToJson(member));
			json["ID"] = prefabID.ToString();
			Json& components = json[ComponentsKey];
			components.erase(linkInfo->GetName());
			components.erase(instanceInfo->GetName());
			toPrefab.RemapComponents(json);
			if (!ambiguous.empty())
				return std::unexpected(Utils::MakeAmbiguousReferenceError(member.GetUUID(), "/Components", ambiguous.front()));
			if (member == instanceRoot)
			{
				// The root's Name, Transform and Parent are the instance's own (implicit overrides), never the prefab's.
				json["Parent"] = nullptr;
				json["Name"] = Utils::GetMember(*prefabRoot, "Name");
				components[transformInfo->GetName()] = Utils::GetMember(Utils::GetMember(*prefabRoot, ComponentsKey), transformInfo->GetName());
			}
			else
			{
				// The nearest member above it (user children in between are not part of the prefab).
				Entity parent = member.GetParent();
				while (parent.IsValid() && !members.IDs.contains(parent.GetUUID()))
					parent = parent.GetParent();
				json["Parent"] = parent.IsValid() ? Json(Utils::Lookup(members.ToPrefab, parent.GetUUID()).ToString()) : Json(nullptr);
			}
			entities.push_back(std::move(json));
			Utils::CollectUnknownVersions(member, unknownVersions);
		}

		// Unknown components follow the prefab, so a member that records no version for one (a scene file that lists none)
		// takes the prefab's.
		ENGINE_TRY_ASSIGN(const Json prefabVersions, Utils::GetUnknownComponentVersions(registry, prefab));
		for (auto& [component, version] : unknownVersions)
		{
			if (version != 0)
				continue;
			if (const std::optional<JsonReader> recorded = JsonReader(prefabVersions).FindMember(component))
			{
				if (const Result<uint32_t> prefabVersion = recorded->ReadUInt32())
					version = *prefabVersion;
			}
		}

		const Json document = Utils::MakePrefabDocument(registry, prefab.GetName(), prefab.GetRootID(), std::move(entities), unknownVersions);
		LoadOptions loadOptions;
		loadOptions.Schemas = options.Schemas;
		LoadReport report;
		return Prefab::FromJson(document, registry, loadOptions, report);
	}

}
