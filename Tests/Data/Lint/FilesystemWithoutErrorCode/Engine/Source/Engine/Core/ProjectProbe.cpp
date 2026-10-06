#include "Engine/Core/ProjectProbe.h"

#include <system_error>

namespace Engine {

	bool IsProjectDirectory(const std::filesystem::path& directory)
	{
		std::error_code error;
		const bool isDirectory = std::filesystem::is_directory(directory, error);
		// Seeded defect: the throwing overload of exists().
		return isDirectory && std::filesystem::exists(directory / "Project.eproj");
	}

}
