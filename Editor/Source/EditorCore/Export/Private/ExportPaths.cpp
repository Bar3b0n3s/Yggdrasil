#include "EditorPCH.h"
#include "EditorCore/Export/Private/ExportPaths.h"

#include "Engine/Core/PathGlob.h"
#include "Engine/Core/VfsPath.h"

#include <format>
#include <string>

namespace Engine {

	namespace Utils {

		Status CheckExportOutputDirectory(std::string_view directory)
		{
			if (directory.empty())
				return {};
			std::string hint = std::format("pass a directory below {0}/, such as \"{0}/Windows-Release/Game\", or leave outDir out for the default", ExportBuildDirectory);
			if (Status valid = VfsPath::ValidateRelativePath(directory); !valid.has_value())
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("the output directory '{}' is not a valid project-relative path: {}", directory, valid.error().GetMessageText()))
						.WithHint(std::move(hint)));
			}
			const size_t separator = directory.find('/');
			if (separator == std::string_view::npos || directory.substr(0, separator) != ExportBuildDirectory)
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("the output directory '{}' is not below {}/, the only directory an export writes to", directory, ExportBuildDirectory))
						.WithHint(std::move(hint)));
			}
			return {};
		}

		bool MatchesExportGlob(std::string_view pattern, std::string_view path)
		{
			return MatchesPathGlob(pattern, path);
		}

	}

}
