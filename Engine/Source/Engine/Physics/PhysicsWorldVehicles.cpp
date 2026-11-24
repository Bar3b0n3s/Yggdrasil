#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/Assert.h"
#include "Engine/Physics/Private/PhysicsContactListener.h"
#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsEngineState.h"
#include "Engine/Physics/Private/PhysicsWorldState.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The physics world's wheeled vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; PhysicsWorld.h's
// vehicles section). Every value is checked before it reaches Jolt (§9.1): the asserts of Jolt's vehicle constructors
// (VehicleConstraint.cpp, Wheel.cpp, WheeledVehicleController.cpp, VehicleDifferential.cpp), the divisions a curve or an
// engine would make by zero, and the indices Jolt would read out of range are refused or clamped here. Jolt's vehicle
// objects are reference counted; the world owns each through a Scope and marks it embedded, so the system's references
// never delete it, and it removes the constraint and the step listener before it destroys them.

namespace Engine {

	namespace Utils {

		// How far the engine torque shares of a vehicle's driven differentials may sum from 1: Jolt's own assert in
		// WheeledVehicleController::PostCollide, at every step.
		constexpr float TorqueRatioTolerance = 1.0e-6f;
		// The smallest length of a wheel's Forward x Up (the axle) before the two count as parallel.
		constexpr float MinAxleLength = 1.0e-3f;
		// The largest magnitude of a wheel curve's X: a slip ratio, or a slip angle in degrees.
		constexpr float MaxWheelCurveX = 1000.0f;

		[[nodiscard]] static bool IsKnownCast(PhysicsVehicleCast cast)
		{
			return cast == PhysicsVehicleCast::Ray || cast == PhysicsVehicleCast::Sphere || cast == PhysicsVehicleCast::Cylinder;
		}

		[[nodiscard]] static bool IsKnownTransmissionMode(PhysicsVehicleTransmissionMode mode)
		{
			return mode == PhysicsVehicleTransmissionMode::Auto || mode == PhysicsVehicleTransmissionMode::Manual;
		}

		// InvalidArgument unless `value` is finite and in [minimum, maximum].
		[[nodiscard]] static Status CheckRange(std::string_view what, float value, float minimum, float maximum)
		{
			// A value that is not finite fails the comparison too.
			if (!(value >= minimum && value <= maximum))
				return MakeError(ErrorCode::InvalidArgument, "{} must be in [{}, {}] (got {})", what, minimum, maximum, value);
			return {};
		}

		// InvalidArgument unless `value` is finite and in (0, maximum].
		[[nodiscard]] static Status CheckPositive(std::string_view what, float value, float maximum)
		{
			if (!(value > 0.0f && value <= maximum))
				return MakeError(ErrorCode::InvalidArgument, "{} must be above 0 and at most {} (got {})", what, maximum, value);
			return {};
		}

		// InvalidArgument unless `value` is 0 (open) or finite in (1, MaxPhysicsVehicleSlipRatio].
		[[nodiscard]] static Status CheckLimitedSlip(std::string_view what, float value)
		{
			if (value == 0.0f || (value > 1.0f && value <= MaxPhysicsVehicleSlipRatio))
				return {};
			return MakeError(ErrorCode::InvalidArgument, "{} must be 0 (open) or above 1 and at most {} (got {})", what, MaxPhysicsVehicleSlipRatio, value);
		}

		// InvalidArgument unless `keys` is a curve Jolt evaluates without dividing by zero: at most MaxPhysicsVehicleCurveKeys
		// keys, X finite in [minimumX, maximumX] and strictly increasing, Y in [0, MaxPhysicsVehicleRate].
		[[nodiscard]] static Status CheckCurve(std::string_view what, std::span<const PhysicsVehicleCurveKey> keys, float minimumX, float maximumX)
		{
			if (keys.size() > MaxPhysicsVehicleCurveKeys)
				return MakeError(ErrorCode::InvalidArgument, "{} has {} keys; at most {}", what, keys.size(), MaxPhysicsVehicleCurveKeys);
			for (size_t index = 0; index < keys.size(); ++index)
			{
				ENGINE_TRY(CheckRange(std::format("{} key {}'s X", what, index), keys[index].X, minimumX, maximumX));
				ENGINE_TRY(CheckRange(std::format("{} key {}'s Y", what, index), keys[index].Y, 0.0f, MaxPhysicsVehicleRate));
				if (index > 0 && !(keys[index].X > keys[index - 1].X))
					return MakeError(ErrorCode::InvalidArgument, "{}'s X must increase from key to key (key {} has {} after {})", what, index, keys[index].X, keys[index - 1].X);
			}
			return {};
		}

