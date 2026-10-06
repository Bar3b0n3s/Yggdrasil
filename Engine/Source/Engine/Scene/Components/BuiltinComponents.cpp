#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <cstddef>

namespace Engine {

	void RegisterBuiltinComponents(TypeRegistry& registry)
	{
		ENGINE_CORE_ASSERT(!registry.IsFrozen(), "Built-in components are registered before the registry is frozen");
		const size_t first = registry.GetComponents().size();

		RegisterCoreComponents(registry);
		RegisterRenderingComponents(registry);
		RegisterPhysicsComponents(registry);
		RegisterAudioComponents(registry);
		RegisterScriptingComponents(registry);

		// The startup check of §5.4: every listed type is registered, in list order and nothing else, so the registration
		// order (the canonical component order of files) is exactly BuiltinComponents. A mismatch would reorder or drop
		// components in every file written, so it is verified in Dist too.
		const size_t registered = registry.GetComponents().size() - first;
		ENGINE_CORE_VERIFY(registered == BuiltinComponents::Size, "The category registration functions registered {} components; BuiltinComponents lists {}",
			registered, BuiltinComponents::Size);
		size_t position = first;
		ForEachType(BuiltinComponents{}, [&registry, &position, first]<typename T>()
		{
			const ComponentInfo* component = registry.FindComponent<T>();
			ENGINE_CORE_VERIFY(component != nullptr, "BuiltinComponents entry {} is not registered", position - first);
			ENGINE_CORE_VERIFY(component->GetIndex() == position, "Built-in component '{}' is registered at index {}, but BuiltinComponents lists it at {}",
				component->GetName(), component->GetIndex(), position);
			++position;
		});
	}

}
