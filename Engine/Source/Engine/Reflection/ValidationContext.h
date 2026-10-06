#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	enum class DiagnosticSeverity : uint8_t
	{
		Warning,
		Error
	};

	// One finding of a validation or load: severity, a stable diagnostic code where one exists (the codes of §13.3/§14 and
	// the REFLECTION_*/SCENE_* codes declared next to their producers; empty for plain field-range errors), the RFC 6901
	// pointer of the offending value, a message, and an optional hint and suggestions ("did you mean").
	struct ValidationIssue
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		std::string Code;
		std::string JsonPointer;
		std::string Message;
		std::string Hint;
		std::vector<std::string> Suggestions;
	};

	// Collects the issues of one validation pass (Architecture §5.4): field metadata checks (finite values, Min, Max,
	// MinMagnitude, enum names, Variant schemas) and the per-type Validate callbacks of the registry,
	//     .Validate([](const RigidBodyComponent& component, ValidationContext& context)
	//     {
	//         if (component.Type == BodyType::Dynamic && component.Mass <= 0.0f)
	//             context.Error("Mass", "must be > 0 for Dynamic bodies");
	//     });
	// Issues are located relative to a current JSON pointer: the base pointer given at construction plus every key pushed
	// with PushKey. A validation reports every problem it finds instead of stopping at the first one, so one automation
	// request or load reports all bad fields at once (ErrorIssue, ADR 0003 decision 12). Not thread-safe (one owner).
	class ValidationContext
	{
	public:
		// `basePointer` locates the validated object in its document ("" for the root, "/components/Transform" for a
		// component inside an automation request).
		explicit ValidationContext(std::string basePointer = {});

		// An Error at <current pointer>/<field> (`field` is one key, escaped per RFC 6901 by the context). An empty `field`
		// locates the issue at the current pointer itself.
		void Error(std::string_view field, std::string message);
		// A Warning at <current pointer>/<field>.
		void Warning(std::string_view field, std::string message);
		// Adds `issue` as is; its JsonPointer is absolute.
		void AddIssue(ValidationIssue issue);

		// Descends into `key` (a field name, map key or decimal array index): later issues are located below it. Every
		// PushKey is matched by one PopKey (asserted).
		void PushKey(std::string_view key);
		void PopKey();

		// The current absolute pointer.
		[[nodiscard]] const std::string& GetPointer() const { return m_Pointer; }

		[[nodiscard]] bool HasErrors() const;
		[[nodiscard]] size_t GetErrorCount() const;
		[[nodiscard]] std::span<const ValidationIssue> GetIssues() const { return m_Issues; }
		// Moves the collected issues out; the context is empty afterwards.
		[[nodiscard]] std::vector<ValidationIssue> TakeIssues();

		// Success when no Error was collected (warnings never fail). Otherwise a Validation error whose message reads
		// "<n> invalid field(s) in <subject>" (one error: its own message) and which carries one ErrorIssue per Error, in
		// the order they were added, with hint and suggestions; located at the base pointer.
		[[nodiscard]] Status ToStatus(std::string_view subject) const;
	private:
		std::string m_Pointer;
		std::vector<size_t> m_KeyLengths; // m_Pointer's length before each PushKey
		std::vector<ValidationIssue> m_Issues;
	};

}