		[[nodiscard]] static Status CheckWheel(const PhysicsVehicleWheelDescription& wheel, size_t index)
		{
			const std::string name = std::format("wheel {}", index);
			if (!Detail::IsFinite(wheel.Position) || !IsWithinPhysicsRange(wheel.Position))
			{
				return MakeError(ErrorCode::InvalidArgument, "{}'s Position ({}, {}, {}) must be finite and at most {} m from the origin", name, wheel.Position.x,
					wheel.Position.y, wheel.Position.z, MaxPhysicsCoordinate);
			}
			if (!Detail::IsUnitVector(wheel.Up) || !Detail::IsUnitVector(wheel.Forward))
				return MakeError(ErrorCode::InvalidArgument, "{}'s Up and Forward must be unit vectors", name);
			if (!(glm::length(glm::cross(wheel.Forward, wheel.Up)) >= MinAxleLength))
				return MakeError(ErrorCode::InvalidArgument, "{}'s Up and Forward are parallel, so it has no axle", name);
			ENGINE_TRY(CheckRange(name + "'s Radius", wheel.Radius, MinPhysicsVehicleWheelRadius, MaxPhysicsVehicleLength));
			ENGINE_TRY(CheckRange(name + "'s Width", wheel.Width, 0.0f, MaxPhysicsVehicleLength));
			ENGINE_TRY(CheckRange(name + "'s SuspensionMinLength", wheel.SuspensionMinLength, 0.0f, MaxPhysicsVehicleLength));
			ENGINE_TRY(CheckRange(name + "'s SuspensionMaxLength", wheel.SuspensionMaxLength, wheel.SuspensionMinLength, MaxPhysicsVehicleLength));
			ENGINE_TRY(CheckRange(name + "'s SuspensionPreload", wheel.SuspensionPreload, 0.0f, MaxPhysicsVehicleLength));
			ENGINE_TRY(CheckPositive(name + "'s SpringFrequency", wheel.SpringFrequency, MaxPhysicsVehicleRate));
			ENGINE_TRY(CheckRange(name + "'s SpringDamping", wheel.SpringDamping, 0.0f, MaxPhysicsVehicleRate));
			ENGINE_TRY(CheckRange(name + "'s MaxSteerAngle", wheel.MaxSteerAngle, 0.0f, 0.5f * JPH::JPH_PI));
			ENGINE_TRY(CheckRange(name + "'s MaxBrakeTorque", wheel.MaxBrakeTorque, 0.0f, MaxPhysicsVehicleTorque));
			ENGINE_TRY(CheckRange(name + "'s MaxHandBrakeTorque", wheel.MaxHandBrakeTorque, 0.0f, MaxPhysicsVehicleTorque));
			ENGINE_TRY(CheckRange(name + "'s Inertia", wheel.Inertia, MinPhysicsVehicleInertia, MaxPhysicsVehicleInertia));
			ENGINE_TRY(CheckRange(name + "'s AngularDamping", wheel.AngularDamping, 0.0f, MaxPhysicsVehicleRate));
			ENGINE_TRY(CheckCurve(name + "'s LongitudinalFriction", wheel.LongitudinalFriction, -MaxWheelCurveX, MaxWheelCurveX));
			ENGINE_TRY(CheckCurve(name + "'s LateralFriction", wheel.LateralFriction, -MaxWheelCurveX, MaxWheelCurveX));
			return {};
		}

		// InvalidArgument unless `wheel` is -1 (none, when `allowNone`) or a wheel index below `wheelCount`.
		[[nodiscard]] static Status CheckWheelIndex(std::string_view what, int32_t wheel, size_t wheelCount, bool allowNone)
		{
			if ((allowNone && wheel == -1) || (wheel >= 0 && static_cast<size_t>(wheel) < wheelCount))
				return {};
			return MakeError(ErrorCode::InvalidArgument, "{} names wheel {}, which the vehicle of {} wheels lacks", what, wheel, wheelCount);
		}

