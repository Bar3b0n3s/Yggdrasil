#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Expected failures as values (Architecture §4.6). Programmer errors assert instead (Assert.h); fatal environment
// failures go through FatalError (FatalError.h).

namespace Engine {

	// The category of an expected failure. Automation maps each code to a JSON-RPC error (§13.3); scripts and the editor
	// show its name. Values are stable: append new codes at the end.
	enum class ErrorCode : uint16_t
	{
		Unknown,
		InvalidArgument,    // a parameter is malformed or out of range
		NotFound,           // file, entity, asset, component, method ... does not exist
		AlreadyExists,      // creating something whose name or ID is taken
		InvalidState,       // the operation is not allowed in the current state
		Io,                 // the operating system reported an I/O failure
		Parse,              // syntactically malformed input (JSON syntax, truncated binary data)
		Validation,         // well-formed input that violates a rule (wrong JSON type, case mismatch, out-of-range value)
		UnsupportedVersion, // a file newer than this build supports
		ImportFailed,
		CompileFailed,
		Script,
		Gpu,
		Timeout,
		PermissionDenied, // includes writes to a read-only mount
		Unsupported,      // the operation exists but is not available here (also what contract stubs return)
		Conflict,         // optimistic-concurrency mismatch (ifRevision)
		Cancelled
	};

	// The enumerator name ("NotFound"); "Unknown" for a value outside the enumeration.
	[[nodiscard]] std::string_view ErrorCodeToString(ErrorCode code);

	// Where an error was found. Every field is optional: an empty File, a zero Line or Column, a JsonPointer without a
	// value and the invalid Entity mean "not set".
	struct ErrorLocation
	{
		std::string File;    // VFS or native path of the file
		uint32_t Line = 0;   // 1-based
		uint32_t Column = 0; // 1-based
		// RFC 6901 pointer of the offending value. "" is set: it is the document root (and "/" the pointer to the key "").
		std::optional<std::string> JsonPointer;
		UUID Entity;

		// True when at least one field is set.
		[[nodiscard]] bool IsSet() const
		{
			return !File.empty() || Line != 0 || Column != 0 || JsonPointer.has_value() || Entity.IsValid();
		}
	};

	// One of several problems reported by a single error, such as every invalid field of one automation request
	// (Architecture §13.3 "issues", §5.4 validation). The error's own message summarizes them ("2 invalid fields").
	struct ErrorIssue
	{
		std::string JsonPointer;              // RFC 6901 pointer of the offending value; "" is the document root
		std::string Message;                  // "unknown field 'Mas' on 'RigidBody'"
		std::string Hint;                     // "did you mean 'Mass'?"; empty when there is none
		std::vector<std::string> Suggestions; // candidate values, best first (fuzzy name suggestions); may be empty
	};

	// An expected failure: a code, a message, the contexts it was propagated through, a location, a hint and any number
	// of issues. A plain value type: copyable, movable, thread-compatible. The builders are rvalue-qualified so an error
	// is decorated while it is returned:
	//     return std::unexpected(Error(ErrorCode::NotFound, "no such file").WithHint("did you mean 'Level1.scene'?"));
	// The accessor is GetMessageText, not GetMessage: <windows.h> defines GetMessage as a macro (ADR 0003).
	class Error
	{
	public:
		Error(ErrorCode code, std::string message)
			: m_Code(code), m_Message(std::move(message))
		{
		}

		// Appends a context describing what was being done when the error passed through, as a phrase such as
		// "while importing 'Assets/Track.glb'". Contexts accumulate innermost first; the code, message, location and
		// hint are kept unchanged.
		Error&& WithContext(std::string context) &&
		{
			m_Contexts.push_back(std::move(context));
			return std::move(*this);
		}

