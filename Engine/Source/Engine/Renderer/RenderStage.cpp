#include "EnginePCH.h"
#include "Engine/Renderer/RenderStage.h"

#include <array>
#include <cstddef>
#include <utility>

namespace Engine {

	namespace Utils {

		// The stages and their names, in enumerator order.
		constexpr std::array<std::pair<RenderStage, std::string_view>, RenderStageCount> RenderStageNames = { {
			{ RenderStage::Prepare, "Prepare" },
			{ RenderStage::Deform, "Deform" },
			{ RenderStage::Shadows, "Shadows" },
			{ RenderStage::DepthNormal, "DepthNormal" },
			{ RenderStage::Ambient, "Ambient" },
			{ RenderStage::Forward, "Forward" },
			{ RenderStage::Sprites, "Sprites" },
			{ RenderStage::HdrPost, "HdrPost" },
			{ RenderStage::Tonemap, "Tonemap" },
			{ RenderStage::LdrPost, "LdrPost" },
			{ RenderStage::SceneOverlay, "SceneOverlay" },
			{ RenderStage::Output, "Output" },
			{ RenderStage::EditorOverlay, "EditorOverlay" },
		} };

		// Every stage has its entry at its enumerator's index with a name; an entry the array value-initialized because a stage
		// was appended without one fails here.
		[[nodiscard]] static consteval bool AreRenderStageNamesComplete()
		{
			for (size_t index = 0; index < RenderStageNames.size(); ++index)
			{
				if (static_cast<size_t>(RenderStageNames[index].first) != index || RenderStageNames[index].second.empty())
					return false;
			}
			return true;
		}

		static_assert(AreRenderStageNamesComplete(), "every RenderStage needs its name, in enumerator order");

	}

	std::string_view RenderStageToString(RenderStage stage)
	{
		const auto index = static_cast<size_t>(stage);
		return index < Utils::RenderStageNames.size() ? Utils::RenderStageNames[index].second : std::string_view("Unknown");
	}

}
