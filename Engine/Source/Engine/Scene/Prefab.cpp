#include "EnginePCH.h"
#include "Engine/Scene/Prefab.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Migrations.h"
#include "Engine/Scene/Private/EntityDocument.h"
#include "Engine/Scene/Private/PrefabNesting.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// The subtree of `root` in canonical order: depth-first, children in sibling order, without recursion (hierarchies
		// may be deep).
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
					ENGINE_CORE_ASSERT(handle.IsValid(), "Child {} of {} is not an entity of the scene", *child, entity.GetUUID());
					if (handle.IsValid())
						stack.push_back(handle);
				}
			}
			return entities;
		}

		Json MakePrefabDocument(const TypeRegistry& registry, std::string_view name, UUID root, Json entities,
			std::span<const std::pair<std::string, uint32_t>> unknownVersions)
		{
			const auto appears = [&entities](const std::string& component)
			{
				return std::any_of(entities.begin(), entities.end(), [&component](const Json& entity)
				{
					const auto components = entity.find("Components");
					return components != entity.end() && components->is_object() && components->contains(component);
				});
			};

			// Known components in registry order, then unknown ones as recorded (ADR 0006 decision 14).
			Json versions = Json::object();
			for (const ComponentInfo* info : registry.GetComponents())
			{
				if (appears(info->GetName()))
					versions[info->GetName()] = info->GetVersion();
			}
			for (const auto& [component, version] : unknownVersions)
			{
				if (version != 0 && !versions.contains(component) && registry.FindComponent(component) == nullptr && appears(component))
					versions[component] = version;
			}

			Json document = Json::object();
			document["Format"] = std::string(Prefab::FormatName);
			document["Version"] = Migrations::CurrentVersion;
			document["Name"] = std::string(name);
			document["Root"] = root.ToString();
			document["ComponentVersions"] = std::move(versions);
			document["Entities"] = std::move(entities);
			return document;
		}

	}

	Prefab::Prefab(std::string name, UUID root, Ref<const Json> document)
		: m_Name(std::move(name)), m_Root(root), m_Document(std::move(document))
	{
	}

	Result<Prefab> Prefab::FromJson(const Json& document, const TypeRegistry& registry, const LoadOptions& options, LoadReport& report)
	{
		// The document is loaded into a scratch scene by the scene loader, so a prefab is validated exactly as a scene is,
		// then written back canonically. The scratch generator never draws: every entity of the document has its ID, and
		// repairs draw from options.RepairIdGenerator.
		UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
		SceneSpecification specification;
		specification.Name = "Prefab";
		specification.Registry = &registry;
		specification.IdGenerator = &scratchIds;
		const Scope<Scene> scratch = Scene::Create(specification);
		ENGINE_TRY(Utils::LoadEntityDocument(*scratch, document, DocumentKind::Prefab, options, report));

		// Structural pre-validation (PREFAB_INVALID_ROOT) leaves exactly one root, the entity "Root" names.
		ENGINE_CORE_ASSERT(scratch->GetRootEntities().size() == 1, "A validated prefab has exactly one root");
		const Scene& loaded = *scratch;
		const ConstEntity root = loaded.FindEntityByID(scratch->GetRootEntities().front());
		return CreateFromEntity(root, scratch->GetName());
	}

	Result<Prefab> Prefab::LoadFromString(std::string_view text, const TypeRegistry& registry, const LoadOptions& options, LoadReport& report)
	{
		report = LoadReport{};
		Result<Json> document = JsonReader::Parse(text);
		if (!document)
			return std::unexpected(Utils::WithSourceFile(std::move(document).error(), options.SourcePath));
		return FromJson(*document, registry, options, report);
	}

	Result<Prefab> Prefab::LoadFromFile(const VirtualFileSystem& vfs, const VfsPath& path, const TypeRegistry& registry,
		const LoadOptions& options, LoadReport& report)
	{
		report = LoadReport{};
		LoadOptions fileOptions = options;
		if (fileOptions.SourcePath.empty())
			fileOptions.SourcePath = path.ToString();
		Result<std::string> text = vfs.ReadText(path);
		if (!text)
			return std::unexpected(Utils::WithSourceFile(std::move(text).error(), fileOptions.SourcePath));
		return LoadFromString(*text, registry, fileOptions, report);
	}

	Result<Prefab> Prefab::CreateFromEntity(ConstEntity root, std::string name)
	{
		ENGINE_CORE_ASSERT(root.IsValid(), "Prefab::CreateFromEntity needs a valid entity");
		const TypeRegistry& registry = root.GetScene()->GetTypeRegistry();
		const ComponentInfo* instanceInfo = registry.FindComponent<PrefabInstanceComponent>();
		const ComponentInfo* linkInfo = registry.FindComponent<PrefabLinkComponent>();
		ENGINE_CORE_ASSERT(instanceInfo != nullptr && linkInfo != nullptr, "The prefab components are built in");
		if (instanceInfo == nullptr || linkInfo == nullptr)
			return MakeError(ErrorCode::InvalidState, "the prefab components are not registered");

		// Live nested prefabs (Docs/Decisions/0031-m20-contract.md decision 8): every instance whose root is strictly below
		// `root` stays live; the instance of `root` itself and every instance whose root is outside the subtree are unpacked
		// in the copy, as prefab.unpack leaves them.
		const std::vector<ConstEntity> subtree = Utils::CollectSubtree(root);
		std::unordered_set<UUID> live; // lookup only: the instance roots that stay live
		for (const ConstEntity entity : subtree)
		{
			if (entity != root && entity.HasComponent<PrefabInstanceComponent>())
				live.insert(entity.GetUUID());
		}

		Json entities = Json::array();
		std::vector<std::pair<std::string, uint32_t>> unknownVersions;
		for (const ConstEntity entity : subtree)
		{
			ENGINE_TRY_ASSIGN(Json json, SceneSerializer::EntityToJson(entity));
			if (entity == root)
				json["Parent"] = nullptr;
			Json& components = json["Components"];
			const PrefabLinkComponent* link = entity.TryGetComponent<PrefabLinkComponent>();
			if (live.contains(entity.GetUUID()))
			{
				// A live instance nested in one that is unpacked becomes outermost in the document and takes over the
				// overrides recorded for it; nested in a live one, it stays nested.
				if (Utils::IsNestedInstanceRoot(entity))
				{
					ENGINE_TRY_ASSIGN(const std::vector<ConstEntity> levels, Utils::FindInstanceLevels(entity));
					ENGINE_CORE_ASSERT(levels.size() > 1, "A nested instance root has an enclosing instance");
					if (levels.size() > 1 && !live.contains(levels[levels.size() - 2].GetUUID()))
					{
						PrefabInstanceComponent instance = entity.GetComponent<PrefabInstanceComponent>();
						ENGINE_TRY_ASSIGN(instance.Overrides, Utils::TakeNestedOverrides(entity));
						PrefabLinkComponent outermost = *link;
						outermost.OuterPrefabEntityID = UUID();
						ENGINE_TRY_ASSIGN(components[instanceInfo->GetName()], instanceInfo->ToJson(&instance));
						ENGINE_TRY_ASSIGN(components[linkInfo->GetName()], linkInfo->ToJson(&outermost));
					}
				}
			}
			else if (link == nullptr || link->InstanceRoot == entity.GetUUID() || !live.contains(link->InstanceRoot))
			{
				// An entity of an unpacked instance: its entities and data stay, its instance and link components go.
				components.erase(instanceInfo->GetName());
				components.erase(linkInfo->GetName());
			}
			entities.push_back(std::move(json));
			Utils::CollectUnknownVersions(entity, unknownVersions);
		}

		const UUID rootID = root.GetUUID();
		Json document = Utils::MakePrefabDocument(registry, name, rootID, std::move(entities), unknownVersions);
		return Prefab(std::move(name), rootID, CreateRef<const Json>(std::move(document)));
	}

	Result<Json> Prefab::ToJson() const
	{
		if (m_Document == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "an empty prefab has no document");
		return *m_Document;
	}

	Result<std::string> Prefab::SaveToString(JsonStyle style) const
	{
		ENGINE_TRY_ASSIGN(const Json document, ToJson());
		return JsonWriter::Write(document, style);
	}

	Status Prefab::SaveToFile(VirtualFileSystem& vfs, const VfsPath& path) const
	{
		ENGINE_TRY_ASSIGN(const std::string text, SaveToString(JsonStyle::Pretty));
		return WithContext(vfs.WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))),
			std::format("while saving prefab '{}'", path.ToString()));
	}

	const Json& Prefab::GetEntities() const
	{
		static const Json EmptyArray = Json::array(); // immutable after its thread-safe initialization
		if (m_Document == nullptr)
			return EmptyArray;
		const auto entities = m_Document->find("Entities");
		ENGINE_CORE_ASSERT(entities != m_Document->end(), "A prefab document always has \"Entities\"");
		return entities != m_Document->end() ? *entities : EmptyArray;
	}

	const Json* Prefab::FindEntity(UUID id) const
	{
		if (!id.IsValid())
			return nullptr;
		const Json& entities = GetEntities();
		const auto found = std::find_if(entities.begin(), entities.end(), [id](const Json& entity)
		{
			return Utils::ReadEntityUUID(entity, "ID") == id;
		});
		return found != entities.end() ? &*found : nullptr;
	}

	std::vector<UUID> Prefab::GetEntityIDs() const
	{
		const Json& entities = GetEntities();
		std::vector<UUID> ids;
		ids.reserve(entities.size());
		for (const Json& entity : entities)
			ids.push_back(Utils::ReadEntityUUID(entity, "ID"));
		return ids;
	}

}
