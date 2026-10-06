#include "TestsPCH.h"

#include "Support/ChildOutput.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

namespace Engine {

	// Writes `text` to `file`; false when that fails.
	static bool WriteText(const std::filesystem::path& file, std::string_view text)
	{
		return FileSystem::WriteFileAtomic(file, AsBytes(text)).has_value();
	}

	TEST_SUITE("Support")
	{
		TEST_CASE("ChildOutput: FindBracketedValue returns the first bracketed value after its prefix")
		{
			const std::string text = "[info] Report: [a/b.txt] Report: [second]\n[warn] Empty: []";
			CHECK(Test::FindBracketedValue(text, "Report: ") == "a/b.txt");
			CHECK(Test::FindBracketedValue(text, "Empty: ").empty());
			CHECK(Test::FindBracketedValue(text, "Missing: ").empty());
			CHECK(Test::FindBracketedValue("Unclosed: [value", "Unclosed: ").empty());
		}

		TEST_CASE("ChildOutput: ContainsInOrder needs every part, in order and without overlap")
		{
			const std::array<std::string, 3> parts = { "one", "two", "three" };
			CHECK(Test::ContainsInOrder("one, two, three", parts));
			CHECK(Test::ContainsInOrder("xonextwoxthreex", parts));
			CHECK_FALSE(Test::ContainsInOrder("two, one, three", parts));
			CHECK_FALSE(Test::ContainsInOrder("one, two", parts));
			const std::array<std::string, 2> repeated = { "aa", "aa" };
			CHECK_FALSE(Test::ContainsInOrder("aaa", repeated));
			CHECK(Test::ContainsInOrder("aaaa", repeated));
			CHECK(Test::ContainsInOrder("anything", std::span<const std::string>()));
		}

		TEST_CASE("ChildOutput: ListCrashFiles and ReadOnlyCrashReport find a child's crash reports")
		{
			Test::TempDirectory directory("ChildOutput");
			const std::filesystem::path crashes = directory / ENGINE_PRODUCT_NAME / "Crashes";
			CHECK(Test::ListCrashFiles(crashes, ".txt").empty());
			const Result<std::string> missing = Test::ReadOnlyCrashReport(directory.GetPath());
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);

			REQUIRE(FileSystem::CreateDirectories(crashes).has_value());
			REQUIRE(WriteText(crashes / "crash-2-7.txt", "second"));
			REQUIRE(WriteText(crashes / "crash-1-7.dmp", "dump"));
			REQUIRE(WriteText(crashes / "notes.txt", "not a report"));
			const std::vector<std::filesystem::path> dumps = Test::ListCrashFiles(crashes, ".dmp");
			REQUIRE(dumps.size() == 1);
			CHECK(dumps.front().filename() == "crash-1-7.dmp");
			const Result<std::string> report = Test::ReadOnlyCrashReport(directory.GetPath());
			REQUIRE(report.has_value());
			CHECK(*report == "second");

			REQUIRE(WriteText(crashes / "crash-1-7.txt", "first"));
			const std::vector<std::filesystem::path> reports = Test::ListCrashFiles(crashes, ".txt");
			REQUIRE(reports.size() == 2);
			CHECK(reports[0].filename() == "crash-1-7.txt");
			CHECK(reports[1].filename() == "crash-2-7.txt");
			const Result<std::string> ambiguous = Test::ReadOnlyCrashReport(directory.GetPath());
			REQUIRE_FALSE(ambiguous.has_value());
			CHECK(ambiguous.error().GetCode() == ErrorCode::NotFound);
		}
	}

}
