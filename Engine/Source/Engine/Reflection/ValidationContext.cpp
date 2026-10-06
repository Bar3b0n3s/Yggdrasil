#include "EnginePCH.h"
#include "Engine/Reflection/ValidationContext.h"

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements issue collection and pointer composition.

namespace Engine {

	ValidationContext::ValidationContext(std::string basePointer)
		: m_Pointer(std::move(basePointer))
	{
	}

	void ValidationContext::Error(std::string_view /*field*/, std::string /*message*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ValidationContext::Warning(std::string_view /*field*/, std::string /*message*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ValidationContext::AddIssue(ValidationIssue /*issue*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ValidationContext::PushKey(std::string_view /*key*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void ValidationContext::PopKey()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool ValidationContext::HasErrors() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	size_t ValidationContext::GetErrorCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::vector<ValidationIssue> ValidationContext::TakeIssues()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status ValidationContext::ToStatus(std::string_view /*subject*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValidationContext::ToStatus is an M3 contract stub");
	}

}