		[[nodiscard]] static Status CheckSettings(const PhysicsVehicleSettings& settings, size_t wheelCount)
		{
			const PhysicsVehicleEngine& engine = settings.Engine;
			ENGINE_TRY(CheckRange("the engine's MaxTorque", engine.MaxTorque, 0.0f, MaxPhysicsVehicleTorque));
			ENGINE_TRY(CheckRange("the engine's MaxRpm", engine.MaxRpm, 1.0f, MaxPhysicsVehicleRpm));
			ENGINE_TRY(CheckRange("the engine's MinRpm", engine.MinRpm, 0.0f, engine.MaxRpm));
			ENGINE_TRY(CheckCurve("the engine's TorqueCurve", engine.TorqueCurve, 0.0f, 1.0f));
			ENGINE_TRY(CheckRange("the engine's Inertia", engine.Inertia, MinPhysicsVehicleInertia, MaxPhysicsVehicleInertia));
			ENGINE_TRY(CheckRange("the engine's AngularDamping", engine.AngularDamping, 0.0f, MaxPhysicsVehicleRate));

			const PhysicsVehicleTransmission& transmission = settings.Transmission;
			if (!IsKnownTransmissionMode(transmission.Mode))
				return MakeError(ErrorCode::InvalidArgument, "unknown transmission mode {}", std::to_underlying(transmission.Mode));
			if (transmission.GearRatios.empty() || transmission.GearRatios.size() > MaxPhysicsVehicleGears)
				return MakeError(ErrorCode::InvalidArgument, "a vehicle needs 1 to {} forward gears (got {})", MaxPhysicsVehicleGears, transmission.GearRatios.size());
			if (transmission.ReverseGearRatios.size() > MaxPhysicsVehicleGears)
				return MakeError(ErrorCode::InvalidArgument, "a vehicle has at most {} reverse gears (got {})", MaxPhysicsVehicleGears, transmission.ReverseGearRatios.size());
			for (size_t gear = 0; gear < transmission.GearRatios.size(); ++gear)
				ENGINE_TRY(CheckPositive(std::format("forward gear {}'s ratio", gear + 1), transmission.GearRatios[gear], MaxPhysicsVehicleRatio));
			for (size_t gear = 0; gear < transmission.ReverseGearRatios.size(); ++gear)
				ENGINE_TRY(CheckPositive(std::format("reverse gear {}'s ratio, negated,", gear + 1), -transmission.ReverseGearRatios[gear], MaxPhysicsVehicleRatio));
			ENGINE_TRY(CheckRange("the transmission's SwitchTime", transmission.SwitchTime, 0.0f, MaxPhysicsVehicleTime));
			ENGINE_TRY(CheckRange("the transmission's ClutchReleaseTime", transmission.ClutchReleaseTime, 0.0f, MaxPhysicsVehicleTime));
			ENGINE_TRY(CheckRange("the transmission's SwitchLatency", transmission.SwitchLatency, 0.0f, MaxPhysicsVehicleTime));
			ENGINE_TRY(CheckPositive("the transmission's ShiftDownRpm", transmission.ShiftDownRpm, MaxPhysicsVehicleRpm));
			if (!(transmission.ShiftUpRpm > transmission.ShiftDownRpm && transmission.ShiftUpRpm <= MaxPhysicsVehicleRpm))
			{
				return MakeError(ErrorCode::InvalidArgument, "the transmission's ShiftUpRpm must be above its ShiftDownRpm {} (got {})", transmission.ShiftDownRpm,
					transmission.ShiftUpRpm);
			}
			if (transmission.Mode == PhysicsVehicleTransmissionMode::Auto && !(transmission.ShiftUpRpm < engine.MaxRpm))
			{
				return MakeError(ErrorCode::InvalidArgument, "an automatic transmission's ShiftUpRpm must be below the engine's MaxRpm {} (got {})", engine.MaxRpm,
					transmission.ShiftUpRpm);
			}
			ENGINE_TRY(CheckPositive("the transmission's ClutchStrength", transmission.ClutchStrength, MaxPhysicsVehicleRate));

			if (settings.Differentials.size() > MaxPhysicsVehicleDifferentials)
				return MakeError(ErrorCode::InvalidArgument, "a vehicle has at most {} differentials (got {})", MaxPhysicsVehicleDifferentials, settings.Differentials.size());
			// As Jolt sums them: in float, in list order, over the driven differentials (those that name a wheel).
			float torqueRatios = 0.0f;
			bool driven = false;
			for (size_t index = 0; index < settings.Differentials.size(); ++index)
			{
				const PhysicsVehicleDifferential& differential = settings.Differentials[index];
				const std::string name = std::format("differential {}", index);
				ENGINE_TRY(CheckWheelIndex(name, differential.LeftWheel, wheelCount, true));
				ENGINE_TRY(CheckWheelIndex(name, differential.RightWheel, wheelCount, true));
				if (differential.LeftWheel != -1 && differential.LeftWheel == differential.RightWheel)
					return MakeError(ErrorCode::InvalidArgument, "{} names wheel {} twice", name, differential.LeftWheel);
				ENGINE_TRY(CheckPositive(name + "'s Ratio", differential.Ratio, MaxPhysicsVehicleRatio));
				ENGINE_TRY(CheckRange(name + "'s LeftRightSplit", differential.LeftRightSplit, 0.0f, 1.0f));
				ENGINE_TRY(CheckLimitedSlip(name + "'s LimitedSlipRatio", differential.LimitedSlipRatio));
				ENGINE_TRY(CheckRange(name + "'s EngineTorqueRatio", differential.EngineTorqueRatio, 0.0f, 1.0f));
				if (differential.LeftWheel != -1 || differential.RightWheel != -1)
				{
					torqueRatios += differential.EngineTorqueRatio;
					driven = true;
				}
			}
			if (!driven)
				return MakeError(ErrorCode::InvalidArgument, "a vehicle needs a differential that names a wheel (Jolt drives the wheels through one)");
			if (!(std::abs(torqueRatios - 1.0f) < TorqueRatioTolerance))
			{
				return MakeError(ErrorCode::InvalidArgument, "the driven differentials' EngineTorqueRatio sum to {}; they must sum to 1 within {}", torqueRatios,
					TorqueRatioTolerance);
			}
			ENGINE_TRY(CheckLimitedSlip("DifferentialLimitedSlipRatio", settings.DifferentialLimitedSlipRatio));

			if (settings.AntiRollBars.size() > MaxPhysicsVehicleAntiRollBars)
				return MakeError(ErrorCode::InvalidArgument, "a vehicle has at most {} anti-roll bars (got {})", MaxPhysicsVehicleAntiRollBars, settings.AntiRollBars.size());
			for (size_t index = 0; index < settings.AntiRollBars.size(); ++index)
			{
				const PhysicsVehicleAntiRollBar& bar = settings.AntiRollBars[index];
				const std::string name = std::format("anti-roll bar {}", index);
				ENGINE_TRY(CheckWheelIndex(name, bar.LeftWheel, wheelCount, false));
				ENGINE_TRY(CheckWheelIndex(name, bar.RightWheel, wheelCount, false));
				if (bar.LeftWheel == bar.RightWheel)
					return MakeError(ErrorCode::InvalidArgument, "{} names wheel {} twice", name, bar.LeftWheel);
				ENGINE_TRY(CheckRange(name + "'s Stiffness", bar.Stiffness, 0.0f, MaxPhysicsVehicleBarStiffness));
			}
			ENGINE_TRY(CheckRange("MaxPitchRollAngle", settings.MaxPitchRollAngle, 0.0f, JPH::JPH_PI));
			return {};
		}

