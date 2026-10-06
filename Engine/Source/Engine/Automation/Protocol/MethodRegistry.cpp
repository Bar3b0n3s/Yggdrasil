#include "EnginePCH.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"

#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements the method registry. The constructor keeps the type
// registry, and the storage exists, so hosts can be built while registration, lookup and invocation are stubs.

namespace Engine {

	struct MethodRegistry::Storage
	{
	};

	Result<const FieldInfo*> ResolveComponentValue(const ResolveContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ResolveComponentValue is an M4 contract stub");
	}

	MethodRegistry::MethodRegistry(const TypeRegistry& types)
		: m_Types(&types), m_Storage(CreateScope<Storage>())
	{
	}

	MethodRegistry::~MethodRegistry() = default;

	void MethodRegistry::Freeze()
	{
		ENGINE_CONTRACT_STUB();
		m_IsFrozen = true;
	}

	const MethodDescriptor* MethodRegistry::Find(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<std::string> MethodRegistry::SuggestMethodNames(std::string_view /*name*/, size_t /*maxResults*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<std::string> MethodRegistry::GetDomains() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<PreparedParams> MethodRegistry::PrepareParams(const MethodDescriptor& /*method*/, const Json& /*params*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "MethodRegistry::PrepareParams is an M4 contract stub");
	}

	MethodResult MethodRegistry::Invoke(MethodContext& /*context*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Error(ErrorCode::Unsupported, "MethodRegistry::Invoke is an M4 contract stub");
	}

	Result<Json> MethodRegistry::InvokeNested(MethodContext& /*parent*/, std::string_view /*method*/, const Json& /*params*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "MethodRegistry::InvokeNested is an M4 contract stub");
	}

	Json MethodRegistry::GetParamsSchema(const MethodDescriptor& /*method*/, SchemaStyle /*style*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MethodRegistry::GetResultSchema(const MethodDescriptor& /*method*/, SchemaStyle /*style*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MethodRegistry::Describe(const MethodDescriptor& /*method*/) const
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MethodRegistry::BuildMethodCatalog() const
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	Json MethodRegistry::BuildToolCatalog() const
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

	void MethodRegistry::AddEntry(MethodSpecification /*specification*/, TypeKey /*host*/, TypeKey /*params*/, TypeKey /*result*/,
		bool /*pending*/, Invoker /*invoker*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	std::string MethodNameToToolName(std::string_view /*method*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
