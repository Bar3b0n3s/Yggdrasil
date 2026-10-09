#include "TestsPCH.h"
#include "EditorCore/EditorUiState.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Entity.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorUiState: default panels are stable and reset only on request")
		{
			EditorUiState state;
			state.ResetLayout(false);
			CHECK(std::is_sorted(state.GetOpenPanels().begin(), state.GetOpenPanels().end()));
			CHECK(state.GetOpenPanels().back() == EditorPanel::ProjectLauncher);
			REQUIRE(state.SetPanelOpen(EditorPanel::Stats, true));
			REQUIRE(state.SetPanelOpen(EditorPanel::Stats, true));
			CHECK(std::count(state.GetOpenPanels().begin(), state.GetOpenPanels().end(), EditorPanel::Stats) == 1);
			REQUIRE(state.SetPanelOpen(EditorPanel::Inspector, false));
			state.CompleteFrame();
			CHECK(std::find(state.GetOpenPanels().begin(), state.GetOpenPanels().end(), EditorPanel::Inspector) == state.GetOpenPanels().end());
			const auto invalid = state.SetPanelOpen(static_cast<EditorPanel>(255), true);
			REQUIRE_FALSE(invalid);
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			state.ResetLayout(true);
			CHECK(state.GetOpenPanels().size() == 6);
			CHECK(std::find(state.GetOpenPanels().begin(), state.GetOpenPanels().end(), EditorPanel::ProjectLauncher) == state.GetOpenPanels().end());
		}

		TEST_CASE("EditorUiState: completed frames increase after UI construction")
		{
			EditorUiState state;
			CHECK(state.GetCompletedFrame() == 0);
			state.ResetLayout(true);
			CHECK(state.GetCompletedFrame() == 0);
			state.CompleteFrame();
			state.CompleteFrame();
			CHECK(state.GetCompletedFrame() == 2);
			state.ResetLayout(false);
			CHECK(state.GetCompletedFrame() == 2);
		}

		TEST_CASE("EditorUiState: asset and entity selections cannot ambiguously drive the inspector")
		{
			Test::EditorTestFixture fixture("PanelSelection");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			auto& editor = fixture.GetEditor();
			const UUID id = editor.GetScene().CreateEntity("Selected").GetUUID();
			editor.GetUiState().SetSelectedAsset(AssetHandle(123));
			REQUIRE(editor.SetSelection({ id }, SceneTarget::Edit));
			CHECK_FALSE(editor.GetUiState().GetSelectedAsset().IsValid());
			REQUIRE(editor.SetSelection({}, SceneTarget::Edit));
			editor.GetUiState().SetSelectedAsset(AssetHandle(456));
			CHECK(editor.GetSelection().empty());
			CHECK(editor.GetUiState().GetSelectedAsset() == AssetHandle(456));
		}
	}

}
