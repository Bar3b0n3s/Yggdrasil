#include "EnginePCH.h"
#include "Engine/Scene/SceneSerializer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Migrations.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/Private/EntityDocument.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/StructuralValidator.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		// The members of a scene and of a prefab document and the keys of an entity, in the order they are written (§6.2,
		// §6.3, ADR 0006 decision 7).
		constexpr std::array<std::string_view, 6> SceneKeys = { "Format", "Version", "Name", "Seed", "ComponentVersions", "Entities" };
		constexpr std::array<std::string_view, 6> PrefabKeys = { "Format", "Version", "Name", "Root", "ComponentVersions", "Entities" };
		constexpr std::array<std::string_view, 6> EntityKeys = { "ID", "Name", "Parent", "Active", "Tags", "Components" };

		// A component read from a file and validated, not added to an entity yet.
		struct PendingComponent
		{
			const ComponentInfo* Info = nullptr;
			ObjectPtr Object{ nullptr, nullptr };
			std::string Pointer;
			const Json* Source = nullptr; // the JSON it was read from (repair reports); outlives the load
			bool Rejected = false;        // a relation or uniqueness problem; erased once the checks are done
		};

		// An entity read from a file and validated, not created yet. Loads read every entity first, so a failed load
		// creates nothing.
		struct PendingEntity
		{
			UUID ID;
			bool HasID = false; // "ID" was read (it may still be the invalid UUID)
			std::string Name = "Entity";
			UUID Parent;
			bool Active = true;
			std::vector<std::string> Tags;
			std::vector<PendingComponent> Components;  // registry order once read
			std::vector<UnknownComponentData> Unknown; // file order; Version filled in by the caller
			std::string Pointer;                       // the entity's pointer in the document as given
			UUID WrittenID;                            // diagnostics name the entity by its ID as written
		};

		// What HostOps::Patch copies into a component.
		struct ComponentCopy
		{
			void (*Copy)(void* destination, const void* source) = nullptr;
			const void* Source = nullptr;
		};

		// Reads entity objects into PendingEntity, recording warnings in the report and collecting errors, so one load
		// reports every invalid key and component at once. Repair mode drops or resets invalid components instead (§6,
		// ADR 0006 decision 14).
		class EntityReader
		{
		public:
			EntityReader(const TypeRegistry& registry, const LoadOptions& options, LoadReport& report)
				: m_Registry(registry), m_Options(options), m_Report(report)
			{
			}

			// Reads the entity object `entity` into `pending` (whose Pointer and WrittenID the caller may have set).
			void Read(const JsonReader& entity, PendingEntity& pending);

			// Unknown keys of an object: warnings (dropped), or errors with LoadOptions::StrictUnknowns.
			void ReportUnknownKeys(const JsonReader& object, std::span<const std::string_view> knownKeys, UUID entity);

			// A problem with a component that Repair mode fixes by dropping it, or Strict mode reports as an error. Either way
			// the component is marked Rejected; EraseRejected removes it.
			void RejectComponent(PendingEntity& pending, size_t index, std::string_view code, std::string message, std::string repair);
			static void EraseRejected(PendingEntity& pending);

			void AddError(const Error& error, std::string_view code, UUID entity);
			void AddError(std::string_view code, std::string pointer, std::string message, UUID entity);

			// Success when no error was collected; otherwise the one error, or a Validation error summarizing all of them,
			// located at the first and carrying every problem as an issue.
			[[nodiscard]] Status ToStatus() const;
		private:
			void ReadTags(const JsonReader& tags, PendingEntity& pending);
			void ReadComponent(const std::string& name, const JsonReader& component, PendingEntity& pending);
			void CheckRelations(PendingEntity& pending);
			void ReportUnknown(std::string_view code, std::string pointer, std::string message, std::vector<std::string> suggestions, UUID entity);
		private:
			const TypeRegistry& m_Registry;
			const LoadOptions& m_Options;
			LoadReport& m_Report;
			std::optional<Error> m_First;
			std::vector<ErrorIssue> m_Issues;
			size_t m_ErrorCount = 0;
		};

	}

	namespace Utils {

		// `error` with `prefix` put in front of its JSON pointer and of every issue's pointer (StructInfo::ToJson locates
		// its errors below the object, the document locates the object).
		static Error PrefixPointers(const Error& error, std::string_view prefix)
		{
			ErrorLocation location = error.GetLocation();
			location.JsonPointer = std::string(prefix) + location.JsonPointer.value_or(std::string());

			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = std::string(prefix) + issue.JsonPointer;

			Error result = Error(error.GetCode(), error.GetMessageText())
							   .WithLocation(std::move(location))
							   .WithHint(error.GetHint())
							   .WithIssues(std::move(issues));
			for (const std::string& context : error.GetContexts())
			{
				Error next = std::move(result).WithContext(context);
				result = std::move(next);
			}
			return result;
		}

		Error WithSourceFile(Error error, const std::string& path)
		{
			if (path.empty() || !error.GetLocation().File.empty())
				return error;
			ErrorLocation location;
			location.File = path;
			return std::move(error).WithLocation(std::move(location));
		}

		static Status WithSourceFile(Status status, const std::string& path)
		{
			if (status)
				return status;
			return std::unexpected(WithSourceFile(std::move(status).error(), path));
		}

		void CollectUnknownVersions(ConstEntity entity, std::vector<std::pair<std::string, uint32_t>>& versions)
		{
			const UnknownComponentsComponent* preserved = entity.TryGetComponent<UnknownComponentsComponent>();
			if (preserved == nullptr)
				return;
			for (const UnknownComponentData& data : preserved->Components)
			{
				const bool listed = std::any_of(versions.begin(), versions.end(), [&data](const std::pair<std::string, uint32_t>& entry)
				{
					return entry.first == data.Name;
				});
				if (!listed)
					versions.emplace_back(data.Name, data.Version);
			}
		}

		UUID ReadEntityUUID(const Json& entity, std::string_view key)
		{
			const std::optional<JsonReader> member = JsonReader(entity).FindMember(key);
			if (!member || member->IsNull())
				return UUID();
			const Result<UUID> value = member->ReadUUID();
			return value ? *value : UUID();
		}

		// Components written under an entity's "Components": Serializable, not entity-level, and with ECS operations.
		static bool IsWrittenUnderComponents(const ComponentInfo& info)
		{
			return info.HasFlag(ComponentFlags::Serializable) && !info.HasFlag(ComponentFlags::EntityLevel) && info.GetHostOps() != nullptr;
		}

		// The canonical JSON of `entity`; `pointer` locates it for errors. Marks the known components it writes in `used`
		// (indexed by ComponentInfo::GetIndex) and appends unknown component names with their versions to `unknown`
		// (first use only), when they are given.
		static Result<Json> WriteEntity(ConstEntity entity, std::string_view pointer, const TypeRegistry& registry, std::vector<bool>* used,
			std::vector<std::pair<std::string, uint32_t>>* unknown)
		{
			Json json = Json::object();
			json["ID"] = entity.GetUUID().ToString();
			json["Name"] = entity.GetName();
			const UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
			json["Parent"] = parent.IsValid() ? Json(parent.ToString()) : Json(nullptr);
			json["Active"] = entity.IsActiveSelf();
			Json tags = Json::array();
			for (const std::string& tag : entity.GetTags())
				tags.push_back(tag);
			json["Tags"] = std::move(tags);

			const std::string componentsPointer = JsonReader::AppendPointer(pointer, "Components");
			Json components = Json::object();
			for (const ComponentInfo* info : registry.GetComponents())
			{
				if (!IsWrittenUnderComponents(*info))
					continue;
				const void* object = info->GetHostOps()->GetConst(entity);
				if (object == nullptr)
					continue;
				Result<Json> value = info->ToJson(object);
				if (!value)
					return std::unexpected(PrefixPointers(value.error(), JsonReader::AppendPointer(componentsPointer, info->GetName())));
				components[info->GetName()] = std::move(*value);
				if (used != nullptr)
					(*used)[info->GetIndex()] = true;
			}

			if (const UnknownComponentsComponent* preserved = entity.TryGetComponent<UnknownComponentsComponent>())
			{
				for (const UnknownComponentData& data : preserved->Components)
				{
					// Only the serializer fills UnknownComponentsComponent, with names the registry does not write itself.
					ENGINE_CORE_ASSERT(!components.contains(data.Name), "Preserved component '{}' collides with a written component", data.Name);
					if (!components.contains(data.Name))
						components[data.Name] = data.Data.Get();
				}
				if (unknown != nullptr)
					CollectUnknownVersions(entity, *unknown);
			}
			json["Components"] = std::move(components);
			return json;
		}

		// Adds the component to `entity`, or overwrites it when the entity has it already (the Required components every
		// entity is created with), through the scene so the change tracker and the revision see it.
		static void AddOrOverwriteComponent(Entity entity, const PendingComponent& component)
		{
			const ComponentHostOps& ops = *component.Info->GetHostOps();
			const TypeOps& typeOps = component.Info->GetType().GetOps();
			if (!ops.Has(entity))
			{
				typeOps.Copy(ops.Add(entity), component.Object.get());
				return;
			}

			ComponentCopy copy{ typeOps.Copy, component.Object.get() };
			ops.Patch(entity, [](void* destination, void* context)
			{
				const ComponentCopy& source = *static_cast<const ComponentCopy*>(context);
				source.Copy(destination, source.Source);
			}, &copy);
		}

		// Creates the active state, tags, components and preserved unknown components of a new entity.
		static void ApplyNewEntityContents(Entity entity, PendingEntity& pending)
		{
			if (!pending.Active)
				entity.SetActive(false);
			for (const std::string& tag : pending.Tags)
				entity.AddTag(tag);
			for (const PendingComponent& component : pending.Components)
				AddOrOverwriteComponent(entity, component);
			if (!pending.Unknown.empty())
				entity.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent{ std::move(pending.Unknown) });
		}

		// True when an entity of `scene` other than `except` has the component.
		static bool IsHeldByAnotherEntity(const Scene& scene, const ComponentInfo& info, UUID except)
		{
			const ComponentHostOps& ops = *info.GetHostOps();
			for (const UUID id : scene.GetCanonicalOrder())
			{
				if (id == except)
					continue;
				const ConstEntity other = scene.FindEntityByID(id);
				if (other.IsValid() && ops.Has(other))
					return true;
			}
			return false;
		}

		// Rejects the UniquePerScene components of `pending` that another entity of `scene` already has.
		static void CheckUniqueComponentsInScene(const Scene& scene, EntityReader& reader, PendingEntity& pending, UUID except)
		{
			for (size_t index = 0; index < pending.Components.size(); ++index)
			{
				const ComponentInfo& info = *pending.Components[index].Info;
				if (!info.HasFlag(ComponentFlags::UniquePerScene) || !IsHeldByAnotherEntity(scene, info, except))
					continue;
				reader.RejectComponent(pending, index, SceneDuplicateUniqueComponentCode,
					std::format("'{}' is unique per scene and another entity of the scene already has one", info.GetName()),
					std::format("dropped the extra '{}' component", info.GetName()));
			}
			EntityReader::EraseRejected(pending);
		}

		// The version of each preserved component of `pending`: the one recorded for the same name on `entity` (when it is
		// valid), else on the first entity of `scene` in canonical order that preserves a component of that name, else 0.
		static void FillUnknownVersionsFromScene(const Scene& scene, ConstEntity entity, PendingEntity& pending)
		{
			const auto findVersion = [](ConstEntity holder, const std::string& name) -> std::optional<uint32_t>
			{
				const UnknownComponentsComponent* preserved = holder.TryGetComponent<UnknownComponentsComponent>();
				if (preserved == nullptr)
					return std::nullopt;
				for (const UnknownComponentData& data : preserved->Components)
				{
					if (data.Name == name)
						return data.Version;
				}
				return std::nullopt;
			};

			for (UnknownComponentData& data : pending.Unknown)
			{
				std::optional<uint32_t> version = entity.IsValid() ? findVersion(entity, data.Name) : std::nullopt;
				for (const UUID id : scene.GetCanonicalOrder())
				{
					if (version)
						break;
					const ConstEntity other = scene.FindEntityByID(id);
					if (other.IsValid())
						version = findVersion(other, data.Name);
				}
				data.Version = version.value_or(0);
			}
		}

		// The version of each preserved component of `pending` from `componentVersions`, a document's "ComponentVersions"
		// object; 0 for a component it does not list.
		static void FillUnknownVersionsFromTable(const Json& componentVersions, PendingEntity& pending)
		{
			const JsonReader versions(componentVersions);
			for (UnknownComponentData& data : pending.Unknown)
			{
				const std::optional<JsonReader> entry = versions.FindMember(data.Name);
				const Result<uint32_t> version = entry ? entry->ReadUInt32() : Result<uint32_t>(0u);
				data.Version = version ? *version : 0;
			}
		}

		// Unpacks every member whose instance root has no readable Prefab component, in document order (`readOrder`). Structural
		// validation unpacks a member whose root lacks the component's key (§6); a root whose Prefab component Repair then
		// drops as invalid would otherwise leave links that the next strict load (the play copy included) rejects. A Strict
		// load never gets here with such a root: the invalid component has failed it already.
		static void UnpackMembersOfLostRoots(const TypeRegistry& registry, EntityReader& reader, std::vector<PendingEntity>& pending,
			std::span<const std::pair<size_t, size_t>> readOrder)
		{
			const ComponentInfo* linkInfo = registry.FindComponent<PrefabLinkComponent>();
			const ComponentInfo* instanceInfo = registry.FindComponent<PrefabInstanceComponent>();
			if (linkInfo == nullptr || instanceInfo == nullptr)
				return;

			std::unordered_set<UUID> roots; // lookup only
			for (const PendingEntity& entity : pending)
			{
				const auto isInstance = [instanceInfo](const PendingComponent& component)
				{
					return component.Info == instanceInfo;
				};
				const bool isRoot = std::any_of(entity.Components.begin(), entity.Components.end(), isInstance);
				if (isRoot)
					roots.insert(entity.ID);
			}

			for (const auto& [inputIndex, position] : readOrder)
			{
				PendingEntity& entity = pending[position];
				for (size_t index = 0; index < entity.Components.size(); ++index)
				{
					const PendingComponent& component = entity.Components[index];
					if (component.Info != linkInfo)
						continue;
					const UUID root = static_cast<const PrefabLinkComponent*>(component.Object.get())->InstanceRoot;
					if (roots.contains(root))
						continue;
					reader.RejectComponent(entity, index, SceneInconsistentPrefabLinkCode,
						std::format("\"InstanceRoot\" {} is not a prefab instance root (it has no valid '{}' component)", root,
							instanceInfo->GetName()),
						std::format("unpacked the member: removed its '{}' component", linkInfo->GetName()));
				}
				EntityReader::EraseRejected(entity);
			}
		}

		// Where each entity of the validated document was in the migrated input: its index there and its ID as written.
		// Validation keeps every valid first-held ID; it gives the other entities fresh IDs in file order and reports each
		// such fix in `repairs` in document order, which pairs them up with the input entities that needed one.
		struct InputPosition
		{
			size_t Index = 0;
			UUID WrittenID;
		};

		static std::unordered_map<UUID, InputPosition> MapInputPositions(const Json& input, std::span<const LoadRepair> repairs)
		{
			std::unordered_map<UUID, InputPosition> positions; // lookup only
			const auto entities = input.find("Entities");
			if (entities == input.end() || !entities->is_array())
				return positions;

			std::vector<InputPosition> renamed;
			for (size_t index = 0; index < entities->size(); ++index)
			{
				UUID written;
				if (const std::optional<JsonReader> id = JsonReader((*entities)[index]).FindMember("ID"))
				{
					if (const Result<UUID> value = id->ReadUUID())
						written = *value;
				}
				if (!written.IsValid() || !positions.try_emplace(written, InputPosition{ index, written }).second)
					renamed.push_back(InputPosition{ index, written });
			}

			size_t next = 0;
			for (const LoadRepair& repair : repairs)
			{
				if ((repair.Code == SceneDuplicateIdCode || repair.Code == SceneInvalidIdCode) && next < renamed.size())
					positions.try_emplace(repair.Entity, renamed[next++]);
			}
			return positions;
		}

		// The whole load of a scene or prefab document after the report was reset (FromJson, Utils::LoadEntityDocument).
		// A prefab document has "Root" (checked by StructuralValidator) instead of "Seed", so the scene's seed stays 0.
		static Status LoadDocument(Scene& scene, const Json& document, DocumentKind kind, const LoadOptions& options, LoadReport& report)
		{
			const TypeRegistry& registry = scene.GetTypeRegistry();
			const bool isScene = kind == DocumentKind::Scene;
			const std::string_view format = isScene ? SceneSerializer::FormatName : Prefab::FormatName;
			ENGINE_TRY(JsonReader(document).ReadFormatHeader(format, Migrations::MinimumVersion, Migrations::CurrentVersion));

			Json migrated = document;
			ENGINE_TRY(Migrations::UpgradeDocument(migrated, kind, registry, report));
			const JsonReader root(migrated);

			// The document's own members.
			EntityReader reader(registry, options, report);
			reader.ReportUnknownKeys(root, isScene ? std::span<const std::string_view>(SceneKeys) : std::span<const std::string_view>(PrefabKeys),
				UUID());
			std::string name = SceneSpecification().Name;
			uint32_t seed = 0;
			if (const std::optional<JsonReader> member = root.FindMember("Name"))
			{
				if (Result<std::string> value = member->ReadString())
					name = std::move(*value);
				else
					reader.AddError(value.error(), {}, UUID());
			}
			if (const std::optional<JsonReader> member = isScene ? root.FindMember("Seed") : std::nullopt)
			{
				if (const Result<uint32_t> value = member->ReadUInt32())
					seed = *value;
				else
					reader.AddError(value.error(), {}, UUID());
			}
			std::unordered_map<std::string, uint32_t> versions; // lookup only
			if (const std::optional<JsonReader> member = root.FindMember("ComponentVersions"))
			{
				ENGINE_TRY_ASSIGN(const std::vector<std::string> names, member->GetMemberNames());
				for (const std::string& component : names)
				{
					ENGINE_TRY_ASSIGN(const JsonReader entry, member->GetMember(component));
					ENGINE_TRY_ASSIGN(const uint32_t version, entry.ReadUInt32());
					versions.emplace(component, version);
				}
			}
			ENGINE_TRY(reader.ToStatus());

			// Structure, then every entity's contents (in file order, so diagnostics are in document order) before any
			// entity is created.
			const size_t firstRepair = report.Repairs.size();
			ENGINE_TRY_ASSIGN(const Json validated, StructuralValidator::Validate(migrated, kind, registry, options, report));
			const std::unordered_map<UUID, InputPosition> positions =
				MapInputPositions(migrated, std::span<const LoadRepair>(report.Repairs).subspan(firstRepair));

			const auto entitiesMember = validated.find("Entities");
			const Json* entities = entitiesMember != validated.end() ? &*entitiesMember : nullptr;
			const size_t count = entities != nullptr ? entities->size() : 0;
			std::vector<PendingEntity> pending(count);
			std::vector<std::pair<size_t, size_t>> readOrder; // input index, canonical position
			readOrder.reserve(count);
			for (size_t position = 0; position < count; ++position)
			{
				const JsonReader entity((*entities)[position]);
				UUID id;
				if (const std::optional<JsonReader> member = entity.FindMember("ID"))
				{
					if (const Result<UUID> value = member->ReadUUID())
						id = *value;
				}
				const auto found = positions.find(id);
				const InputPosition input = found != positions.end() ? found->second : InputPosition{ position, id };
				pending[position].Pointer = JsonReader::AppendPointer("/Entities", input.Index);
				pending[position].WrittenID = input.WrittenID;
				readOrder.emplace_back(input.Index, position);
			}
			std::sort(readOrder.begin(), readOrder.end());
			for (const auto& [inputIndex, position] : readOrder)
			{
				reader.Read(JsonReader((*entities)[position], pending[position].Pointer), pending[position]);
				for (UnknownComponentData& data : pending[position].Unknown)
				{
					const auto version = versions.find(data.Name);
					data.Version = version != versions.end() ? version->second : 0;
				}
			}
			ENGINE_TRY(reader.ToStatus());
			UnpackMembersOfLostRoots(registry, reader, pending, readOrder);
			ENGINE_TRY(reader.ToStatus());

			// Creation in canonical order, which puts every parent before its children. Nothing below can fail.
			for (PendingEntity& entity : pending)
			{
				const Entity parent = entity.Parent.IsValid() ? scene.FindEntityByID(entity.Parent) : Entity();
				ENGINE_CORE_ASSERT(!entity.Parent.IsValid() || parent.IsValid(), "Structural validation orders parents first");
				const Entity created = parent.IsValid() ? scene.CreateEntityWithID(entity.ID, entity.Name, parent)
														: scene.CreateEntityWithID(entity.ID, entity.Name);
				ApplyNewEntityContents(created, entity);
			}
			scene.SetName(std::move(name));
			scene.SetSeed(seed);
			return {};
		}

	}

	namespace {

		void EntityReader::Read(const JsonReader& entity, PendingEntity& pending)
		{
			if (Status status = entity.ExpectType(JsonType::Object); !status)
			{
				AddError(status.error(), {}, pending.WrittenID);
				return;
			}

			if (const std::optional<JsonReader> id = entity.FindMember("ID"))
			{
				if (const Result<UUID> value = id->ReadUUID())
				{
					pending.ID = *value;
					pending.HasID = true;
				}
				else
				{
					AddError(value.error(), {}, pending.WrittenID);
				}
			}
			else
			{
				AddError(SceneInvalidIdCode, entity.GetPointer(), "the entity has no \"ID\"", pending.WrittenID);
			}

			ReportUnknownKeys(entity, EntityKeys, pending.WrittenID);

			if (const std::optional<JsonReader> name = entity.FindMember("Name"))
			{
				if (Result<std::string> value = name->ReadString())
					pending.Name = std::move(*value);
				else
					AddError(value.error(), {}, pending.WrittenID);
			}

			if (const std::optional<JsonReader> parent = entity.FindMember("Parent"); parent && !parent->IsNull())
			{
				if (const Result<UUID> value = parent->ReadUUID())
					pending.Parent = *value;
				else
					AddError(value.error(), {}, pending.WrittenID);
			}

			if (const std::optional<JsonReader> active = entity.FindMember("Active"))
			{
				if (const Result<bool> value = active->ReadBool())
					pending.Active = *value;
				else
					AddError(value.error(), {}, pending.WrittenID);
			}

			if (const std::optional<JsonReader> tags = entity.FindMember("Tags"))
				ReadTags(*tags, pending);

			if (const std::optional<JsonReader> components = entity.FindMember("Components"))
			{
				Result<std::vector<std::string>> names = components->GetMemberNames();
				if (!names)
				{
					AddError(names.error(), {}, pending.WrittenID);
					return;
				}
				for (const std::string& name : *names)
				{
					const Result<JsonReader> component = components->GetMember(name);
					if (component)
						ReadComponent(name, *component, pending);
					else
						AddError(component.error(), {}, pending.WrittenID);
				}
				CheckRelations(pending);
			}
		}

		void EntityReader::ReadTags(const JsonReader& tags, PendingEntity& pending)
		{
			const Result<size_t> count = tags.GetArraySize();
			if (!count)
			{
				AddError(count.error(), {}, pending.WrittenID);
				return;
			}
			for (size_t index = 0; index < *count; ++index)
			{
				const Result<JsonReader> element = tags.GetElement(index);
				if (!element)
				{
					AddError(element.error(), {}, pending.WrittenID);
					continue;
				}
				Result<std::string> tag = element->ReadString();
				if (!tag)
				{
					AddError(tag.error(), {}, pending.WrittenID);
					continue;
				}
				if (tag->empty())
				{
					AddError({}, element->GetPointer(), "a tag must not be empty", pending.WrittenID);
					continue;
				}
				if (std::find(pending.Tags.begin(), pending.Tags.end(), *tag) != pending.Tags.end())
				{
					AddError({}, element->GetPointer(), std::format("the tag '{}' is listed twice", *tag), pending.WrittenID);
					continue;
				}
				pending.Tags.push_back(std::move(*tag));
			}
		}

		void EntityReader::ReadComponent(const std::string& name, const JsonReader& component, PendingEntity& pending)
		{
			const ComponentInfo* info = m_Registry.FindComponent(name);
			if (info == nullptr || info->GetHostOps() == nullptr)
			{
				ReportUnknown(SceneUnknownComponentCode, component.GetPointer(), std::format("unknown component '{}'; it is kept as written", name),
					m_Registry.SuggestComponentNames(name), pending.WrittenID);
				pending.Unknown.push_back(UnknownComponentData{ name, 0, VariantValue(component.GetValue()) });
				return;
			}

			if (info->HasFlag(ComponentFlags::EntityLevel) || !info->HasFlag(ComponentFlags::Serializable))
			{
				// Structural validation removes these from documents; single entities (EntityFromJson, ApplyEntityJson) get the
				// same treatment here. Never added: adding a Required one a second time would assert.
				const std::string message = info->HasFlag(ComponentFlags::EntityLevel)
					? std::format("'{}' is an entity key, not an entry of \"Components\"", name)
					: std::format("'{}' is not serialized; it cannot appear in \"Components\"", name);
				if (m_Options.Mode == LoadMode::Strict)
				{
					AddError(SceneMisplacedComponentCode, component.GetPointer(), message, pending.WrittenID);
					return;
				}
				const std::string repair = std::format("dropped the misplaced '{}' component", name);
				m_Report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Warning, std::string(SceneMisplacedComponentCode),
					std::format("{} (repaired: {})", message, repair), component.GetPointer(), pending.WrittenID });
				m_Report.Repairs.push_back(LoadRepair{ std::string(SceneMisplacedComponentCode), repair, component.GetPointer(), pending.ID,
					VariantValue(component.GetValue()) });
				return;
			}

			ObjectPtr object = info->CreateDefault();
			std::vector<ValidationIssue> warnings;
			ReadContext context;
			context.Schemas = m_Options.Schemas;
			context.Strict = m_Options.StrictUnknowns;
			context.Diagnostics = &warnings;
			const Status status = info->FromJson(object.get(), component, context);
			for (ValidationIssue& warning : warnings)
			{
				m_Report.Diagnostics.push_back(
					LoadDiagnostic{ warning.Severity, std::move(warning.Code), std::move(warning.Message), std::move(warning.JsonPointer),
						pending.WrittenID });
			}

			if (status)
			{
				pending.Components.push_back(PendingComponent{ info, std::move(object), component.GetPointer(), &component.GetValue() });
				return;
			}
			if (m_Options.Mode == LoadMode::Strict)
			{
				AddError(status.error(), SceneInvalidComponentCode, pending.WrittenID);
				return;
			}

			// Repair: an invalid Required component (Transform) is reset to its defaults, any other one is dropped.
			const bool required = info->HasFlag(ComponentFlags::Required);
			const std::string repair = required ? std::format("reset the Required component '{}' to its defaults", name)
												: std::format("dropped the invalid component '{}'", name);
			m_Report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Warning, std::string(SceneInvalidComponentCode),
				std::format("{} (repaired: {})", status.error().ToString(), repair), component.GetPointer(), pending.WrittenID });
			m_Report.Repairs.push_back(
				LoadRepair{ std::string(SceneInvalidComponentCode), repair, component.GetPointer(), pending.ID, VariantValue(component.GetValue()) });
			if (required)
				pending.Components.push_back(PendingComponent{ info, info->CreateDefault(), component.GetPointer(), &component.GetValue() });
		}

		void EntityReader::CheckRelations(PendingEntity& pending)
		{
			std::sort(pending.Components.begin(), pending.Components.end(), [](const PendingComponent& left, const PendingComponent& right)
			{
				return left.Info->GetIndex() < right.Info->GetIndex();
			});

			const auto excludes = [](const ComponentInfo& component, const ComponentInfo& other)
			{
				const std::span<const ComponentInfo* const> excluded = component.GetExcludes();
				return std::find(excluded.begin(), excluded.end(), &other) != excluded.end();
			};

			// Excludes: of two conflicting components, the first in registry order stays.
			for (size_t index = 1; index < pending.Components.size(); ++index)
			{
				const ComponentInfo& info = *pending.Components[index].Info;
				for (size_t earlier = 0; earlier < index; ++earlier)
				{
					const ComponentInfo& other = *pending.Components[earlier].Info;
					if (pending.Components[earlier].Rejected || (!excludes(info, other) && !excludes(other, info)))
						continue;
					RejectComponent(pending, index, SceneInvalidComponentCode,
						std::format("'{}' cannot be combined with '{}' on one entity", info.GetName(), other.GetName()),
						std::format("dropped '{}', which cannot be combined with '{}'", info.GetName(), other.GetName()));
					break;
				}
			}

			// Requires: every required component is present, or is one that every entity has (Transform). Each pass rejects
			// at least one component or ends the loop.
			const auto isPresent = [&pending](const ComponentInfo* required)
			{
				return required->HasFlag(ComponentFlags::Required)
					|| std::any_of(pending.Components.begin(), pending.Components.end(), [required](const PendingComponent& component)
				{
					return component.Info == required && !component.Rejected;
				});
			};
			bool rejected = true;
			while (rejected)
			{
				rejected = false;
				for (size_t index = 0; index < pending.Components.size() && !rejected; ++index)
				{
					if (pending.Components[index].Rejected)
						continue;
					const ComponentInfo& info = *pending.Components[index].Info;
					const std::span<const ComponentInfo* const> required = info.GetRequires();
					const auto missing = std::find_if_not(required.begin(), required.end(), isPresent);
					if (missing == required.end())
						continue;
					RejectComponent(pending, index, SceneInvalidComponentCode, std::format("'{}' requires '{}'", info.GetName(), (*missing)->GetName()),
						std::format("dropped '{}', which requires the missing '{}'", info.GetName(), (*missing)->GetName()));
					rejected = true;
				}
			}
			EraseRejected(pending);
		}

		void EntityReader::RejectComponent(PendingEntity& pending, size_t index, std::string_view code, std::string message, std::string repair)
		{
			PendingComponent& component = pending.Components[index];
			component.Rejected = true;
			if (m_Options.Mode == LoadMode::Strict)
			{
				AddError(code, component.Pointer, std::move(message), pending.WrittenID);
				return;
			}
			m_Report.Diagnostics.push_back(
				LoadDiagnostic{ DiagnosticSeverity::Warning, std::string(code), std::format("{} (repaired: {})", message, repair), component.Pointer,
					pending.WrittenID });
			m_Report.Repairs.push_back(LoadRepair{ std::string(code), std::move(repair), component.Pointer, pending.ID,
				component.Source != nullptr ? VariantValue(*component.Source) : VariantValue() });
		}

		void EntityReader::EraseRejected(PendingEntity& pending)
		{
			std::erase_if(pending.Components, [](const PendingComponent& component)
			{
				return component.Rejected;
			});
		}

		void EntityReader::ReportUnknownKeys(const JsonReader& object, std::span<const std::string_view> knownKeys, UUID entity)
		{
			const Result<std::vector<std::string>> unknown = object.FindUnknownMembers(knownKeys);
			if (!unknown)
				return; // not an object: reported by the caller's own type check
			for (const std::string& key : *unknown)
			{
				ReportUnknown(SceneUnknownKeyCode, JsonReader::AppendPointer(object.GetPointer(), key),
					std::format("unknown key '{}'; it is dropped", key),
					FuzzySuggest(key, knownKeys), entity);
			}
		}

		void EntityReader::ReportUnknown(std::string_view code, std::string pointer, std::string message, std::vector<std::string> suggestions,
			UUID entity)
		{
			std::string hint = MakeDidYouMeanHint(suggestions);
			if (m_Options.StrictUnknowns)
			{
				m_Report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Error, std::string(code),
					hint.empty() ? message : std::format("{} ({})", message, hint), pointer, entity });
				if (!m_First)
				{
					ErrorLocation location;
					location.JsonPointer = pointer;
					location.Entity = entity;
					m_First = Error(ErrorCode::Validation, message).WithLocation(std::move(location)).WithHint(hint);
				}
				m_Issues.push_back(ErrorIssue{ std::move(pointer), std::move(message), std::move(hint), std::move(suggestions) });
				++m_ErrorCount;
				return;
			}
			m_Report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Warning, std::string(code),
				hint.empty() ? std::move(message) : std::format("{} ({})", message, hint), std::move(pointer), entity });
		}

		void EntityReader::AddError(const Error& error, std::string_view code, UUID entity)
		{
			const std::string pointer = error.GetLocation().JsonPointer.value_or(std::string());
			if (error.GetIssues().empty())
			{
				m_Report.Diagnostics.push_back(
					LoadDiagnostic{ DiagnosticSeverity::Error, std::string(code), error.GetMessageText(), pointer, entity });
				m_Issues.push_back(ErrorIssue{ pointer, error.GetMessageText(), error.GetHint(), {} });
			}
			for (const ErrorIssue& issue : error.GetIssues())
			{
				m_Report.Diagnostics.push_back(
					LoadDiagnostic{ DiagnosticSeverity::Error, std::string(code), issue.Message, issue.JsonPointer, entity });
				m_Issues.push_back(issue);
			}
			if (!m_First)
				m_First = error;
			++m_ErrorCount;
		}

		void EntityReader::AddError(std::string_view code, std::string pointer, std::string message, UUID entity)
		{
			std::string text = code.empty() ? message : std::format("{}: {}", code, message);
			m_Report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Error, std::string(code), std::move(message), pointer, entity });
			if (!m_First)
			{
				ErrorLocation location;
				location.JsonPointer = pointer;
				location.Entity = entity;
				m_First = Error(ErrorCode::Validation, text).WithLocation(std::move(location));
			}
			m_Issues.push_back(ErrorIssue{ std::move(pointer), std::move(text), {}, {} });
			++m_ErrorCount;
		}

		Status EntityReader::ToStatus() const
		{
			if (!m_First)
				return {};
			if (m_ErrorCount == 1)
				return std::unexpected(*m_First);

			ErrorLocation location;
			location.JsonPointer = m_First->GetLocation().JsonPointer;
			location.Entity = m_First->GetLocation().Entity;
			return std::unexpected(Error(ErrorCode::Validation, std::format("{} problems; the first is: {}", m_ErrorCount, m_First->GetMessageText()))
					.WithLocation(std::move(location))
					.WithHint(m_First->GetHint())
					.WithIssues(m_Issues));
		}

	}

	namespace Utils {

		Status LoadEntityDocument(Scene& scene, const Json& document, DocumentKind kind, const LoadOptions& options, LoadReport& report)
		{
			ENGINE_CORE_ASSERT(scene.GetEntityCount() == 0, "Scene and prefab documents load into an empty scene");
			ENGINE_CORE_ASSERT(options.Mode == LoadMode::Strict || options.RepairIdGenerator != nullptr,
				"Repair loading needs LoadOptions::RepairIdGenerator");
			report = LoadReport{};
			return WithSourceFile(LoadDocument(scene, document, kind, options, report), options.SourcePath);
		}

		Result<Entity> EntityFromJson(Scene& scene, const JsonReader& entity, std::optional<uint32_t> siblingIndex, const Json* componentVersions,
			const LoadOptions& options, LoadReport& report)
		{
			EntityReader reader(scene.GetTypeRegistry(), options, report);
			PendingEntity pending;
			pending.Pointer = entity.GetPointer();
			if (const std::optional<JsonReader> id = entity.FindMember("ID"))
			{
				if (const Result<UUID> value = id->ReadUUID())
					pending.WrittenID = *value;
			}
			reader.Read(entity, pending);

			const Scene& constScene = scene;
			if (pending.HasID)
			{
				const std::string idPointer = JsonReader::AppendPointer(entity.GetPointer(), "ID");
				if (!pending.ID.IsValid())
				{
					reader.AddError(SceneInvalidIdCode, idPointer, "the entity needs a valid ID", pending.WrittenID);
				}
				else if (constScene.FindEntityByID(pending.ID).IsValid())
				{
					reader.AddError(SceneDuplicateIdCode, idPointer, std::format("the ID {} is already used in the scene", pending.ID),
						pending.WrittenID);
				}
			}
			Entity parent;
			if (pending.Parent.IsValid())
			{
				parent = scene.FindEntityByID(pending.Parent);
				if (!parent.IsValid())
				{
					reader.AddError({}, JsonReader::AppendPointer(entity.GetPointer(), "Parent"),
						std::format("the parent {} is not an entity of the scene", pending.Parent), pending.WrittenID);
				}
			}
			CheckUniqueComponentsInScene(scene, reader, pending, UUID());
			if (componentVersions != nullptr)
				FillUnknownVersionsFromTable(*componentVersions, pending);
			else
				FillUnknownVersionsFromScene(scene, ConstEntity(), pending);
			ENGINE_TRY(reader.ToStatus());

			const Entity created =
				parent.IsValid() ? scene.CreateEntityWithID(pending.ID, pending.Name, parent) : scene.CreateEntityWithID(pending.ID, pending.Name);
			if (siblingIndex)
			{
				if (Status moved = scene.SetParent(created, parent, siblingIndex, false); !moved)
				{
					scene.DestroyEntity(created);
					return std::unexpected(std::move(moved).error());
				}
			}
			ApplyNewEntityContents(created, pending);
			return created;
		}

		Status ApplyEntityJson(Entity entity, const JsonReader& json, const Json* componentVersions, const LoadOptions& options, LoadReport& report)
		{
			ENGINE_CORE_ASSERT(entity.IsValid(), "SceneSerializer::ApplyEntityJson needs a valid entity");
			Scene& scene = *entity.GetScene();
			const TypeRegistry& registry = scene.GetTypeRegistry();
			EntityReader reader(registry, options, report);
			PendingEntity pending;
			pending.Pointer = json.GetPointer();
			pending.WrittenID = entity.GetUUID();
			reader.Read(json, pending);
			CheckUniqueComponentsInScene(scene, reader, pending, entity.GetUUID());
			if (componentVersions != nullptr)
				FillUnknownVersionsFromTable(*componentVersions, pending);
			else
				FillUnknownVersionsFromScene(scene, entity, pending);
			ENGINE_TRY(reader.ToStatus());

			ENGINE_CORE_ASSERT(pending.ID == entity.GetUUID(), "ApplyEntityJson: the JSON describes entity {}, not {}", pending.ID, entity.GetUUID());
			ENGINE_CORE_ASSERT(pending.Parent == entity.GetComponent<RelationshipComponent>().Parent,
				"ApplyEntityJson: the parent changed; hierarchy moves go through Scene::SetParent");

			if (entity.GetName() != pending.Name)
				entity.SetName(pending.Name);
			if (entity.IsActiveSelf() != pending.Active)
				entity.SetActive(pending.Active);
			const std::span<const std::string> tags = entity.GetTags();
			if (!std::equal(tags.begin(), tags.end(), pending.Tags.begin(), pending.Tags.end()))
			{
				const std::vector<std::string> previous(tags.begin(), tags.end());
				for (const std::string& tag : previous)
					entity.RemoveTag(tag);
				for (const std::string& tag : pending.Tags)
					entity.AddTag(tag);
			}

			// Removals first, so that a component excluded by an incoming one is gone before that one is added.
			const auto findPending = [&pending](const ComponentInfo* info) -> const PendingComponent*
			{
				const auto found = std::find_if(pending.Components.begin(), pending.Components.end(), [info](const PendingComponent& component)
				{
					return component.Info == info;
				});
				return found != pending.Components.end() ? &*found : nullptr;
			};
			for (const ComponentInfo* info : registry.GetComponents())
			{
				if (!IsWrittenUnderComponents(*info) || info->HasFlag(ComponentFlags::Required) || findPending(info) != nullptr)
					continue;
				if (info->GetHostOps()->Has(entity))
					info->GetHostOps()->Remove(entity);
			}
			for (const PendingComponent& component : pending.Components)
			{
				if (const void* current = component.Info->GetHostOps()->GetConst(entity))
				{
					const Result<Json> before = component.Info->ToJson(current);
					const Result<Json> after = component.Info->ToJson(component.Object.get());
					if (before && after && *before == *after)
						continue; // unchanged: no patch, no revision
				}
				AddOrOverwriteComponent(entity, component);
			}

			if (pending.Unknown.empty())
			{
				if (entity.HasComponent<UnknownComponentsComponent>())
					entity.RemoveComponent<UnknownComponentsComponent>();
			}
			else if (entity.HasComponent<UnknownComponentsComponent>())
			{
				entity.Patch<UnknownComponentsComponent>([&pending](UnknownComponentsComponent& preserved)
				{
					preserved.Components = std::move(pending.Unknown);
				});
			}
			else
			{
				entity.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent{ std::move(pending.Unknown) });
			}
			return {};
		}

	}

	Result<Json> SceneSerializer::ToJson(const Scene& scene)
	{
		const TypeRegistry& registry = scene.GetTypeRegistry();
		std::vector<bool> used(registry.GetComponents().size(), false);
		std::vector<std::pair<std::string, uint32_t>> unknown;
		Json entities = Json::array();
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid())
				continue; // marked for destruction in a runtime scene
			const std::string pointer = JsonReader::AppendPointer("/Entities", entities.size());
			ENGINE_TRY_ASSIGN(Json json, Utils::WriteEntity(entity, pointer, registry, &used, &unknown));
			entities.push_back(std::move(json));
		}

		// Known components in registry order, then preserved ones as read (ADR 0006 decision 14). A preserved component
		// whose file listed no version is not listed either, so its file re-saves unchanged.
		Json versions = Json::object();
		for (const ComponentInfo* info : registry.GetComponents())
		{
			if (used[info->GetIndex()])
				versions[info->GetName()] = info->GetVersion();
		}
		for (const auto& [name, version] : unknown)
		{
			if (version != 0 && !versions.contains(name))
				versions[name] = version;
		}

		Json document = Json::object();
		document["Format"] = std::string(FormatName);
		document["Version"] = Migrations::CurrentVersion;
		document["Name"] = scene.GetName();
		document["Seed"] = scene.GetSeed();
		document["ComponentVersions"] = std::move(versions);
		document["Entities"] = std::move(entities);
		return document;
	}

	Result<std::string> SceneSerializer::SaveToString(const Scene& scene, JsonStyle style)
	{
		ENGINE_TRY_ASSIGN(const Json document, ToJson(scene));
		return JsonWriter::Write(document, style);
	}

	Status SceneSerializer::FromJson(Scene& scene, const Json& document, const LoadOptions& options, LoadReport& report)
	{
		return Utils::LoadEntityDocument(scene, document, DocumentKind::Scene, options, report);
	}

	Status SceneSerializer::LoadFromString(Scene& scene, std::string_view text, const LoadOptions& options, LoadReport& report)
	{
		report = LoadReport{};
		Result<Json> document = JsonReader::Parse(text);
		if (!document)
			return std::unexpected(Utils::WithSourceFile(std::move(document).error(), options.SourcePath));
		return FromJson(scene, *document, options, report);
	}

	Status SceneSerializer::SaveToFile(const Scene& scene, VirtualFileSystem& vfs, const VfsPath& path)
	{
		ENGINE_TRY_ASSIGN(const std::string text, SaveToString(scene, JsonStyle::Pretty));
		return WithContext(vfs.WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))),
			std::format("while saving scene '{}'", path.ToString()));
	}

	Status SceneSerializer::LoadFromFile(Scene& scene, const VirtualFileSystem& vfs, const VfsPath& path, const LoadOptions& options,
		LoadReport& report)
	{
		report = LoadReport{};
		LoadOptions fileOptions = options;
		if (fileOptions.SourcePath.empty())
			fileOptions.SourcePath = path.ToString();
		Result<std::string> text = vfs.ReadText(path);
		if (!text)
			return std::unexpected(Utils::WithSourceFile(std::move(text).error(), fileOptions.SourcePath));
		return LoadFromString(scene, *text, fileOptions, report);
	}

	Result<Json> SceneSerializer::EntityToJson(ConstEntity entity)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "SceneSerializer::EntityToJson needs a valid entity");
		return Utils::WriteEntity(entity, {}, entity.GetScene()->GetTypeRegistry(), nullptr, nullptr);
	}

	Result<Entity> SceneSerializer::EntityFromJson(Scene& scene, const JsonReader& entity, std::optional<uint32_t> siblingIndex,
		const LoadOptions& options, LoadReport& report)
	{
		return Utils::EntityFromJson(scene, entity, siblingIndex, nullptr, options, report);
	}

	Status SceneSerializer::ApplyEntityJson(Entity entity, const JsonReader& json, const LoadOptions& options, LoadReport& report)
	{
		return Utils::ApplyEntityJson(entity, json, nullptr, options, report);
	}

}
