#include "EditorPCH.h"
#include "EditorCore/Private/EditorFileError.h"

#include <format>

namespace Engine {

	namespace Utils {

		Error ToEditorFileError(Error error)
		{
			if (error.GetCode() != ErrorCode::PermissionDenied)
				return error;
			// Each With* step moves into a new object: assigning an Error to itself through With* would self-move.
			Error converted = Error(ErrorCode::Io, std::format("{} (the operating system denied access)", error.GetMessageText()))
								  .WithLocation(error.GetLocation())
								  .WithHint(error.GetHint())
								  .WithIssues(error.GetIssues());
			for (const std::string& context : error.GetContexts())
			{
				Error next = std::move(converted).WithContext(context);
				converted = std::move(next);
			}
			return converted;
		}

		Status ToEditorFileStatus(Status status)
		{
			if (status)
				return status;
			return std::unexpected(ToEditorFileError(std::move(status).error()));
		}

	}

}
