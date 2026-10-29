#include "EnginePCH.h"
#include "Engine/Renderer/UiDrawList.h"

#include "Engine/Core/Assert.h"

#include <memory_resource>

namespace Engine {

	namespace Utils {

		// `resource`, which must not be null (asserted; the default resource stands in for a null one).
		[[nodiscard]] static std::pmr::memory_resource* CheckUiMemoryResource(std::pmr::memory_resource* resource)
		{
			ENGINE_CORE_ASSERT(resource != nullptr, "UiDrawList needs a memory resource");
			return resource != nullptr ? resource : std::pmr::get_default_resource();
		}

	}

	UiDrawList::UiDrawList(std::pmr::memory_resource* resource)
		: Items(Utils::CheckUiMemoryResource(resource)), Annotations(Items.get_allocator().resource())
	{
		// Items is declared first, so Annotations takes its checked resource.
	}

}
