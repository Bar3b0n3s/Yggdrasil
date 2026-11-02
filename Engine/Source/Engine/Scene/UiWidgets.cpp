#include "EnginePCH.h"
#include "Engine/Scene/UiWidgets.h"

// The widgets' behaviours (UiWidgets.h): stubs of the M22 contract (C22.2, Docs/Decisions/0033-m22-contract.md decisions 24
// and 26) that stream B (Button, Toggle, Slider) and stream C (ScrollView) implement.

namespace Engine {

	void ActivateUiWidget(Scene& /*scene*/, UUID /*element*/, std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	float ReadUiSliderValue(const UiSliderComponent& /*slider*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	float GetUiSliderPosition(const UiSliderComponent& /*slider*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	void SetUiSliderFromPointer(Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*slider*/, glm::vec2 /*pointer*/, std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool StepUiSlider(Scene& /*scene*/, UUID /*slider*/, UiDirection /*direction*/, std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	glm::vec2 ClampUiScrollPosition(const UiScrollViewComponent& /*scrollView*/, glm::vec2 /*viewSize*/, glm::vec2 /*contentSize*/, glm::vec2 /*position*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	UUID FindUiScrollView(const Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*element*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	bool ScrollUiScrollView(Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*scrollView*/, glm::vec2 /*delta*/, std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void DragUiScrollView(Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*scrollView*/, glm::vec2 /*start*/, glm::vec2 /*pointerMove*/,
		std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool StepUiScrollView(Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*scrollView*/, UiDirection /*direction*/, std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	void RevealInUiScrollViews(Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*element*/, std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
