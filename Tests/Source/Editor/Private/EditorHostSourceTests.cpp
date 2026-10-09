#include "TestsPCH.h"

#include "Editor/Private/EditorHostSource.h"
#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostSource: Console links resolve project URIs and engine source without launching a shell")
		{
			Test::TempDirectory directory("HostSource");
			const auto project = directory / "Project";
			const auto repository = directory / "Repository";
			REQUIRE(FileSystem::CreateDirectories(project / "Assets/Scripts").has_value());
			REQUIRE(FileSystem::CreateDirectories(repository / "Engine/Source").has_value());
			REQUIRE(FileSystem::CreateDirectories(repository / "Resources/Scripting").has_value());
			const auto script = project / "Assets/Scripts/Game {line}.luau";
			const auto cpp = repository / "Engine/Source/App.cpp";
			const auto builtin = repository / "Resources/Scripting/Engine.d.luau";
			for (const auto& file : { script, cpp, builtin })
				REQUIRE(FileSystem::WriteFileAtomic(file, {}).has_value());
			const auto check = [&project, &repository](const std::filesystem::path& input, const std::filesystem::path& expected)
			{
				const auto result = ResolveEditorSourcePath(input, project, repository);
				REQUIRE(result.has_value());
				std::error_code error;
				CHECK(std::filesystem::equivalent(*result, expected, error));
				CHECK_FALSE(error);
			};
			check("project://Assets/Scripts/Game {line}.luau", script);
			check("project:/Assets/Scripts/Game {line}.luau", script);
			check("Assets/Scripts/Game {line}.luau", script);
			check(script, script);
			check("Engine/Source/App.cpp", cpp);
			check(cpp, cpp);
			check("engine://Scripting/Engine.d.luau", builtin);
			CHECK(ResolveEditorSourcePath(cpp, {}, repository).has_value());
			const auto noProject = ResolveEditorSourcePath("project://Assets/Scripts/Game.luau", {}, repository);
			REQUIRE_FALSE(noProject.has_value());
			CHECK(noProject.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorHostSource: missing files directories and mount escapes are rejected")
		{
			Test::TempDirectory directory("HostSourceErrors");
			const auto project = directory / "Project";
			const auto repository = directory / "Repository";
			REQUIRE(FileSystem::CreateDirectories(project).has_value());
			REQUIRE(FileSystem::CreateDirectories(repository / "Resources").has_value());
			const auto outside = directory / "outside.luau";
			REQUIRE(FileSystem::WriteFileAtomic(outside, {}).has_value());
			const auto check = [&project, &repository](const std::filesystem::path& input, ErrorCode expected)
			{
				const auto result = ResolveEditorSourcePath(input, project, repository);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == expected);
			};
			check("project://../outside.luau", ErrorCode::PermissionDenied);
			check("engine://../../outside.luau", ErrorCode::PermissionDenied);
			check(outside, ErrorCode::PermissionDenied);
			check("project://missing.luau", ErrorCode::NotFound);
			check("project://.", ErrorCode::InvalidArgument);
			check("cache://script.luau", ErrorCode::InvalidArgument);
			check("project:script.luau", ErrorCode::InvalidArgument);
			check({}, ErrorCode::InvalidArgument);
		}
	}

}
