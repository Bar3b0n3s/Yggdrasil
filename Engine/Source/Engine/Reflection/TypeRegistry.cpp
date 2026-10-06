#include "EnginePCH.h"
#include "Engine/Reflection/TypeRegistry.h"

#include "Engine/Reflection/FuzzySuggest.h"

#include <functional>
#include <map>
#include <unordered_map>

namespace Engine {

	struct TypeRegistry::Storage
	{
		std::vector<Scope<ComponentInfo>> Components;
		std::vector<Scope<StructInfo>> Structs;
		std::vector<Scope<EnumInfo>> Enums;
		std::vector<Scope<TypeInfo>> Types;

		// Lookups only: never iterated to produce output (the lists in registration order are).
		std::map<std::string, const ComponentInfo*, std::less<>> ComponentsByName;
		std::map<std::string, const StructInfo*, std::less<>> StructsByName;
		std::map<std::string, const EnumInfo*, std::less<>> EnumsByName;
		std::unordered_map<TypeKey, const ComponentInfo*> ComponentsByKey;
		std::unordered_map<TypeKey, const StructInfo*> StructsByKey;
		std::unordered_map<TypeKey, const EnumInfo*> EnumsByKey;
		std::unordered_map<TypeKey, const TypeInfo*> TypesByKey;

		[[nodiscard]] bool IsNameUsed(std::string_view name) const
		{
			return ComponentsByName.contains(name) || StructsByName.contains(name) || EnumsByName.contains(name);
		}
	};

	namespace Utils {

		template<typename Map>
		static auto FindByName(const Map& map, std::string_view name) -> typename Map::mapped_type
		{
			const auto found = map.find(name);
			return found != map.end() ? found->second : nullptr;
		}

		template<typename Map>
		static auto FindByKey(const Map& map, TypeKey key) -> typename Map::mapped_type
		{
			const auto found = map.find(key);
			return found != map.end() ? found->second : nullptr;
		}

		// Registration checks of one struct or component, run by Freeze.
		static void CheckRegisteredStruct(const StructInfo& type)
		{
			ENGINE_CORE_ASSERT(!type.GetDescription().empty(), "Type '{}' needs a description", type.GetName());
			for (const Scope<FieldInfo>& field : type.GetFields())
			{
				ENGINE_CORE_ASSERT(!field->GetDescription().empty(), "Field '{}.{}' needs a description", type.GetName(), field->GetName());
				const TypeInfo* fieldType = &field->GetType();
				while (fieldType != nullptr && IsContainerFieldType(fieldType->GetKind()))
					fieldType = fieldType->GetElement();
				ENGINE_CORE_ASSERT(fieldType != nullptr, "Field '{}.{}' is a container without an element type", type.GetName(), field->GetName());
				if (fieldType == nullptr)
					continue;
				ENGINE_CORE_ASSERT(fieldType->GetKind() != FieldType::Struct || fieldType->GetStruct() != nullptr,
					"Field '{}.{}' uses a struct type that is not registered", type.GetName(), field->GetName());
				ENGINE_CORE_ASSERT(fieldType->GetKind() != FieldType::Enum || fieldType->GetEnum() != nullptr,
					"Field '{}.{}' uses an enum type that is not registered", type.GetName(), field->GetName());
			}
			ENGINE_CORE_ASSERT(!type.HasValidators() || type.HasGenerators(),
				"Type '{}' has a Validate rule but no Generate hook (FieldBuilder::Generate), so random values cannot satisfy it", type.GetName());
		}

	}

	TypeRegistry::TypeRegistry()
		: m_Storage(CreateScope<Storage>())
	{
	}

	TypeRegistry::~TypeRegistry() = default;

	void TypeRegistry::Freeze()
	{
		if (m_IsFrozen)
			return;

		for (const Scope<ComponentInfo>& component : m_Storage->Components)
		{
			component->Resolve(*this);
			Utils::CheckRegisteredStruct(*component);
		}
		for (const Scope<StructInfo>& type : m_Storage->Structs)
			Utils::CheckRegisteredStruct(*type);
		for (const Scope<EnumInfo>& enumInfo : m_Storage->Enums)
		{
			ENGINE_CORE_ASSERT(!enumInfo->GetDescription().empty(), "Enum '{}' needs a description", enumInfo->GetName());
			ENGINE_CORE_ASSERT(!enumInfo->GetEntries().empty(), "Enum '{}' has no entries (EnumBuilder::Entry)", enumInfo->GetName());
		}
		m_IsFrozen = true;
	}

	const ComponentInfo* TypeRegistry::FindComponent(std::string_view name) const
	{
		return Utils::FindByName(m_Storage->ComponentsByName, name);
	}

	const ComponentInfo* TypeRegistry::FindComponentByKey(TypeKey key) const
	{
		return Utils::FindByKey(m_Storage->ComponentsByKey, key);
	}

	const StructInfo* TypeRegistry::FindStruct(std::string_view name) const
	{
		return Utils::FindByName(m_Storage->StructsByName, name);
	}

	const StructInfo* TypeRegistry::FindStructByKey(TypeKey key) const
	{
		return Utils::FindByKey(m_Storage->StructsByKey, key);
	}

	const EnumInfo* TypeRegistry::FindEnum(std::string_view name) const
	{
		return Utils::FindByName(m_Storage->EnumsByName, name);
	}

	const EnumInfo* TypeRegistry::FindEnumByKey(TypeKey key) const
	{
		return Utils::FindByKey(m_Storage->EnumsByKey, key);
	}

