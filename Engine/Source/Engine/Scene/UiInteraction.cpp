#include "EnginePCH.h"
#include "Engine/Scene/UiInteraction.h"

// UI interaction (UiInteraction.h): the names are final; the rest are stubs of the M22 contract (C22.2,
// Docs/Decisions/0033-m22-contract.md decisions 22 and 23) that stream A implements.

namespace Engine {

	std::string_view UiWidgetTypeToString(UiWidgetType type)
	{
		switch (type)
		{
			case UiWidgetType::None:       return {};
			case UiWidgetType::Button:     return "Button";
			case UiWidgetType::Toggle:     return "Toggle";
			case UiWidgetType::Slider:     return "Slider";
			case UiWidgetType::ScrollView: return "ScrollView";
		}
		return {};
	}

	std::string_view UiEventKindToString(UiEventKind kind)
	{
		switch (kind)
		{
			case UiEventKind::Click:        return "Click";
			case UiEventKind::ValueChanged: return "ValueChanged";
			case UiEventKind::Focus:        return "Focus";
			case UiEventKind::Hover:        return "Hover";
			case UiEventKind::Cancel:       return "Cancel";
			case UiEventKind::Scroll:       return "Scroll";
		}
		return {};
	}

	std::string_view GetUiEventCallbackName(UiEventKind kind)
	{
		switch (kind)
		{
			case UiEventKind::Click:        return "OnUiClick";
			case UiEventKind::ValueChanged: return "OnUiValueChanged";
			case UiEventKind::Focus:        return "OnUiFocus";
			case UiEventKind::Hover:        return "OnUiHover";
			case UiEventKind::Cancel:       return "OnUiCancel";
			case UiEventKind::Scroll:       return "OnUiScroll";
		}
		return {};
	}

	UiWidgetType GetUiWidgetType(const Scene& /*scene*/, UUID /*element*/)
	{
		ENGINE_CONTRACT_STUB();
		return UiWidgetType::None;
	}

	bool IsUiInteractable(const Scene& /*scene*/, UUID /*element*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool IsUiFocusable(const Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*element*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	UiHit HitTestUi(const Scene& /*scene*/, const UiLayoutTable& /*table*/, glm::vec2 /*point*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	UUID FindUiNavigationTarget(const Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*from*/, UiDirection /*direction*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	UUID FindNextUiFocus(const Scene& /*scene*/, const UiLayoutTable& /*table*/, UUID /*from*/, bool /*forward*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	UiStickDirection GetUiStickDirection(glm::vec2 /*stick*/)
	{
		ENGINE_CONTRACT_STUB();
		return UiStickDirection::None;
	}

}
