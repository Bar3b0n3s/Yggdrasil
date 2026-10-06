#include "EnginePCH.h"
#include "Engine/Reflection/TypeRegistry.h"

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements the registry: name and key indexes, TypeInfo
// ownership, Freeze checks and lookups. The stubs below only keep the objects they must return a reference to.

namespace Engine {

	struct TypeRegistry::Storage
	{
		std::vector<Scope<ComponentInfo>> Components;
		std::vector<Scope<StructInfo>> Structs;
		std::vector<Scope<EnumInfo>> Enums;
	};

	TypeRegistry::TypeRegistry()
		: m_Storage(CreateScope<Storage>())
	{
	}

	TypeRegistry::~TypeRegistry() = default;

	void TypeRegistry::Freeze()
	{
		ENGINE_CONTRACT_STUB();
	}

	const ComponentInfo* TypeRegistry::FindComponent(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const ComponentInfo* TypeRegistry::FindComponentByKey(TypeKey /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const StructInfo* TypeRegistry::FindStruct(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const StructInfo* TypeRegistry::FindStructByKey(TypeKey /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const EnumInfo* TypeRegistry::FindEnum(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const EnumInfo* TypeRegistry::FindEnumByKey(TypeKey /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const TypeInfo* TypeRegistry::FindType(TypeKey /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<std::string> TypeRegistry::SuggestComponentNames(std::string_view /*name*/, size_t /*maxResults*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ComponentInfo& TypeRegistry::AddComponent(TypeKey /*key*/, std::string_view name, std::string_view description, TypeOps /*ops*/)
	{
		ENGINE_CONTRACT_STUB();
		const size_t index = m_Storage->Components.size();
		m_Storage->Components.push_back(CreateScope<ComponentInfo>(std::string(name), std::string(description), *this, index));
		return *m_Storage->Components.back();
	}

	StructInfo& TypeRegistry::AddStruct(TypeKey /*key*/, std::string_view name, std::string_view description, TypeOps /*ops*/)
	{
		ENGINE_CONTRACT_STUB();
		m_Storage->Structs.push_back(CreateScope<StructInfo>(std::string(name), std::string(description), *this));
		return *m_Storage->Structs.back();
	}

	EnumInfo& TypeRegistry::AddEnum(TypeKey /*key*/, std::string_view name, std::string_view description, TypeOps /*ops*/)
	{
		ENGINE_CONTRACT_STUB();
		m_Storage->Enums.push_back(CreateScope<EnumInfo>(std::string(name), std::string(description)));
		return *m_Storage->Enums.back();
	}

	const TypeInfo* TypeRegistry::AddType(TypeInfo::Specification /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