		static void FillCurve(JPH::LinearCurve& curve, std::span<const PhysicsVehicleCurveKey> keys)
		{
			curve.Clear();
			curve.Reserve(static_cast<JPH::uint>(keys.size()));
			for (const PhysicsVehicleCurveKey& key : keys)
				curve.AddPoint(key.X, key.Y);
		}

		[[nodiscard]] static float ToJoltLimitedSlip(float ratio)
		{
			return ratio == 0.0f ? FLT_MAX : ratio;
		}

		static void FillEngine(JPH::VehicleEngineSettings& engine, const PhysicsVehicleEngine& description)
		{
			engine.mMaxTorque = description.MaxTorque;
			engine.mMinRPM = description.MinRpm;
			engine.mMaxRPM = description.MaxRpm;
			FillCurve(engine.mNormalizedTorque, description.TorqueCurve);
			engine.mInertia = description.Inertia;
			engine.mAngularDamping = description.AngularDamping;
		}

		[[nodiscard]] static JPH::ETransmissionMode ToTransmissionMode(PhysicsVehicleTransmissionMode mode)
		{
			return mode == PhysicsVehicleTransmissionMode::Manual ? JPH::ETransmissionMode::Manual : JPH::ETransmissionMode::Auto;
		}

		// Ends a gear change in progress through Jolt's public state layout (VehicleTransmission::SaveState: the gear, the
		// clutch friction, then the switch, clutch release and latency times left): the gear stays, the clutch is engaged and no
		// timer runs. The timers are private; a change of the transmission's mode or times mid-change would otherwise divide the
		// time left by a new release time of 0, make the clutch negative, or (Manual) freeze them.
		static void EndGearChange(JPH::VehicleTransmission& transmission)
		{
			JPH::StateRecorderImpl recorder;
			recorder.Write(transmission.GetCurrentGear());
			recorder.Write(1.0f);
			recorder.Write(0.0f);
			recorder.Write(0.0f);
			recorder.Write(0.0f);
			recorder.Rewind();
			transmission.RestoreState(recorder);
		}

		static void FillTransmission(JPH::VehicleTransmissionSettings& transmission, const PhysicsVehicleTransmission& description)
		{
			transmission.mMode = ToTransmissionMode(description.Mode);
			transmission.mGearRatios.assign(description.GearRatios.begin(), description.GearRatios.end());
			transmission.mReverseGearRatios.assign(description.ReverseGearRatios.begin(), description.ReverseGearRatios.end());
			transmission.mSwitchTime = description.SwitchTime;
			transmission.mClutchReleaseTime = description.ClutchReleaseTime;
			transmission.mSwitchLatency = description.SwitchLatency;
			transmission.mShiftUpRPM = description.ShiftUpRpm;
			transmission.mShiftDownRPM = description.ShiftDownRpm;
			transmission.mClutchStrength = description.ClutchStrength;
		}

		[[nodiscard]] static JPH::VehicleDifferentialSettings MakeDifferential(const PhysicsVehicleDifferential& description)
		{
			JPH::VehicleDifferentialSettings differential;
			differential.mLeftWheel = description.LeftWheel;
			differential.mRightWheel = description.RightWheel;
			differential.mDifferentialRatio = description.Ratio;
			differential.mLeftRightSplit = description.LeftRightSplit;
			differential.mLimitedSlipRatio = ToJoltLimitedSlip(description.LimitedSlipRatio);
			differential.mEngineTorqueRatio = description.EngineTorqueRatio;
			return differential;
		}

		[[nodiscard]] static JPH::VehicleAntiRollBar MakeAntiRollBar(const PhysicsVehicleAntiRollBar& description)
		{
			JPH::VehicleAntiRollBar bar;
			bar.mLeftWheel = description.LeftWheel;
			bar.mRightWheel = description.RightWheel;
			bar.mStiffness = description.Stiffness;
			return bar;
		}

		// Jolt's settings of one wheel, embedded (the world owns it; the wheel Jolt makes from it keeps a reference).
		[[nodiscard]] static Scope<JPH::WheelSettingsWV> MakeWheelSettings(const PhysicsVehicleWheelDescription& description)
		{
			Scope<JPH::WheelSettingsWV> wheel = CreateScope<JPH::WheelSettingsWV>();
			wheel->SetEmbedded();
			wheel->mPosition = Detail::ToJolt(description.Position);
			wheel->mSuspensionForcePoint = Detail::ToJolt(description.Position);
			wheel->mSuspensionDirection = JPH::Vec3(0.0f, -1.0f, 0.0f);
			wheel->mSteeringAxis = JPH::Vec3(0.0f, 1.0f, 0.0f);
			wheel->mWheelUp = Detail::ToJolt(glm::normalize(description.Up));
			wheel->mWheelForward = Detail::ToJolt(glm::normalize(description.Forward));
			wheel->mSuspensionMinLength = description.SuspensionMinLength;
			wheel->mSuspensionMaxLength = description.SuspensionMaxLength;
			wheel->mSuspensionPreloadLength = description.SuspensionPreload;
			wheel->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, description.SpringFrequency, description.SpringDamping);
			wheel->mRadius = description.Radius;
			wheel->mWidth = description.Width;
			wheel->mInertia = description.Inertia;
			wheel->mAngularDamping = description.AngularDamping;
			wheel->mMaxSteerAngle = description.MaxSteerAngle;
			FillCurve(wheel->mLongitudinalFriction, description.LongitudinalFriction);
			FillCurve(wheel->mLateralFriction, description.LateralFriction);
			wheel->mMaxBrakeTorque = description.MaxBrakeTorque;
			wheel->mMaxHandBrakeTorque = description.MaxHandBrakeTorque;
			return wheel;
		}

