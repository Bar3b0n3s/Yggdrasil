#include "EnginePCH.h"
#include "Engine/Scene/PrefabInstantiator.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Private/EntityDocument.h"
#include "Engine/Scene/Private/PrefabNesting.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
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
		constexpr std::string_view OverridesKey = "Overrides";
		constexpr std::array<std::string_view, 3> EntityKeyNames = { "Name", "Active", "Tags" };

		// A mapping between IDs of two spaces (prefab-local, instance, document), in either direction. Lookup only, never
		// iterated to produce output (§5.1).
		using IdMap = std::unordered_map<UUID, UUID>;

		// An override path (PrefabOverride::Path; Docs/Decisions/0031-m20-contract.md decision 8).
		using OverridePath = std::vector<UUID>;

		// Rewrites the entity references inside canonical entity and component JSON through an IdMap: "ID", "Parent", every
		// EntityRef value of a registered component (nested structs, arrays and maps included) and every Variant value whose
		// resolved schema contains EntityRefs (Entity-kind script fields through IFieldSchemaSource, §5.5). A reference the
		// map does not contain (an entity outside the instance, a value that cannot be resolved) is kept as it is. The
		// engine-maintained Prefab and PrefabLink components are never remapped: their IDs are prefab-local or paths, and
		// whoever builds an instance or a document writes them.
		//
		// Mapping instance IDs back to prefab-local IDs is ambiguous for a reference the map does not contain whose UUID is a
		// prefab-local ID: kept as it is, it would name a prefab entity, and the next update would point it at the instance's
		// own member. Such references are appended to `ambiguous` when the caller passes the prefab's IDs as `reserved`.
		class EntityRefRemapper
		{
		public:
			EntityRefRemapper(const TypeRegistry& registry, const IFieldSchemaSource* schemas, const IdMap& map)
				: m_Registry(registry),
				  m_Schemas(schemas),
				  m_Map(map),
				  m_Link(registry.FindComponent<PrefabLinkComponent>()),
				  m_Instance(registry.FindComponent<PrefabInstanceComponent>())
			{
			}

			// Also reports every reference the map does not contain but `reserved` does to `ambiguous`, which the caller owns
			// and reads after each remap (both outlive the remapper).
			EntityRefRemapper(const TypeRegistry& registry, const IFieldSchemaSource* schemas, const IdMap& map,
				const std::unordered_set<UUID>& reserved, std::vector<UUID>& ambiguous)
				: EntityRefRemapper(registry, schemas, map)
			{
				m_Reserved = &reserved;
				m_Ambiguous = &ambiguous;
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

			// Every registered component of the entity object `entity` but Prefab and PrefabLink.
			void RemapComponents(Json& entity) const
			{
				const auto components = entity.find(ComponentsKey);
				if (components == entity.end() || !components->is_object())
					return;
				for (auto component = components->begin(); component != components->end(); ++component)
				{
					const ComponentInfo* info = m_Registry.FindComponent(component.key());
					if (info != nullptr && info != m_Link && info != m_Instance)
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
			const ComponentInfo* m_Link = nullptr;                // PrefabLink, never remapped
			const ComponentInfo* m_Instance = nullptr;            // Prefab, never remapped
			const std::unordered_set<UUID>* m_Reserved = nullptr; // lookup only
			std::vector<UUID>* m_Ambiguous = nullptr;             // the caller's output
		};

		// One entity of a prefab's document with its override path (Docs/Decisions/0031-m20-contract.md decision 8): [its ID]
		// for an entity of the prefab itself (its root included) and for the root of an instance outermost in the document;
		// the path of the nested root it belongs to, then its ID in that instance's prefab, for any other entity of a nested
		// instance; the enclosing nested root's path, then its OuterPrefabEntityID, for a nested root inside another.
		struct DocumentEntity
		{
			UUID ID;     // its ID in the document
			UUID Parent; // its parent's ID in the document; invalid for the root
			OverridePath Path;
			UUID InnerRoot;               // the document ID of the nested instance root it belongs to (itself for one); invalid otherwise
			bool IsNestedRoot = false;    // the root of a nested instance
			const Json* Object = nullptr; // its canonical JSON, in the prefab's document (which outlives the index)
		};

		// The entities of a prefab's document by ID and by override path. Lookup only.
		class PrefabDocument
		{
		public:
			PrefabDocument(const Prefab& prefab, const ComponentInfo& linkInfo, const ComponentInfo& instanceInfo);

			[[nodiscard]] UUID GetRootID() const { return m_Root; }
			// In document (canonical) order.
			[[nodiscard]] std::span<const DocumentEntity> GetEntities() const { return m_Entities; }
			[[nodiscard]] const DocumentEntity* FindByPath(const OverridePath& path) const
			{
				const auto found = m_ByPath.find(path);
				return found != m_ByPath.end() ? &m_Entities[found->second] : nullptr;
			}
			[[nodiscard]] const DocumentEntity* FindByID(UUID id) const
			{
				const auto found = m_ByID.find(id);
				return found != m_ByID.end() ? &m_Entities[found->second] : nullptr;
			}
			// True when the document holds an instance of another prefab.
			[[nodiscard]] bool HasNestedInstances() const { return m_HasNested; }
			// The entity `id` and its descendants in the document, parents first; empty for an unknown ID.
			[[nodiscard]] std::vector<const DocumentEntity*> CollectSubtree(UUID id) const;
		private:
			UUID m_Root;
			std::vector<DocumentEntity> m_Entities;
			std::map<OverridePath, size_t> m_ByPath; // lookup only; the first entity of a path wins (a hand edit may repeat one)
			std::unordered_map<UUID, size_t> m_ByID; // lookup only
			bool m_HasNested = false;
		};

		// The entities of one outermost instance: the root, its own members (PrefabLink naming the root) and the entities
		// of every instance nested in it, in canonical order (the root first), each with its override path and the member
		// ID of its innermost instance root (itself for an instance root). Entities without such a link are user children
		// (or entities of other instances: one a user placed under a member is its own outermost instance).
		struct InstanceMembers
		{
			std::vector<UUID> Members;
			std::vector<OverridePath> Paths;
			std::vector<UUID> InnerRoots;
			std::unordered_set<UUID> IDs; // lookup only
		};

		// One entity of the rebuilt instance: the prefab entity (its ID in the document), its member ID, its parent in the
		// instance and its JSON.
		struct MemberTarget
		{
			UUID DocumentID;
			UUID ID;
			UUID Parent;
			Json Object;
		};

		// The prefab entities that an instance's RemoveEntity overrides take out of it (Docs/Decisions/0031-m20-contract.md
		// decision 7), by document ID: the entity each applicable one names, with its descendants in the document. Problems
		// holds, by index in the overrides, why a RemoveEntity does not apply; it is empty for one that applies and for every
		// other kind.
		struct RemovedEntities
		{
			std::unordered_set<UUID> IDs; // lookup only
			std::vector<std::string> Problems;
		};

	}

	namespace Utils {

		static Status RequireInstanceRoot(ConstEntity entity)
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

		// The member of `object` named `key`, or JSON null.
		static const Json& GetMember(const Json& object, std::string_view key)
		{
			static const Json Null; // immutable after its thread-safe initialization
			if (!object.is_object())
				return Null;
			const auto member = object.find(key);
			return member != object.end() ? *member : Null;
		}

		// `base` folded with every element of `path` through DeriveInstanceID: the ID an entity whose path is `path` has in
		// the instance rooted at `base` (for every entity but that root).
		static UUID FoldPath(UUID base, std::span<const UUID> path)
		{
			for (const UUID element : path)
				base = PrefabInstantiator::DeriveInstanceID(base, element);
			return base;
		}

		// The ID in its prefab's document of the entity whose override path is `path`: the first element, derived through
		// the others (an entity of a nested instance has the ID that instance's expansion gave it in the document).
		static UUID GetDocumentID(std::span<const UUID> path)
		{
			return path.empty() ? UUID() : FoldPath(path.front(), path.subspan(1));
		}

		// `path` as written in diagnostics: "[a, b]".
		static std::string FormatPath(std::span<const UUID> path)
		{
			std::string text = "[";
			for (size_t index = 0; index < path.size(); ++index)
				text += std::format("{}{}", index == 0 ? "" : ", ", path[index]);
			return text + "]";
		}

		static bool StartsWith(std::span<const UUID> path, std::span<const UUID> prefix)
		{
			return path.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
		}

		bool IsNestedInstanceRoot(ConstEntity entity)
		{
			const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>();
			return entity.HasComponent<PrefabInstanceComponent>() && link != nullptr && link->InstanceRoot == entity.GetUUID()
				&& link->OuterPrefabEntityID.IsValid();
		}

		// The nearest ancestor instance root R of `entity` with DeriveInstanceID(R, outer) == the entity's ID: the root of the
		// instance that encloses the nested root `entity` (decision 8). Invalid when none is.
		static ConstEntity FindEnclosingRoot(ConstEntity entity, UUID outer)
		{
			for (ConstEntity ancestor = entity.GetParent(); ancestor.IsValid(); ancestor = ancestor.GetParent())
			{
				if (ancestor.HasComponent<PrefabInstanceComponent>() && PrefabInstantiator::DeriveInstanceID(ancestor.GetUUID(), outer) == entity.GetUUID())
					return ancestor;
			}
			return {};
		}

		Result<std::vector<ConstEntity>> FindInstanceLevels(ConstEntity entity)
		{
			if (!entity.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "the entity whose prefab instance levels were asked for is not a valid entity");
			const Scene& scene = *entity.GetScene();
			ConstEntity innermost;
			if (entity.HasComponent<PrefabInstanceComponent>())
			{
				innermost = entity;
			}
			else if (const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>(); link != nullptr)
			{
				innermost = scene.FindEntityByID(link->InstanceRoot);
				if (!innermost.IsValid() || !innermost.HasComponent<PrefabInstanceComponent>())
				{
					return MakeError(ErrorCode::InvalidState, "the PrefabLink of '{}' ({}) names {} as its instance root, which is not a prefab instance root",
						entity.GetName(), entity.GetUUID(), link->InstanceRoot);
				}
			}
			else
			{
				return std::vector<ConstEntity>{};
			}

			std::vector<ConstEntity> levels{ innermost };
			for (ConstEntity current = innermost;;)
			{
				const PrefabLinkComponent* link = current.TryGetComponent<PrefabLinkComponent>();
				if (link == nullptr || !link->OuterPrefabEntityID.IsValid())
					break;
				const ConstEntity enclosing = FindEnclosingRoot(current, link->OuterPrefabEntityID);
				if (!enclosing.IsValid())
				{
					return MakeError(ErrorCode::InvalidState,
						"the nested instance root '{}' ({}) has the OuterPrefabEntityID {}, but no ancestor instance root derives its ID from it",
						current.GetName(), current.GetUUID(), link->OuterPrefabEntityID);
				}
				levels.push_back(enclosing);
				current = enclosing;
			}
			std::reverse(levels.begin(), levels.end());
			return levels;
		}

		// RequireInstanceRoot, and refuses a nested instance root (decision 8: an instance is rebuilt, diffed and applied from
		// its outermost root), naming that root.
		static Status RequireOutermostRoot(ConstEntity root)
		{
			ENGINE_TRY(RequireInstanceRoot(root));
			if (!IsNestedInstanceRoot(root))
				return {};
			const Result<std::vector<ConstEntity>> levels = FindInstanceLevels(root);
			if (!levels || levels->empty())
			{
				return MakeError(ErrorCode::InvalidArgument, "Entity '{}' ({}) is a nested prefab instance root whose enclosing instance is missing",
					root.GetName(), root.GetUUID());
			}
			const ConstEntity outermost = levels->front();
			return std::unexpected(Error(ErrorCode::InvalidArgument,
				std::format("Entity '{}' ({}) is nested in the prefab instance '{}' ({}), which records its overrides", root.GetName(), root.GetUUID(),
					outermost.GetName(), outermost.GetUUID()))
					.WithHint(std::format("use its outermost instance root '{}' ({})", outermost.GetName(), outermost.GetUUID())));
		}

		// The subtree of `root` in canonical order (depth first, children in sibling order), without recursion.
		static std::vector<ConstEntity> CollectSubtree(ConstEntity root)
		{
			const Scene& scene = *root.GetScene();
			std::vector<ConstEntity> entities;
			std::vector<ConstEntity> stack{ root };
			while (!stack.empty())
			{
				const ConstEntity entity = stack.back();
				stack.pop_back();
				entities.push_back(entity);
				const std::span<const UUID> children = entity.GetChildren();
				for (auto child = children.rbegin(); child != children.rend(); ++child)
				{
					const ConstEntity handle = scene.FindEntityByID(*child);
					if (handle.IsValid())
						stack.push_back(handle);
				}
			}
			return entities;
		}

		// The entities of the outermost instance rooted at `root` (InstanceMembers). The root's path is [rootPrefabID] (its
		// prefab's root); a nested root belongs to the instance when the root of the instance enclosing it does.
		static InstanceMembers CollectMembers(ConstEntity root, UUID rootPrefabID)
		{
			const std::vector<ConstEntity> subtree = CollectSubtree(root);
			// The path prefix of each instance root of the instance (the root's is empty), found parents first.
			std::unordered_map<UUID, OverridePath> prefixes{ { root.GetUUID(), OverridePath{} } };
			for (const ConstEntity entity : subtree)
			{
				if (entity == root || !IsNestedInstanceRoot(entity))
					continue;
				const UUID outer = entity.GetComponent<PrefabLinkComponent>().OuterPrefabEntityID;
				const ConstEntity enclosing = FindEnclosingRoot(entity, outer);
				const auto prefix = enclosing.IsValid() ? prefixes.find(enclosing.GetUUID()) : prefixes.end();
				if (prefix == prefixes.end())
					continue;
				OverridePath path = prefix->second;
				path.push_back(outer);
				prefixes.emplace(entity.GetUUID(), std::move(path));
			}

			InstanceMembers members;
			const auto add = [&members](ConstEntity entity, OverridePath path, UUID innerRoot)
			{
				members.Members.push_back(entity.GetUUID());
				members.Paths.push_back(std::move(path));
				members.InnerRoots.push_back(innerRoot);
				members.IDs.insert(entity.GetUUID());
			};
			for (const ConstEntity entity : subtree)
			{
				if (entity == root)
				{
					add(entity, OverridePath{ rootPrefabID }, root.GetUUID());
					continue;
				}
				const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>();
				if (link == nullptr)
					continue;
				if (link->InstanceRoot == entity.GetUUID())
				{
					if (const auto prefix = prefixes.find(entity.GetUUID()); prefix != prefixes.end())
						add(entity, prefix->second, entity.GetUUID());
				}
				else if (const auto prefix = prefixes.find(link->InstanceRoot); prefix != prefixes.end())
				{
					OverridePath path = prefix->second;
					path.push_back(link->PrefabEntityID);
					add(entity, std::move(path), link->InstanceRoot);
				}
			}
			return members;
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

		// Applies the RFC 7386 key patch `patch` to the map `map` (a key's null removes it).
		static void ApplyKeyPatch(Json& map, const Json& patch)
		{
			if (!map.is_object())
				map = Json::object();
			for (auto entry = patch.begin(); entry != patch.end(); ++entry)
			{
				if (entry->is_null())
					map.erase(entry.key());
				else
					map[entry.key()] = *entry;
			}
		}

		// The ID `map` gives `id`; the invalid UUID when it has none.
		static UUID Lookup(const IdMap& map, UUID id)
		{
			const auto found = map.find(id);
			return found != map.end() ? found->second : UUID();
		}

		// The member ID, in the instance rooted at `rootID`, of the prefab entity `entity`.
		static UUID GetMemberID(const PrefabDocument& document, const DocumentEntity& entity, UUID rootID)
		{
			return entity.ID == document.GetRootID() ? rootID : FoldPath(rootID, entity.Path);
		}

		// The member ID of every entity of `document` in the instance rooted at `rootID`.
		static IdMap MakeInstanceMap(const PrefabDocument& document, UUID rootID)
		{
			IdMap map;
			for (const DocumentEntity& entity : document.GetEntities())
				map.emplace(entity.ID, GetMemberID(document, entity, rootID));
			return map;
		}

		// The document ID of member `index` of `members`: its prefab entity's, or the ID its path gives when the prefab no
		// longer has it.
		static UUID GetMemberDocumentID(const PrefabDocument& document, const InstanceMembers& members, size_t index)
		{
			const DocumentEntity* entity = document.FindByPath(members.Paths[index]);
			return entity != nullptr ? entity->ID : GetDocumentID(members.Paths[index]);
		}

		// Components a prefab override can target on a member: written under "Components" and not engine-maintained
		// (Prefab, PrefabLink). The instance root's Transform is an implicit override (§5.5) and never recorded.
		static bool IsOverridable(const ComponentInfo& info, bool isRoot, const ComponentInfo* transform)
		{
			if (!info.HasFlag(ComponentFlags::Serializable) || info.HasFlag(ComponentFlags::EntityLevel) || info.HasFlag(ComponentFlags::Hidden))
				return false;
			return info.GetHostOps() != nullptr && !(isRoot && &info == transform);
		}

		// The JSON of the document entity `entity` as a member of the instance rooted at `rootID`, in instance space: IDs and
		// internal references through `toInstance` (whose map is `toInstanceMap`), and its PrefabLink: the prefab's own
		// entities link to the root; an entity of a nested instance keeps its link to its nested root, now at its member ID,
		// and a nested root records its OuterPrefabEntityID (its ID in the prefab that encloses it) and no overrides (its
		// prefab's document holds the instance with them applied, decision 8). A root's "Parent" stays null for the caller to
		// set.
		static Result<Json> MakeMemberJson(const DocumentEntity& entity, UUID rootID, const IdMap& toInstanceMap, const EntityRefRemapper& toInstance,
			const ComponentInfo& linkInfo, const ComponentInfo& instanceInfo)
		{
			Json member = *entity.Object;
			toInstance.RemapEntity(member);
			Json& components = member[ComponentsKey];
			PrefabLinkComponent link{ entity.ID, rootID, UUID() };
			if (entity.InnerRoot.IsValid())
			{
				const Json& documentLink = GetMember(GetMember(*entity.Object, ComponentsKey), linkInfo.GetName());
				link.PrefabEntityID = ReadEntityUUID(documentLink, "PrefabEntityID");
				link.InstanceRoot = Lookup(toInstanceMap, entity.InnerRoot);
				if (entity.IsNestedRoot)
				{
					link.OuterPrefabEntityID = entity.Path.back();
					if (Json& instance = components[instanceInfo.GetName()]; instance.is_object())
						instance[OverridesKey] = Json::array();
				}
			}
			ENGINE_TRY_ASSIGN(components[linkInfo.GetName()], linkInfo.ToJson(&link));
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
			if (left.Path != right.Path)
				return left.Path < right.Path;
			if (left.Component != right.Component)
				return left.Component < right.Component;
			if (left.Kind != right.Kind)
				return left.Kind < right.Kind;
			return left.Field < right.Field;
		}

		static bool AreEqual(const PrefabOverride& left, const PrefabOverride& right)
		{
			return left.Path == right.Path && left.Kind == right.Kind && left.Component == right.Component && left.Field == right.Field
				&& left.Value == right.Value;
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
		static Status DiffMember(const TypeRegistry& registry, UUID memberID, const OverridePath& path, bool isRoot, const Json& expected,
			const Json& actual, const EntityRefRemapper& toPrefab, std::vector<UUID>& ambiguous, std::vector<PrefabOverride>& overrides)
		{
			const auto record = [&overrides, &path](PrefabOverrideKind kind, std::string component, std::string field, Json value)
			{
				overrides.push_back(PrefabOverride{ path, kind, std::move(component), std::move(field), VariantValue(std::move(value)) });
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

		// The value of the override `entry` with its entity references mapped through `remapper`; `owner` is the JSON of the
		// component it targets, or null (Variant resolution reads it). Field and AddComponent values only: the other kinds
		// hold none.
		static void RemapOverrideValue(const TypeRegistry& registry, const EntityRefRemapper& remapper, PrefabOverride& entry, const Json* owner)
		{
			static const Json NoOwner = Json::object(); // immutable after its thread-safe initialization
			if (entry.Kind != PrefabOverrideKind::Field && entry.Kind != PrefabOverrideKind::AddComponent)
				return;
			const ComponentInfo* info = registry.FindComponent(entry.Component);
			if (info == nullptr)
				return;
			Json value = entry.Value.Get();
			if (entry.Kind == PrefabOverrideKind::AddComponent)
			{
				remapper.RemapStruct(value, *info);
			}
			else if (const FieldInfo* field = info->FindField(entry.Field); field != nullptr)
			{
				remapper.RemapField(value, *field, *info, owner != nullptr ? *owner : NoOwner);
			}
			entry.Value = VariantValue(std::move(value));
		}

		// The components of entities by path (the JSON RebaseOverrides resolves an override's Variant value against), and the
		// lookup over them.
		class ComponentOwners
		{
		public:
			// The current JSON of the members of `members` whose paths lie within `prefix`.
			ComponentOwners(const Scene& scene, const InstanceMembers& members, std::span<const UUID> prefix)
			{
				for (size_t index = 0; index < members.Members.size(); ++index)
				{
					if (!StartsWith(members.Paths[index], prefix))
						continue;
					if (Result<Json> json = SceneSerializer::EntityToJson(scene.FindEntityByID(members.Members[index])))
					{
						const auto [owned, inserted] = m_Owned.emplace(members.Paths[index], std::move(*json));
						if (inserted)
							m_Entities.emplace(owned->first, &owned->second);
					}
				}
			}

			// The entities of `document` (which outlives this).
			explicit ComponentOwners(const PrefabDocument& document)
			{
				for (const DocumentEntity& entity : document.GetEntities())
					m_Entities.emplace(entity.Path, entity.Object);
			}

			// The JSON of `component` on the entity whose path is `path`, or null.
			[[nodiscard]] const Json* Find(const OverridePath& path, std::string_view component) const
			{
				const auto found = m_Entities.find(path);
				if (found == m_Entities.end())
					return nullptr;
				const Json& owner = GetMember(GetMember(*found->second, ComponentsKey), component);
				return owner.is_object() ? &owner : nullptr;
			}
		private:
			std::map<OverridePath, Json> m_Owned;           // the members' JSON (node addresses are stable)
			std::map<OverridePath, const Json*> m_Entities; // lookup only
		};

		// True for an override of a level root's Name or Transform: its placement in the prefab enclosing it, implicit at its
		// own level (decision 9), never written into the level's prefab or a nested instance's record.
		static bool IsPlacementOverride(const PrefabOverride& entry, const ComponentInfo* transform)
		{
			if (entry.Kind == PrefabOverrideKind::EntityKey)
				return entry.Field == "Name";
			return transform != nullptr && entry.Component == transform->GetName();
		}

		// The overrides of `overrides` whose paths lie within `prefix` (all of them for an empty prefix), re-based onto the
		// level whose path is `prefix`: the prefix dropped, an override of the level root itself at [levelRootID] unless it is
		// a placement override (IsPlacementOverride), which stays out, and each value's entity references mapped through
		// `values`. `owners` gives the JSON of the component an override targets, by its path before re-basing. Sorted.
		static std::vector<PrefabOverride> RebaseOverrides(const TypeRegistry& registry, std::span<const PrefabOverride> overrides,
			std::span<const UUID> prefix, UUID levelRootID, const EntityRefRemapper& values, const ComponentOwners& owners)
		{
			const ComponentInfo* transform = registry.FindComponent<TransformComponent>();
			std::vector<PrefabOverride> rebased;
			for (const PrefabOverride& entry : overrides)
			{
				if (!StartsWith(entry.Path, prefix))
					continue;
				PrefabOverride moved = entry;
				moved.Path.assign(entry.Path.begin() + static_cast<std::ptrdiff_t>(prefix.size()), entry.Path.end());
				if (moved.Path.empty())
				{
					if (IsPlacementOverride(entry, transform))
						continue;
					moved.Path = { levelRootID };
				}
				RemapOverrideValue(registry, values, moved, owners.Find(entry.Path, entry.Component));
				rebased.push_back(std::move(moved));
			}
			std::sort(rebased.begin(), rebased.end(), &IsLess);
			return rebased;
		}

		// The record `record` of an instance (its overrides relative to its own prefab) composed with `changes`, overrides of
		// the same instance in the same space that describe what changed on top of the record's result (a nested instance's
		// record in a document, after prefab.apply wrote an edit of it there, decision 9): a change replaces the record's
		// override of the same target; a key patch of a Map of Variants merges into the record's; a field of a component the
		// record adds is written into the added value; a removed component drops the record's overrides of it, and is
		// recorded unless the record had added it; `removed` lists the paths a RemoveEntity of `changes` takes out with their
		// descendants, whose overrides go. The result applied to the prefab gives what the record and the changes give; it is
		// not always minimal (a change back to the prefab's value is kept), which the next RefreshOverrides of the instance
		// makes exact. Sorted.
		static std::vector<PrefabOverride> MergeOverrides(const TypeRegistry& registry, std::vector<PrefabOverride> record,
			std::span<const PrefabOverride> changes, std::span<const OverridePath> removed)
		{
			const std::set<OverridePath> gone(removed.begin(), removed.end());
			std::erase_if(record, [&gone](const PrefabOverride& entry)
			{
				return gone.contains(entry.Path);
			});
			const auto find = [&record](const OverridePath& path, PrefabOverrideKind kind, std::string_view component, std::string_view field)
			{
				return std::find_if(record.begin(), record.end(), [&](const PrefabOverride& entry)
				{
					return entry.Path == path && entry.Kind == kind && entry.Component == component && entry.Field == field;
				});
			};
			const auto keyedMap = [&registry](std::string_view component, std::string_view field)
			{
				const ComponentInfo* info = registry.FindComponent(component);
				const FieldInfo* fieldInfo = info != nullptr ? info->FindField(field) : nullptr;
				return fieldInfo != nullptr && IsKeyedVariantMap(*fieldInfo);
			};

			for (const PrefabOverride& change : changes)
			{
				switch (change.Kind)
				{
					case PrefabOverrideKind::RemoveEntity:
						record.push_back(change);
						break;
					case PrefabOverrideKind::EntityKey:
					{
						if (const auto existing = find(change.Path, change.Kind, change.Component, change.Field); existing != record.end())
							existing->Value = change.Value;
						else
							record.push_back(change);
						break;
					}
					case PrefabOverrideKind::AddComponent:
					{
						std::erase_if(record, [&change](const PrefabOverride& entry)
						{
							return entry.Path == change.Path && entry.Component == change.Component
								&& (entry.Kind == PrefabOverrideKind::Field || entry.Kind == PrefabOverrideKind::AddComponent);
						});
						record.push_back(change);
						break;
					}
					case PrefabOverrideKind::RemoveComponent:
					{
						const bool added = find(change.Path, PrefabOverrideKind::AddComponent, change.Component, {}) != record.end();
						const bool removedBefore = find(change.Path, PrefabOverrideKind::RemoveComponent, change.Component, {}) != record.end();
						std::erase_if(record, [&change](const PrefabOverride& entry)
						{
							return entry.Path == change.Path && entry.Component == change.Component
								&& (entry.Kind == PrefabOverrideKind::Field || entry.Kind == PrefabOverrideKind::AddComponent);
						});
						// A component the record added is not the prefab's: removing it leaves nothing to record.
						if (!added && !removedBefore)
							record.push_back(change);
						break;
					}
					case PrefabOverrideKind::Field:
					{
						const bool keyed = keyedMap(change.Component, change.Field);
						if (const auto added = find(change.Path, PrefabOverrideKind::AddComponent, change.Component, {}); added != record.end())
						{
							Json value = added->Value.Get();
							if (keyed && change.Value.Get().is_object())
								ApplyKeyPatch(value[change.Field], change.Value.Get());
							else
								value[change.Field] = change.Value.Get();
							added->Value = VariantValue(std::move(value));
						}
						else if (const auto existing = find(change.Path, change.Kind, change.Component, change.Field); existing != record.end())
						{
							if (keyed && existing->Value.Get().is_object() && change.Value.Get().is_object())
							{
								Json patch = existing->Value.Get();
								for (auto entry = change.Value.Get().begin(); entry != change.Value.Get().end(); ++entry)
									patch[entry.key()] = *entry;
								existing->Value = VariantValue(std::move(patch));
							}
							else
							{
								existing->Value = change.Value;
							}
						}
						else
						{
							record.push_back(change);
						}
						break;
					}
				}
			}
			std::sort(record.begin(), record.end(), &IsLess);
			return record;
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

		// The prefab entities `overrides` remove from an instance of `document`'s prefab (RemovedEntities). A RemoveEntity does
		// not apply when the prefab no longer has its entity, when it names the prefab's root (deleting the root deletes the
		// instance), or when another one removes an ancestor of its entity (the diff records only the top of each removed
		// subtree); the entity of the last kind is removed all the same.
		static RemovedEntities CollectRemovedEntities(const PrefabDocument& document, std::span<const PrefabOverride> overrides)
		{
			RemovedEntities removed;
			removed.Problems.resize(overrides.size());
			std::unordered_set<UUID> tops; // lookup only
			for (size_t index = 0; index < overrides.size(); ++index)
			{
				const PrefabOverride& prefabOverride = overrides[index];
				if (prefabOverride.Kind != PrefabOverrideKind::RemoveEntity)
					continue;
				const DocumentEntity* entity = document.FindByPath(prefabOverride.Path);
				if (entity == nullptr)
					removed.Problems[index] = "the prefab no longer has that entity";
				else if (entity->ID == document.GetRootID())
					removed.Problems[index] = "the instance root cannot be removed from its instance";
				else
					tops.insert(entity->ID);
			}
			if (tops.empty())
				return removed;

			// The document's entities come parents first, so a removed parent is known before its children.
			for (const DocumentEntity& entity : document.GetEntities())
			{
				if (tops.contains(entity.ID) || removed.IDs.contains(entity.Parent))
					removed.IDs.insert(entity.ID);
			}
			for (size_t index = 0; index < overrides.size(); ++index)
			{
				const PrefabOverride& prefabOverride = overrides[index];
				if (prefabOverride.Kind != PrefabOverrideKind::RemoveEntity || !removed.Problems[index].empty())
					continue;
				const DocumentEntity* entity = document.FindByPath(prefabOverride.Path);
				if (entity != nullptr && removed.IDs.contains(entity->Parent))
					removed.Problems[index] = "another RemoveEntity override removes an ancestor of that entity";
			}
			return removed;
		}

		// Applies the prefab overrides to the instance's member JSON, each validated against the registry on its own (as a
		// file read validates it: unresolvable Variant values are kept). An override that no longer matches, or whose value
		// is no longer valid there, is dropped with a PREFAB_STALE_OVERRIDE warning. Returns the overrides that were applied,
		// sorted. RemoveEntity overrides were applied when the targets were chosen (`removed`), so only their problems are
		// reported here, and an override of a removed entity is dropped. RemoveComponent overrides go first so that an added
		// component may replace one the prefab excludes it with. Unique-per-scene components are checked against `taken`
		// and the other targets when `checkUnique` (an instance in a scene; a prefab's document does not limit them).
		class OverrideApplier
		{
		public:
			OverrideApplier(const TypeRegistry& registry, const PrefabOptions& options, const PrefabDocument& document,
				const EntityRefRemapper& toInstance, std::vector<MemberTarget>& targets, const std::unordered_set<const ComponentInfo*>& taken,
				const RemovedEntities& removed, UUID instanceRoot, bool checkUnique, LoadReport& report)
				: m_Registry(registry),
				  m_Options(options),
				  m_Document(document),
				  m_ToInstance(toInstance),
				  m_Targets(targets),
				  m_Taken(taken),
				  m_Removed(removed),
				  m_InstanceRoot(instanceRoot),
				  m_CheckUnique(checkUnique),
				  m_Report(report)
			{
				for (size_t index = 0; index < targets.size(); ++index)
					m_TargetIndex.emplace(targets[index].DocumentID, index);
				m_Transform = registry.FindComponent<TransformComponent>();
			}

			[[nodiscard]] std::vector<PrefabOverride> Apply(std::span<const PrefabOverride> overrides)
			{
				std::vector<PrefabOverride> applied;
				for (const PrefabOverrideKind phase : { PrefabOverrideKind::RemoveEntity, PrefabOverrideKind::RemoveComponent,
						 PrefabOverrideKind::AddComponent, PrefabOverrideKind::Field, PrefabOverrideKind::EntityKey })
				{
					for (size_t index = 0; index < overrides.size(); ++index)
					{
						const PrefabOverride& prefabOverride = overrides[index];
						if (prefabOverride.Kind != phase)
							continue;
						const std::string problem = prefabOverride.Kind == PrefabOverrideKind::RemoveEntity ? m_Removed.Problems[index]
																											: ApplyOne(prefabOverride);
						if (problem.empty())
						{
							applied.push_back(prefabOverride);
							continue;
						}
						m_Report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Warning, std::string(PrefabStaleOverrideCode),
							std::format("dropped the {} override {}at path {}: {}", GetKindName(m_Registry, prefabOverride.Kind),
								DescribeTarget(prefabOverride), FormatPath(prefabOverride.Path), problem),
							std::format("/Components/Prefab/Overrides/{}", index), m_InstanceRoot });
					}
				}
				std::sort(applied.begin(), applied.end(), &IsLess);
				return applied;
			}
		private:
			// "of '<target>' " for an override of a component, a field or an entity key; empty for a RemoveEntity, whose
			// target is the entity itself.
			[[nodiscard]] static std::string DescribeTarget(const PrefabOverride& prefabOverride)
			{
				switch (prefabOverride.Kind)
				{
					case PrefabOverrideKind::RemoveEntity:
						return {};
					case PrefabOverrideKind::EntityKey:
						return std::format("of '{}' ", prefabOverride.Field);
					case PrefabOverrideKind::Field:
					case PrefabOverrideKind::AddComponent:
					case PrefabOverrideKind::RemoveComponent:
						break;
				}
				return prefabOverride.Field.empty() ? std::format("of '{}' ", prefabOverride.Component)
													: std::format("of '{}.{}' ", prefabOverride.Component, prefabOverride.Field);
			}

			// Applies one override other than a RemoveEntity; the reason it does not apply otherwise.
			[[nodiscard]] std::string ApplyOne(const PrefabOverride& prefabOverride)
			{
				const DocumentEntity* entity = m_Document.FindByPath(prefabOverride.Path);
				if (entity == nullptr)
					return "the prefab no longer has that entity";
				if (m_Removed.IDs.contains(entity->ID))
					return "a RemoveEntity override removes that entity from the instance";
				const auto found = m_TargetIndex.find(entity->ID);
				if (found == m_TargetIndex.end())
					return "the prefab no longer has that entity";
				MemberTarget& target = m_Targets[found->second];
				const bool isRoot = target.DocumentID == m_Document.GetRootID();

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
					case PrefabOverrideKind::RemoveEntity:
						break; // applied above, before the component lookup, and when the targets were chosen
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
				if (m_CheckUnique && info.HasFlag(ComponentFlags::UniquePerScene))
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
					ApplyKeyPatch(candidate[field->GetName()], value);
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
			const PrefabDocument& m_Document;
			const EntityRefRemapper& m_ToInstance;
			std::vector<MemberTarget>& m_Targets;
			const std::unordered_set<const ComponentInfo*>& m_Taken;
			const RemovedEntities& m_Removed;
			std::unordered_map<UUID, size_t> m_TargetIndex; // document ID -> index in m_Targets; lookup only
			const ComponentInfo* m_Transform = nullptr;
			UUID m_InstanceRoot;
			bool m_CheckUnique = true;
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

		// The prefab components every operation writes; Errors: InvalidState when they are not registered.
		struct PrefabComponentInfos
		{
			const ComponentInfo* Link = nullptr;
			const ComponentInfo* Instance = nullptr;
			const ComponentInfo* Transform = nullptr;
		};

		static Result<PrefabComponentInfos> FindPrefabComponents(const TypeRegistry& registry)
		{
			const PrefabComponentInfos infos{ registry.FindComponent<PrefabLinkComponent>(), registry.FindComponent<PrefabInstanceComponent>(),
				registry.FindComponent<TransformComponent>() };
			ENGINE_CORE_ASSERT(infos.Link != nullptr && infos.Instance != nullptr && infos.Transform != nullptr, "The prefab components are built in");
			if (infos.Link == nullptr || infos.Instance == nullptr || infos.Transform == nullptr)
				return MakeError(ErrorCode::InvalidState, "the prefab components are not registered");
			return infos;
		}

		// The body of UpdateInstance and Revert: the instance rebuilt as `prefab` plus `overrides` (§5.5 "Update"), its
		// nested instances with it. Every check that can fail runs before the scene changes.
		static Status RebuildInstance(Scene& scene, Entity root, const Prefab& prefab, std::span<const PrefabOverride> overrides,
			const PrefabOptions& options, LoadReport& report)
		{
			ENGINE_TRY(RequireOutermostRoot(root));
			if (root.GetScene() != &scene)
			{
				return MakeError(ErrorCode::InvalidArgument, "Entity '{}' ({}) is an instance root of another scene", root.GetName(),
					root.GetUUID());
			}
			ENGINE_TRY(RequirePrefab(prefab));
			const Scene& constScene = scene;
			const TypeRegistry& registry = scene.GetTypeRegistry();
			ENGINE_TRY_ASSIGN(const PrefabComponentInfos infos, FindPrefabComponents(registry));
			ENGINE_TRY_ASSIGN(const Json unknownVersions, GetUnknownComponentVersions(registry, prefab));

			const UUID rootID = root.GetUUID();
			const PrefabDocument document(prefab, *infos.Link, *infos.Instance);
			const InstanceMembers members = CollectMembers(root, prefab.GetRootID());
			const IdMap toInstanceMap = MakeInstanceMap(document, rootID);
			const EntityRefRemapper toInstance(registry, options.Schemas, toInstanceMap);
			// The RemoveEntity overrides decide which prefab entities the instance has: the removed ones are not built, and
			// their members, if any are left, go like those of vanished prefab entities.
			const RemovedEntities removed = CollectRemovedEntities(document, overrides);

			// Member IDs: the derived ID of every prefab entity the instance keeps is free or already a member of this
			// instance. A removed entity's ID may be taken (prefab.create {replaceWithInstance} keeps it for a new instance).
			std::unordered_set<UUID> targetIDs; // lookup only
			for (const DocumentEntity& entity : document.GetEntities())
			{
				if (removed.IDs.contains(entity.ID))
					continue;
				const UUID id = Lookup(toInstanceMap, entity.ID);
				const ConstEntity holder = constScene.FindEntityByID(id);
				if (!id.IsValid() || !targetIDs.insert(id).second || (holder.IsValid() && !members.IDs.contains(id)))
				{
					return MakeError(ErrorCode::InvalidState,
						"the member ID {} of prefab entity {} is already used in the scene outside the instance", id, entity.ID);
				}
			}

			// The rebuilt members' JSON in prefab order: the prefab in instance space, the root keeping its Name, Transform
			// and Parent (implicit overrides), then the overrides.
			const Entity rootParent = root.GetParent();
			std::vector<MemberTarget> targets;
			targets.reserve(document.GetEntities().size());
			for (const DocumentEntity& entity : document.GetEntities())
			{
				if (removed.IDs.contains(entity.ID))
					continue;
				MemberTarget target;
				target.DocumentID = entity.ID;
				target.ID = Lookup(toInstanceMap, entity.ID);
				ENGINE_TRY_ASSIGN(target.Object, MakeMemberJson(entity, rootID, toInstanceMap, toInstance, *infos.Link, *infos.Instance));
				if (entity.ID == prefab.GetRootID())
				{
					target.Object["Parent"] = rootParent.IsValid() ? Json(rootParent.GetUUID().ToString()) : Json(nullptr);
					target.Object["Name"] = root.GetName();
					ENGINE_TRY_ASSIGN(target.Object[ComponentsKey][infos.Transform->GetName()],
						infos.Transform->ToJson(&root.GetComponent<TransformComponent>()));
				}
				target.Parent = ReadEntityUUID(target.Object, "Parent");
				targets.push_back(std::move(target));
			}

			// The overrides go first, so that a unique component the instance removed (RemoveComponent) and the scene now has
			// elsewhere does not block the update. Their warnings join `report` once nothing can fail any more.
			const std::unordered_set<const ComponentInfo*> taken =
				CollectUniqueComponentsOutside(constScene, CollectUniqueCandidates(registry, targets, overrides), members.IDs);
			LoadReport overrideReport;
			OverrideApplier applier(registry, options, document, toInstance, targets, taken, removed, rootID, true, overrideReport);
			PrefabInstanceComponent instance;
			instance.Prefab = root.GetComponent<PrefabInstanceComponent>().Prefab;
			instance.Overrides = applier.Apply(overrides);
			ENGINE_TRY(CheckUniqueComponents(registry, targets, taken));
			ENGINE_CORE_ASSERT(targets.front().DocumentID == prefab.GetRootID(), "Canonical order starts with the prefab's root");
			ENGINE_TRY_ASSIGN(targets.front().Object[ComponentsKey][infos.Instance->GetName()], infos.Instance->ToJson(&instance));
			report.Diagnostics.insert(report.Diagnostics.end(), std::make_move_iterator(overrideReport.Diagnostics.begin()),
				std::make_move_iterator(overrideReport.Diagnostics.end()));

			// Nothing below fails for a validated prefab: every ID, parent and unique component was checked above.
			std::unordered_set<UUID> doomed; // lookup only: members whose prefab entity is gone or removed
			for (const UUID member : members.Members)
			{
				if (!targetIDs.contains(member))
					doomed.insert(member);
			}
			const auto nearestSurvivor = [&doomed](Entity entity)
			{
				Entity ancestor = entity.GetParent();
				while (ancestor.IsValid() && doomed.contains(ancestor.GetUUID()))
					ancestor = ancestor.GetParent();
				return ancestor;
			};
			for (const UUID memberID : members.Members)
			{
				const Entity member = scene.FindEntityByID(memberID);
				if (!doomed.contains(memberID) || !member.IsValid())
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
			for (const UUID memberID : members.Members)
			{
				const Entity member = scene.FindEntityByID(memberID);
				if (member.IsValid() && doomed.contains(memberID))
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

		// Maps the document IDs of the outermost instance `members` (rooted at `root`, its prefab's root `rootPrefabID`) that
		// lie within the level whose path is `prefix` to their IDs in that level's prefab (the level root to
		// `levelRootID`), and, when `outsideToScene`, those outside it to their IDs in the scene.
		static IdMap MakeLevelMap(const InstanceMembers& members, std::span<const UUID> prefix, UUID levelRootID, bool outsideToScene)
		{
			IdMap map;
			for (size_t index = 0; index < members.Members.size(); ++index)
			{
				const OverridePath& path = members.Paths[index];
				const UUID documentID = GetDocumentID(path);
				if (!prefix.empty() && StartsWith(path, prefix))
				{
					const std::span<const UUID> rest = std::span<const UUID>(path).subspan(prefix.size());
					map.emplace(documentID, rest.empty() ? levelRootID : GetDocumentID(rest));
				}
				else if (outsideToScene)
				{
					map.emplace(documentID, members.Members[index]);
				}
			}
			return map;
		}

		// The paths, relative to the nested root whose path is `prefix`, of the entities each RemoveEntity of `changes` (its
		// path relative to that root) removes from `document` with their descendants (MergeOverrides' `removed`).
		static std::vector<OverridePath> CollectRemovedPaths(const PrefabDocument& document, std::span<const UUID> prefix,
			std::span<const PrefabOverride> changes)
		{
			std::vector<OverridePath> removed;
			for (const PrefabOverride& change : changes)
			{
				if (change.Kind != PrefabOverrideKind::RemoveEntity)
					continue;
				OverridePath full(prefix.begin(), prefix.end());
				full.insert(full.end(), change.Path.begin(), change.Path.end());
				const DocumentEntity* top = document.FindByPath(full);
				if (top == nullptr)
					continue;
				for (const DocumentEntity* entity : document.CollectSubtree(top->ID))
				{
					if (StartsWith(entity->Path, prefix) && entity->Path.size() > prefix.size())
						removed.emplace_back(entity->Path.begin() + static_cast<std::ptrdiff_t>(prefix.size()), entity->Path.end());
				}
			}
			return removed;
		}

		// Reads the Prefab component JSON `json` (a document's nested root's) into a PrefabInstanceComponent. Errors: those of
		// the registry read.
		static Result<PrefabInstanceComponent> ReadInstanceComponent(const ComponentInfo& instanceInfo, const Json& json, const IFieldSchemaSource* schemas)
		{
			const ObjectPtr object = instanceInfo.CreateDefault();
			ReadContext context;
			context.Schemas = schemas;
			ENGINE_TRY(instanceInfo.FromJson(object.get(), JsonReader(json), context));
			return *static_cast<const PrefabInstanceComponent*>(object.get());
		}

		// The Prefab component of the instance nested in `document` at the outermost-in-document root `nestedRoot`, with its
		// record composed with `changes` (decision 9: an override written into a prefab that nests an instance becomes that
		// instance's own override there; `changes` are the overrides that targeted it, relative to it, in its prefab's space).
		// Errors: those of reading its Prefab component.
		static Result<PrefabInstanceComponent> MergeNestedRecord(const TypeRegistry& registry, const ComponentInfo& instanceInfo,
			const PrefabDocument& document, const DocumentEntity& nestedRoot, std::span<const PrefabOverride> changes, const IFieldSchemaSource* schemas)
		{
			const Json& component = GetMember(GetMember(*nestedRoot.Object, ComponentsKey), instanceInfo.GetName());
			ENGINE_TRY_ASSIGN(PrefabInstanceComponent current, ReadInstanceComponent(instanceInfo, component, schemas));
			const std::vector<OverridePath> removed = CollectRemovedPaths(document, nestedRoot.Path, changes);
			current.Overrides = MergeOverrides(registry, std::move(current.Overrides), changes, removed);
			return current;
		}

		// `prefab` with `overrides` (paths and values in its own space) written into its document (the prefab.apply of a level,
		// decision 9): applied to its entities' JSON as an update applies them, dropping the stale ones with a warning into
		// `report`, a RemoveEntity removing its entity with its descendants; an override that targets an instance nested in
		// the document, or the nested root itself (but its Name and Transform), also becomes that instance's own override in
		// its record (MergeNestedRecord). Errors: Validation when the result fails to load as a prefab; those of the reads.
		static Result<Prefab> WriteOverridesToDocument(const TypeRegistry& registry, const Prefab& prefab, std::span<const PrefabOverride> overrides,
			const PrefabOptions& options, LoadReport& report)
		{
			ENGINE_TRY_ASSIGN(const PrefabComponentInfos infos, FindPrefabComponents(registry));
			const PrefabDocument document(prefab, *infos.Link, *infos.Instance);
			const IdMap identity;
			const EntityRefRemapper unchanged(registry, options.Schemas, identity);
			const RemovedEntities removed = CollectRemovedEntities(document, overrides);
			std::vector<MemberTarget> targets;
			targets.reserve(document.GetEntities().size());
			for (const DocumentEntity& entity : document.GetEntities())
				targets.push_back(MemberTarget{ entity.ID, entity.ID, entity.Parent, *entity.Object });
			const std::unordered_set<const ComponentInfo*> taken;
			OverrideApplier applier(registry, options, document, unchanged, targets, taken, removed, prefab.GetRootID(), false, report);
			const std::vector<PrefabOverride> applied = applier.Apply(overrides);

			// The applied overrides of each outermost-in-document nested instance, re-based onto it, with the document IDs of
			// its entities mapped to its prefab's, become its own record's.
			const ComponentOwners owners(document);
			for (MemberTarget& target : targets)
			{
				const DocumentEntity* entity = document.FindByID(target.DocumentID);
				if (entity == nullptr || !entity->IsNestedRoot || entity->Path.size() != 1 || removed.IDs.contains(entity->ID))
					continue;
				const UUID nestedPrefabRoot = ReadEntityUUID(GetMember(GetMember(*entity->Object, ComponentsKey), infos.Link->GetName()), "PrefabEntityID");
				IdMap values{ { entity->ID, nestedPrefabRoot } };
				for (const DocumentEntity& inner : document.GetEntities())
				{
					if (inner.Path.size() > 1 && inner.Path.front() == entity->ID)
						values.emplace(inner.ID, GetDocumentID(std::span<const UUID>(inner.Path).subspan(1)));
				}
				const EntityRefRemapper remapper(registry, options.Schemas, values);
				const std::vector<PrefabOverride> changes = RebaseOverrides(registry, applied, entity->Path, nestedPrefabRoot, remapper, owners);
				if (changes.empty())
					continue;
				ENGINE_TRY_ASSIGN(const PrefabInstanceComponent record,
					MergeNestedRecord(registry, *infos.Instance, document, *entity, changes, options.Schemas));
				ENGINE_TRY_ASSIGN(target.Object[ComponentsKey][infos.Instance->GetName()], infos.Instance->ToJson(&record));
			}

			Json entities = Json::array();
			for (MemberTarget& target : targets)
			{
				if (!removed.IDs.contains(target.DocumentID))
					entities.push_back(std::move(target.Object));
			}
			ENGINE_TRY_ASSIGN(const Json versions, GetUnknownComponentVersions(registry, prefab));
			std::vector<std::pair<std::string, uint32_t>> unknownVersions;
			for (auto entry = versions.begin(); entry != versions.end(); ++entry)
			{
				if (const Result<uint32_t> version = JsonReader(*entry).ReadUInt32())
					unknownVersions.emplace_back(entry.key(), *version);
			}
			const Json written = MakePrefabDocument(registry, prefab.GetName(), prefab.GetRootID(), std::move(entities), unknownVersions);
			LoadOptions loadOptions;
			loadOptions.Schemas = options.Schemas;
			LoadReport loadReport;
			return Prefab::FromJson(written, registry, loadOptions, loadReport);
		}

		Result<std::vector<PrefabOverride>> TakeNestedOverrides(ConstEntity nestedRoot)
		{
			if (!IsNestedInstanceRoot(nestedRoot))
			{
				return MakeError(ErrorCode::InvalidArgument, "Entity '{}' ({}) is not a nested prefab instance root", nestedRoot.GetName(),
					nestedRoot.GetUUID());
			}
			ENGINE_TRY_ASSIGN(const std::vector<ConstEntity> levels, FindInstanceLevels(nestedRoot));
			const ConstEntity outermost = levels.front();
			const PrefabLinkComponent* outermostLink = outermost.TryGetComponent<PrefabLinkComponent>();
			const InstanceMembers members = CollectMembers(outermost, outermostLink != nullptr ? outermostLink->PrefabEntityID : UUID());
			const auto position = std::find(members.Members.begin(), members.Members.end(), nestedRoot.GetUUID());
			if (position == members.Members.end())
				return std::vector<PrefabOverride>{};
			const OverridePath prefix = members.Paths[static_cast<size_t>(position - members.Members.begin())];
			const UUID levelRoot = nestedRoot.GetComponent<PrefabLinkComponent>().PrefabEntityID;

			const Scene& scene = *nestedRoot.GetScene();
			const TypeRegistry& registry = scene.GetTypeRegistry();
			const IdMap values = MakeLevelMap(members, prefix, levelRoot, true);
			const EntityRefRemapper remapper(registry, nullptr, values);
			const ComponentOwners owners(scene, members, prefix);
			return RebaseOverrides(registry, outermost.GetComponent<PrefabInstanceComponent>().Overrides, prefix, levelRoot, remapper, owners);
		}

	}

	namespace {

		PrefabDocument::PrefabDocument(const Prefab& prefab, const ComponentInfo& linkInfo, const ComponentInfo& instanceInfo)
			: m_Root(prefab.GetRootID())
		{
			std::unordered_map<UUID, OverridePath> prefixes; // nested root document ID -> its path; lookup only
			const Json& entities = prefab.GetEntities();
			m_Entities.reserve(entities.size());
			for (const Json& object : entities)
			{
				DocumentEntity entity;
				entity.Object = &object;
				entity.ID = Utils::ReadEntityUUID(object, "ID");
				entity.Parent = Utils::ReadEntityUUID(object, "Parent");
				const Json& components = Utils::GetMember(object, ComponentsKey);
				const Json& link = Utils::GetMember(components, linkInfo.GetName());
				const bool isInstanceRoot = components.is_object() && components.contains(instanceInfo.GetName());
				const UUID innerRoot = link.is_object() ? Utils::ReadEntityUUID(link, "InstanceRoot") : UUID();
				if (entity.ID == m_Root || !link.is_object())
				{
					entity.Path = { entity.ID };
				}
				else if (innerRoot == entity.ID && isInstanceRoot)
				{
					// A nested root: outermost in the document, or nested in the instance of the ancestor it derives from.
					OverridePath path{ entity.ID };
					const UUID outer = Utils::ReadEntityUUID(link, "OuterPrefabEntityID");
					for (UUID ancestor = entity.Parent; outer.IsValid() && ancestor.IsValid();)
					{
						const auto prefix = prefixes.find(ancestor);
						if (prefix != prefixes.end() && PrefabInstantiator::DeriveInstanceID(ancestor, outer) == entity.ID)
						{
							path = prefix->second;
							path.push_back(outer);
							break;
						}
						const DocumentEntity* parent = FindByID(ancestor);
						ancestor = parent != nullptr ? parent->Parent : UUID();
					}
					entity.Path = path;
					entity.InnerRoot = entity.ID;
					entity.IsNestedRoot = true;
					prefixes.emplace(entity.ID, std::move(path));
				}
				else if (const auto prefix = prefixes.find(innerRoot); prefix != prefixes.end())
				{
					entity.Path = prefix->second;
					entity.Path.push_back(Utils::ReadEntityUUID(link, "PrefabEntityID"));
					entity.InnerRoot = innerRoot;
				}
				else
				{
					entity.Path = { entity.ID };
				}
				m_HasNested = m_HasNested || entity.InnerRoot.IsValid();
				m_ByID.emplace(entity.ID, m_Entities.size());
				m_ByPath.emplace(entity.Path, m_Entities.size());
				m_Entities.push_back(std::move(entity));
			}
		}

		std::vector<const DocumentEntity*> PrefabDocument::CollectSubtree(UUID id) const
		{
			std::vector<const DocumentEntity*> subtree;
			std::unordered_set<UUID> inside; // lookup only
			for (const DocumentEntity& entity : m_Entities)
			{
				if (entity.ID == id || inside.contains(entity.Parent))
				{
					inside.insert(entity.ID);
					subtree.push_back(&entity);
				}
			}
			return subtree;
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
		ENGINE_TRY_ASSIGN(const Utils::PrefabComponentInfos infos, Utils::FindPrefabComponents(registry));

		// IDs: a pure function of the root and each entity's path, never re-hashed (ADR 0006 decision 18), so a taken derived
		// ID is an inconsistent scene rather than the caller's choice.
		const PrefabDocument document(prefab, *infos.Link, *infos.Instance);
		const IdMap toInstanceMap = Utils::MakeInstanceMap(document, instance.RootID);
		std::unordered_set<UUID> ids; // lookup only
		for (const DocumentEntity& entity : document.GetEntities())
		{
			const UUID id = Utils::Lookup(toInstanceMap, entity.ID);
			if (!id.IsValid() || !ids.insert(id).second || constScene.FindEntityByID(id).IsValid())
				return MakeError(ErrorCode::InvalidState, "the member ID {} of prefab entity {} is already used in the scene", id, entity.ID);
		}
		ENGINE_TRY_ASSIGN(const Json unknownVersions, Utils::GetUnknownComponentVersions(registry, prefab));

		Json rootTransform;
		if (instance.RootTransform)
		{
			ENGINE_TRY_ASSIGN(const Json written, WithContext(infos.Transform->ToJson(&*instance.RootTransform), "in the instance's root transform"));
			ENGINE_TRY_ASSIGN(rootTransform, WithContext(Utils::Canonicalize(*infos.Transform, written, nullptr), "in the instance's root transform"));
		}

		const EntityRefRemapper toInstance(registry, options.Schemas, toInstanceMap);
		std::vector<MemberTarget> targets;
		targets.reserve(document.GetEntities().size());
		for (const DocumentEntity& entity : document.GetEntities())
		{
			MemberTarget target;
			target.DocumentID = entity.ID;
			target.ID = Utils::Lookup(toInstanceMap, entity.ID);
			ENGINE_TRY_ASSIGN(target.Object, Utils::MakeMemberJson(entity, instance.RootID, toInstanceMap, toInstance, *infos.Link, *infos.Instance));
			if (entity.ID == prefab.GetRootID())
			{
				target.Object["Parent"] = instance.Parent.IsValid() ? Json(instance.Parent.GetUUID().ToString()) : Json(nullptr);
				if (instance.RootTransform)
					target.Object[ComponentsKey][infos.Transform->GetName()] = rootTransform;
				PrefabInstanceComponent component;
				component.Prefab = TypedAssetHandle<AssetType::Prefab>(instance.PrefabHandle);
				ENGINE_TRY_ASSIGN(target.Object[ComponentsKey][infos.Instance->GetName()], infos.Instance->ToJson(&component));
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
		ENGINE_TRY(Utils::RequireOutermostRoot(instanceRoot));
		const std::vector<PrefabOverride> overrides = instanceRoot.GetComponent<PrefabInstanceComponent>().Overrides;
		return Utils::RebuildInstance(scene, instanceRoot, prefab, overrides, options, report);
	}

	Result<std::vector<PrefabOverride>> PrefabInstantiator::ComputeOverrides(Entity instanceRoot, const Prefab& prefab,
		const PrefabOptions& options)
	{
		ENGINE_TRY(Utils::RequireOutermostRoot(instanceRoot));
		ENGINE_TRY(Utils::RequirePrefab(prefab));
		const Scene& scene = *instanceRoot.GetScene();
		const TypeRegistry& registry = scene.GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const Utils::PrefabComponentInfos infos, Utils::FindPrefabComponents(registry));

		const UUID rootID = instanceRoot.GetUUID();
		const PrefabDocument document(prefab, *infos.Link, *infos.Instance);
		const InstanceMembers members = Utils::CollectMembers(instanceRoot, prefab.GetRootID());
		const IdMap toInstanceMap = Utils::MakeInstanceMap(document, rootID);
		const EntityRefRemapper toInstance(registry, options.Schemas, toInstanceMap);
		IdMap toPrefabMap;
		for (size_t index = 0; index < members.Members.size(); ++index)
			toPrefabMap.emplace(members.Members[index], Utils::GetMemberDocumentID(document, members, index));
		const std::unordered_set<UUID> prefabIDs = Utils::CollectPrefabIDs(prefab);
		std::vector<UUID> ambiguous;
		const EntityRefRemapper toPrefab(registry, options.Schemas, toPrefabMap, prefabIDs, ambiguous);

		std::vector<PrefabOverride> overrides;
		std::unordered_set<UUID> present; // lookup only: the prefab entities the instance has a member of
		for (size_t index = 0; index < members.Members.size(); ++index)
		{
			const DocumentEntity* entity = document.FindByPath(members.Paths[index]);
			if (entity == nullptr)
				continue; // a member of a prefab entity that no longer exists: the next update removes it
			present.insert(entity->ID);
			const ConstEntity member = scene.FindEntityByID(members.Members[index]);
			ENGINE_TRY_ASSIGN(const Json expected, Utils::MakeMemberJson(*entity, rootID, toInstanceMap, toInstance, *infos.Link, *infos.Instance));
			ENGINE_TRY_ASSIGN(const Json actual, SceneSerializer::EntityToJson(member));
			ENGINE_TRY(Utils::DiffMember(registry, member.GetUUID(), entity->Path, member == instanceRoot, expected, actual, toPrefab, ambiguous,
				overrides));
		}

		// Deleted members (Docs/Decisions/0031-m20-contract.md decision 7): one RemoveEntity for the top of each removed
		// subtree, a prefab entity without a member whose parent in the document has one; its descendants go with it and get
		// no override of their own. The root always has its member, the instance root itself.
		for (const DocumentEntity& entity : document.GetEntities())
		{
			if (!present.contains(entity.ID) && present.contains(entity.Parent))
				overrides.push_back(PrefabOverride{ entity.Path, PrefabOverrideKind::RemoveEntity, {}, {}, VariantValue() });
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
		ENGINE_TRY(Utils::RequireOutermostRoot(instanceRoot));
		const PrefabLinkComponent* rootLink = instanceRoot.TryGetComponent<PrefabLinkComponent>();
		const InstanceMembers members = Utils::CollectMembers(instanceRoot, rootLink != nullptr ? rootLink->PrefabEntityID : UUID());
		Scene& scene = *instanceRoot.GetScene();

		// One level (decision 8): each instance nested directly in this one becomes outermost, taking over the overrides
		// recorded for it, before the root's records go.
		std::vector<std::pair<UUID, std::vector<PrefabOverride>>> nested;
		for (size_t index = 0; index < members.Members.size(); ++index)
		{
			const UUID memberID = members.Members[index];
			if (memberID == instanceRoot.GetUUID() || members.InnerRoots[index] != memberID || members.Paths[index].size() != 1)
				continue;
			ENGINE_TRY_ASSIGN(std::vector<PrefabOverride> moved, Utils::TakeNestedOverrides(scene.FindEntityByID(memberID)));
			nested.emplace_back(memberID, std::move(moved));
		}
		for (auto& [rootID, overrides] : nested)
		{
			const Entity root = scene.FindEntityByID(rootID);
			root.Patch<PrefabLinkComponent>([](PrefabLinkComponent& link)
			{
				link.OuterPrefabEntityID = UUID();
			});
			root.Patch<PrefabInstanceComponent>([&overrides](PrefabInstanceComponent& component)
			{
				component.Overrides = std::move(overrides);
			});
		}
		for (size_t index = 0; index < members.Members.size(); ++index)
		{
			if (members.InnerRoots[index] != instanceRoot.GetUUID())
				continue;
			const Entity member = scene.FindEntityByID(members.Members[index]);
			if (member.HasComponent<PrefabLinkComponent>())
				member.RemoveComponent<PrefabLinkComponent>();
		}
		instanceRoot.RemoveComponent<PrefabInstanceComponent>();
		return {};
	}

	Result<Prefab> PrefabInstantiator::ApplyOverrides(Entity instanceRoot, const Prefab& prefab, const PrefabOptions& options)
	{
		ENGINE_TRY(Utils::RequireOutermostRoot(instanceRoot));
		ENGINE_TRY(Utils::RequirePrefab(prefab));
		const Scene& scene = *instanceRoot.GetScene();
		const TypeRegistry& registry = scene.GetTypeRegistry();
		ENGINE_TRY_ASSIGN(const Utils::PrefabComponentInfos infos, Utils::FindPrefabComponents(registry));

		const UUID rootID = instanceRoot.GetUUID();
		const PrefabDocument document(prefab, *infos.Link, *infos.Instance);
		const InstanceMembers members = Utils::CollectMembers(instanceRoot, prefab.GetRootID());
		std::vector<UUID> documentIDs(members.Members.size());
		std::unordered_map<UUID, size_t> memberIndex; // member ID -> index in members; lookup only
		IdMap toPrefabMap;
		for (size_t index = 0; index < members.Members.size(); ++index)
		{
			documentIDs[index] = Utils::GetMemberDocumentID(document, members, index);
			memberIndex.emplace(members.Members[index], index);
			toPrefabMap.emplace(members.Members[index], documentIDs[index]);
		}
		// A prefab entity without a member was deleted from the instance (RemoveEntity), so the written document leaves it
		// out with its descendants. A reference to its member, whose ID no entity of the scene holds any more, maps back to
		// its prefab-local ID: it stays as the prefab had it and names no entity, as any reference to a deleted entity does
		// (Docs/Decisions/0031-m20-contract.md decision 7). An entity that holds that ID now (prefab.create
		// {replaceWithInstance} keeps a member's ID for its new instance) is outside the instance and is kept as it is.
		for (const DocumentEntity& entity : document.GetEntities())
		{
			const UUID id = Utils::GetMemberID(document, entity, rootID);
			if (entity.ID != prefab.GetRootID() && !scene.FindEntityByID(id).IsValid())
				toPrefabMap.emplace(id, entity.ID);
		}
		const std::unordered_set<UUID> prefabIDs = Utils::CollectPrefabIDs(prefab);
		std::vector<UUID> ambiguous;
		const EntityRefRemapper toPrefab(registry, options.Schemas, toPrefabMap, prefabIDs, ambiguous);
		const Json* prefabRoot = prefab.FindEntity(prefab.GetRootID());
		ENGINE_CORE_ASSERT(prefabRoot != nullptr, "A prefab contains its root");

		// The overrides within each instance nested directly in this one: written below as that instance's own record in
		// the document (decision 9), composed with what the document records for it.
		std::vector<PrefabOverride> current;
		const bool nests = std::any_of(members.Paths.begin(), members.Paths.end(), [](const OverridePath& path)
		{
			return path.size() > 1;
		}) || document.HasNestedInstances();
		if (nests)
		{
			ENGINE_TRY_ASSIGN(current, ComputeOverrides(instanceRoot, prefab, options));
		}

		Json entities = Json::array();
		std::vector<std::pair<std::string, uint32_t>> unknownVersions;
		std::unordered_set<UUID> written; // lookup only: prefab entities already written
		for (size_t index = 0; index < members.Members.size(); ++index)
		{
			const UUID documentID = documentIDs[index];
			if (!written.insert(documentID).second)
				continue; // a second member claiming the same prefab entity (a hand edit): the first in canonical order wins
			const ConstEntity member = scene.FindEntityByID(members.Members[index]);
			const UUID innerRoot = members.InnerRoots[index];

			// The ID and parent are set from the member table, and the link components of the prefab's own entities go;
			// an entity of a nested instance keeps them, in document space (MakeMemberJson's inverse). The other components'
			// references go through the remapper.
			ENGINE_TRY_ASSIGN(Json json, SceneSerializer::EntityToJson(member));
			json["ID"] = documentID.ToString();
			Json& components = json[ComponentsKey];
			if (innerRoot == rootID)
			{
				components.erase(infos.Link->GetName());
				components.erase(infos.Instance->GetName());
			}
			else
			{
				PrefabLinkComponent link = member.GetComponent<PrefabLinkComponent>();
				const auto rootIndex = memberIndex.find(innerRoot);
				link.InstanceRoot = rootIndex != memberIndex.end() ? documentIDs[rootIndex->second] : link.InstanceRoot;
				const bool isNestedRoot = innerRoot == member.GetUUID();
				const bool outermostInDocument = isNestedRoot && members.Paths[index].size() == 1;
				if (!isNestedRoot || outermostInDocument)
					link.OuterPrefabEntityID = UUID();
				ENGINE_TRY_ASSIGN(components[infos.Link->GetName()], infos.Link->ToJson(&link));
				if (outermostInDocument)
				{
					// Its record: what the document records for it, composed with the instance's overrides within it.
					const IdMap values = Utils::MakeLevelMap(members, members.Paths[index], link.PrefabEntityID, false);
					const EntityRefRemapper remapper(registry, options.Schemas, values);
					const Utils::ComponentOwners owners(scene, members, members.Paths[index]);
					const std::vector<PrefabOverride> changes = Utils::RebaseOverrides(registry, current, members.Paths[index], link.PrefabEntityID, remapper, owners);
					PrefabInstanceComponent instance = member.GetComponent<PrefabInstanceComponent>();
					if (const DocumentEntity* nestedRoot = document.FindByPath(members.Paths[index]); nestedRoot != nullptr && nestedRoot->IsNestedRoot)
					{
						ENGINE_TRY_ASSIGN(instance, Utils::MergeNestedRecord(registry, *infos.Instance, document, *nestedRoot, changes, options.Schemas));
					}
					else
					{
						instance.Overrides = changes;
					}
					ENGINE_TRY_ASSIGN(components[infos.Instance->GetName()], infos.Instance->ToJson(&instance));
				}
			}
			toPrefab.RemapComponents(json);
			if (!ambiguous.empty())
				return std::unexpected(Utils::MakeAmbiguousReferenceError(member.GetUUID(), "/Components", ambiguous.front()));
			if (member == instanceRoot)
			{
				// The root's Name, Transform and Parent are the instance's own (implicit overrides), never the prefab's.
				json["Parent"] = nullptr;
				json["Name"] = Utils::GetMember(*prefabRoot, "Name");
				components[infos.Transform->GetName()] = Utils::GetMember(Utils::GetMember(*prefabRoot, ComponentsKey), infos.Transform->GetName());
			}
			else
			{
				// The nearest member above it (user children in between are not part of the prefab).
				ConstEntity parent = member.GetParent();
				while (parent.IsValid() && !members.IDs.contains(parent.GetUUID()))
					parent = parent.GetParent();
				const auto parentIndex = parent.IsValid() ? memberIndex.find(parent.GetUUID()) : memberIndex.end();
				json["Parent"] = parentIndex != memberIndex.end() ? Json(documentIDs[parentIndex->second].ToString()) : Json(nullptr);
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

		const Json result = Utils::MakePrefabDocument(registry, prefab.GetName(), prefab.GetRootID(), std::move(entities), unknownVersions);
		LoadOptions loadOptions;
		loadOptions.Schemas = options.Schemas;
		LoadReport report;
		return Prefab::FromJson(result, registry, loadOptions, report);
	}

	Result<std::vector<Entity>> PrefabInstantiator::GetInstanceLevels(Entity entity)
	{
		ENGINE_TRY_ASSIGN(const std::vector<ConstEntity> levels, Utils::FindInstanceLevels(entity));
		std::vector<Entity> result;
		result.reserve(levels.size());
		for (const ConstEntity level : levels)
			result.push_back(entity.GetScene()->FindEntityByID(level.GetUUID()));
		return result;
	}

	Result<std::vector<UUID>> PrefabInstantiator::GetOverridePath(ConstEntity entity)
	{
		ENGINE_TRY_ASSIGN(const std::vector<ConstEntity> levels, Utils::FindInstanceLevels(entity));
		if (levels.empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "Entity '{}' ({}) belongs to no prefab instance, so it has no override path", entity.GetName(),
				entity.GetUUID());
		}
		std::vector<UUID> path;
		for (size_t level = 1; level < levels.size(); ++level)
			path.push_back(levels[level].GetComponent<PrefabLinkComponent>().OuterPrefabEntityID);
		if (entity == levels.back() && levels.size() > 1)
			return path; // a nested root: its path ends with its ID in the prefab that encloses it
		const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>();
		if (link == nullptr)
		{
			return MakeError(ErrorCode::InvalidState, "the prefab instance root '{}' ({}) has no PrefabLink naming its prefab entity", entity.GetName(),
				entity.GetUUID());
		}
		path.push_back(link->PrefabEntityID);
		return path;
	}

	Result<Prefab> PrefabInstantiator::ApplyOverrides(Entity instance, Entity level, const Prefab& levelPrefab, const PrefabOptions& options)
	{
		ENGINE_TRY(Utils::RequireInstanceRoot(instance));
		ENGINE_TRY(Utils::RequirePrefab(levelPrefab));
		ENGINE_TRY_ASSIGN(const std::vector<Entity> levels, GetInstanceLevels(instance));
		ENGINE_CORE_ASSERT(!levels.empty() && levels.back() == instance, "The levels of an instance root end with it");
		if (std::find(levels.begin(), levels.end(), level) == levels.end())
		{
			return MakeError(ErrorCode::InvalidArgument, "Entity '{}' ({}) is not one of the levels of the prefab instance '{}' ({})",
				level.IsValid() ? level.GetName() : std::string("<invalid>"), level.IsValid() ? level.GetUUID() : UUID(), instance.GetName(),
				instance.GetUUID());
		}
		const Entity outermost = levels.front();
		if (level == outermost && instance == outermost)
			return ApplyOverrides(instance, levelPrefab, options);

		const Scene& scene = *instance.GetScene();
		const TypeRegistry& registry = scene.GetTypeRegistry();
		// The overrides of the outermost instance within `instance`: as recorded, or, at the outermost level, whose prefab
		// this is, what the instance has now.
		std::vector<PrefabOverride> recorded;
		if (level == outermost)
		{
			ENGINE_TRY_ASSIGN(recorded, ComputeOverrides(outermost, levelPrefab, options));
		}
		else
		{
			recorded = outermost.GetComponent<PrefabInstanceComponent>().Overrides;
		}
		std::vector<UUID> within;
		if (instance != outermost)
		{
			ENGINE_TRY_ASSIGN(within, GetOverridePath(instance));
		}
		std::vector<UUID> levelPath;
		if (level != outermost)
		{
			ENGINE_TRY_ASSIGN(levelPath, GetOverridePath(level));
		}
		std::vector<PrefabOverride> selected;
		for (const PrefabOverride& entry : recorded)
		{
			if (Utils::StartsWith(entry.Path, within))
				selected.push_back(entry);
		}

		// Re-based onto the level (the level root's placement left on the outermost root), its entities' references mapped
		// to the level prefab's IDs.
		const PrefabLinkComponent* outermostLink = outermost.TryGetComponent<PrefabLinkComponent>();
		const InstanceMembers members = Utils::CollectMembers(outermost, outermostLink != nullptr ? outermostLink->PrefabEntityID : levelPrefab.GetRootID());
		const IdMap values = Utils::MakeLevelMap(members, levelPath, levelPrefab.GetRootID(), false);
		const EntityRefRemapper remapper(registry, options.Schemas, values);
		const Utils::ComponentOwners owners(scene, members, within);
		const std::vector<PrefabOverride> rebased = Utils::RebaseOverrides(registry, selected, levelPath, levelPrefab.GetRootID(), remapper, owners);
		LoadReport report;
		return Utils::WriteOverridesToDocument(registry, levelPrefab, rebased, options, report);
	}

	std::optional<ScriptSlotField> PrefabInstantiator::ParseScriptSlotField(std::string_view /*field*/)
	{
		ENGINE_CONTRACT_STUB();
		// M20 per-slot overrides (Docs/Decisions/0031-m20-contract.md decision 19, stream M-B).
		return std::nullopt;
	}

	std::string PrefabInstantiator::MakeScriptSlotField(uint32_t /*slot*/, std::string_view /*member*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
