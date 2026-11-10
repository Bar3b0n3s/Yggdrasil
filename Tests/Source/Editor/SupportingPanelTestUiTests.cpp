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

		// W23.3's review (the animation preview panel's "Preview" button below its window's title): a later occurrence of the
		// same text is found, and clicked, past the earlier ones, in drawing order; one past the last is no match.
		TEST_CASE("SupportingPanelTestUi: a later occurrence of a text is found past the earlier ones")
		{
			Test::UtilityPanelFixture fixture;
			Utils::ApplyEditorStyle(1.0f);
			struct RepeatedTarget
			{
				ImVec2 Minimum{};
				ImVec2 Maximum{};
				uint32_t Clicks = 0;

				[[nodiscard]] Status Draw(EditorPanelContext&)
				{
					ImGui::TextUnformatted("Repeated");
					if (ImGui::Button("Repeated"))
						++Clicks;
					Minimum = ImGui::GetItemRectMin();
					Maximum = ImGui::GetItemRectMax();
					return {};
				}
			} target;
			const auto first = Test::SupportingPanelTestUi::FindText(fixture, target, "Repeated");
			const auto second = Test::SupportingPanelTestUi::FindText(fixture, target, "Repeated", 1);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(first->y < target.Minimum.y);
			CHECK(second->y > target.Minimum.y);
			CHECK(second->y < target.Maximum.y);
			CHECK_FALSE(Test::SupportingPanelTestUi::FindText(fixture, target, "Repeated", 2).has_value());
			Test::SupportingPanelTestUi::ClickText(fixture, target, "Repeated");
			CHECK(target.Clicks == 0);
			Test::SupportingPanelTestUi::ClickText(fixture, target, "Repeated", 1);
			CHECK(target.Clicks == 1);
		}
	}

}