		// The wheel-to-ground tester of `cast` on `layer`, embedded (the world owns it; the constraint keeps a reference).
		[[nodiscard]] static Scope<JPH::VehicleCollisionTester> MakeTester(PhysicsVehicleCast cast, JPH::ObjectLayer layer,
			std::span<const PhysicsVehicleWheelDescription> wheels)
		{
			Scope<JPH::VehicleCollisionTester> tester;
			switch (cast)
			{
				case PhysicsVehicleCast::Ray:
					tester = CreateScope<JPH::VehicleCollisionTesterRay>(layer);
					break;
				case PhysicsVehicleCast::Sphere:
				{
					// Jolt's sphere tester takes one radius: the vehicle's smallest wheel's.
					float radius = wheels.front().Radius;
					for (const PhysicsVehicleWheelDescription& wheel : wheels)
						radius = std::min(radius, wheel.Radius);
					tester = CreateScope<JPH::VehicleCollisionTesterCastSphere>(layer, radius);
					break;
				}
				case PhysicsVehicleCast::Cylinder:
					tester = CreateScope<JPH::VehicleCollisionTesterCastCylinder>(layer);
					break;
			}
			tester->SetEmbedded();
			return tester;
		}

		// The live body `body` names in `system`, or null.
		[[nodiscard]] static JPH::Body* FindLiveBody(const JPH::PhysicsSystem& system, BodyHandle body)
		{
			const std::optional<JPH::BodyID> id = Detail::ToJoltBodyID(body);
			if (!id.has_value())
				return nullptr;
			return system.GetBodyLockInterfaceNoLock().TryGetBody(*id);
		}

		// Wakes `id` when it names a live body that is not static.
		static void WakeBody(JPH::PhysicsSystem& system, const JPH::BodyID& id)
		{
			const JPH::Body* body = system.GetBodyLockInterfaceNoLock().TryGetBody(id);
			if (body != nullptr && !body->IsStatic())
				system.GetBodyInterface().ActivateBody(id);
		}

		[[nodiscard]] static const JPH::WheeledVehicleController& GetController(const JPH::VehicleConstraint& constraint)
		{
			return static_cast<const JPH::WheeledVehicleController&>(*constraint.GetController());
		}

		[[nodiscard]] static JPH::WheeledVehicleController& GetController(JPH::VehicleConstraint& constraint)
		{
			return static_cast<JPH::WheeledVehicleController&>(*constraint.GetController());
		}

		// The gear `gear` clamped to the transmission's gears: -reverse count to forward count.
		[[nodiscard]] static int ClampGear(int64_t gear, const JPH::VehicleTransmissionSettings& transmission)
		{
			const int64_t reverse = static_cast<int64_t>(transmission.mReverseGearRatios.size());
			const int64_t forward = static_cast<int64_t>(transmission.mGearRatios.size());
			return static_cast<int>(std::clamp(gear, -reverse, forward));
		}

		[[nodiscard]] static glm::mat4 ToGlm(JPH::Mat44Arg matrix)
		{
			glm::mat4 result(1.0f);
			for (int column = 0; column < 4; ++column)
			{
				const JPH::Vec4 values = matrix.GetColumn4(column);
				result[column] = glm::vec4(values.GetX(), values.GetY(), values.GetZ(), values.GetW());
			}
			return result;
		}

