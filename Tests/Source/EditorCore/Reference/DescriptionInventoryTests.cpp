#include "TestsPCH.h"
#include "EditorCore/Reference/DescriptionInventory.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/EditorTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		// Architecture §15.6 gate 7, mode-independent and editor-only (ADR 0022 decision 6): the editor's registries are the
		// superset of every mode's types, script API and automation methods.
		TEST_CASE("Descriptions: every type, field, function, method, property and automation method has a non-empty description" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("DescriptionGate");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = false });
			methods.Freeze();
			const auto entries = CollectDescriptions(fixture.GetEngine().GetTypeRegistry(), fixture.GetEngine().GetScriptApiRegistry(), methods);
			REQUIRE_MESSAGE(entries.has_value(), entries.error().ToString());
			CHECK(std::ranges::is_sorted(*entries, {}, [](const DescriptionEntry& entry)
			{
				return std::tie(entry.Kind, entry.Name);
			}));
			for (const std::string_view kind : { "type", "field", "enum value", "script module", "script member", "method" })
			{
				CAPTURE(kind);
				CHECK(std::ranges::any_of(*entries, [kind](const DescriptionEntry& entry)
				{
					return entry.Kind == kind;
				}));
			}
			CHECK(std::ranges::count(*entries, std::string("method"), &DescriptionEntry::Kind) == static_cast<std::ptrdiff_t>(methods.GetMethods().size()));
			CHECK(FindMissingDescriptions(*entries).empty());
		}

		// Tests/Data/CoverageGaps/Gate7/Descriptions.json is a DescriptionEntry list ({"Kind", "Name", "Description"}) with
		// exactly one empty description; Expected.json's Missing names it.
		TEST_CASE("Descriptions: the gate 7 seeded-gap fixture names exactly its empty description" * doctest::skip(true))
		{
			const auto descriptions = Test::ReadTestDataText("CoverageGaps/Gate7/Descriptions.json");
			const auto expected = Test::ReadTestDataText("CoverageGaps/Gate7/Expected.json");
			REQUIRE_MESSAGE(descriptions.has_value(), descriptions.error().ToString());
			REQUIRE_MESSAGE(expected.has_value(), expected.error().ToString());
			const auto document = JsonReader::Parse(*descriptions);
			REQUIRE(document.has_value());
			const JsonReader reader(*document);
			const auto size = reader.GetArraySize();
			REQUIRE(size.has_value());
			std::vector<DescriptionEntry> entries;
			for (size_t index = 0; index < *size; ++index)
			{
				const auto element = reader.GetElement(index);
				REQUIRE(element.has_value());
				const auto kind = element->ReadMember<std::string>("Kind");
				const auto name = element->ReadMember<std::string>("Name");
				const auto description = element->ReadMember<std::string>("Description");
				REQUIRE((kind.has_value() && name.has_value() && description.has_value()));
				entries.push_back(DescriptionEntry{ .Kind = *kind, .Name = *name, .Description = *description });
			}
			const std::vector<std::string> missing = FindMissingDescriptions(entries);
			REQUIRE(missing.size() == 1);
			const auto expectedDocument = JsonReader::Parse(*expected);
			REQUIRE(expectedDocument.has_value());
			const auto names = JsonReader(*expectedDocument).GetMember("Missing");
			REQUIRE(names.has_value());
			const auto first = names->GetElement(0);
			REQUIRE(first.has_value());
			CHECK(first->ReadString() == missing.front());
		}

		TEST_CASE("Descriptions: an empty or whitespace-only description is named by gate 7" * doctest::skip(true))
		{
			const std::vector<DescriptionEntry> entries = {
				DescriptionEntry{ .Kind = "field", .Name = "Camera.Projection", .Description = "Perspective or orthographic projection." },
				DescriptionEntry{ .Kind = "method", .Name = "scene.tree", .Description = " \t" },
				DescriptionEntry{ .Kind = "type", .Name = "Camera", .Description = "" },
			};
			CHECK(FindMissingDescriptions(entries) == std::vector<std::string>{ "method scene.tree", "type Camera" });
		}
	}

}
