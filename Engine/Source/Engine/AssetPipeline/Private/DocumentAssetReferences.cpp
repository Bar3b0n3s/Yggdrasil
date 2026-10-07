#include "EnginePCH.h"
#include "Engine/AssetPipeline/Private/DocumentAssetReferences.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace Engine {

	namespace {

		// Walks component JSON by its registered schema, the way PrefabInstantiator remaps entity references, and collects
		// the valid handles of AssetRef values.
		class AssetReferenceCollector
		{
		public:
			AssetReferenceCollector(const TypeRegistry& registry, std::vector<AssetHandle>& handles)
				: m_Registry(registry), m_Handles(handles)
			{
			}

			// Every registered component of the entity object `entity`.
			void CollectEntity(const Json& entity) const
			{
				if (!entity.is_object())
					return;
				const auto components = entity.find("Components");
				if (components == entity.end() || !components->is_object())
					return;
				for (auto component = components->begin(); component != components->end(); ++component)
				{
					if (const ComponentInfo* info = m_Registry.FindComponent(component.key()))
						CollectStruct(*component, *info);
				}
			}
		private:
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

			void CollectHandle(const Json& value) const
			{
				if (!value.is_string())
					return; // null: no asset
				const Result<UUID> handle = JsonReader(value).ReadUUID();
				if (handle.has_value() && handle->IsValid())
					m_Handles.push_back(*handle);
			}

			// The value `value` of `field` (or of an element of it), a field of `ownerType` whose JSON object is `ownerJson`
			// (Variant resolution reads the members declared before the field from it).
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
						const Result<const FieldInfo*> resolved = field.ResolveVariant(context);
						if (!resolved || *resolved == &field)
							return; // unresolvable without external schemas (script fields): kept verbatim by every reader
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
			std::vector<AssetHandle>& m_Handles; // the caller's output
		};

	}

	std::vector<AssetHandle> Utils::CollectDocumentAssetReferences(const Json& document, const TypeRegistry& registry)
	{
		std::vector<AssetHandle> handles;
		if (!document.is_object())
			return handles;
		const auto entities = document.find("Entities");
		if (entities == document.end() || !entities->is_array())
			return handles;

		const AssetReferenceCollector collector(registry, handles);
		for (const Json& entity : *entities)
			collector.CollectEntity(entity);
		std::ranges::sort(handles);
		const auto [first, last] = std::ranges::unique(handles);
		handles.erase(first, last);
		return handles;
	}

}