	const TypeInfo* TypeRegistry::FindType(TypeKey key) const
	{
		return Utils::FindByKey(m_Storage->TypesByKey, key);
	}

	std::vector<std::string> TypeRegistry::SuggestComponentNames(std::string_view name, size_t maxResults) const
	{
		std::vector<std::string_view> names;
		names.reserve(m_ComponentList.size());
		for (const ComponentInfo* component : m_ComponentList)
			names.push_back(component->GetName());
		return FuzzySuggest(name, names, maxResults);
	}

	ComponentInfo& TypeRegistry::AddComponent(TypeKey key, std::string_view name, std::string_view description, TypeOps ops)
	{
		ENGINE_CORE_ASSERT(!m_IsFrozen, "Component '{}' is registered after TypeRegistry::Freeze", name);
		ENGINE_CORE_ASSERT(!name.empty(), "A component needs a name");
		ENGINE_CORE_ASSERT(!description.empty(), "Component '{}' needs a description", name);
		ENGINE_CORE_ASSERT(!m_Storage->IsNameUsed(name), "The type name '{}' is already registered", name);
		ENGINE_CORE_ASSERT(FindType(key) == nullptr, "The C++ type of component '{}' is already registered", name);

		const size_t index = m_Storage->Components.size();
		m_Storage->Components.push_back(CreateScope<ComponentInfo>(std::string(name), std::string(description), *this, index));
		ComponentInfo& info = *m_Storage->Components.back();

		TypeInfo::Specification specification;
		specification.Kind = FieldType::Struct;
		specification.Name = std::string(name);
		specification.Struct = &info;
		specification.Ops = ops;
		specification.Key = key;
		info.SetType(*AddType(std::move(specification)));

		m_Storage->ComponentsByName.emplace(std::string(name), &info);
		m_Storage->ComponentsByKey.emplace(key, &info);
		m_ComponentList.push_back(&info);
		return info;
	}

	StructInfo& TypeRegistry::AddStruct(TypeKey key, std::string_view name, std::string_view description, TypeOps ops)
	{
		ENGINE_CORE_ASSERT(!m_IsFrozen, "Struct '{}' is registered after TypeRegistry::Freeze", name);
		ENGINE_CORE_ASSERT(!name.empty(), "A struct needs a name");
		ENGINE_CORE_ASSERT(!description.empty(), "Struct '{}' needs a description", name);
		ENGINE_CORE_ASSERT(!m_Storage->IsNameUsed(name), "The type name '{}' is already registered", name);
		ENGINE_CORE_ASSERT(FindType(key) == nullptr, "The C++ type of struct '{}' is already registered", name);

		m_Storage->Structs.push_back(CreateScope<StructInfo>(std::string(name), std::string(description), *this));
		StructInfo& info = *m_Storage->Structs.back();

		TypeInfo::Specification specification;
		specification.Kind = FieldType::Struct;
		specification.Name = std::string(name);
		specification.Struct = &info;
		specification.Ops = ops;
		specification.Key = key;
		info.SetType(*AddType(std::move(specification)));

		m_Storage->StructsByName.emplace(std::string(name), &info);
		m_Storage->StructsByKey.emplace(key, &info);
		m_StructList.push_back(&info);
		return info;
	}

	EnumInfo& TypeRegistry::AddEnum(TypeKey key, std::string_view name, std::string_view description, TypeOps ops)
	{
		ENGINE_CORE_ASSERT(!m_IsFrozen, "Enum '{}' is registered after TypeRegistry::Freeze", name);
		ENGINE_CORE_ASSERT(!name.empty(), "An enum needs a name");
		ENGINE_CORE_ASSERT(!description.empty(), "Enum '{}' needs a description", name);
		ENGINE_CORE_ASSERT(!m_Storage->IsNameUsed(name), "The type name '{}' is already registered", name);
		ENGINE_CORE_ASSERT(FindType(key) == nullptr, "The C++ type of enum '{}' is already registered", name);

		m_Storage->Enums.push_back(CreateScope<EnumInfo>(std::string(name), std::string(description)));
		EnumInfo& info = *m_Storage->Enums.back();

		TypeInfo::Specification specification;
		specification.Kind = FieldType::Enum;
		specification.Name = std::string(name);
		specification.Enum = &info;
		specification.Ops = ops;
		specification.Key = key;
		static_cast<void>(AddType(std::move(specification)));

		m_Storage->EnumsByName.emplace(std::string(name), &info);
		m_Storage->EnumsByKey.emplace(key, &info);
		m_EnumList.push_back(&info);
		return info;
	}

	const TypeInfo* TypeRegistry::AddType(TypeInfo::Specification specification)
	{
		ENGINE_CORE_ASSERT(!m_IsFrozen, "Type '{}' is created after TypeRegistry::Freeze", specification.Name);
		ENGINE_CORE_ASSERT(specification.Key != nullptr, "Type '{}' needs a TypeKey", specification.Name);
		ENGINE_CORE_ASSERT(FindType(specification.Key) == nullptr, "Type '{}' is already registered under its TypeKey", specification.Name);

		const TypeKey key = specification.Key;
		m_Storage->Types.push_back(CreateScope<TypeInfo>(std::move(specification)));
		const TypeInfo* type = m_Storage->Types.back().get();
		m_Storage->TypesByKey.emplace(key, type);
		return type;
	}

}
