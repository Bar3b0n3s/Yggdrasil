#include "EnginePCH.h"
#include "Engine/Reflection/ComponentInfo.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements component configuration, dependency resolution and
// the migration chain. The constructor is complete.

namespace Engine {

	ComponentInfo::ComponentInfo(std::string name, std::string description, const TypeRegistry& registry, size_t index)
		: StructInfo(std::move(name), std::move(description), registry), m_Index(index)
	{
	}

	Status ComponentInfo::Migrate(uint32_t /*fromVersion*/, Json& /*component*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentInfo::Migrate is an M3 contract stub");
	}

	void ComponentInfo::SetCategory(std::string /*category*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ComponentInfo::SetVersion(uint32_t /*version*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ComponentInfo::SetFlags(ComponentFlags /*flags*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ComponentInfo::AddRequires(TypeKey /*component*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ComponentInfo::AddExcludes(TypeKey /*component*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ComponentInfo::AddMigration(uint32_t /*fromVersion*/, ComponentMigration /*migration*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ComponentInfo::SetHostOps(const ComponentHostOps* /*hostOps*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ComponentInfo::Resolve(const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
