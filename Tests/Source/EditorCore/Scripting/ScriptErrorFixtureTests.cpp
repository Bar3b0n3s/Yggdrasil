#include "TestsPCH.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		struct ScriptErrorExpectation
		{
			std::string File{};
			std::string Kind{};
			std::string Code{};
			uint32_t Line = 0;
			uint32_t Column = 0;
			std::string Message{};
		};

		std::vector<ScriptErrorExpectation> ReadScriptErrorExpectations()
		{
			const auto text = Test::ReadTestDataText("Scripts/Errors/Expected.json");
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const auto document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			const JsonReader root(*document);
			const auto count = root.GetArraySize();
			REQUIRE(count.has_value());
			std::vector<ScriptErrorExpectation> expectations;
			for (size_t index = 0; index < *count; ++index)
			{
				const auto entry = root.GetElement(index);
				REQUIRE(entry.has_value());
				ScriptErrorExpectation expectation;
				const auto file = entry->ReadMember<std::string>("File");
				const auto kind = entry->ReadMember<std::string>("Kind");
				const auto code = entry->ReadMember<std::string>("Code");
				const auto line = entry->ReadMember<uint32_t>("Line");
				const auto column = entry->ReadMember<uint32_t>("Column");
				const auto message = entry->ReadMember<std::string>("Message");
				REQUIRE((file && kind && code && line && column && message));
				expectations.push_back(ScriptErrorExpectation{ *file, *kind, *code, *line, *column, *message });
			}
			return expectations;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Architecture §15.5 "Errors.scene": compile and type error kinds are covered by unit fixtures, not by the project
		// (ADR 0022 decision 15). Tests/Data/Scripts/Errors/*.luau and Expected.json are written by MakeScriptFixtures.py;
		// each entry pins {File, Kind (compile|type), Code, Line, Column, Message (a substring)}. Each fixture is written into a
		// project through EditorScriptService::Write, which runs the importer's compiler, load-time VM and type checker.
		TEST_CASE("ScriptErrors: each Tests/Data/Scripts/Errors fixture reports its kind, code and location")
		{
			const auto sources = Test::ListTestDataFiles("Scripts/Errors", { ".luau" });
			REQUIRE_MESSAGE(sources.has_value(), sources.error().ToString());
			REQUIRE_FALSE(sources->empty());
			const std::vector<ScriptErrorExpectation> expectations = ReadScriptErrorExpectations();
			std::set<std::string> listed;
			for (const ScriptErrorExpectation& expectation : expectations)
				listed.insert("Scripts/Errors/" + expectation.File);
			CHECK(listed == std::set<std::string>(sources->begin(), sources->end()));

			Test::EditorTestFixture fixture("ScriptErrorFixtures");
			fixture.CreateAndOpenProject();
			EditorScriptService* service = fixture.GetEditor().GetScriptService();
			REQUIRE(service != nullptr);
			std::set<std::string> kinds;
			for (const ScriptErrorExpectation& expectation : expectations)
			{
				CAPTURE(expectation.File);
				CHECK(((expectation.Kind == "compile" && expectation.Code == "SCRIPT_COMPILE_ERROR")
					|| (expectation.Kind == "type" && expectation.Code == "SCRIPT_TYPE_ERROR")));
				kinds.insert(expectation.Kind);
				const auto source = Test::ReadTestDataText("Scripts/Errors/" + expectation.File);
				REQUIRE_MESSAGE(source.has_value(), source.error().ToString());
				const std::string path = "Assets/Scripts/Errors/" + expectation.File;
				const auto file = VfsPath::Create("project", path);
				REQUIRE(file.has_value());
				// Invalid source is saved for iteration: its findings are part of a successful write result.
				const auto written = service->Write(*file, *source);
				REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
				std::string actual;
				for (const ScriptDiagnostic& diagnostic : written->Diagnostics)
					actual += std::format("{} {}:{}:{} {}\n", diagnostic.Code, diagnostic.File, diagnostic.Line, diagnostic.Column, diagnostic.Message);
				INFO(actual);
				const auto found = std::ranges::find_if(written->Diagnostics, [&expectation, &path](const ScriptDiagnostic& diagnostic)
				{
					return diagnostic.Code == expectation.Code && diagnostic.File == path && diagnostic.Line == expectation.Line
						&& diagnostic.Column == expectation.Column && diagnostic.Severity == DiagnosticSeverity::Error
						&& diagnostic.Message.find(expectation.Message) != std::string::npos;
				});
				CHECK(found != written->Diagnostics.end());
			}
			CHECK(kinds == std::set<std::string>{ "compile", "type" });
		}
	}

}
