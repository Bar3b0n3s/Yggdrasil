#include "TestsPCH.h"
#include "Support/ExportBinaryRoot.h"

#include "EditorCore/Export/Exporter.h"
#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <filesystem>
#include <string_view>
#include <system_error>

namespace Engine {

	namespace Test {

		namespace {

			// Copies every file below `source` into `target`, keeping the relative paths.
			void CopyDirectory(const std::filesystem::path& source, const std::filesystem::path& target)
			{
				std::error_code error;
				std::filesystem::copy(source, target, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
				REQUIRE_MESSAGE(!error, "copying ", PathToUtf8(source), " failed: ", error.message());
			}

		}

		std::filesystem::path MakeExportBinaryRoot(const TempDirectory& directory, const ExportBinaryRootOptions& options)
		{
			const Result<std::filesystem::path> runtime = GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			const std::filesystem::path root = directory.GetPath() / "bin";
			const std::filesystem::path target = root / Exporter::GetBuildOutputDirectoryName(options.Configuration) / "Runtime";
			REQUIRE(FileSystem::CreateDirectories(target).has_value());
			std::error_code error;
			REQUIRE_MESSAGE(std::filesystem::copy_file(*runtime, target / runtime->filename(), std::filesystem::copy_options::overwrite_existing, error),
				"copying the Runtime failed: ", error.message());

			const std::filesystem::path built = runtime->parent_path();
#if defined(ENGINE_PLATFORM_WINDOWS)
			REQUIRE_MESSAGE(std::filesystem::is_directory(built / "Redist", error),
				"the built Runtime has no Redist/ (the app-local CRT): build with python Scripts/Build.py, which stages it");
#endif
			for (const std::string_view staged : { "Redist", "Frameworks", "Vulkan" })
			{
				const std::filesystem::path source = built / std::filesystem::path(staged);
				if (std::filesystem::is_directory(source, error))
					CopyDirectory(source, target / std::filesystem::path(staged));
			}
			if (options.Notices.has_value())
			{
				REQUIRE(FileSystem::WriteFileAtomic(target / "THIRD_PARTY_NOTICES.txt", AsBytes(*options.Notices), { .KeepBackup = false })
						.has_value());
			}
			return root;
		}

	}

}
