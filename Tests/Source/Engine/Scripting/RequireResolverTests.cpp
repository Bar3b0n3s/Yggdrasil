#include "TestsPCH.h"
#include "Engine/Scripting/RequireResolver.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <doctest/doctest.h>

#include <format>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		class RecordingModuleReader final : public IScriptModuleReader
		{
		public:
			[[nodiscard]] Result<std::string> ReadModule(const VfsPath& path) override
			{
				Reads.push_back(path.ToString());
				return std::string("return {}");
			}
		public:
			std::vector<std::string> Reads{};
		};

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("RequireResolver: relative modules normalize inside Assets and record canonical unique edges")
		{
			auto root = VfsPath::Parse("project://Assets/Scripts/Game.luau");
			REQUIRE(root.has_value());
			RequireResolver resolver;
			auto board = resolver.Resolve(*root, "./Board");
			REQUIRE(board.has_value());
			CHECK(board->ToString() == "project://Assets/Scripts/Board.luau");
			REQUIRE(resolver.Resolve(*root, "./Board").has_value());
			auto utility = resolver.Resolve(*root, "../Shared/Utility.luau");
			REQUIRE(utility.has_value());
			CHECK(utility->ToString() == "project://Assets/Shared/Utility.luau");
			const auto dependencies = resolver.GetRequires();
			REQUIRE(dependencies.size() == 2);
			CHECK(dependencies[0].From == "Assets/Scripts/Game.luau");
			CHECK(dependencies[0].Request == "../Shared/Utility.luau");
			CHECK(dependencies[1].Path == "Assets/Scripts/Board.luau");
			CHECK_FALSE(dependencies[1].Handle.IsValid());
		}

		TEST_CASE("RequireResolver: a resolver records a bounded number of distinct edges")
		{
			auto root = VfsPath::Parse("project://Assets/Scripts/Game.luau");
			REQUIRE(root.has_value());
			RequireResolver resolver;
			size_t resolved = 0;
			for (size_t index = 0; index < MaxScriptRequireEdges; ++index)
				resolved += resolver.Resolve(*root, std::format("./Module{:05}", index)).has_value() ? 1 : 0;
			CHECK(resolved == MaxScriptRequireEdges);
			CHECK(resolver.GetRequires().size() == MaxScriptRequireEdges);
			// A recorded edge resolves again; a new one is refused before it is recorded.
			CHECK(resolver.Resolve(*root, "./Module00000").has_value());
			const auto refused = resolver.Resolve(*root, "./Another");
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			CHECK(refused.error().GetMessageText().find(std::format("at most {} distinct require edges", MaxScriptRequireEdges)) != std::string::npos);
			CHECK(resolver.GetRequires().size() == MaxScriptRequireEdges);
		}

		TEST_CASE("RequireResolver: compilation labels and replay origins do not relax module path validation")
		{
			RequireResolver resolver;
			for (const std::string_view identity : { "project://Assets/Tests/X.replay",
					 "project://Assets/__Automation/eval" })
			{
				CAPTURE(identity);
				auto importer = VfsPath::Parse(identity);
				REQUIRE(importer.has_value());
				const auto module = resolver.Resolve(*importer, "../Shared/Probe");
				REQUIRE_FALSE(module.has_value());
				CHECK(module.error().GetCode() == ErrorCode::Validation);
				CHECK_FALSE(resolver.Resolve(*importer, "../../outside").has_value());
				CHECK_FALSE(resolver.Resolve(*importer, "./Other.replay").has_value());
			}
		}

		TEST_CASE("RequireResolver: rejects escapes schemes aliases and invalid path spellings before reading")
		{
			auto root = VfsPath::Parse("project://Assets/Scripts/Game.luau");
			REQUIRE(root.has_value());
			RequireResolver resolver;
			for (const std::string_view request : { "../../outside", "./../../../Assets/Back", "project://Assets/Other.luau",
					 "/tmp/module", "C:/module", "@alias/module", ".\\Board", "./Board.txt", "./nul", "./Board:stream" })
			{
				CAPTURE(request);
				const auto result = resolver.Resolve(*root, request);
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::Validation);
			}
			RecordingModuleReader reader;
			auto outside = VfsPath::Parse("project://Settings.luau");
			REQUIRE(outside.has_value());
			CHECK_FALSE(resolver.ReadSource(reader, *outside).has_value());
			CHECK(reader.Reads.empty());
			CHECK(resolver.GetRequires().empty());
		}

		TEST_CASE("RequireResolver: reads source only through the dependency reader with exact case")
		{
			RequireResolver resolver;
			RecordingModuleReader reader;
			auto path = VfsPath::Parse("project://Assets/Scripts/Board.luau");
			REQUIRE(path.has_value());
			const auto source = resolver.ReadSource(reader, *path);
			REQUIRE(source.has_value());
			CHECK(*source == "return {}");
			REQUIRE(reader.Reads.size() == 1);
			CHECK(reader.Reads.front() == path->ToString());
		}

		TEST_CASE("RequireResolver: cycles report the complete chain and failed entry preserves the active stack")
		{
			auto first = VfsPath::Parse("project://Assets/A.luau");
			auto second = VfsPath::Parse("project://Assets/B.luau");
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			RequireResolver resolver;
			REQUIRE(resolver.EnterModule(*first).has_value());
			REQUIRE(resolver.EnterModule(*second).has_value());
			const Status cycle = resolver.EnterModule(*first);
			REQUIRE_FALSE(cycle.has_value());
			CHECK(cycle.error().GetCode() == ErrorCode::Script);
			CHECK(cycle.error().GetMessageText().find("Assets/A.luau") != std::string::npos);
			CHECK(cycle.error().GetMessageText().find("Assets/B.luau") != std::string::npos);
			resolver.LeaveModule(*second);
			resolver.LeaveModule(*first);
			CHECK(resolver.EnterModule(*first).has_value());
			resolver.LeaveModule(*first);
		}
	}

}
