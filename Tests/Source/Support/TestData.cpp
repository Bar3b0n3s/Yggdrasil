#include "TestsPCH.h"
#include "Support/TestData.h"

#include "Engine/Core/FileSystem.h"

#include <algorithm>

#if !defined(ENGINE_REPO_ROOT)
	#error "ENGINE_REPO_ROOT must be defined for the Tests project (Dependencies.lua, ApplyFirstPartySettings)"
#endif

namespace Engine {

	namespace Test {

		std::filesystem::path GetRepositoryRoot()
		{
			return std::filesystem::path(ENGINE_REPO_ROOT);
		}

		std::filesystem::path GetTestDataPath(std::string_view relative)
		{
			std::filesystem::path path = GetRepositoryRoot() / "Tests" / "Data";
			if (!relative.empty())
				path /= std::filesystem::path(std::string(relative));
			return path.lexically_normal();
		}

		Result<std::string> ReadTestDataText(std::string_view relative)
		{
			return FileSystem::ReadText(GetTestDataPath(relative));
		}

		Result<std::vector<std::string>> ListTestDataFiles(std::string_view relativeDirectory, const std::vector<std::string>& extensions)
		{
			const std::filesystem::path root = GetTestDataPath();
			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(GetTestDataPath(relativeDirectory), true));

			std::vector<std::string> files;
			for (const std::filesystem::path& entry : entries)
			{
				const std::string extension = entry.extension().generic_string();
				if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
					continue;
				std::error_code error;
				if (!std::filesystem::is_regular_file(entry, error) || error)
					continue;
				files.push_back(entry.lexically_relative(root).generic_string());
			}
			std::sort(files.begin(), files.end());
			return files;
		}

	}

}
