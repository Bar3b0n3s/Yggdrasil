#pragma once

#include "Engine/Core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Locating the repository's fixtures (Tests/Data) from the Tests binary, decided by M3 (ADR 0003 decision 27,
// Docs/Decisions/0006-m3-decisions.md decision 13): through ENGINE_REPO_ROOT, the absolute repository root that
// ApplyFirstPartySettings defines for every non-Dist configuration (the Tests project has no Dist configuration). The
// path therefore works from any working directory and for workspaces generated elsewhere with --to.

namespace Engine {

	namespace Test {

		// The absolute path of the repository root.
		[[nodiscard]] std::filesystem::path GetRepositoryRoot();

		// The absolute path of Tests/Data, or of `relative` below it ('/'-separated, e.g. "Scenes/Invalid/ZeroId.scene").
		[[nodiscard]] std::filesystem::path GetTestDataPath(std::string_view relative = {});

		// The UTF-8 text of Tests/Data/<relative> (FileSystem::ReadText). Errors: NotFound, Io, Validation (invalid UTF-8).
		[[nodiscard]] Result<std::string> ReadTestDataText(std::string_view relative);

		// Every regular file below Tests/Data/<relativeDirectory> whose extension is one of `extensions` (".scene"), as
		// '/'-separated paths relative to Tests/Data, sorted byte-wise so test order never depends on the file system.
		// Errors: as FileSystem::ListDirectory.
		[[nodiscard]] Result<std::vector<std::string>> ListTestDataFiles(std::string_view relativeDirectory,
			const std::vector<std::string>& extensions);

	}

}
