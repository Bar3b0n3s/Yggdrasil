#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <string>
#include <string_view>

// The build id (Docs/Decisions/0030-m19-contract.md decision 13, B-BVER): which engine build made a file or answers a
// session. Before M19 the engine reported the constant EngineVersionString everywhere; the build id adds the commit the
// binary was built from and whether its working tree had uncommitted changes, so an exported game, a session file or a
// session.hello names the exact build.
//
// Where it appears (decision 13): session.hello's and session.info's "build", the automation session files' "build"
// (§13.2), Engine.pak's metadata "Build" and GameManifest v2's "Build" (Game.json), each as FormatBuildId's text. Replay
// headers keep writing ReplayEngineVersionString ("0.1.0", Asset/ReplayData.h), never the build id, so neither a commit
// nor a version bump rewrites a replay; the state hash never sees either (§9.1).

namespace Engine {

	// The engine's semantic version (MAJOR.MINOR.PATCH): session.hello's, session.info's and the session files'
	// "engineVersion" (§13.2), and the EngineVersion of exported manifests and Engine.pak's metadata. It moved here from
	// Automation/Protocol/JsonRpc.h, which includes this header, with the build id. The backlog program's reservation
	// ledgers (Docs/Decisions/0028-backlog-program.md) own its value: the last lander of each phase bumps the minor
	// version (0.2.0 at the end of phase 1, 0.3.0 and 0.4.0 after phases 2 and 3), so parallel milestone branches never
	// take the same value. 0.2.0 since M20, phase 1's last lander (Docs/Decisions/0031-m20-contract.md decision 10).
	inline constexpr std::string_view EngineVersionString = "0.2.0";

	// The build a binary is, stamped when it was built (decision 13).
	struct BuildId
	{
		// EngineVersionString of the build.
		std::string Version{};
		// The commit the build's working tree had checked out: exactly 12 lowercase hex digits (the abbreviation is fixed, so
		// a later commit never lengthens it); empty when the build had no git repository or git was unavailable.
		std::string Commit{};
		// True when tracked files of that working tree differed from Commit (staged or not), or when a file git neither
		// tracks nor ignores lay below a directory the engine, editor and Runtime are built from (Engine/, Editor/, Runtime/,
		// Resources/), since premake compiles every source there, added or not, and the build copies the resources; so a
		// new source not yet added marks the build dirty. Always false without a Commit.
		bool Dirty = false;

		[[nodiscard]] bool operator==(const BuildId& other) const = default;
	};

	// This binary's build id: Version is EngineVersionString; Commit and Dirty are the values Scripts/Build.py stamped into
	// the build (decision 13's stamp), or empty and false for a build that has no stamp (a build outside Build.py, such as
	// the IDE's, without an earlier stamp). Thread-safe; the same value for the whole process.
	[[nodiscard]] const BuildId& GetBuildId();

	// The build id as one string, semantic-versioning build metadata: "<Version>" without a commit, "<Version>+<Commit>"
	// for a clean tree, "<Version>+<Commit>.dirty" for a dirty one ("0.2.0+3f2a9c1d4e5b.dirty"). Asserts a valid id (a
	// Version of three decimal numbers without leading zeros, a Commit of 12 lowercase hex digits or empty, Dirty only with
	// a Commit).
	[[nodiscard]] std::string FormatBuildId(const BuildId& id);

	// Parses FormatBuildId's text back. Errors: InvalidArgument naming the text for anything else (a version that is not
	// three decimal numbers without signs or leading zeros, a commit that is not 12 lowercase hex digits, a suffix other
	// than ".dirty", other build metadata, a pre-release part).
	[[nodiscard]] Result<BuildId> ParseBuildId(std::string_view text);

}
