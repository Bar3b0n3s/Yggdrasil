#include "TestsPCH.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/AssetTestFixture.h"

#include <doctest/doctest.h>

namespace Engine {

	class ContractScriptModuleReader final : public IScriptModuleReader
	{
	public:
		[[nodiscard]] Result<std::string> ReadModule(const VfsPath& /*path*/) override
		{
			return MakeError(ErrorCode::NotFound, "This checker fixture has no required modules");
		}
	};

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptTypeChecker: strict findings have exclusive source ranges" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			REQUIRE(api.Freeze(types));
			const auto checker = ScriptTypeChecker::Create(api);
			REQUIRE(checker);
			ContractScriptModuleReader modules;
			const auto findings = (*checker)->CheckScript({
				.Path = Test::ParseVfsPath("project://Assets/Bad.luau"),
				.Source = "local count: number = \"wrong\"\nreturn count",
				.Modules = &modules,
			});
			REQUIRE(findings.size() == 1);
			CHECK(findings[0].Severity == DiagnosticSeverity::Error);
			CHECK(findings[0].Code == "SCRIPT_TYPE_ERROR");
			CHECK(findings[0].File == "Assets/Bad.luau");
			CHECK(findings[0].Line == 1);
			CHECK(findings[0].Column > 0);
			CHECK(findings[0].EndLine == 1);
			CHECK(findings[0].EndColumn > findings[0].Column);
		}

		TEST_CASE("TypeChecker: fixtures report expected diagnostics" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Run pinned New-solver fixtures through generated definitions: vector arithmetic, Entity/component proxies, "
				 "read-only writes, strict enum unions, callback self types, generic Script.Define identity, Field.Array nesting, "
				 "engine APIs and exported module types; compare all codes/files/ranges and require clean positive controls");
		}

		TEST_CASE("ScriptTypeChecker: bounds APIs generate optional multiple returns accepted by the New solver" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Generate and load real definitions for Entity:GetWorldBounds, Physics.GetBodyBounds and GetColliderBounds "
				 "with (vector?, vector?), never (vector, vector)?; check two separately refined locals as clean and both "
				 "unrefined vector uses as located errors. Preserve the runtime contract that both values exist or both are nil");
		}

		TEST_CASE("ScriptTypeChecker: generated definitions are copied from a frozen registry" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			ScriptApiRegistry unfrozen;
			const auto rejected = ScriptTypeChecker::Create(unfrozen);
			REQUIRE_FALSE(rejected);
			CHECK(rejected.error().GetCode() == ErrorCode::InvalidState);
			FAIL("Also destroy a frozen source registry/TypeRegistry after Create and check successfully; parse actual "
				 "GenerateDefinitions output with SolverMode::New, reject invalid definitions and expose no Luau header types");
		}

		TEST_CASE("ScriptTypeChecker: configuration snapshots distinguish missing present and inherited configs" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Capture root and nested .luaurc through VFS including absent files; check strict default, source directives "
				 "and inheritance; malformed user configs produce located errors; content/add/remove/directory changes alter "
				 "the environment hash while unchanged snapshots do not; configs cannot widen the Assets require sandbox");
		}

		TEST_CASE("ScriptTypeChecker: unsaved source and required modules use the supplied reader only" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Root text overrides disk and every dependency read uses IScriptModuleReader/RequireResolver; errors in a "
				 "required module name that file, missing modules and cycles are located findings, every graph edge invalidates "
				 "cached checks, and no request view/reader/VFS callback is retained after return");
		}

		TEST_CASE("ScriptTypeChecker: simultaneous checks are deterministic and failures never appear clean" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Overlap independent requests/readers with latches after main-thread flag initialization; compare sorted "
				 "findings and environment hashes to serial checks; malformed request and analysis internal error/timeout "
				 "yield Error findings at line one without leaking state into subsequent valid checks");
		}
	}

}
