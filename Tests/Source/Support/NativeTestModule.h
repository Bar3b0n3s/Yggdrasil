#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Native/NativeApi.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string_view>

// A native gameplay module compiled into the Tests executable (Docs/Decisions/0030-m19-contract.md decision 37): what the
// unit tests of the native module, the native world, the editor's host and the native methods register through
// BuildNativeModuleTypes without a toolchain or a shared library, as the exported Runtime registers its linked module. Two
// layouts of the same module stand for a module before and after its sources changed, for the hot-reload tests.

namespace Engine {

	namespace Test {

		// The project input action its Move system reads (InputSettings::Actions of the session's project).
		inline constexpr std::string_view NativeTestMoveAction = "MoveX";

		// Layout 1's component "Native.TestSpinner": turned by Spin about Axis; Spin counts its turns in Turns, so the
		// component's data changes every tick.
		struct NativeTestSpinner
		{
			static constexpr std::string_view NativeTypeName = "TestSpinner";
			float DegreesPerSecond = 90.0f;
			glm::vec3 Axis = glm::vec3(0.0f, 1.0f, 0.0f);
			uint32_t Turns = 0;
		};

		// Layout 1's component "Native.TestMover": moved along +X by Move, Speed * fixedDelta * the MoveX axis per tick.
		struct NativeTestMover
		{
			static constexpr std::string_view NativeTypeName = "TestMover";
			float Speed = 2.0f;
			NativeEntity Follow{};
		};

		// Layout 1's resource "TestCounter": Count adds 1 to Ticks every tick, and Move adds the distance it moved.
		struct NativeTestCounter
		{
			static constexpr std::string_view NativeTypeName = "TestCounter";
			uint32_t Ticks = 0;
			float Distance = 0.0f;
		};

		// Layout 2's "Native.TestSpinner": DegreesPerSecond renamed Speed (RenamedFrom), and a new field Reversed.
		struct NativeTestSpinnerV2
		{
			static constexpr std::string_view NativeTypeName = "TestSpinner";
			float Speed = 90.0f;
			glm::vec3 Axis = glm::vec3(0.0f, 1.0f, 0.0f);
			uint32_t Turns = 0;
			bool Reversed = false;
		};

		// Layout 2's "Native.TestMover": Follow removed.
		struct NativeTestMoverV2
		{
			static constexpr std::string_view NativeTypeName = "TestMover";
			float Speed = 2.0f;
		};

		// Layout 1: the components TestSpinner and TestMover, the resource TestCounter and the systems Spin, Move and Count, in
		// this order.
		void RegisterNativeTestModule(INativeRegistrar& registrar);
		// Layout 2: TestSpinner and TestMover of layout 2, TestCounter, and the systems Spin, Move and Count.
		void RegisterNativeTestModuleV2(INativeRegistrar& registrar);
		// Layout 1 plus a system Fail, last, in which the standard library throws (std::vector::at past the end).
		void RegisterFailingNativeTestModule(INativeRegistrar& registrar);

		// Their entry points, of the current interface version.
		[[nodiscard]] NativeModuleEntryPoint GetNativeTestModule();
		[[nodiscard]] NativeModuleEntryPoint GetNativeTestModuleV2();
		[[nodiscard]] NativeModuleEntryPoint GetFailingNativeTestModule();

	}

}
