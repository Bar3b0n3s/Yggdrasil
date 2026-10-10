#include "EnginePCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

namespace Engine {

	struct ScriptApiRegistry::Storage
	{
	};

	ScriptApiRegistry::ScriptApiRegistry()
	{
		ENGINE_CONTRACT_STUB();
	}

	ScriptApiRegistry::~ScriptApiRegistry()
	{
		ENGINE_CONTRACT_STUB();
	}

	ScriptModuleBuilder ScriptApiRegistry::Module(std::string_view /*name*/, std::string_view /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ScriptTypeBuilder ScriptApiRegistry::Type(std::string_view /*name*/, std::string_view /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status ScriptApiRegistry::RegisterEnum(const EnumInfo& /*enumeration*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script API registration is not implemented");
	}

	Status ScriptApiRegistry::Freeze(const TypeRegistry& /*types*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script API freezing is not implemented");
	}

	Status ScriptApiRegistry::RegisterAlias(std::string_view /*name*/, std::string_view /*definition*/,
		std::string_view /*description*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script API type aliases are not implemented");
	}

	bool ScriptApiRegistry::IsFrozen() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::span<const ScriptApiGroup> ScriptApiRegistry::GetModules() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::span<const ScriptApiGroup> ScriptApiRegistry::GetTypes() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::span<const ScriptApiEnum> ScriptApiRegistry::GetEnums() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const ScriptApiEnum* ScriptApiRegistry::FindEnum(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::span<const ScriptApiAlias> ScriptApiRegistry::GetAliases() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status ScriptApiRegistry::Bind(ScriptCall& /*call*/, ScriptApiEnvironment /*environment*/, RunModes /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script API binding is not implemented");
	}

	Result<std::string> ScriptApiRegistry::GenerateDefinitions() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script API definitions are not implemented");
	}

	Result<std::string> ScriptApiRegistry::GenerateDocumentation() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script API documentation is not implemented");
	}

	Result<ScriptApiCoverage> ScriptApiRegistry::GetCoverage(RunModes /*mode*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script API coverage is not implemented");
	}

	void ScriptApiRegistry::ResetCoverage()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ScriptApiRegistry::RecordCallback(ScriptCall& /*call*/, std::string_view /*type*/, std::string_view /*name*/,
		RunModes /*mode*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script callback coverage is not implemented");
	}

	ScriptModuleBuilder& ScriptModuleBuilder::Function(std::string_view /*name*/, ScriptNativeFunction /*function*/,
		std::string_view /*signature*/, std::string_view /*description*/, ScriptMemberOptions /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return *this;
	}

	Status ScriptApiRegistry::RecordProxyAccess(ScriptCall& /*call*/, size_t /*componentTypeIndex*/,
		std::string_view /*field*/, bool /*write*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script proxy coverage is not implemented");
	}

	ScriptModuleBuilder& ScriptModuleBuilder::Constant(std::string_view /*name*/, const Value& /*value*/,
		std::string_view /*type*/, std::string_view /*description*/, RunModes /*modes*/, ScriptApiEnvironment /*environments*/)
	{
		ENGINE_CONTRACT_STUB();
		return *this;
	}

	ScriptTypeBuilder& ScriptTypeBuilder::Method(std::string_view /*name*/, ScriptNativeFunction /*function*/,
		std::string_view /*signature*/, std::string_view /*description*/, ScriptMemberOptions /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return *this;
	}

	ScriptTypeBuilder& ScriptTypeBuilder::Property(std::string_view /*name*/, ScriptNativeFunction /*getter*/,
		ScriptNativeFunction /*setter*/, std::string_view /*type*/, std::string_view /*description*/, ScriptMemberOptions /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return *this;
	}

	ScriptTypeBuilder& ScriptTypeBuilder::Operator(std::string_view /*name*/, ScriptNativeFunction /*function*/,
		std::string_view /*signature*/, std::string_view /*description*/, ScriptMemberOptions /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return *this;
	}

	ScriptTypeBuilder& ScriptTypeBuilder::Constructor(std::string_view /*name*/, ScriptNativeFunction /*function*/,
		std::string_view /*signature*/, std::string_view /*description*/, ScriptMemberOptions /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return *this;
	}

	ScriptTypeBuilder& ScriptTypeBuilder::Callback(std::string_view /*name*/, std::string_view /*signature*/,
		std::string_view /*description*/, ScriptMemberOptions /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return *this;
	}

}
