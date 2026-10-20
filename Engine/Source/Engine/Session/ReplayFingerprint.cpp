#include "EnginePCH.h"
#include "Engine/Session/ReplayFingerprint.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace Engine {

	namespace {

		// The hash of one value of the scene's canonical document, as the minified serialization the state hash covers
		// writes it.
		Result<std::string> HashJson(const Json& value)
		{
			ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(value, JsonStyle::Minified));
			return UUID(XXH64(text)).ToString();
		}

		// Walks a component's JSON by its registered schema, as the importers' asset reference collection does
		// (AssetPipeline/Private/DocumentAssetReferences.cpp), and collects the valid handles of its AssetRef values. Variant
		// values resolve through the registry and `schemas`, so a Script component's asset fields are typed by the script's
		// field schema and its entity fields are never taken for assets.
		class AssetReferenceCollector
		{
		public:
			AssetReferenceCollector(const TypeRegistry& registry, const IFieldSchemaSource* schemas, std::vector<AssetHandle>& handles)
				: m_Registry(registry), m_Schemas(schemas), m_Handles(handles)
			{
			}

			// Every serialized stored field of the object `object` of `type`.
			void CollectStruct(const Json& object, const StructInfo& type) const
			{
				if (!object.is_object())
					return;
				for (const Scope<FieldInfo>& field : type.GetFields())
				{
					if (!field->IsStored() || !field->GetMeta().Serialized)
						continue;
					if (const auto member = object.find(field->GetName()); member != object.end())
						CollectValue(*member, field->GetType(), *field, &type, &object, {});
				}
			}
		private:
			void CollectHandle(const Json& value) const
			{
				if (!value.is_string())
					return; // null: no asset
				const Result<UUID> handle = JsonReader(value).ReadUUID();
				if (handle.has_value() && handle->IsValid())
					m_Handles.push_back(*handle);
			}

			// The value `value` of `field` (or of an element of it), a field of `ownerType` whose JSON object is `ownerJson`.
			void CollectValue(const Json& value, const TypeInfo& type, const FieldInfo& field, const StructInfo* ownerType, const Json* ownerJson,
				std::string_view key) const
			{
				switch (type.GetKind())
				{
					case FieldType::AssetRef:
						CollectHandle(value);
						return;
					case FieldType::Struct:
					{
						if (type.GetStruct() != nullptr)
							CollectStruct(value, *type.GetStruct());
						return;
					}
					case FieldType::Array:
					{
						if (!value.is_array() || type.GetElement() == nullptr)
							return;
						for (const Json& element : value)
							CollectValue(element, *type.GetElement(), field, ownerType, ownerJson, {});
						return;
					}
					case FieldType::Map:
					{
						if (!value.is_object() || type.GetElement() == nullptr)
							return;
						for (auto element = value.begin(); element != value.end(); ++element)
							CollectValue(*element, *type.GetElement(), field, ownerType, ownerJson, element.key());
						return;
					}
					case FieldType::Variant:
					{
						if (field.GetResolver() == nullptr || ownerJson == nullptr)
							return; // free-form JSON has no schema to follow
						const JsonReader owner(*ownerJson);
						ResolveContext context;
						context.Registry = &m_Registry;
						context.OwnerType = ownerType;
						context.OwnerJson = &owner;
						context.Key = key;
						context.Schemas = m_Schemas;
						const Result<const FieldInfo*> resolved = field.ResolveVariant(context);
						if (!resolved || *resolved == &field)
							return; // no schema (no script field schemas, an unknown field): names no asset
						const FieldInfo& schema = **resolved;
						CollectValue(value, schema.GetType(), schema, ownerType, ownerJson, {});
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
						return; // no asset references
				}
				ENGINE_CORE_ASSERT(false, "Unknown FieldType {}", std::to_underlying(type.GetKind()));
			}
		private:
			const TypeRegistry& m_Registry;
			const IFieldSchemaSource* m_Schemas = nullptr;
			std::vector<AssetHandle>& m_Handles; // the caller's output
		};

		// The fingerprint of one entity object of the scene's canonical document: its own members and each component, with
		// the AssetRef values of its registered components added to `handles`.
		Result<ReplayEntityFingerprint> FingerprintEntity(const Scene& scene, const Json& entity, const AssetReferenceCollector& collector)
		{
			const JsonReader reader(entity);
			ReplayEntityFingerprint print;
			ENGINE_TRY_ASSIGN(print.Entity, reader.ReadMember<UUID>("ID"));
			print.Path = scene.GetEntityPath(scene.FindEntityByID(print.Entity));
			Json own = Json::object();
			for (auto member = entity.begin(); member != entity.end(); ++member)
			{
				if (member.key() != "Components")
					own[member.key()] = member.value();
			}
			ENGINE_TRY_ASSIGN(print.Hash, HashJson(own));
			const auto components = entity.find("Components");
			if (components == entity.end() || !components->is_object())
				return print;
			const TypeRegistry& registry = scene.GetTypeRegistry();
			for (auto component = components->begin(); component != components->end(); ++component)
			{
				ReplayComponentFingerprint item;
				item.Component = component.key();
				ENGINE_TRY_ASSIGN(item.Hash, HashJson(*component));
				print.Components.push_back(std::move(item));
				if (const ComponentInfo* info = registry.FindComponent(component.key()))
					collector.CollectStruct(*component, *info);
			}
			return print;
		}

		// The asset fingerprint of `handle`, or nullopt for one the fingerprint leaves out: a handle that names no asset, and
		// an asset without a cooked artifact of its own (a procedural built-in or a dependency file, InvalidArgument; an
		// unknown handle, NotFound; every asset of a host without cooked artifacts, the default's Unsupported).
		Result<std::optional<ReplayAssetFingerprint>> FingerprintAsset(AssetManager& assets, AssetHandle handle)
		{
			const AssetType type = assets.GetAssetType(handle);
			if (type == AssetType::None)
				return std::optional<ReplayAssetFingerprint>();
			std::string path = assets.GetReferencePath(handle);
			Result<uint64_t> hash = assets.GetCookedHash(handle);
			if (!hash)
			{
				const ErrorCode code = hash.error().GetCode();
				if (code == ErrorCode::InvalidArgument || code == ErrorCode::NotFound || code == ErrorCode::Unsupported)
					return std::optional<ReplayAssetFingerprint>();
				return std::unexpected(std::move(hash).error().WithContext(std::format("while fingerprinting asset {} ('{}')", handle, path)));
			}
			return std::optional<ReplayAssetFingerprint>(ReplayAssetFingerprint{
				.Handle = handle,
				.Path = std::move(path),
				.Type = std::string(AssetTypeToString(type)),
				.Hash = UUID(*hash).ToString(),
			});
		}

		// What `recorded` and `observed`, both in registry order, hold, merged in that order: (the recorded component or
		// null, the observed one or null) per component either holds.
		std::vector<std::pair<const ReplayComponentFingerprint*, const ReplayComponentFingerprint*>> MergeComponents(
			const std::vector<ReplayComponentFingerprint>& recorded, const std::vector<ReplayComponentFingerprint>& observed)
		{
			const auto find = [](const std::vector<ReplayComponentFingerprint>& list, std::string_view name) -> const ReplayComponentFingerprint*
			{
				const auto found = std::ranges::find(list, name, &ReplayComponentFingerprint::Component);
				return found == list.end() ? nullptr : &*found;
			};
			std::vector<std::pair<const ReplayComponentFingerprint*, const ReplayComponentFingerprint*>> merged;
			size_t next = 0; // the first observed component not yet emitted
			for (const ReplayComponentFingerprint& component : recorded)
			{
				const ReplayComponentFingerprint* match = find(observed, component.Component);
				if (match != nullptr)
				{
					// The observed components before the match that the recording lacks come first: they are earlier in the order.
					const size_t position = static_cast<size_t>(match - observed.data());
					for (; next < position; ++next)
					{
						if (find(recorded, observed[next].Component) == nullptr)
							merged.emplace_back(nullptr, &observed[next]);
					}
					next = std::max(next, position + 1);
				}
				merged.emplace_back(&component, match);
			}
			for (; next < observed.size(); ++next)
			{
				if (find(recorded, observed[next].Component) == nullptr)
					merged.emplace_back(nullptr, &observed[next]);
			}
			return merged;
		}

	}

	Result<ReplayFingerprint> ComputeReplayFingerprint(const Scene& scene, const ScriptFieldSchemaSource* schemas, AssetManager* assets)
	{
		ENGINE_TRY_ASSIGN(const Json document, SceneSerializer::ToJson(scene));
		ReplayFingerprint fingerprint;
		std::vector<AssetHandle> handles;
		const AssetReferenceCollector collector(scene.GetTypeRegistry(), schemas, handles);
		if (const auto entities = document.find("Entities"); entities != document.end() && entities->is_array())
		{
			for (const Json& entity : *entities)
			{
				ENGINE_TRY_ASSIGN(ReplayEntityFingerprint print, FingerprintEntity(scene, entity, collector));
				fingerprint.Entities.push_back(std::move(print));
			}
		}
		std::ranges::sort(fingerprint.Entities, {}, [](const ReplayEntityFingerprint& entity)
		{
			return entity.Entity.GetValue();
		});
		if (assets == nullptr)
			return fingerprint;
		std::ranges::sort(handles, {}, &AssetHandle::GetValue);
		const auto duplicates = std::ranges::unique(handles);
		handles.erase(duplicates.begin(), duplicates.end());
		for (const AssetHandle handle : handles)
		{
			ENGINE_TRY_ASSIGN(std::optional<ReplayAssetFingerprint> asset, FingerprintAsset(*assets, handle));
			if (asset.has_value())
				fingerprint.Assets.push_back(std::move(*asset));
		}
		return fingerprint;
	}

	std::vector<std::string> DescribeReplayFingerprintChanges(const ReplayFingerprint& recorded, const ReplayFingerprint& observed, size_t limit)
	{
		std::vector<std::string> presence;   // 1. entities added or removed
		std::vector<std::string> changed;    // 2. entities whose own members differ
		std::vector<std::string> components; // 3. components of the entities both hold
		std::vector<std::pair<const ReplayEntityFingerprint*, const ReplayEntityFingerprint*>> shared;
		size_t r = 0;
		size_t o = 0;
		while (r < recorded.Entities.size() || o < observed.Entities.size())
		{
			const ReplayEntityFingerprint* before = r < recorded.Entities.size() ? &recorded.Entities[r] : nullptr;
			const ReplayEntityFingerprint* after = o < observed.Entities.size() ? &observed.Entities[o] : nullptr;
			if (before != nullptr && (after == nullptr || before->Entity.GetValue() < after->Entity.GetValue()))
			{
				presence.push_back(std::format("entity {} ({}) was removed", before->Path, before->Entity));
				++r;
			}
			else if (after != nullptr && (before == nullptr || after->Entity.GetValue() < before->Entity.GetValue()))
			{
				presence.push_back(std::format("entity {} ({}) was added", after->Path, after->Entity));
				++o;
			}
			else
			{
				shared.emplace_back(before, after);
				++r;
				++o;
			}
		}
		for (const auto& [before, after] : shared)
		{
			if (before->Hash != after->Hash)
				changed.push_back(std::format("entity {} ({}) changed", before->Path, before->Entity));
			for (const auto& [was, now] : MergeComponents(before->Components, after->Components))
			{
				if (was == nullptr)
					components.push_back(std::format("component {} of {} ({}) was added", now->Component, before->Path, before->Entity));
				else if (now == nullptr)
					components.push_back(std::format("component {} of {} ({}) was removed", was->Component, before->Path, before->Entity));
				else if (was->Hash != now->Hash)
					components.push_back(std::format("component {} of {} ({}) changed", was->Component, before->Path, before->Entity));
			}
		}
		std::vector<std::string> assets; // 4. assets
		r = 0;
		o = 0;
		while (r < recorded.Assets.size() || o < observed.Assets.size())
		{
			const ReplayAssetFingerprint* before = r < recorded.Assets.size() ? &recorded.Assets[r] : nullptr;
			const ReplayAssetFingerprint* after = o < observed.Assets.size() ? &observed.Assets[o] : nullptr;
			if (before != nullptr && (after == nullptr || before->Handle.GetValue() < after->Handle.GetValue()))
			{
				assets.push_back(std::format("{} {} ({}) is no longer referenced", before->Type, before->Path, before->Handle));
				++r;
			}
			else if (after != nullptr && (before == nullptr || after->Handle.GetValue() < before->Handle.GetValue()))
			{
				assets.push_back(std::format("{} {} ({}) is now referenced", after->Type, after->Path, after->Handle));
				++o;
			}
			else
			{
				if (before->Hash != after->Hash)
					assets.push_back(std::format("{} {} ({}) changed", before->Type, before->Path, before->Handle));
				++r;
				++o;
			}
		}

		std::vector<std::string> lines = std::move(presence);
		for (std::vector<std::string>* group : { &changed, &components, &assets })
			lines.insert(lines.end(), std::make_move_iterator(group->begin()), std::make_move_iterator(group->end()));
		const size_t shown = std::max<size_t>(limit, 1);
		if (lines.size() > shown)
		{
			const size_t more = lines.size() - (shown - 1);
			lines.resize(shown - 1);
			lines.push_back(std::format("... and {} more", more));
		}
		return lines;
	}

}
