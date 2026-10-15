#include "TestsPCH.h"

#include "EditorCore/Automation/ObserveMethods.h"

#include "Engine/App/EngineContext.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/AutomationTestClient.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		void WriteDocsFile(const std::filesystem::path& root, std::string_view path, std::string_view text)
		{
			const std::filesystem::path file = root / FileSystem::PathFromUtf8(path);
			REQUIRE(FileSystem::CreateDirectories(file.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false }).has_value());
		}

		// A docs tree with one topic of every kind.
		constexpr std::string_view GuideText = "# Guide\n"
											   "Intro.\n"
											   "## Input\n"
											   "Input text.\n"
											   "### Input.GetAxis\n"
											   "Axis text.\n"
											   "```bash\n"
											   "# not a heading\n"
											   "```\n"
											   "## Input\n"
											   "The second Input.\n"
											   "## Output ##\n"
											   "Out.\n";

		constexpr std::string_view DeclarationsText = "-- Generated.\n"
													  "export type Mode = \"A\" | \"B\"\n"
													  "\n"
													  "-- A colour.\n"
													  "declare extern type Color with\n"
													  "\tR: number\n"
													  "end\n"
													  "\n"
													  "declare Color: {\n"
													  "\tnew: (r: number) -> Color,\n"
													  "}\n"
													  "\n"
													  "-- Reads input.\n"
													  "declare Input: {\n"
													  "\tGetAxis: (name: string) -> number,\n"
													  "}\n";

		std::filesystem::path MakeDocsTree(const std::filesystem::path& root)
		{
			WriteDocsFile(root, ".claude/skills/sample/SKILL.md", "---\nname: sample\ndescription: A sample skill.\n---\n# Sample\n");
			WriteDocsFile(root, "Docs/Reference/Guide.md", GuideText);
			WriteDocsFile(root, "Docs/Reference/Schemas/Thing.schema.json", "{\n\t\"title\": \"Thing\",\n\t\"description\": \"A thing file.\"\n}\n");
			WriteDocsFile(root, "Resources/Scripting/Engine.d.luau", DeclarationsText);
			WriteDocsFile(root, "Resources/Templates/Scripts/Behaviour.luau", "local {{Name}} = {}\nreturn {{Name}}\n");
			return root;
		}

		AutomationServerSpecification MakeDocsSpecification(const std::filesystem::path& root)
		{
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.DocsRoot = root;
			return specification;
		}

		Json CallDocs(Test::AutomationTestClient& client, const Json& params)
		{
			Result<Json> result = client.Call("docs.get", params);
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			REQUIRE_FALSE(result->contains("truncated"));
			return std::move(*result);
		}

		std::string ReadContent(const Json& result)
		{
			return JsonReader(result).ReadMember<std::string>("content").value_or(std::string());
		}

		// The whole text of `topic`, page by page as an agent reads it.
		std::string ReadWholeTopic(Test::AutomationTestClient& client, std::string_view topic)
		{
			std::string text;
			uint32_t offset = 0;
			size_t pages = 0;
			do
			{
				const Json page = offset == 0 ? CallDocs(client, Json{ { "topic", topic } }) : CallDocs(client, Json{ { "topic", topic }, { "offset", offset } });
				text += ReadContent(page);
				offset = JsonReader(page).ReadMember<uint32_t>("nextOffset").value_or(0);
				++pages;
			} while (offset != 0 && pages < 1000);
			return text;
		}

		// The words of `text` that name a path: the longest runs of letters, digits and "_.-/<>" that hold a '/', without a
		// sentence's closing periods.
		std::vector<std::string> FindPathWords(std::string_view text)
		{
			const auto isPathCharacter = [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
					|| character == '_' || character == '.' || character == '-' || character == '/' || character == '<' || character == '>';
			};
			std::vector<std::string> words;
			size_t index = 0;
			while (index < text.size())
			{
				if (!isPathCharacter(text[index]))
				{
					++index;
					continue;
				}
				const size_t begin = index;
				while (index < text.size() && isPathCharacter(text[index]))
					++index;
				std::string_view word = text.substr(begin, index - begin);
				while (word.ends_with('.'))
					word.remove_suffix(1);
				if (word.contains('/'))
					words.emplace_back(word);
			}
			return words;
		}

		// The lines of `text`, without their line breaks.
		std::vector<std::string> SplitDocumentLines(std::string_view text)
		{
			std::vector<std::string> lines;
			size_t start = 0;
			while (start < text.size())
			{
				const size_t end = text.find('\n', start);
				lines.emplace_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
			return lines;
		}

		// A heading's anchor by the rule of ObserveMethods.h, before a repeat is numbered.
		std::string ToAnchor(std::string_view text)
		{
			std::string anchor;
			for (const char character : text)
			{
				if (character >= 'A' && character <= 'Z')
					anchor.push_back(static_cast<char>(character - 'A' + 'a'));
				else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '-' || character == '_'
					|| static_cast<unsigned char>(character) >= 0x80)
					anchor.push_back(character);
				else if (character == ' ')
					anchor.push_back('-');
			}
			return anchor;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ObserveMethods: docs.get lists the skills and returns a topic")
		{
			Test::EditorTestFixture fixture("DocsGet");
			Test::AutomationTestClient client(fixture.GetEditor());
			Result<Json> topics = client.Call("docs.get", Json::object());
			REQUIRE(topics.has_value());
			CHECK((*topics)["topics"].dump().contains("skills/build-and-test"));
			CHECK((*topics)["topics"].dump().contains("skills/add-automation-method"));
			Result<Json> skill = client.Call("docs.get", Json{ { "topic", "skills/add-automation-method" } });
			REQUIRE(skill.has_value());
			CHECK((*skill)["content"].dump().contains("RegisterMethods.cpp"));
			CHECK((*skill)["path"] == Json(".claude/skills/add-automation-method/SKILL.md"));
			CHECK(client.Call("docs.get", Json{ { "topic", "skills/missing" } }).error().GetCode() == ErrorCode::NotFound);
		}

		// Docs/Decisions/0026-m16-contract.md, friction S1-01: a heading's subtree is one section, addressed by its anchor, by
		// its text or as "<topic>#<section>"; headings inside code fences do not count and repeated anchors are numbered.
		TEST_CASE("ObserveMethods: docs.get serves a Markdown topic's sections by anchor or heading text")
		{
			Test::EditorTestFixture fixture("DocsSections");
			Test::AutomationTestClient client(fixture.GetEditor(), MakeDocsSpecification(MakeDocsTree(fixture.GetDirectory() / "Docs")));

			const Json input = CallDocs(client, Json{ { "topic", "reference/Guide" }, { "section", "input" } });
			CHECK(input["topic"] == Json("reference/Guide"));
			CHECK(input["section"] == Json("input"));
			CHECK(ReadContent(input) == "## Input\nInput text.\n### Input.GetAxis\nAxis text.\n```bash\n# not a heading\n```\n");
			CHECK(input["totalBytes"] == Json(ReadContent(input).size()));
			CHECK(input["nextOffset"] == Json(0));

			const Json axis = CallDocs(client, Json{ { "topic", "reference/Guide#inputgetaxis" } });
			CHECK(axis["section"] == Json("inputgetaxis"));
			CHECK(ReadContent(axis) == "### Input.GetAxis\nAxis text.\n```bash\n# not a heading\n```\n");
			CHECK(ReadContent(CallDocs(client, Json{ { "topic", "reference/Guide" }, { "section", "Input.GetAxis" } })) == ReadContent(axis));
			CHECK(ReadContent(CallDocs(client, Json{ { "topic", "reference/Guide#input-1" } })) == "## Input\nThe second Input.\n");
			// A closing '#' sequence is not part of the heading's text.
			CHECK(ReadContent(CallDocs(client, Json{ { "topic", "reference/Guide" }, { "section", "output" } })) == "## Output ##\nOut.\n");
			// The top heading holds the whole document.
			CHECK(ReadContent(CallDocs(client, Json{ { "topic", "reference/Guide#guide" } })) == GuideText);

			// The text of two headings names neither (friction S2-05): the error lists their anchors.
			const Json repeated = client.Request("docs.get", Json{ { "topic", "reference/Guide" }, { "section", "Input" } });
			CHECK(repeated["error"]["data"]["errorCode"] == Json("InvalidArgument"));
			CHECK(repeated["error"]["data"]["hint"].dump().contains("'input', 'input-1'"));

			const Json missing = client.Request("docs.get", Json{ { "topic", "reference/Guide" }, { "section", "Inptu" } });
			CHECK(missing["error"]["data"]["errorCode"] == Json("NotFound"));
			CHECK(missing["error"]["data"]["hint"].dump().contains("'input'"));
			const Result<Json> conflicting = client.Call("docs.get", Json{ { "topic", "reference/Guide#output" }, { "section", "input" } });
			REQUIRE_FALSE(conflicting.has_value());
			CHECK(conflicting.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Json> plain = client.Call("docs.get", Json{ { "topic", "reference/Schemas/Thing" }, { "section", "title" } });
			REQUIRE_FALSE(plain.has_value());
			CHECK(plain.error().GetCode() == ErrorCode::NotFound);
			CHECK(client.Call("docs.get", Json{ { "section", "input" } }).error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Docs/Decisions/0026-m16-contract.md, friction S2-05: anchors drop '.', so "TestSuite" (the type) and "Test.Suite"
		// (the function) shared the anchor "testsuite", and a section asked for by text resolved through the anchor to the
		// function. The exact heading text now comes first, then the text without a kind qualifier, then the anchor.
		TEST_CASE("ObserveMethods: docs.get resolves a section by its exact heading text before its anchor")
		{
			Test::EditorTestFixture fixture("DocsHeadingText");
			const std::filesystem::path root = MakeDocsTree(fixture.GetDirectory() / "Docs");
			WriteDocsFile(root, "Docs/Reference/Api.md",
				"# Api\n"
				"## Test\n"
				"The Test global.\n"
				"### Test.Suite\n"
				"The function.\n"
				"## Script\n"
				"The Script global.\n"
				"## Script (component)\n"
				"The component.\n"
				"### Script.Fields\n"
				"Its fields.\n"
				"## TestSuite (type)\n"
				"The type.\n"
				"## Value (enum)\n"
				"An enum.\n"
				"## Value (type)\n"
				"A type.\n");
			Test::AutomationTestClient client(fixture.GetEditor(), MakeDocsSpecification(root));
			const auto read = [&client](std::string_view section)
			{
				return CallDocs(client, Json{ { "topic", "reference/Api" }, { "section", section } });
			};

			CHECK(ReadContent(read("Test.Suite")) == "### Test.Suite\nThe function.\n");
			CHECK(read("Test.Suite")["section"] == Json("testsuite"));
			// The type, by its name without its qualifier: its anchor is not "testsuite", which a lowercase name still means.
			const Json type = read("TestSuite");
			CHECK(ReadContent(type) == "## TestSuite (type)\nThe type.\n");
			CHECK(type["section"] == Json("testsuite-type"));
			CHECK(ReadContent(read("TestSuite (type)")) == ReadContent(type));
			CHECK(ReadContent(read("testsuite")) == "### Test.Suite\nThe function.\n");
			// The exact text wins over a qualified heading of the same name.
			CHECK(ReadContent(read("Script")) == "## Script\nThe Script global.\n");
			CHECK(ReadContent(read("Script (component)")) == "## Script (component)\nThe component.\n### Script.Fields\nIts fields.\n");
			CHECK(ReadContent(read("script-component")) == ReadContent(read("Script (component)")));

			// A name two qualified headings share names neither: the error lists their anchors.
			const Json shared = client.Request("docs.get", Json{ { "topic", "reference/Api" }, { "section", "Value" } });
			CHECK(shared["error"]["data"]["errorCode"] == Json("InvalidArgument"));
			CHECK(shared["error"]["data"]["hint"].dump().contains("'value-enum', 'value-type'"));
			CHECK(ReadContent(read("Value (type)")) == "## Value (type)\nA type.\n");

			// "<topic>#<section>" and section must name the same section, however each spells it.
			CHECK(ReadContent(CallDocs(client, Json{ { "topic", "reference/Api#TestSuite" }, { "section", "testsuite-type" } })) == ReadContent(type));
			const Result<Json> different = client.Call("docs.get", Json{ { "topic", "reference/Api#Test.Suite" }, { "section", "TestSuite" } });
			REQUIRE_FALSE(different.has_value());
			CHECK(different.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Friction S2-05: the section list came only with a full first page (32 KiB of text); toc returns just the list.
		TEST_CASE("ObserveMethods: docs.get with toc returns the section list with sizes and no text")
		{
			Test::EditorTestFixture fixture("DocsToc");
			const std::filesystem::path root = MakeDocsTree(fixture.GetDirectory() / "Docs");
			// A long text: 12 groups of 200 entries, whose full list is over DocsTocBytes.
			std::string longText;
			for (int group = 0; group < 12; ++group)
			{
				longText += std::format("## Group {}\n", group);
				for (int entry = 0; entry < 200; ++entry)
					longText += std::format("### Group{}.Entry{}\nText.\n", group, entry);
			}
			WriteDocsFile(root, "Docs/Reference/Long.md", longText);
			Test::AutomationTestClient client(fixture.GetEditor(), MakeDocsSpecification(root));

			const Json toc = CallDocs(client, Json{ { "topic", "reference/Guide" }, { "toc", true } });
			CHECK(ReadContent(toc).empty());
			CHECK(toc["totalBytes"] == Json(GuideText.size()));
			CHECK(toc["nextOffset"] == Json(0));
			std::vector<std::tuple<std::string, std::string, uint32_t>> entries;
			for (const Json& section : toc["sections"])
			{
				const JsonReader reader(section);
				entries.emplace_back(reader.ReadMember<std::string>("title").value_or(""), reader.ReadMember<std::string>("anchor").value_or(""),
					reader.ReadMember<uint32_t>("level").value_or(0));
				const uint32_t offset = reader.ReadMember<uint32_t>("offset").value_or(0);
				const uint32_t bytes = reader.ReadMember<uint32_t>("bytes").value_or(0);
				CHECK(offset + bytes <= GuideText.size());
				// The offset and size are exactly the section's text, as the section call returns it.
				CHECK(ReadContent(CallDocs(client, Json{ { "topic", "reference/Guide" }, { "section", std::get<1>(entries.back()) } }))
					== GuideText.substr(offset, bytes));
			}
			CHECK(entries == std::vector<std::tuple<std::string, std::string, uint32_t>>{ { "Guide", "guide", 1 }, { "Input", "input", 2 }, { "Input.GetAxis", "inputgetaxis", 3 }, { "Input", "input-1", 2 }, { "Output", "output", 2 } });

			// A section's toc lists its subsections, offsets counted from its own start.
			const Json input = CallDocs(client, Json{ { "topic", "reference/Guide" }, { "section", "input" }, { "toc", true } });
			CHECK(input["section"] == Json("input"));
			REQUIRE(input["sections"].size() == 1);
			CHECK(input["sections"][0]["title"] == Json("Input.GetAxis"));
			CHECK(input["sections"][0]["offset"] == Json(std::string_view("## Input\nInput text.\n").size()));
			CHECK(CallDocs(client, Json{ { "topic", "scripting/Engine.d.luau" }, { "toc", true } })["sections"].size() == 2);
			const Json plain = CallDocs(client, Json{ { "topic", "reference/Schemas/Thing" }, { "toc", true } });
			CHECK(plain["sections"].empty());
			CHECK(ReadContent(plain).empty());

			// Over DocsTocBytes, the deepest level whose list fits: the 12 groups, inline.
			const Json longToc = CallDocs(client, Json{ { "topic", "reference/Long" }, { "toc", true } });
			REQUIRE(longToc["sections"].size() == 12);
			CHECK(longToc["sections"][11]["title"] == Json("Group 11"));
			CHECK(longToc.dump().size() < DefaultOffloadThresholdBytes);
			CHECK(CallDocs(client, Json{ { "topic", "reference/Long" }, { "section", "Group 3" }, { "toc", true } })["sections"].size() == 200);

			for (const Json& params : { Json{ { "topic", "reference/Guide" }, { "toc", true }, { "offset", 0 } },
					 Json{ { "topic", "reference/Guide" }, { "toc", true }, { "length", 10 } }, Json{ { "toc", true } } })
			{
				CAPTURE(params.dump());
				const Result<Json> refused = client.Call("docs.get", params);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		// Friction S2-05: every heading of the generated script API reference is unique and reachable by its text, and none
		// needed a numbered anchor; the colliding names the triage listed resolve to the section they name.
		TEST_CASE("ObserveMethods: docs.get reaches every section of the generated script API reference by its heading text")
		{
			Test::EditorTestFixture fixture("DocsScriptApi");
			const Result<std::string> documentation = fixture.GetEngine().GetScriptApiRegistry().GenerateDocumentation();
			REQUIRE_MESSAGE(documentation.has_value(), documentation.error().ToString());
			const std::filesystem::path root = MakeDocsTree(fixture.GetDirectory() / "Docs");
			WriteDocsFile(root, "Docs/Reference/ScriptAPI.md", *documentation);
			Test::AutomationTestClient client(fixture.GetEditor(), MakeDocsSpecification(root));

			// The headings, as docs.get finds them (outside code fences).
			std::vector<std::pair<std::string, std::string>> headings; // the line, the text
			bool fenced = false;
			for (const std::string& line : SplitDocumentLines(*documentation))
			{
				if (line.starts_with("```"))
					fenced = !fenced;
				else if (!fenced && line.starts_with('#'))
					headings.emplace_back(line, line.substr(line.find(' ') + 1));
			}
			REQUIRE(headings.size() > 100);
			std::set<std::string> texts;
			std::set<std::string> anchors;
			for (const auto& [line, text] : headings)
			{
				CAPTURE(line);
				CHECK(texts.insert(text).second);
				const Json section = CallDocs(client, Json{ { "topic", "reference/ScriptAPI" }, { "section", text } });
				CHECK(ReadContent(section).starts_with(line + "\n"));
				// Its anchor is its text converted, never numbered.
				const std::string anchor = JsonReader(section).ReadMember<std::string>("section").value_or("");
				CHECK(anchor == ToAnchor(text));
				CHECK(anchors.insert(anchor).second);
			}

			for (const auto& [name, line] : std::vector<std::pair<std::string_view, std::string_view>>{ { "TestSuite", "## TestSuite (type)\n" },
					 { "Test.Suite", "### Test.Suite\n" }, { "TextAlignment", "## TextAlignment (enum)\n" }, { "Text.Alignment", "### Text.Alignment\n" },
					 { "TextSpace", "## TextSpace (enum)\n" }, { "Text.Space", "### Text.Space\n" }, { "Script", "## Script\n" },
					 { "Script (component)", "## Script (component)\n" }, { "Input", "## Input\n" } })
			{
				CAPTURE(name);
				CHECK(ReadContent(CallDocs(client, Json{ { "topic", "reference/ScriptAPI" }, { "section", name } })).starts_with(line));
			}
			const Json toc = CallDocs(client, Json{ { "topic", "reference/ScriptAPI" }, { "toc", true } });
			CHECK_FALSE(toc["sections"].empty());
			CHECK(ReadContent(toc).empty());
		}

		// M16 review R25: the generated declarations declare the Script component's extern type and, further on, the Script
		// global, so the topic had two sections titled "Script" and docs.get refused the name as ambiguous. A name declared
		// again titles its extern type's section "<name> (type)", and every declaration is reachable by its title.
		TEST_CASE("ObserveMethods: docs.get reaches every section of the generated script declarations by its title")
		{
			Test::EditorTestFixture fixture("DocsDeclarations");
			const Result<std::string> declarations = fixture.GetEngine().GetScriptApiRegistry().GenerateDefinitions();
			REQUIRE_MESSAGE(declarations.has_value(), declarations.error().ToString());
			const std::filesystem::path root = MakeDocsTree(fixture.GetDirectory() / "Docs");
			WriteDocsFile(root, "Resources/Scripting/Engine.d.luau", *declarations);
			Test::AutomationTestClient client(fixture.GetEditor(), MakeDocsSpecification(root));
			constexpr std::string_view Topic = "scripting/Engine.d.luau";

			const Json toc = CallDocs(client, Json{ { "topic", Topic }, { "toc", true } });
			std::set<std::string> titles;
			std::set<std::string> anchors;
			for (const Json& entry : toc["sections"])
			{
				const std::string title = JsonReader(entry).ReadMember<std::string>("title").value_or(std::string());
				const std::string anchor = JsonReader(entry).ReadMember<std::string>("anchor").value_or(std::string());
				CAPTURE(title);
				CHECK(titles.insert(title).second);
				CHECK(anchors.insert(anchor).second);
				CHECK(anchor == ToAnchor(title)); // never numbered
				const Json section = CallDocs(client, Json{ { "topic", Topic }, { "section", title } });
				CHECK(JsonReader(section).ReadMember<std::string>("section").value_or(std::string()) == anchor);
			}
			CHECK(titles.size() > 30);

			const std::string component = ReadContent(CallDocs(client, Json{ { "topic", Topic }, { "section", "Script (type)" } }));
			CHECK(component.contains("declare extern type Script with"));
			CHECK_FALSE(component.contains("declare Script: {"));
			const std::string global = ReadContent(CallDocs(client, Json{ { "topic", Topic }, { "section", "Script" } }));
			CHECK(global.contains("declare Script: {"));
			CHECK_FALSE(global.contains("declare extern type Script"));
			// Adjacent declarations of one name stay one section, under the plain name.
			const std::string color = ReadContent(CallDocs(client, Json{ { "topic", Topic }, { "section", "Color" } }));
			CHECK(color.contains("declare extern type Color with"));
			CHECK(color.contains("declare Color: {"));
		}

		TEST_CASE("ObserveMethods: docs.get pages a text without gaps or overlap and ends pages at line breaks")
		{
			Test::EditorTestFixture fixture("DocsPages");
			const std::filesystem::path root = MakeDocsTree(fixture.GetDirectory() / "Docs");
			// Lines of 2-byte characters, so a page can neither split a line nor a character.
			std::string text;
			for (int line = 0; line < 40; ++line)
				text += std::format("{:02} \xC3\xA9\xC3\xA9\xC3\xA9\n", line);
			WriteDocsFile(root, "Docs/Reference/Long.md", text);
			Test::AutomationTestClient client(fixture.GetEditor(), MakeDocsSpecification(root));

			std::string joined;
			uint32_t offset = 0;
			size_t pages = 0;
			do
			{
				const Json page = CallDocs(client, Json{ { "topic", "reference/Long" }, { "offset", offset }, { "length", 25 } });
				const std::string content = ReadContent(page);
				CHECK(page["offset"] == Json(offset));
				CHECK(page["totalBytes"] == Json(text.size()));
				CHECK(content.size() <= 25);
				CHECK(content.ends_with('\n'));
				joined += content;
				offset = JsonReader(page).ReadMember<uint32_t>("nextOffset").value_or(0);
				++pages;
			} while (offset != 0 && pages < 100);
			CHECK(joined == text);
			CHECK(pages == 20);

			// Shorter than a line: the page ends at a character boundary instead.
			const Json split = CallDocs(client, Json{ { "topic", "reference/Long" }, { "offset", 3 }, { "length", 4 } });
			CHECK(ReadContent(split) == "\xC3\xA9\xC3\xA9");
			CHECK(split["nextOffset"] == Json(7));
			// An offset inside a character starts at the next one.
			CHECK(CallDocs(client, Json{ { "topic", "reference/Long" }, { "offset", 4 }, { "length", 2 } })["offset"] == Json(5));

			const Result<Json> beyond = client.Call("docs.get", Json{ { "topic", "reference/Long" }, { "offset", text.size() + 1 } });
			REQUIRE_FALSE(beyond.has_value());
			CHECK(beyond.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(client.Call("docs.get", Json{ { "topic", "reference/Long" }, { "length", 0 } }).has_value());
			CHECK_FALSE(client.Call("docs.get", Json{ { "topic", "reference/Long" }, { "length", DocsPageBytes + 1 } }).has_value());

			// A section pages like a topic, its offsets counted from its own start.
			const Json section = CallDocs(client, Json{ { "topic", "reference/Guide#input" }, { "offset", 9 }, { "length", 12 } });
			CHECK(ReadContent(section) == "Input text.\n");
			CHECK(section["nextOffset"] == Json(21));
		}

		TEST_CASE("ObserveMethods: docs.get serves the schemas, the script declarations and the script templates")
		{
			Test::EditorTestFixture fixture("DocsTopics");
			Test::AutomationTestClient client(fixture.GetEditor(), MakeDocsSpecification(MakeDocsTree(fixture.GetDirectory() / "Docs")));

			const Json listed = CallDocs(client, Json::object());
			std::vector<std::string> names;
			for (const Json& topic : listed["topics"])
				names.push_back(JsonReader(topic).ReadMember<std::string>("name").value_or(std::string()));
			CHECK(names == std::vector<std::string>{ "reference/Guide", "reference/Schemas/Thing", "scripting/Engine.d.luau", "skills/sample", "templates/Scripts/Behaviour" });
			CHECK(listed["topics"][0]["bytes"] == Json(GuideText.size()));
			CHECK(listed["topics"][1]["title"] == Json("A thing file."));
			CHECK(listed["topics"][3]["title"] == Json("A sample skill."));

			const Json schema = CallDocs(client, Json{ { "topic", "reference/Schemas/Thing" } });
			CHECK(schema["path"] == Json("Docs/Reference/Schemas/Thing.schema.json"));
			CHECK(ReadContent(schema).contains("\"A thing file.\""));
			const Json templateTopic = CallDocs(client, Json{ { "topic", "templates/Scripts/Behaviour" } });
			CHECK(templateTopic["path"] == Json("Resources/Templates/Scripts/Behaviour.luau"));
			CHECK(ReadContent(templateTopic).contains("{{Name}}"));

			// Declarations: each with its comment lines, adjacent declarations of one name together; the type aliases before
			// the first declaration belong to none.
			const Json declarations = CallDocs(client, Json{ { "topic", "scripting/Engine.d.luau" } });
			CHECK(declarations["path"] == Json("Resources/Scripting/Engine.d.luau"));
			CHECK(ReadContent(declarations) == DeclarationsText);
			const Json color = CallDocs(client, Json{ { "topic", "scripting/Engine.d.luau#Color" } });
			CHECK(ReadContent(color) == "-- A colour.\ndeclare extern type Color with\n\tR: number\nend\n\ndeclare Color: {\n\tnew: (r: number) -> Color,\n}\n\n");
			CHECK(ReadContent(CallDocs(client, Json{ { "topic", "scripting/Engine.d.luau" }, { "section", "input" } })) == "-- Reads input.\ndeclare Input: {\n\tGetAxis: (name: string) -> number,\n}\n");
			CHECK(client.Call("docs.get", Json{ { "topic", "scripting/Engine.d.luau#Mode" } }).error().GetCode() == ErrorCode::NotFound);

			// Names that would leave their directory are not topics.
			for (const std::string_view topic : { "scripting/../Engine.d.luau", "reference/Schemas/../Guide", "templates/Scripts/Behaviour.luau", "scripting/Engine" })
				CHECK_MESSAGE(client.Call("docs.get", Json{ { "topic", topic } }).error().GetCode() == ErrorCode::NotFound, topic);
		}

		// Friction S1-01: reference/Automation, reference/ScriptAPI and reference/Components were over the offload threshold
		// and came back as one offloaded line. Every topic of the repository now comes back inline: whole, or as its first page
		// with the next offset and its sections.
		TEST_CASE("ObserveMethods: docs.get returns every repository topic inline and the script API reference completely in pages")
		{
			Test::EditorTestFixture fixture("DocsRepository");
			Test::AutomationTestClient client(fixture.GetEditor());
			const Json listed = CallDocs(client, Json::object());
			for (const std::string_view required : { "reference/Schemas/Scene", "scripting/Engine.d.luau", "templates/Scripts/Behaviour", "reference/ScriptAPI" })
				CHECK_MESSAGE(listed["topics"].dump().contains(std::format("\"{}\"", required)), required);
			for (const Json& topic : listed["topics"])
			{
				const std::string name = JsonReader(topic).ReadMember<std::string>("name").value_or(std::string());
				CAPTURE(name);
				const Json first = CallDocs(client, Json{ { "topic", name } });
				if (first["nextOffset"] != Json(0))
				{
					CHECK(first["offset"] == Json(0));
					if (!name.starts_with("reference/Schemas/"))
						CHECK_FALSE(first["sections"].empty());
				}
			}

			const auto file = FileSystem::ReadText(Test::GetRepositoryRoot() / "Docs" / "Reference" / "ScriptAPI.md");
			REQUIRE(file.has_value());
			std::string joined;
			uint32_t offset = 0;
			size_t pages = 0;
			do
			{
				const Json page = offset == 0 ? CallDocs(client, Json{ { "topic", "reference/ScriptAPI" } }) : CallDocs(client, Json{ { "topic", "reference/ScriptAPI" }, { "offset", offset } });
				if (offset == 0)
				{
					CHECK(page["nextOffset"] != Json(0));
					CHECK(std::ranges::any_of(page["sections"], [](const Json& section)
					{
						return section["anchor"] == Json("input");
					}));
				}
				joined += ReadContent(page);
				offset = JsonReader(page).ReadMember<uint32_t>("nextOffset").value_or(0);
				++pages;
			} while (offset != 0 && pages < 1000);
			CHECK(joined == *file);

			const Json input = CallDocs(client, Json{ { "topic", "reference/ScriptAPI#Input" } });
			CHECK(ReadContent(input).starts_with("## Input\n"));
			CHECK(ReadContent(input).contains("### Input.GetAxis"));
		}

		// Friction S1-05 (Docs/Decisions/0026-m16-contract.md, F04): an automation-only builder reads the documentation through
		// docs.get alone, so the skills it follows (game-building, luau-gameplay and game-debugging, which name each other) may
		// send it only to what docs.get serves: a topic by its name, or a repository document by the path of a served topic.
		// Not documents: the Python commands under Scripts/ (the repository's gate), game project directories
		// (Projects/<Name>, Tests/Projects/<Name>: what the builder builds), placeholders ("<Game>") and the build output.
		TEST_CASE("ObserveMethods: every document and topic the game-building skills name is one docs.get serves")
		{
			Test::EditorTestFixture fixture("DocsSkillReferences");
			Test::AutomationTestClient client(fixture.GetEditor());
			std::set<std::string> topics;
			std::set<std::string> paths;
			for (const Json& topic : CallDocs(client, Json::object())["topics"])
			{
				const std::string name = JsonReader(topic).ReadMember<std::string>("name").value_or(std::string());
				topics.insert(name);
				paths.insert(JsonReader(CallDocs(client, Json{ { "topic", name }, { "length", 1 } })).ReadMember<std::string>("path").value_or(std::string()));
			}

			const std::filesystem::path root = Test::GetRepositoryRoot();
			const auto isGameProject = [](std::string_view path)
			{
				if (path.ends_with('/'))
					path.remove_suffix(1);
				const auto isProjectName = [](std::string_view rest, std::string_view prefix)
				{
					return rest.starts_with(prefix) && rest.size() > prefix.size() && !rest.substr(prefix.size()).contains('/');
				};
				return isProjectName(path, "Projects/") || isProjectName(path, "Tests/Projects/");
			};
			size_t checked = 0;
			for (const std::string_view skill : { "skills/game-building", "skills/luau-gameplay", "skills/game-debugging" })
			{
				REQUIRE_MESSAGE(topics.contains(std::string(skill)), skill);
				for (const std::string& word : FindPathWords(ReadWholeTopic(client, skill)))
				{
					if (word.contains('<') || word.contains('>'))
						continue;
					const std::string_view first = std::string_view(word).substr(0, word.find('/'));
					if (first == "skills" || first == "reference" || first == "scripting" || first == "templates")
					{
						if (word.ends_with('/'))
							continue; // a prefix of topic names
						++checked;
						CHECK_MESSAGE(topics.contains(word), std::format("{} names the topic '{}', which docs.get does not serve", skill, word));
						continue;
					}
					std::error_code error;
					// A repository path starts with a directory of the repository root; a script-relative require path ("./Ball",
					// "../../Scripts/Ball") does not.
					if (first.empty() || first == "." || first == ".." || first == "bin" || first == "bin-int" || first == "Scripts"
						|| !std::filesystem::is_directory(root / FileSystem::PathFromUtf8(first), error))
						continue;
					if (isGameProject(word))
						continue;
					++checked;
					CHECK_MESSAGE(paths.contains(word), std::format("{} names the repository path '{}', which is no docs.get topic's file", skill, word));
				}
			}
			// The skills name topics (reference/Automation, scripting/Engine.d.luau, templates/Scripts/Test, ...): a scan that
			// finds none is broken.
			CHECK(checked >= 10);
		}
	}

}
