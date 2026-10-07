#include "EnginePCH.h"
#include "Engine/Core/Error.h"

#include <array>
#include <iterator>

namespace Engine {

	namespace Utils {

		// RFC 6901 writes the document root as the empty pointer, which would vanish in a one-line message.
		static std::string_view DisplayJsonPointer(std::string_view pointer)
		{
			return pointer.empty() ? std::string_view("(root)") : pointer;
		}

		// "<File>[:<Line>[:<Column>]]". Without a file the position reads "line <Line>[:<Column>]", and a column without
		// a line reads "column <Column>", so that no set field is lost.
		static void AppendFilePosition(std::string& where, const ErrorLocation& location)
		{
			const size_t start = where.size();
			where += location.File;
			if (location.Line != 0)
			{
				where += location.File.empty() ? "line " : ":";
				std::format_to(std::back_inserter(where), "{}", location.Line);
				if (location.Column != 0)
					std::format_to(std::back_inserter(where), ":{}", location.Column);
			}
			else if (location.Column != 0)
			{
				if (where.size() != start)
					where += ' ';
				std::format_to(std::back_inserter(where), "column {}", location.Column);
			}
		}

		static std::string FormatWhere(const ErrorLocation& location)
		{
			std::string where;
			AppendFilePosition(where, location);
			if (location.JsonPointer.has_value())
			{
				if (!where.empty())
					where += ' ';
				where += DisplayJsonPointer(*location.JsonPointer);
			}
			if (location.Entity.IsValid())
			{
				if (!where.empty())
					where += ' ';
				std::format_to(std::back_inserter(where), "entity {}", location.Entity);
			}
			return where;
		}

		static void AppendIssue(std::string& text, const ErrorIssue& issue)
		{
			text += " | ";
			text += DisplayJsonPointer(issue.JsonPointer);
			text += ": ";
			text += issue.Message;
			if (!issue.Hint.empty())
				std::format_to(std::back_inserter(text), " (hint: {})", issue.Hint);
			if (!issue.Suggestions.empty())
			{
				text += " (suggestions: ";
				for (size_t index = 0; index < issue.Suggestions.size(); ++index)
				{
					if (index != 0)
						text += ", ";
					text += issue.Suggestions[index];
				}
				text += ')';
			}
		}

	}

	std::string_view ErrorCodeToString(ErrorCode code)
	{
		switch (code)
		{
			case ErrorCode::Unknown:            return "Unknown";
			case ErrorCode::InvalidArgument:    return "InvalidArgument";
			case ErrorCode::NotFound:           return "NotFound";
			case ErrorCode::AlreadyExists:      return "AlreadyExists";
			case ErrorCode::InvalidState:       return "InvalidState";
			case ErrorCode::Io:                 return "Io";
			case ErrorCode::Parse:              return "Parse";
			case ErrorCode::Validation:         return "Validation";
			case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
			case ErrorCode::ImportFailed:       return "ImportFailed";
			case ErrorCode::CompileFailed:      return "CompileFailed";
			case ErrorCode::Script:             return "Script";
			case ErrorCode::Gpu:                return "Gpu";
			case ErrorCode::Timeout:            return "Timeout";
			case ErrorCode::PermissionDenied:   return "PermissionDenied";
			case ErrorCode::Unsupported:        return "Unsupported";
			case ErrorCode::Conflict:           return "Conflict";
			case ErrorCode::Cancelled:          return "Cancelled";
		}

		// Documented: a value outside the enumeration (a code read from a newer peer, for example) is "Unknown". Not a
		// programmer error, so no assert.
		return "Unknown";
	}

	std::optional<ErrorCode> ErrorCodeFromString(std::string_view name)
	{
		// Every enumerator, in order: ErrorCodeToString's switch names each, and its test checks that the value after the
		// last one listed here is not a code, so an appended enumerator cannot be left out.
		static constexpr std::array<ErrorCode, 18> Codes = { ErrorCode::Unknown, ErrorCode::InvalidArgument, ErrorCode::NotFound,
			ErrorCode::AlreadyExists, ErrorCode::InvalidState, ErrorCode::Io, ErrorCode::Parse, ErrorCode::Validation, ErrorCode::UnsupportedVersion,
			ErrorCode::ImportFailed, ErrorCode::CompileFailed, ErrorCode::Script, ErrorCode::Gpu, ErrorCode::Timeout, ErrorCode::PermissionDenied,
			ErrorCode::Unsupported, ErrorCode::Conflict, ErrorCode::Cancelled };
		for (const ErrorCode code : Codes)
		{
			if (ErrorCodeToString(code) == name)
				return code;
		}
		return std::nullopt;
	}

	std::string Error::ToString() const
	{
		std::string text(ErrorCodeToString(m_Code));
		text += ": ";

		const std::string where = Utils::FormatWhere(m_Location);
		if (!where.empty())
		{
			text += where;
			text += ": ";
		}
		text += m_Message;

		for (const std::string& context : m_Contexts)
		{
			text += "; ";
			text += context;
		}
		if (!m_Hint.empty())
			std::format_to(std::back_inserter(text), " (hint: {})", m_Hint);
		for (const ErrorIssue& issue : m_Issues)
			Utils::AppendIssue(text, issue);
		return text;
	}

}
