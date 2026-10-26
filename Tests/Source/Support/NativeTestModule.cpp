#include "TestsPCH.h"
#include "Support/NativeTestModule.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <vector>

namespace Engine {

	namespace Test {

		namespace Utils {

			inline constexpr float DegreesToRadians = 0.017453292519943295f;

			// Turns `entity` by `degreesPerSecond` about `axis` for one tick.
			static void Turn(INativeStepContext& context, NativeEntity entity, float degreesPerSecond, const glm::vec3& axis)
			{
				NativeTransform transform{};
				if (!context.GetTransform(entity, transform))
					return;
				const float radians = degreesPerSecond * DegreesToRadians * static_cast<float>(context.GetFixedDelta());
				transform.Rotation = glm::normalize(context.AngleAxis(radians, axis) * transform.Rotation);
				static_cast<void>(context.SetTransform(entity, transform));
			}

			// Moves `entity` by `distance` along +X and adds it to the counter.
			static void Shift(INativeStepContext& context, NativeEntity entity, float distance)
			{
				NativeTransform transform{};
				if (!context.GetTransform(entity, transform))
					return;
				transform.Translation.x += distance;
				static_cast<void>(context.SetTransform(entity, transform));
				if (NativeTestCounter* counter = context.GetResource<NativeTestCounter>(); counter != nullptr)
					counter->Distance += std::abs(distance);
			}

			static void Spin(INativeStepContext& context)
			{
				for (const NativeEntity entity : context.Query<NativeTestSpinner>())
				{
					if (NativeTestSpinner* spinner = context.Get<NativeTestSpinner>(entity); spinner != nullptr)
					{
						Turn(context, entity, spinner->DegreesPerSecond, spinner->Axis);
						++spinner->Turns;
					}
				}
			}

			static void SpinV2(INativeStepContext& context)
			{
				for (const NativeEntity entity : context.Query<NativeTestSpinnerV2>())
				{
					if (NativeTestSpinnerV2* spinner = context.Get<NativeTestSpinnerV2>(entity); spinner != nullptr)
					{
						Turn(context, entity, spinner->Reversed ? -spinner->Speed : spinner->Speed, spinner->Axis);
						++spinner->Turns;
					}
				}
			}

			static void Move(INativeStepContext& context)
			{
				const float input = context.GetActionAxis(NativeTestMoveAction);
				for (const NativeEntity entity : context.Query<NativeTestMover>())
				{
					if (const NativeTestMover* mover = context.Get<NativeTestMover>(entity); mover != nullptr)
						Shift(context, entity, mover->Speed * static_cast<float>(context.GetFixedDelta()) * input);
				}
			}

			static void MoveV2(INativeStepContext& context)
			{
				const float input = context.GetActionAxis(NativeTestMoveAction);
				for (const NativeEntity entity : context.Query<NativeTestMoverV2>())
				{
					if (const NativeTestMoverV2* mover = context.Get<NativeTestMoverV2>(entity); mover != nullptr)
						Shift(context, entity, mover->Speed * static_cast<float>(context.GetFixedDelta()) * input);
				}
			}

			static void Count(INativeStepContext& context)
			{
				if (NativeTestCounter* counter = context.GetResource<NativeTestCounter>(); counter != nullptr)
					++counter->Ticks;
			}

			// The standard library throws std::out_of_range: an exception escaping a system (NativeWorld's boundary).
			static void Fail(INativeStepContext& /*context*/)
			{
				const std::vector<int> empty{};
				static_cast<void>(empty.at(1));
			}

			static void RegisterCounter(INativeRegistrar& registrar)
			{
				registrar.Add(NativeResource<NativeTestCounter>("What the test module's systems counted this session.")
						.Field("Ticks", &NativeTestCounter::Ticks, "Ticks stepped.")
						.Field("Distance", &NativeTestCounter::Distance, "Metres the movers moved.", { .Min = 0.0, .Unit = "m" }));
			}

		}

		void RegisterNativeTestModule(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<NativeTestSpinner>("Turns its entity about an axis.")
					.Field("DegreesPerSecond", &NativeTestSpinner::DegreesPerSecond, "How fast it turns.", { .Min = -3600.0, .Max = 3600.0, .Unit = "deg/s" })
					.Field("Axis", &NativeTestSpinner::Axis, "The local axis it turns about.")
					.Field("Turns", &NativeTestSpinner::Turns, "The ticks it turned."));
			registrar.Add(NativeComponent<NativeTestMover>("Moves its entity along +X with the MoveX action.")
					.Field("Speed", &NativeTestMover::Speed, "Metres per second at full input.", { .Min = 0.0, .Unit = "m/s" })
					.Field("Follow", &NativeTestMover::Follow, "An entity it would follow."));
			Utils::RegisterCounter(registrar);
			registrar.AddSystem({ .Name = "Spin", .Description = "Turns every TestSpinner.", .Run = &Utils::Spin });
			registrar.AddSystem({ .Name = "Move", .Description = "Moves every TestMover with the MoveX action.", .Run = &Utils::Move });
			registrar.AddSystem({ .Name = "Count", .Description = "Counts the ticks.", .Run = &Utils::Count });
		}

		void RegisterNativeTestModuleV2(INativeRegistrar& registrar)
		{
			registrar.Add(NativeComponent<NativeTestSpinnerV2>("Turns its entity about an axis, either way.")
					.Field("Speed", &NativeTestSpinnerV2::Speed, "How fast it turns.", { .Min = -3600.0, .Max = 3600.0, .Unit = "deg/s", .RenamedFrom = "DegreesPerSecond" })
					.Field("Axis", &NativeTestSpinnerV2::Axis, "The local axis it turns about.")
					.Field("Turns", &NativeTestSpinnerV2::Turns, "The ticks it turned.")
					.Field("Reversed", &NativeTestSpinnerV2::Reversed, "Turns the other way."));
			registrar.Add(NativeComponent<NativeTestMoverV2>("Moves its entity along +X with the MoveX action.")
					.Field("Speed", &NativeTestMoverV2::Speed, "Metres per second at full input.", { .Min = 0.0, .Unit = "m/s" }));
			Utils::RegisterCounter(registrar);
			registrar.AddSystem({ .Name = "Spin", .Description = "Turns every TestSpinner.", .Run = &Utils::SpinV2 });
			registrar.AddSystem({ .Name = "Move", .Description = "Moves every TestMover with the MoveX action.", .Run = &Utils::MoveV2 });
			registrar.AddSystem({ .Name = "Count", .Description = "Counts the ticks.", .Run = &Utils::Count });
		}

		void RegisterFailingNativeTestModule(INativeRegistrar& registrar)
		{
			RegisterNativeTestModule(registrar);
			registrar.AddSystem({ .Name = "Fail", .Description = "Lets the standard library throw.", .Run = &Utils::Fail });
		}

		NativeModuleEntryPoint GetNativeTestModule()
		{
			return NativeModuleEntryPoint{ .ApiVersion = NativeApiVersion, .Register = &RegisterNativeTestModule };
		}

		NativeModuleEntryPoint GetNativeTestModuleV2()
		{
			return NativeModuleEntryPoint{ .ApiVersion = NativeApiVersion, .Register = &RegisterNativeTestModuleV2 };
		}

		NativeModuleEntryPoint GetFailingNativeTestModule()
		{
			return NativeModuleEntryPoint{ .ApiVersion = NativeApiVersion, .Register = &RegisterFailingNativeTestModule };
		}

	}

}
