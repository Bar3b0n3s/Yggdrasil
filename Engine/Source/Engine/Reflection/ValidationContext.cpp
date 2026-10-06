#include "EnginePCH.h"
#include "Engine/Reflection/ValidationContext.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"

#include <algorithm>

namespace Engine {

	ValidationContext::ValidationContext(std::string basePointer)
		: m_Pointer(std::move(basePointer))
	{
	}

	void ValidationContext::Error(std::string_view field, std::string message)
	{
		ValidationIssue issue;
		issue.Severity = DiagnosticSeverity::Error;
		issue.JsonPointer = field.empty() ? m_Pointer : JsonReader::AppendPointer(m_Pointer, field);
		issue.Message = std::move(message);
		m_Issues.push_back(std::move(issue));
	}

	void ValidationContext::Warning(std::string_view field, std::string message)
	{
		ValidationIssue issue;
		issue.Severity = DiagnosticSeverity::Warning;
		issue.JsonPointer = field.empty() ? m_Pointer : JsonReader::AppendPointer(m_Pointer, field);
		issue.Message = std::move(message);
		m_Issues.push_back(std::move(issue));
	}

	void ValidationContext::AddIssue(ValidationIssue issue)
	{
		m_Issues.push_back(std::move(issue));
	}

	void ValidationContext::PushKey(std::string_view key)
	{
		m_KeyLengths.push_back(m_Pointer.size());
		m_Pointer = JsonReader::AppendPointer(m_Pointer, key);
	}

	void ValidationContext::PopKey()
	{
		ENGINE_CORE_ASSERT(!m_KeyLengths.empty(), "ValidationContext::PopKey without a matching PushKey");
		if (m_KeyLengths.empty())
			return;
		m_Pointer.resize(m_KeyLengths.back());
		m_KeyLengths.pop_back();
	}

	bool ValidationContext::HasErrors() const
	{
		return GetErrorCount() > 0;
	}

	size_t ValidationContext::GetErrorCount() const
	{
		return static_cast<size_t>(std::count_if(m_Issues.begin(), m_Issues.end(), [](const ValidationIssue& issue)
		{
			return issue.Severity == DiagnosticSeverity::Error;
		}));
	}

	std::vector<ValidationIssue> ValidationContext::TakeIssues()
	{
		std::vector<ValidationIssue> issues = std::move(m_Issues);
		m_Issues.clear();
		return issues;
	}

	Status ValidationContext::ToStatus(std::string_view subject) const
	{
		std::vector<ErrorIssue> errors;
		std::string singleMessage;
		for (const ValidationIssue& issue : m_Issues)
		{
			if (issue.Severity != DiagnosticSeverity::Error)
				continue;
			singleMessage = issue.Message;
			errors.push_back(ErrorIssue{ issue.JsonPointer, issue.Message, issue.Hint, issue.Suggestions });
		}
		if (errors.empty())
			return {};

		std::string message = errors.size() == 1 ? std::move(singleMessage) : std::format("{} invalid fields in {}", errors.size(), subject);
		const std::string basePointer = m_KeyLengths.empty() ? m_Pointer : m_Pointer.substr(0, m_KeyLengths.front());
		ErrorLocation location;
		location.JsonPointer = basePointer;
		return std::unexpected(Engine::Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location)).WithIssues(std::move(errors)));
	}

}
