#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The editor's observe method (Architecture §13.5, §13.10): docs.get. The reads the Runtime shares live in
// Engine/Automation/Methods: log.read and events.read in ObserveMethods.h (M7, Docs/Decisions/0012-m7-decisions.md
// decision 12), stats.get in StatsMethods.h, physics.bodyInfo in PhysicsMethods.h and audio.stats in AudioMethods.h.
// Conventions as in MethodRegistry.h.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// The most bytes of text one docs.get page holds; the page's text also stays within this many bytes once escaped as a
	// JSON string, so a page with its section list is always returned inline (below DefaultOffloadThresholdBytes).
	inline constexpr size_t DocsPageBytes = 32 * 1024;
	// The most bytes the section list of a first page takes (DocsGetResult::Sections).
	inline constexpr size_t DocsSectionListBytes = 12 * 1024;
	// The most bytes the section list of a toc result takes, so that result always stays below the offload threshold.
	inline constexpr size_t DocsTocBytes = 40 * 1024;

	// docs.get {topic?, section?, offset?, length?, toc?} (§13.10; Docs/Decisions/0026-m16-contract.md, frictions S1-01 and
	// S2-05): without a topic, the list of topics; with one, its text, read from the repository root the server was given
	// (AutomationServerSpecification::DocsRoot). Topics:
	//   - "skills/<name>": .claude/skills/<name>/SKILL.md;
	//   - "reference/<name>": Docs/Reference/<name>.md (generated);
	//   - "reference/Schemas/<Name>": Docs/Reference/Schemas/<Name>.schema.json (the generated JSON schemas);
	//   - "scripting/<name>.d.luau": Resources/Scripting/<name>.d.luau (the generated script declarations,
	//     "scripting/Engine.d.luau");
	//   - "templates/Scripts/<Name>": Resources/Templates/Scripts/<Name>.luau (the templates of script.create);
	//   - "native/NativeApi.h": Engine/Source/Engine/Native/NativeApi.h, the interface of a game's native module, the one
	//     engine header its sources include (Docs/Decisions/0030-m19-contract.md decision 26), as plain text.
	// Sections: the headings of a Markdown topic ("#" to "######" outside code fences), each with its text up to the next
	// heading of the same or a higher level, and the top-level declarations of a .d.luau topic ("declare ..." with the
	// comment lines above it, adjacent declarations of one name together; a name declared again further on titles its
	// extern type's section "<name> (type)" and its global's "<name>", "Script (type)" and "Script"). Each section has an anchor: the heading's text
	// (a declaration's name) lowercased, spaces turned into '-' and every other ASCII character but letters, digits, '-'
	// and '_' removed, with "-1", "-2", ... added to a repeated anchor ("Input.GetAxis" is "inputgetaxis"). `section` names
	// the first of these that matches: the heading whose text is exactly `section` (case and punctuation kept); the heading
	// whose text is `section` followed by a parenthesized qualifier ("TestSuite" names "TestSuite (type)"); the section
	// whose anchor is `section` after the anchor conversion ("testsuite", "input-1"). A text or name that several headings
	// share names none of them: InvalidArgument listing their anchors. "<topic>#<section>" is the same as `section`.
	// Paging: a topic (or section) whose whole result would be over the offload threshold, and any call with `offset` or
	// `length`, returns one page: at most `length` bytes (default and maximum DocsPageBytes) from `offset`, ending after a
	// line break when one falls inside it, with `nextOffset` (0 at the end) and, on a page that starts at 0, the section
	// list. Every page is returned inline. `toc` returns no text, only the section list of the topic (or the subsections
	// of `section`) with every section's offset and size, down to the deepest heading level whose list fits in
	// DocsTocBytes, and the text's totalBytes.
	struct DocsGetParams
	{
		std::string Topic{};
		std::string Section{};
		uint32_t Offset = 0;
		uint32_t Length = static_cast<uint32_t>(DocsPageBytes);
		bool Toc = false; // M16 friction S2-05
	};

	// Registry struct "DocsTopic".
	struct DocsTopic
	{
		std::string Name{}; // "skills/game-building"
		// A skill's description, a reference document's first heading, a schema's description, or the summary of the script
		// declarations, a script template or the native module's interface.
		std::string Title{};
		uint32_t Bytes = 0; // the size of the topic's file
	};

	// Registry struct "DocsSection": one section of a page's section list.
	struct DocsSection
	{
		std::string Title{};  // the heading's text, or the declaration's name
		std::string Anchor{}; // what `section` takes
		uint32_t Level = 1;   // 1 to 6: the heading's '#' count; 1 for a declaration
		uint32_t Offset = 0;  // where the section starts in the text the page belongs to (the topic's, or the section's)
		uint32_t Bytes = 0;   // its size, subsections included
	};

	struct DocsGetResult
	{
		std::vector<DocsTopic> Topics{};     // without a topic
		std::string Topic{};                 // with a topic: its name (without "#<section>")
		std::string Section{};               // the anchor of the section asked for; empty for the whole topic
		std::string Content{};               // the text, or one page of it
		std::string Path{};                  // the topic's file, repository-relative
		uint32_t Offset = 0;                 // where Content starts in the topic's (the section's) text
		uint32_t TotalBytes = 0;             // the size of that text
		uint32_t NextOffset = 0;             // where the next page starts; 0 when Content reaches the end
		std::vector<DocsSection> Sections{}; // with the first page of a text that needs several, and with toc
	};

	namespace Automation {

		// docs.get. Errors: NotFound for an unknown topic or section (with suggestions); InvalidArgument at /section when
		// the topic's "#<section>" and `section` name different sections or a text several headings share, at /offset
		// beyond the end of the text, at /length outside 1 to DocsPageBytes, at /toc with offset or length, and at /topic for
		// a section, offset, length or toc without a topic; Unsupported when the server has no DocsRoot.
		[[nodiscard]] Result<DocsGetResult> DocsGet(EditorMethodContext& context, const DocsGetParams& params);

	}

	// Registers DocsGetParams, DocsTopic, DocsSection and DocsGetResult.
	void RegisterEditorObserveMethodTypes(TypeRegistry& registry);

	// Registers docs.get: read-only, AllowedInBatch, available in the launcher state (§12.1) and a tool (§13.8).
	void RegisterEditorObserveMethods(MethodRegistry& methods);

}
