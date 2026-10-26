#include "Engine/Native/NativeApi.h"
#include "Native/Spinner.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <windows.h>

#include <chrono>
#include <cmath>

namespace Spinners {

	static int s_Calls = 0;

	static void Spin(Engine::INativeStepContext& context)
	{
		static float s_Angle = 0.0f;
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		s_Angle += std::sin(static_cast<float>(context.GetFixedDelta()));
		static_cast<void>(now);
	}

	static void Register(Engine::INativeRegistrar& registrar)
	{
		registrar.AddSystem({ .Name = "Spin", .Description = "Turns.", .Run = &Spin });
	}

}

ENGINE_NATIVE_MODULE(Spinners::Register)
