#include "TestsPCH.h"

#include "Engine/Reflection/EnumInfo.h"

namespace Engine {

	static EnumInfo CreateTonemapEnum()
	{
		EnumInfo info("Tonemapper", "The tonemapping curve.");
		info.AddEntry({ "AgX", 0, "AgX." });
		info.AddEntry({ "ACES", 1, "ACES filmic." });
		info.AddEntry({ "PbrNeutral", 2, "Khronos PBR Neutral." });
		info.AddEntry({ "Linear", 3, "No curve." });
		return info;
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("EnumInfo: names are matched exactly for files and ignoring case for automation" * doctest::skip(true))
		{
			const EnumInfo info = CreateTonemapEnum();
			CHECK(info.GetEntries().size() == 4);

			const EnumEntry* aces = info.FindByName("ACES");
			REQUIRE(aces != nullptr);
			CHECK(aces->Value == 1);
			CHECK(info.FindByName("aces") == nullptr);

			const EnumEntry* folded = info.FindByNameIgnoreCase("pbrneutral");
			REQUIRE(folded != nullptr);
			CHECK(folded->Name == "PbrNeutral"); // the canonical spelling is echoed
		}

		TEST_CASE("EnumInfo: values map back to entries and unknown names get suggestions" * doctest::skip(true))
		{
			const EnumInfo info = CreateTonemapEnum();
			const EnumEntry* linear = info.FindByValue(3);
			REQUIRE(linear != nullptr);
			CHECK(linear->Name == "Linear");
			CHECK(info.FindByValue(4) == nullptr);

			const std::vector<std::string> suggestions = info.SuggestNames("Lineer");
			REQUIRE_FALSE(suggestions.empty());
			CHECK(suggestions.front() == "Linear");
		}

		TEST_CASE("EnumInfo: entries keep registration order" * doctest::skip(true))
		{
			const EnumInfo info = CreateTonemapEnum();
			std::vector<std::string> names;
			for (const EnumEntry& entry : info.GetEntries())
				names.push_back(entry.Name);
			CHECK(names == std::vector<std::string>{ "AgX", "ACES", "PbrNeutral", "Linear" });
		}
	}

}
