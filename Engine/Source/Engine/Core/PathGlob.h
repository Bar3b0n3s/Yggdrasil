#pragma once

#include "Engine/Core/Base.h"

#include <string_view>

// Path globs (Docs/Decisions/0024-m15-contract.md decision 7): the one matcher of Export.Exclude (§7.6) and Testing.Replays
// (§11.10), shared by the exporter, testing exports (EditorCore/Export/TestingExport.h), the editor's replay expansion
// (EditorFeatureTestHost) and the Runtime's expansion over the Game.pak TOC, so a testing export and the runs of its
// replays can never disagree about which paths a pattern names. It replaces the exporter's Utils::MatchesExportGlob and
// the Runtime's private copy (M15 stream B). The Python side of the same rule, Scripts/Test.py's glob_regex (the determinism
// suite's replay expansion), cannot call it, so both run the shared vectors of Tests/Data/PathGlob/Cases.json
// ({"Format": "PathGlobCases", "Version": 1, "Cases": [{"Pattern", "Path", "Matches"}]}).

namespace Engine {

	// Whether the project-relative path `path` ("Assets/Tests/Replays/Input.replay") matches `pattern`: '*' matches any
	// characters except '/', '?' one character except '/', "**" any characters including '/' (and "**/" also matches no
	// directory at all, so "Assets/**/Old.png" matches "Assets/Old.png"); every other character matches itself,
	// case-sensitively (§4.10). A character is a UTF-8 code point, as Test.py's glob_regex counts them over a Python str
	// ('?' matches both bytes of U+00F1, and no wildcard ends inside a code point); a byte of an ill-formed sequence, which no
	// VfsPath holds, counts as one character. Linear in pattern length times path length (no backtracking). Pure. Pattern
	// and path carry no scheme: a caller strips the leading "project://" that a Testing.Replays entry may spell (the
	// editor's replay expansion, the Runtime's TOC expansion, testing exports and Test.py all do), and Export.Exclude has
	// none.
	[[nodiscard]] bool MatchesPathGlob(std::string_view pattern, std::string_view path);

}
