#include "TestsPCH.h"

#include "EditorCore/Scripting/EditorScriptService.h"

#include "Support/EditorTestFixture.h"

#include <array>

namespace Engine {

	namespace {

		class EmptyScriptDiagnostics final : public IScriptDiagnosticsProvider
		{
		public:
			uint64_t GetEnvironmentHash() const override { return 1; }
			std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& /*request*/) override
			{
				return {};
			}
		};

		VfsPath ScriptServicePath(std::string_view path)
		{
			auto parsed = VfsPath::Create("project", path);
			REQUIRE(parsed.has_value());
			return *parsed;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorScriptService: source operations require an open project" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ScriptServiceLauncher");
			EmptyScriptDiagnostics diagnostics;
			EditorScriptService service(fixture.GetEditor(), diagnostics);
			const VfsPath path = ScriptServicePath("Assets/Scripts/Module.luau");
			const auto read = service.Read(path);
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::InvalidState);
			const auto write = service.Write(path, "return 1");
			REQUIRE_FALSE(write.has_value());
			CHECK(write.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorScriptService: writes reject files outside Assets and invalid source encoding" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ScriptServiceInput");
			fixture.CreateAndOpenProject();
			EmptyScriptDiagnostics diagnostics;
			EditorScriptService service(fixture.GetEditor(), diagnostics);
			for (const std::string_view name : std::array{ "Library/Module.luau", "Assets/Module.txt" })
			{
				const auto written = service.Write(ScriptServicePath(name), "return 1");
				REQUIRE_FALSE(written.has_value());
				CHECK(written.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const std::string invalidUtf8(1, static_cast<char>(0xff));
			const auto written = service.Write(ScriptServicePath("Assets/Module.luau"), invalidUtf8);
			REQUIRE_FALSE(written.has_value());
			CHECK(written.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(service.Read(ScriptServicePath("Assets/Module.luau")).has_value());
		}

		TEST_CASE("EditorScriptService: writing unchanged source preserves the handle without another undo step" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ScriptServiceNoOp");
			fixture.CreateAndOpenProject();
			EmptyScriptDiagnostics diagnostics;
			EditorScriptService service(fixture.GetEditor(), diagnostics);
			const VfsPath path = ScriptServicePath("Assets/Scripts/Module.luau");
			const auto first = service.Write(path, "return 42\n");
			REQUIRE(first.has_value());
			const auto same = service.Write(path, "return 42\n");
			REQUIRE(same.has_value());
			CHECK(same->Script == first->Script);
			CHECK(same->UndoIndex == 0);
			const auto source = service.Read(path);
			REQUIRE(source.has_value());
			CHECK(*source == "return 42\n");
		}
	}

}
