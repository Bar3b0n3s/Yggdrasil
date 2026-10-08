#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <string>
#include <vector>

// The editor's observe method (Architecture §13.5, §13.10): docs.get. The reads the Runtime shares (log.read,
// events.read; stats.get, physics.bodyInfo and audio.stats in later milestones) are
// Engine/Automation/Methods/ObserveMethods.h's (M7, Docs/Decisions/0012-m7-decisions.md decision 12). Conventions as in
// MethodRegistry.h.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// docs.get {topic?} (§13.10): without a topic, the list of topics; with one, its Markdown. Topics are
	// "skills/<name>" (.claude/skills/<name>/SKILL.md) and "reference/<name>" (Docs/Reference/<name>.md, generated from
	// M14), read from the repository root the server was given (AutomationServerSpecification::DocsRoot).
	struct DocsGetParams
	{
		std::string Topic{};
	};

	// Registry struct "DocsTopic".
	struct DocsTopic
	{
		std::string Name{};  // "skills/game-building"
		std::string Title{}; // the skill's description, or the document's first heading
	};

	struct DocsGetResult
	{
		std::vector<DocsTopic> Topics{}; // without a topic
		std::string Topic{};             // with a topic: its name, its Markdown and its repository-relative path
		std::string Content{};
		std::string Path{};
	};

	namespace Automation {

		// docs.get. Errors: NotFound for an unknown topic (with suggestions); Unsupported when the server has no DocsRoot.
		[[nodiscard]] Result<DocsGetResult> DocsGet(EditorMethodContext& context, const DocsGetParams& params);

	}

	// Registers DocsGetParams, DocsTopic and DocsGetResult.
	void RegisterEditorObserveMethodTypes(TypeRegistry& registry);

	// Registers docs.get: read-only, AllowedInBatch, available in the launcher state (§12.1) and a tool (§13.8).
	void RegisterEditorObserveMethods(MethodRegistry& methods);

}
