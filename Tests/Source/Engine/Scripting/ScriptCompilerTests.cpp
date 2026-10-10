#include "TestsPCH.h"
#include "Engine/Scripting/ScriptCompiler.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Scripting/Sandbox.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string_view>
#include <vector>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptCompiler: identical input preserves bytecode and exact source coordinates" * doctest::skip())
		{
			auto path = VfsPath::Parse("project://Assets/Scripts/Compile.luau");
			REQUIRE(path.has_value());
			constexpr std::string_view Source = "local answer = 42\nreturn answer\n";
			const auto first = ScriptCompiler::Compile({ .Path = *path, .Source = Source });
			const auto second = ScriptCompiler::Compile({ .Path = *path, .Source = Source });
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK_FALSE(first->Bytecode.empty());
			CHECK(first->Bytecode == second->Bytecode);
			CHECK(first->SourceMap.Path == "Assets/Scripts/Compile.luau");
			CHECK(first->SourceMap.ChunkName == "@Assets/Scripts/Compile.luau");
			CHECK(first->SourceMap.SourceByteCount == Source.size());
			CHECK(first->SourceMap.LineOffsets == std::vector<uint32_t>{ 0, 18, 32 });
			CHECK(first->SourceMap.GeneratedPrefixLines == 0);
			CHECK(first->SourceMap.SourceHash == second->SourceMap.SourceHash);
			CHECK(first->SourceMap.SourceHash == XXH64(Source));
		}

		TEST_CASE("ScriptCompiler: malformed source is a located failure rather than loadable error bytecode" * doctest::skip())
		{
			auto path = VfsPath::Parse("project://Assets/Syntax.luau");
			REQUIRE(path.has_value());
			const auto result = ScriptCompiler::Compile({ .Path = *path, .Source = "local ok = true\nlocal broken = )" });
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::CompileFailed);
			CHECK(result.error().GetLocation().File == "Assets/Syntax.luau");
			CHECK(result.error().GetLocation().Line == 2);
			CHECK_FALSE(result.error().GetMessageText().empty());
		}

		TEST_CASE("ScriptCompiler: fileless evaluation keeps synthetic labels separate from diagnostic origins" * doctest::skip())
		{
			for (const std::string_view label : { "=eval", "=waitFor" })
			{
				CAPTURE(label);
				const auto compiled = ScriptCompiler::Compile({ .Source = "return true", .ChunkName = label, .JsonPointer = "/code" });
				REQUIRE(compiled.has_value());
				CHECK(compiled->SourceMap.Path.empty());
				CHECK(compiled->SourceMap.ChunkName == label);
				CHECK(compiled->SourceMap.JsonPointer == "/code");
				CHECK(compiled->SourceMap.SourceByteCount == 11);
				CHECK(compiled->SourceMap.LineOffsets == std::vector<uint32_t>{ 0 });
				CHECK(compiled->SourceMap.GeneratedPrefixLines == 0);
			}
		}

		TEST_CASE("ScriptCompiler: embedded compile errors retain document pointer and chunk-relative coordinates" * doctest::skip())
		{
			auto path = VfsPath::Parse("project://Assets/Tests/X.replay");
			REQUIRE(path.has_value());
			const auto compiled = ScriptCompiler::Compile({ .Path = *path, .Source = "local ok = true\nreturn )", .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=replay/0000000000000001/Expect/3", .JsonPointer = "/Expect/3/Luau" });
			REQUIRE_FALSE(compiled.has_value());
			CHECK(compiled.error().GetCode() == ErrorCode::CompileFailed);
			CHECK(compiled.error().GetLocation().File == "Assets/Tests/X.replay");
			CHECK(compiled.error().GetLocation().JsonPointer == "/Expect/3/Luau");
			CHECK(compiled.error().GetLocation().Line == 2);
			CHECK(compiled.error().GetLocation().Column == 8);
		}

		TEST_CASE("ScriptCompiler: replay diagnostics map wrapped expressions and return chunks to authored lines" * doctest::skip())
		{
			auto path = VfsPath::Parse("project://Assets/Tests/X.replay");
			REQUIRE(path.has_value());
			Random random(61);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			for (const std::string_view source : { "return error('expectation failed')", "error('expectation failed')" })
			{
				CAPTURE(source);
				const auto compiled = ScriptCompiler::Compile({ .Path = *path, .Source = source, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=replay/0000000000000001/Expect/3", .JsonPointer = "/Expect/3/Luau" });
				REQUIRE(compiled.has_value());
				CHECK(compiled->SourceMap.ChunkName == "=replay/0000000000000001/Expect/3");
				CHECK(compiled->SourceMap.Path == "Assets/Tests/X.replay");
				CHECK(compiled->SourceMap.JsonPointer == "/Expect/3/Luau");
				CHECK(compiled->SourceMap.SourceByteCount == source.size());
				CHECK(compiled->SourceMap.SourceHash == XXH64(source));
				CHECK(compiled->SourceMap.LineOffsets == std::vector<uint32_t>{ 0 });
				CHECK(compiled->SourceMap.GeneratedPrefixLines == (source.starts_with("return") ? 0u : 1u));
				ScriptData script;
				script.Bytecode = compiled->Bytecode;
				script.SourceMap = compiled->SourceMap;
				CHECK_FALSE((*sandbox)->ExecuteBytecode(script).has_value());
				const auto error = (*sandbox)->GetLastError();
				REQUIRE(error.has_value());
				CHECK(error->Script == "Assets/Tests/X.replay");
				CHECK(error->Line == 1);
				CHECK(error->Message.find("/Expect/3/Luau") != std::string::npos);
			}
		}

		TEST_CASE("ScriptCompiler: expression-first parsing requires complete input and preserves authored metadata" * doctest::skip())
		{
			Random random(67);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			for (const std::string_view source : { "math.abs(-7)", "return 7", "print('once'); return 7", "(\n7\n) -- tail comment" })
			{
				CAPTURE(source);
				const auto compiled = ScriptCompiler::Compile({ .Source = source, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=eval" });
				REQUIRE(compiled.has_value());
				CHECK(compiled->SourceMap.SourceHash == XXH64(source));
				CHECK(compiled->SourceMap.SourceByteCount == source.size());
				CHECK(compiled->SourceMap.GeneratedPrefixLines == (source.starts_with("return") || source.starts_with("print") ? 0u : 1u));
				if (source.starts_with("("))
					CHECK(compiled->SourceMap.LineOffsets == std::vector<uint32_t>{ 0, 2, 4 });
				ScriptData script;
				script.Bytecode = compiled->Bytecode;
				script.SourceMap = compiled->SourceMap;
				const auto evaluated = (*sandbox)->ExecuteBytecode(script);
				REQUIRE(evaluated.has_value());
				CHECK(evaluated->Value.Get() == Json(7));
				CHECK(evaluated->Prints.size() == (source.starts_with("print") ? 1u : 0u));
			}
			CHECK_FALSE(ScriptCompiler::Compile({ .Source = "1 + 2; return 4", .Mode = ScriptCompileMode::ExpressionOrChunk }).has_value());
			CHECK_FALSE(ScriptCompiler::Compile({ .Source = "return true", .Mode = ScriptCompileMode::Expression }).has_value());
			const auto expression = ScriptCompiler::Compile({ .Source = "7", .Mode = ScriptCompileMode::Expression });
			REQUIRE(expression.has_value());
			CHECK(expression->SourceMap.GeneratedPrefixLines == 1);
		}

		TEST_CASE("ScriptCompiler: compiled and source execution share the sandbox math policy" * doctest::skip())
		{
			auto path = VfsPath::Parse("project://Assets/Alias.luau");
			REQUIRE(path.has_value());
			constexpr std::string_view Source = "local m = math; local f = m.sin; return { math.sin(0.731), m.sin(0.731), f(0.731) }";
			const auto compiled = ScriptCompiler::Compile({ .Path = *path, .Source = Source });
			REQUIRE(compiled.has_value());
			ScriptData script;
			script.Bytecode = compiled->Bytecode;
			script.SourceMap = compiled->SourceMap;
			Random random(43);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			const auto source = (*sandbox)->Evaluate(Source, *path);
			const auto bytecode = (*sandbox)->ExecuteBytecode(script);
			REQUIRE(source.has_value());
			REQUIRE(bytecode.has_value());
			CHECK(source->Value == bytecode->Value);
		}

		TEST_CASE("ScriptCompiler: nontrivial power exponents retain one expectation across configurations" * doctest::skip())
		{
			auto path = VfsPath::Parse("project://Assets/Powers.luau");
			REQUIRE(path.has_value());
			constexpr std::string_view Source =
				"local base = tonumber('2'); local exponent = tonumber('1.5'); "
				"return { base ^ exponent, 2 ^ 1.5, 16 ^ 1.25, 16 ^ 0.25 }";
			const auto compiled = ScriptCompiler::Compile({ .Path = *path, .Source = Source });
			REQUIRE(compiled.has_value());
			ScriptData script;
			script.Bytecode = compiled->Bytecode;
			script.SourceMap = compiled->SourceMap;
			Random random(47);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			const auto result = (*sandbox)->ExecuteBytecode(script);
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json::array({ 2.8284271247461903, 2.8284271247461903, 32.0, 2.0 }));
			// Keep this same expectation in Debug/Release and exported Dist FeatureTest. The determinism owner also
			// records/verifies a seeded nontrivial-power corpus across configs; no per-config constants or ^ rewriting.
		}
	}

}