		static void AppendFloatBits(std::vector<std::byte>& bytes, float value)
		{
			const uint32_t bits = std::bit_cast<uint32_t>(value);
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffu));
		}

	}

	void PhysicsWorld::State::RemoveVehicle(Detail::PhysicsVehicleRecord& record)
	{
		System->RemoveStepListener(record.Constraint.get());
		System->RemoveConstraint(record.Constraint.get());
	}

	void PhysicsWorld::State::DestroyVehiclesOf(const JPH::BodyID& body)
	{
		for (auto vehicle = Vehicles.begin(); vehicle != Vehicles.end();)
		{
			if (vehicle->second.Body != body)
			{
				++vehicle;
				continue;
			}
			RemoveVehicle(vehicle->second);
			vehicle = Vehicles.erase(vehicle);
		}
	}

	void PhysicsWorld::State::DestroyAllVehicles()
	{
		for (auto& [handle, record] : Vehicles)
			RemoveVehicle(record);
		Vehicles.clear();
	}

	Result<VehicleHandle> PhysicsWorld::CreateVehicle(const PhysicsVehicleDescription& description)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(Detail::IsPhysicsMainThread(), "PhysicsWorld::CreateVehicle called off the main thread");
		if (!Utils::IsKnownCast(description.Cast))
			return MakeError(ErrorCode::InvalidArgument, "unknown wheel cast {}", std::to_underlying(description.Cast));
		JPH::Body* body = Utils::FindLiveBody(*state.System, description.Body);
		if (body == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the vehicle's body 0x{:08x} names no live body of this world", description.Body.GetValue());
		if (!body->IsDynamic())
			return MakeError(ErrorCode::InvalidArgument, "a vehicle needs a Dynamic body; 0x{:08x} is not one", description.Body.GetValue());
		if (FindVehicle(description.Body).IsValid())
			return MakeError(ErrorCode::InvalidArgument, "the body 0x{:08x} already holds a vehicle", description.Body.GetValue());
		if (description.Wheels.empty() || description.Wheels.size() > MaxPhysicsVehicleWheels)
			return MakeError(ErrorCode::InvalidArgument, "a vehicle needs 1 to {} wheels (got {})", MaxPhysicsVehicleWheels, description.Wheels.size());
		for (size_t index = 0; index < description.Wheels.size(); ++index)
			ENGINE_TRY(Utils::CheckWheel(description.Wheels[index], index));
		ENGINE_TRY(Utils::CheckSettings(description.Settings, description.Wheels.size()));

		Detail::PhysicsVehicleRecord record;
		record.Body = body->GetID();
		JPH::WheeledVehicleControllerSettings controller;
		controller.SetEmbedded();
		Utils::FillEngine(controller.mEngine, description.Settings.Engine);
		Utils::FillTransmission(controller.mTransmission, description.Settings.Transmission);
		for (const PhysicsVehicleDifferential& differential : description.Settings.Differentials)
			controller.mDifferentials.push_back(Utils::MakeDifferential(differential));
		controller.mDifferentialLimitedSlipRatio = Utils::ToJoltLimitedSlip(description.Settings.DifferentialLimitedSlipRatio);

		JPH::VehicleConstraintSettings settings;
		settings.SetEmbedded();
		// The engine's axes (PhysicsVehicle.h): up +Y, forward -Z.
		settings.mUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
		settings.mForward = JPH::Vec3(0.0f, 0.0f, -1.0f);
		settings.mMaxPitchRollAngle = description.Settings.MaxPitchRollAngle;
		for (const PhysicsVehicleWheelDescription& wheel : description.Wheels)
		{
			record.Wheels.push_back(Utils::MakeWheelSettings(wheel));
			settings.mWheels.push_back(record.Wheels.back().get());
		}
		for (const PhysicsVehicleAntiRollBar& bar : description.Settings.AntiRollBars)
			settings.mAntiRollBars.push_back(Utils::MakeAntiRollBar(bar));
		settings.mController = &controller;

		record.Tester = Utils::MakeTester(description.Cast, body->GetObjectLayer(), description.Wheels);
		record.Constraint = CreateScope<JPH::VehicleConstraint>(*body, settings);
		record.Constraint->SetEmbedded();
		record.Constraint->SetVehicleCollisionTester(record.Tester.get());
		// Every wheel is tested at every collision step, awake or asleep (VehicleConstraint.h's defaults, made explicit).
		record.Constraint->SetNumStepsBetweenCollisionTestActive(1);
		record.Constraint->SetNumStepsBetweenCollisionTestInactive(1);
		// The ground's friction combined with the tyre's (Detail::ResolveGroundFriction). Called in the step listener's jobs:
		// it reads the body records, which are written only between steps, and every value is finite and not negative.
		const std::vector<Detail::PhysicsBodyRecord>* records = &state.Bodies;
		record.Constraint->SetCombineFriction([records](JPH::uint /*wheel*/, float& longitudinal, float& lateral, const JPH::Body& ground, const JPH::SubShapeID& subShape)
		{
			const float friction = Detail::ResolveGroundFriction(*records, ground, subShape);
			longitudinal = std::sqrt(longitudinal * friction);
			lateral = std::sqrt(lateral * friction);
		});
		record.Constraint->SetConstraintPriority(description.Priority);
		state.System->AddConstraint(record.Constraint.get());
		state.System->AddStepListener(record.Constraint.get());
		Utils::WakeBody(*state.System, record.Body);

		const VehicleHandle handle(state.NextVehicle++);
		state.Vehicles.emplace(handle.GetValue(), std::move(record));
		return handle;
	}

	void PhysicsWorld::DestroyVehicle(VehicleHandle vehicle)
	{
		State& state = *m_State;
		const auto found = state.Vehicles.find(vehicle.GetValue());
		if (found == state.Vehicles.end())
			return;
		const JPH::BodyID body = found->second.Body;
		state.RemoveVehicle(found->second);
		state.Vehicles.erase(found);
		Utils::WakeBody(*state.System, body);
	}

	bool PhysicsWorld::IsVehicleValid(VehicleHandle vehicle) const
	{
		return m_State->Vehicles.contains(vehicle.GetValue());
	}

	VehicleHandle PhysicsWorld::FindVehicle(BodyHandle body) const
	{
		const std::optional<JPH::BodyID> id = Detail::ToJoltBodyID(body);
		if (!id.has_value())
			return {};
		for (const auto& [handle, record] : m_State->Vehicles)
		{
			if (record.Body == *id)
				return VehicleHandle(handle);
		}
		return {};
	}

	void PhysicsWorld::SetVehicleInput(VehicleHandle vehicle, const PhysicsVehicleInput& input)
	{
		State& state = *m_State;
		const auto found = state.Vehicles.find(vehicle.GetValue());
		const bool isFinite = Detail::IsFinite(input.Forward) && Detail::IsFinite(input.Right) && Detail::IsFinite(input.Brake) && Detail::IsFinite(input.HandBrake)
			&& Detail::IsFinite(input.Clutch);
		ENGINE_CORE_ASSERT(found != state.Vehicles.end(), "PhysicsWorld::SetVehicleInput: {} names no live vehicle of this world", vehicle.GetValue());
		ENGINE_CORE_ASSERT(isFinite, "PhysicsWorld::SetVehicleInput: the input is not finite");
		if (found == state.Vehicles.end() || !isFinite)
			return;
		JPH::WheeledVehicleController& controller = Utils::GetController(*found->second.Constraint);
		JPH::VehicleTransmission& transmission = controller.GetTransmission();
		// Reverse is a negative Forward only in Auto, and only with a reverse gear (Jolt would shift into gear -1 and read a ratio
		// it does not have); Manual drives by Forward's magnitude in its gear's direction (Jolt's [0, 1]).
		const bool canReverse = transmission.mMode == JPH::ETransmissionMode::Auto && !transmission.mReverseGearRatios.empty();
		controller.SetDriverInput(std::clamp(input.Forward, canReverse ? -1.0f : 0.0f, 1.0f), std::clamp(input.Right, -1.0f, 1.0f),
			std::clamp(input.Brake, 0.0f, 1.0f), std::clamp(input.HandBrake, 0.0f, 1.0f));
		if (transmission.mMode == JPH::ETransmissionMode::Manual)
			transmission.Set(Utils::ClampGear(input.Gear, transmission), std::clamp(input.Clutch, 0.0f, 1.0f));
	}

	Status PhysicsWorld::SetVehicleSettings(VehicleHandle vehicle, const PhysicsVehicleSettings& settings)
	{
		State& state = *m_State;
		const auto found = state.Vehicles.find(vehicle.GetValue());
		if (found == state.Vehicles.end())
			return MakeError(ErrorCode::InvalidArgument, "vehicle {} names no live vehicle of this world", vehicle.GetValue());
		Detail::PhysicsVehicleRecord& record = found->second;
		ENGINE_TRY(Utils::CheckSettings(settings, record.Wheels.size()));
		// A bar's wheels never change in place: Jolt writes a wheel's anti-roll impulse only while a bar covers it, and the
		// impulse stays in the wheel's suspension constraint otherwise, a force no state hash sees.
		JPH::VehicleConstraint& constraint = *record.Constraint;
		JPH::VehicleAntiRollBars& bars = constraint.GetAntiRollBars();
		const bool samePairs = std::equal(bars.begin(), bars.end(), settings.AntiRollBars.begin(), settings.AntiRollBars.end(),
			[](const JPH::VehicleAntiRollBar& current, const PhysicsVehicleAntiRollBar& wanted)
		{
			return current.mLeftWheel == wanted.LeftWheel && current.mRightWheel == wanted.RightWheel;
		});
		if (!samePairs)
		{
			return MakeError(ErrorCode::InvalidArgument,
				"vehicle {}'s anti-roll bars name other wheels than its own; only their stiffness changes in place (make the vehicle again)", vehicle.GetValue());
		}

		JPH::WheeledVehicleController& controller = Utils::GetController(constraint);
		JPH::VehicleEngine& engine = controller.GetEngine();
		Utils::FillEngine(engine, settings.Engine);
		engine.ClampRPM();
		JPH::VehicleTransmission& transmission = controller.GetTransmission();
		const bool timingChanged = Utils::ToTransmissionMode(settings.Transmission.Mode) != transmission.mMode
			|| settings.Transmission.SwitchTime != transmission.mSwitchTime || settings.Transmission.ClutchReleaseTime != transmission.mClutchReleaseTime
			|| settings.Transmission.SwitchLatency != transmission.mSwitchLatency;
		if (timingChanged && !transmission.AllowSleep())
			Utils::EndGearChange(transmission);
		Utils::FillTransmission(transmission, settings.Transmission);
		// The current gear stays within the new gears (Jolt indexes the ratios by it).
		const int gear = Utils::ClampGear(transmission.GetCurrentGear(), transmission);
		if (gear != transmission.GetCurrentGear())
			transmission.Set(gear, transmission.GetClutchFriction());
		// The driver's Forward Jolt keeps follows the new transmission (SetVehicleInput's rule): Auto without reverse gears never
		// shifts into gear -1 at the next step, and Manual never drives a negative Forward.
		if (controller.GetForwardInput() < 0.0f && (transmission.mMode == JPH::ETransmissionMode::Manual || transmission.mReverseGearRatios.empty()))
			controller.SetForwardInput(0.0f);
		JPH::WheeledVehicleController::Differentials& differentials = controller.GetDifferentials();
		differentials.clear();
		for (const PhysicsVehicleDifferential& differential : settings.Differentials)
			differentials.push_back(Utils::MakeDifferential(differential));
		controller.SetDifferentialLimitedSlipRatio(Utils::ToJoltLimitedSlip(settings.DifferentialLimitedSlipRatio));
		for (size_t index = 0; index < bars.size(); ++index)
			bars[index].mStiffness = settings.AntiRollBars[index].Stiffness;
		constraint.SetMaxPitchRollAngle(settings.MaxPitchRollAngle);
		Utils::WakeBody(*state.System, record.Body);
		return {};
	}

	void PhysicsWorld::SetVehiclePriority(VehicleHandle vehicle, uint32_t priority)
	{
		if (const auto found = m_State->Vehicles.find(vehicle.GetValue()); found != m_State->Vehicles.end())
			found->second.Constraint->SetConstraintPriority(priority);
	}

	PhysicsVehicleState PhysicsWorld::GetVehicleState(VehicleHandle vehicle) const
	{
		const State& state = *m_State;
		const auto found = state.Vehicles.find(vehicle.GetValue());
		if (found == state.Vehicles.end())
			return {};
		const JPH::VehicleConstraint& constraint = *found->second.Constraint;
		const JPH::WheeledVehicleController& controller = Utils::GetController(constraint);
		PhysicsVehicleState reading;
		reading.EngineRpm = controller.GetEngine().GetCurrentRPM();
		reading.Gear = controller.GetTransmission().GetCurrentGear();
		reading.ClutchFriction = controller.GetTransmission().GetClutchFriction();
		reading.Shifting = controller.GetTransmission().IsSwitchingGear();
		const JPH::BodyLockInterfaceNoLock& locks = state.System->GetBodyLockInterfaceNoLock();
		for (JPH::uint index = 0; index < constraint.GetWheels().size(); ++index)
		{
			const JPH::WheelWV& wheel = static_cast<const JPH::WheelWV&>(*constraint.GetWheel(index));
			PhysicsVehicleWheelState wheelState;
			wheelState.Touching = wheel.HasContact();
			if (wheelState.Touching)
			{
				wheelState.Point = Detail::ToGlm(wheel.GetContactPosition());
				wheelState.Normal = Detail::ToGlm(wheel.GetContactNormal());
				// By its ID, never the wheel's per-step pointer: a ground destroyed since the step is no body now.
				const JPH::BodyID ground = wheel.GetContactBodyID();
				if (const JPH::Body* body = locks.TryGetBody(ground); body != nullptr)
				{
					wheelState.ContactBody = Detail::ToBodyHandle(ground);
					wheelState.ContactSubShape = wheel.GetContactSubShapeID().GetValue();
					wheelState.ContactCollider = Detail::ResolvePhysicsCollider(state.Bodies, *body, wheel.GetContactSubShapeID());
				}
			}
			wheelState.SuspensionLength = wheel.GetSuspensionLength();
			if (state.CollisionStepDelta > 0.0f)
				wheelState.SuspensionForce = wheel.GetSuspensionLambda() / state.CollisionStepDelta;
			wheelState.Angle = wheel.GetRotationAngle();
			wheelState.SteerAngle = wheel.GetSteerAngle();
			wheelState.AngularVelocity = wheel.GetAngularVelocity();
			wheelState.LongitudinalSlip = wheel.mLongitudinalSlip;
			wheelState.LateralSlip = wheel.mLateralSlip;
			wheelState.LocalPose = Utils::ToGlm(constraint.GetWheelLocalTransform(index, JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY()));
			reading.Wheels.push_back(wheelState);
		}
		return reading;
	}

	void PhysicsWorld::AppendVehicleState(VehicleHandle vehicle, std::vector<std::byte>& bytes) const
	{
		const auto found = m_State->Vehicles.find(vehicle.GetValue());
		if (found == m_State->Vehicles.end())
			return;
		const JPH::VehicleConstraint& constraint = *found->second.Constraint;
		const JPH::WheeledVehicleController& controller = Utils::GetController(constraint);
		JPH::StateRecorderImpl recorder;
		controller.GetEngine().SaveState(recorder);
		controller.GetTransmission().SaveState(recorder);
		const std::string recorded = recorder.GetData();
		for (const char byte : recorded)
			bytes.push_back(static_cast<std::byte>(byte));
		for (const JPH::Wheel* wheel : constraint.GetWheels())
		{
			Utils::AppendFloatBits(bytes, wheel->GetAngularVelocity());
			Utils::AppendFloatBits(bytes, wheel->GetRotationAngle());
			Utils::AppendFloatBits(bytes, wheel->GetSteerAngle());
			Utils::AppendFloatBits(bytes, wheel->GetSuspensionLength());
		}
	}

	void PhysicsWorld::RestoreVehicleMotion(VehicleHandle vehicle, const PhysicsVehicleState& state)
	{
		const auto found = m_State->Vehicles.find(vehicle.GetValue());
		bool isFinite = Detail::IsFinite(state.EngineRpm) && Detail::IsFinite(state.ClutchFriction);
		for (const PhysicsVehicleWheelState& wheel : state.Wheels)
			isFinite = isFinite && Detail::IsFinite(wheel.AngularVelocity) && Detail::IsFinite(wheel.Angle);
		ENGINE_CORE_ASSERT(found != m_State->Vehicles.end(), "PhysicsWorld::RestoreVehicleMotion: {} names no live vehicle of this world", vehicle.GetValue());
		ENGINE_CORE_ASSERT(isFinite, "PhysicsWorld::RestoreVehicleMotion: the state is not finite");
		if (found == m_State->Vehicles.end() || !isFinite)
			return;
		JPH::VehicleConstraint& constraint = *found->second.Constraint;
		JPH::WheeledVehicleController& controller = Utils::GetController(constraint);
		controller.GetEngine().SetCurrentRPM(state.EngineRpm);
		JPH::VehicleTransmission& transmission = controller.GetTransmission();
		transmission.Set(Utils::ClampGear(state.Gear, transmission), std::clamp(state.ClutchFriction, 0.0f, 1.0f));
		const size_t count = std::min(state.Wheels.size(), static_cast<size_t>(constraint.GetWheels().size()));
		for (size_t index = 0; index < count; ++index)
		{
			JPH::Wheel& wheel = *constraint.GetWheel(static_cast<JPH::uint>(index));
			wheel.SetAngularVelocity(state.Wheels[index].AngularVelocity);
			// Jolt's own angle, signed in (-2 pi, 2 pi) (fmod keeps the sign of a wheel turning backward), carried as it is.
			wheel.SetRotationAngle(state.Wheels[index].Angle);
		}
	}

	uint32_t PhysicsWorld::GetVehicleCount() const
	{
		return static_cast<uint32_t>(m_State->Vehicles.size());
	}

}
