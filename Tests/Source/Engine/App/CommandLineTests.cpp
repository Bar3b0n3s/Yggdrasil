#include "TestsPCH.h"

#include "Engine/App/CommandLine.h"

namespace Engine {

	static constexpr std::array<CommandLineOption, 4> DeclaredOptions = { {
		{ .Name = "--headless", .Description = "Run on GLFW's null platform." },
		{ .Name = "--frames", .Value = CommandLineValue::Required, .ValueName = "N", .Description = "Exit after N frames." },
		{ .Name = "--automation", .Value = CommandLineValue::Optional, .ValueName = "port", .Description = "Serve automation." },
		{ .Name = "--project", .Value = CommandLineValue::Required, .ValueName = "path", .Description = "Open a project." },
	} };

	static Result<CommandLine> Parse(std::vector<std::string> arguments)
	{
		return CommandLine::Parse(arguments, DeclaredOptions);
	}

	TEST_SUITE("App")
	{
		TEST_CASE("CommandLine: flags, values and positional arguments are parsed" * doctest::skip(true))
		{
			const Result<CommandLine> parsed =
				Parse({ "--headless", "--frames", "10", "scene.json", "--automation=5000", "--", "--literal" });
			REQUIRE(parsed.has_value());
			CHECK(parsed->Has("--headless"));
			CHECK_FALSE(parsed->GetValue("--headless").has_value());
			CHECK(parsed->GetValue("--frames") == "10");
			CHECK(parsed->GetValue("--automation") == "5000");
			CHECK_FALSE(parsed->Has("--project"));
			const std::vector<std::string> positional = { "scene.json", "--literal" };
			CHECK(parsed->GetPositional() == positional);
			CHECK(parsed->GetArguments().size() == 7);

			const Result<CommandLine> equals = Parse({ "--frames=3", "--automation", "--project", "--odd-name" });
			REQUIRE(equals.has_value());
			CHECK(equals->GetValue("--frames") == "3");
			CHECK(equals->Has("--automation"));
			CHECK_FALSE(equals->GetValue("--automation").has_value());
			CHECK(equals->GetValue("--project") == "--odd-name"); // a Required value may start with "--"

			const Result<CommandLine> empty = Parse({});
			REQUIRE(empty.has_value());
			CHECK(empty->GetPositional().empty());
		}

		TEST_CASE("CommandLine: unknown options, missing values, values on flags and repeats are InvalidArgument" * doctest::skip(true))
		{
			const std::array<std::vector<std::string>, 7> invalid = { {
				{ "--headles" },
				{ "--frames" },
				{ "--headless=yes" },
				{ "--frames", "1", "--frames", "2" },
				{ "--unknown=1" },
				{ "-headless" },
				{ "-f" },
			} };
			for (const std::vector<std::string>& arguments : invalid)
			{
				CAPTURE(arguments.front());
				const Result<CommandLine> parsed = Parse(arguments);
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(parsed.error().GetMessageText().contains(arguments.front().substr(0, arguments.front().find('='))));
			}

			// The closest declared option is suggested, also for a single dash.
			const Result<CommandLine> typo = Parse({ "--headles" });
			REQUIRE_FALSE(typo.has_value());
			CHECK(typo.error().GetHint().contains("--headless"));
			const Result<CommandLine> singleDash = Parse({ "-headless" });
			REQUIRE_FALSE(singleDash.has_value());
			CHECK(singleDash.error().GetHint().contains("--headless"));

			// A lone "-" and anything after "--" stay positional; a Required value may start with '-'.
			const Result<CommandLine> positional = Parse({ "--frames", "-1", "-", "--", "-x" });
			REQUIRE(positional.has_value());
			CHECK(positional->GetValue("--frames") == "-1");
			const std::vector<std::string> expected = { "-", "-x" };
			CHECK(positional->GetPositional() == expected);
		}

		TEST_CASE("CommandLine: GetUnsigned parses plain decimal values within the maximum" * doctest::skip(true))
		{
			const Result<CommandLine> parsed = Parse({ "--frames", "120" });
			REQUIRE(parsed.has_value());
			const Result<std::optional<uint64_t>> frames = parsed->GetUnsigned("--frames");
			REQUIRE(frames.has_value());
			CHECK(*frames == std::optional<uint64_t>(120));

			const Result<std::optional<uint64_t>> absent = parsed->GetUnsigned("--project");
			REQUIRE(absent.has_value());
			CHECK_FALSE(absent->has_value());

			CHECK_FALSE(parsed->GetUnsigned("--frames", 100).has_value());
			const std::array<std::string_view, 5> malformed = { "-1", "1.5", "0x10", " 7", "99999999999999999999" };
			for (const std::string_view value : malformed)
			{
				CAPTURE(std::string(value));
				const Result<CommandLine> bad = Parse({ "--frames", std::string(value) });
				REQUIRE(bad.has_value());
				const Result<std::optional<uint64_t>> number = bad->GetUnsigned("--frames");
				REQUIRE_FALSE(number.has_value());
				CHECK(number.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(number.error().GetMessageText().contains("--frames"));
			}
		}

		TEST_CASE("CommandLine: FormatUsage lists every option with its value name" * doctest::skip(true))
		{
			const std::string usage = CommandLine::FormatUsage("Editor", DeclaredOptions);
			CHECK(usage.starts_with("Usage: Editor [options]"));
			CHECK(usage.contains("--frames N"));
			CHECK(usage.contains("--automation[=port]"));
			CHECK(usage.contains("Exit after N frames."));
		}
	}

}
