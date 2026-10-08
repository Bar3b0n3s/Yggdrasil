#include "EnginePCH.h"
#include "Engine/AssetPipeline/Private/ImportErrors.h"

#include <utility>

namespace Engine {

	namespace Utils {

		Error MakeImportFailed(const Error& error, const std::string& sourcePath, std::string context)
		{
			ErrorLocation location = error.GetLocation();
			location.File = sourcePath;
			Error failed = Error(ErrorCode::ImportFailed, error.GetMessageText()).WithLocation(std::move(location)).WithHint(error.GetHint()).WithIssues(error.GetIssues());
			for (const std::string& inner : error.GetContexts())
			{
				// Through a second error: assigning `std::move(failed).WithContext(...)` to `failed` would move-assign it to
				// itself (CodeStyle §7).
				Error next = std::move(failed).WithContext(inner);
				failed = std::move(next);
			}
			return std::move(failed).WithContext(std::move(context));
		}

	}

}