		// Merges `location` into the current location: each field that is set in `location` replaces the current one,
		// unset fields keep their value. A JSON reader can thus record the pointer and its caller the file.
		Error&& WithLocation(ErrorLocation location) &&
		{
			if (!location.File.empty())
				m_Location.File = std::move(location.File);
			if (location.Line != 0)
				m_Location.Line = location.Line;
			if (location.Column != 0)
				m_Location.Column = location.Column;
			if (location.JsonPointer.has_value())
				m_Location.JsonPointer = std::move(location.JsonPointer);
			if (location.Entity.IsValid())
				m_Location.Entity = location.Entity;
			return std::move(*this);
		}

		// Sets the hint (replacing any previous one), such as "did you mean 'Mass'?".
		Error&& WithHint(std::string hint) &&
		{
			m_Hint = std::move(hint);
			return std::move(*this);
		}

		// Appends one issue; issues keep the order in which they were added.
		Error&& WithIssue(ErrorIssue issue) &&
		{
			m_Issues.push_back(std::move(issue));
			return std::move(*this);
		}

		// Appends `issues` in order. A validator collects its issues in a vector and attaches them once, which avoids
		// re-assigning an error to itself in a loop.
		Error&& WithIssues(std::vector<ErrorIssue> issues) &&
		{
			for (ErrorIssue& issue : issues)
				m_Issues.push_back(std::move(issue));
			return std::move(*this);
		}

		[[nodiscard]] ErrorCode GetCode() const { return m_Code; }
		[[nodiscard]] const std::string& GetMessageText() const { return m_Message; }
		[[nodiscard]] const std::vector<std::string>& GetContexts() const { return m_Contexts; }
		[[nodiscard]] const ErrorLocation& GetLocation() const { return m_Location; }
		[[nodiscard]] const std::string& GetHint() const { return m_Hint; }
		[[nodiscard]] const std::vector<ErrorIssue>& GetIssues() const { return m_Issues; }

		// One line for logs, the console and automation:
		//     <Code>: [<where>: ]<message>[; <context 1>; <context 2> ...][ (hint: <hint>)][ | <issue 1> | <issue 2> ...]
		// <where> joins the set location parts with spaces: "<File>[:<Line>[:<Column>]]", "<JsonPointer>" and
		// "entity <uuid>". Each issue is written as
		//     <JsonPointer>: <Message>[ (hint: <Hint>)][ (suggestions: <Suggestion 1>, <Suggestion 2> ...)]
		// In both places the root pointer "" is written as "(root)". Examples:
		//     Validation: Assets/Scenes/Level1.scene /Entities/12/Components/RigidBody/Mass: expected number, got string;
		//     while loading scene 'Level1' (hint: write a number such as 1.5)
		//
		//     Validation: 2 invalid fields | /components/RigidBody/Mas: unknown field 'Mas' on 'RigidBody' (hint: did you
		//     mean 'Mass'?) (suggestions: Mass) | /components/RigidBody/Friction: must be >= 0 (got -0.2)
		[[nodiscard]] std::string ToString() const;
	private:
		ErrorCode m_Code = ErrorCode::Unknown;
		std::string m_Message;
		std::vector<std::string> m_Contexts;
		ErrorLocation m_Location;
		std::string m_Hint;
		std::vector<ErrorIssue> m_Issues;
	};

}

// Formats as Error::ToString(); no format specification is accepted ("{}").
template<>
struct std::formatter<Engine::Error, char>
{
	constexpr std::format_parse_context::iterator parse(std::format_parse_context& context)
	{
		return context.begin();
	}

	template<typename FormatContext>
	auto format(const Engine::Error& error, FormatContext& context) const
	{
		return std::format_to(context.out(), "{}", error.ToString());
	}
};

// Formats as ErrorCodeToString(); no format specification is accepted ("{}").
template<>
struct std::formatter<Engine::ErrorCode, char>
{
	constexpr std::format_parse_context::iterator parse(std::format_parse_context& context)
	{
		return context.begin();
	}

	template<typename FormatContext>
	auto format(Engine::ErrorCode code, FormatContext& context) const
	{
		return std::format_to(context.out(), "{}", Engine::ErrorCodeToString(code));
	}
};
