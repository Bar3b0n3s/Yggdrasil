#include "TestsPCH.h"

#include "EditorCore/Private/EditorFileError.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorFileError: an operating-system access failure becomes Io with one wording, keeping its details")
		{
			ErrorLocation location;
			location.JsonPointer = "/path";
			const Error denied = Error(ErrorCode::PermissionDenied, "cannot write 'Assets/Main.scene': Access is denied")
									 .WithHint("close the program that holds it")
									 .WithLocation(location)
									 .WithContext("while saving the scene");
			const Error converted = Utils::ToEditorFileError(denied);
			CHECK(converted.GetCode() == ErrorCode::Io);
			CHECK(converted.GetMessageText() == "cannot write 'Assets/Main.scene': Access is denied (the operating system denied access)");
			CHECK(converted.GetHint() == "close the program that holds it");
			CHECK(converted.GetLocation().JsonPointer == std::optional<std::string>("/path"));
			CHECK(converted.GetContexts() == std::vector<std::string>{ "while saving the scene" });

			const Error notFound(ErrorCode::NotFound, "no such file");
			CHECK(Utils::ToEditorFileError(notFound).GetCode() == ErrorCode::NotFound);
			CHECK(Utils::ToEditorFileError(notFound).GetMessageText() == "no such file");

			CHECK(Utils::ToEditorFileStatus(Status()).has_value());
			const Status failed = Utils::ToEditorFileStatus(std::unexpected(denied));
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Io);
		}
	}

}
