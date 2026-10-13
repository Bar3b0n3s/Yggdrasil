#include "TestsPCH.h"
#include "Editor/SupportingPanelTestUi.h"

#include "Editor/Ui/EditorStyle.h"

#include <cstdint>

namespace Engine {

	namespace {

		struct SupportingTextTarget
		{
			float Scale = 1.0f;
			bool SmallButton = false;
			float RenderedFontSize = 0.0f;
			ImVec2 Minimum{};
			ImVec2 Maximum{};
			uint32_t Clicks = 0;

			[[nodiscard]] Status Draw(EditorPanelContext&)
			{
				ImGui::SetWindowFontScale(Scale);
				RenderedFontSize = ImGui::GetFontSize();
				if (SmallButton ? ImGui::SmallButton("Rendered click target") : ImGui::Button("Rendered click target"))
					++Clicks;
				Minimum = ImGui::GetItemRectMin();
				Maximum = ImGui::GetItemRectMax();
				return {};
			}
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("SupportingPanelTestUi: text clicks retain the rendered window font after the frame ends")
		{
			Test::UtilityPanelFixture fixture;
			// The fixture prebuilds a legacy atlas; window scaling uses its existing glyphs.
			Utils::ApplyEditorStyle(1.0f);
			SupportingTextTarget target;
			SUBCASE("normal window scale")
			{
			}
			SUBCASE("double window scale")
			{
				target.Scale = 2.0f;
			}
			SUBCASE("small button")
			{
				target.SmallButton = true;
			}
			SUBCASE("small button at double window scale")
			{
				target.SmallButton = true;
				target.Scale = 2.0f;
			}
			const auto position = Test::SupportingPanelTestUi::FindText(fixture, target, "Rendered click target");
			REQUIRE(position.has_value());
			CHECK(ImGui::GetFontBaked() == nullptr);
			CHECK(target.RenderedFontSize == doctest::Approx(ImGui::GetFontSize() * target.Scale));
			CHECK(position->x > target.Minimum.x);
			CHECK(position->x < target.Maximum.x);
			CHECK(position->y > target.Minimum.y);
			CHECK(position->y < target.Maximum.y);
			CHECK_FALSE(Test::SupportingPanelTestUi::FindText(fixture, target, "Absent target").has_value());
			CHECK(target.Clicks == 0);
			Test::SupportingPanelTestUi::ClickText(fixture, target, "Rendered click target");
			CHECK(target.Clicks == 1);
		}
	}

}
