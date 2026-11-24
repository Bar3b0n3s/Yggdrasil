#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

// The vehicle proxies' methods (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Architecture §9.11, §11.5):
// WheeledVehicle:GetSpeed, GetEngineRpm, GetGear and IsShifting, and VehicleWheel:IsTouching, GetContact,
// GetSuspensionLength, GetSuspensionForce, GetRotation, GetSteerAngle, GetAngularVelocity and GetSlip: reads of the play
// session's vehicle (PhysicsSystem::GetVehicleInfo) as the last physics step left it, usable in every callback, the fixed
// step's included. Every field (the driver input among them) is read and written through the generic proxy; a write takes
// effect at the next physics pre-step (§9.2, §9.11).

namespace Engine {

	namespace Utils {

		// Which reading a WheeledVehicle method returns.
		enum class VehicleReading : uint8_t
		{
			Speed,
			EngineRpm,
			Gear,
			Shifting
		};

		// Which reading a VehicleWheel method returns.
		enum class WheelReading : uint8_t
		{
			Touching,
			Contact,
			SuspensionLength,
			SuspensionForce,
			Rotation,
			SteerAngle,
			AngularVelocity,
			Slip
		};

		// The report of the vehicle in force that the receiver, a `Component` proxy of a live entity, is or is a wheel of (a read
		// of the physics system's state, refused for an entity pending destruction as RigidBody's are). Errors: the receiver's;
		// "physics is unavailable" without the session's physics (an edit evaluation); "entity has no vehicle in force" for a
		// vehicle that is refused, disabled or not yet created, and for a wheel that is not one of its vehicle's wheels in force.
		static PhysicsVehicleReport ReadVehicleReport(ScriptCall& call, std::string_view component)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponent(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != component)
				Lua::RaiseError(call, "wrong component receiver");
			const PhysicsSystem* physics = host.GetPhysics();
			if (physics == nullptr)
				Lua::RaiseError(call, "physics is unavailable");
			std::optional<PhysicsVehicleReport> report = physics->GetVehicleInfo(proxy.Entity.ID);
			if (!report.has_value() || !report->Created)
				Lua::RaiseError(call, "entity has no vehicle in force");
			return std::move(*report);
		}

		template<VehicleReading Reading>
		static int VehicleRead(ScriptCall& call)
		{
			const PhysicsVehicleReport report = ReadVehicleReport(call, "WheeledVehicle");
			switch (Reading)
			{
				case VehicleReading::Speed:     Lua::Push(call, report.Speed); break;
				case VehicleReading::EngineRpm: Lua::Push(call, report.EngineRpm); break;
				case VehicleReading::Gear:      Lua::Push(call, report.Gear); break;
				case VehicleReading::Shifting:  Lua::Push(call, report.Shifting); break;
			}
			return 1;
		}

		template<WheelReading Reading>
		static int WheelRead(ScriptCall& call)
		{
			const PhysicsVehicleReport report = ReadVehicleReport(call, "VehicleWheel");
			const UUID entity = Lua::Check<ScriptProxyIdentity>(call, 1).Entity.ID;
			const auto found = std::find_if(report.Wheels.begin(), report.Wheels.end(), [entity](const PhysicsVehicleWheelReport& wheel)
			{
				return wheel.Entity == entity;
			});
			if (found == report.Wheels.end())
				return Lua::RaiseError(call, "entity has no vehicle in force");
			const PhysicsVehicleWheelReport& wheel = *found;
			switch (Reading)
			{
				case WheelReading::Touching:         Lua::Push(call, wheel.Touching); return 1;
				case WheelReading::SuspensionLength: Lua::Push(call, wheel.SuspensionLength); return 1;
				case WheelReading::SuspensionForce:  Lua::Push(call, wheel.SuspensionForce); return 1;
				case WheelReading::Rotation:         Lua::Push(call, wheel.Rotation); return 1;
				case WheelReading::SteerAngle:       Lua::Push(call, wheel.SteerAngle); return 1;
				case WheelReading::AngularVelocity:  Lua::Push(call, wheel.AngularVelocity); return 1;
				case WheelReading::Slip:
					Lua::Push(call, wheel.LongitudinalSlip);
					Lua::Push(call, wheel.LateralSlip);
					return 2;
				case WheelReading::Contact:
					if (!wheel.Touching)
					{
						Lua::PushNil(call);
						Lua::PushNil(call);
						Lua::PushNil(call);
						return 3;
					}
					Lua::Push(call, wheel.Point);
					Lua::Push(call, wheel.Normal);
					if (wheel.Collider.IsValid())
						Lua::Push(call, ScriptEntityIdentity{ wheel.Collider, call.Engine->GetHost().GetSceneGeneration() });
					else
						Lua::PushNil(call);
					return 3;
			}
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterVehicles(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Type("WheeledVehicle")
				.Method("GetSpeed", Utils::VehicleRead<Utils::VehicleReading::Speed>, "(self: WheeledVehicle) -> number",
					"The body's speed along its forward (local -Z) in metres per second after the last physics step, negative while it rolls "
					"backward.",
					read)
				.Method("GetEngineRpm", Utils::VehicleRead<Utils::VehicleReading::EngineRpm>, "(self: WheeledVehicle) -> number",
					"The engine's revolutions per minute after the last physics step, between MinRpm and MaxRpm.", read)
				.Method("GetGear", Utils::VehicleRead<Utils::VehicleReading::Gear>, "(self: WheeledVehicle) -> number",
					"The gear the transmission is in after the last physics step: 1 and up forward, -1 and down reverse, 0 neutral. In Manual "
					"it follows Gear (clamped to the ratios); in Auto the transmission chooses it.",
					read)
				.Method("IsShifting", Utils::VehicleRead<Utils::VehicleReading::Shifting>, "(self: WheeledVehicle) -> boolean",
					"Whether a gear change is in progress after the last physics step (an Auto shift's SwitchTime and ClutchReleaseTime).", read);
			api.Type("VehicleWheel")
				.Method("IsTouching", Utils::WheelRead<Utils::WheelReading::Touching>, "(self: VehicleWheel) -> boolean",
					"Whether the wheel's cast found ground within its suspension in the last physics step.", read)
				.Method("GetContact", Utils::WheelRead<Utils::WheelReading::Contact>, "(self: VehicleWheel) -> (vector?, vector?, Entity?)",
					"Where the wheel touched the ground in the last physics step: the world point, the ground's normal and the collider entity "
					"(nil when that collider or its body's owner is disabled or destroyed); three nils while the wheel is in the air. "
					"Physics.GetSurface reads that collider's friction.",
					read)
				.Method("GetSuspensionLength", Utils::WheelRead<Utils::WheelReading::SuspensionLength>, "(self: VehicleWheel) -> number",
					"The suspension's length in metres after the last physics step, from its attachment point to the wheel's centre: "
					"SuspensionMaxLength in the air, shorter as the spring compresses, never below SuspensionMinLength.",
					read)
				.Method("GetSuspensionForce", Utils::WheelRead<Utils::WheelReading::SuspensionForce>, "(self: VehicleWheel) -> number",
					"The force the suspension pushed the body with in the last physics step, in newtons (0 in the air).", read)
				.Method("GetRotation", Utils::WheelRead<Utils::WheelReading::Rotation>, "(self: VehicleWheel) -> number",
					"The wheel's spin about its axle in degrees, in [0, 360), after the last physics step.", read)
				.Method("GetSteerAngle", Utils::WheelRead<Utils::WheelReading::SteerAngle>, "(self: VehicleWheel) -> number",
					"The wheel's steer angle in degrees after the last physics step, positive to the right as Steering is (Steering times "
					"MaxSteerAngle).",
					read)
				.Method("GetAngularVelocity", Utils::WheelRead<Utils::WheelReading::AngularVelocity>, "(self: VehicleWheel) -> number",
					"The wheel's spin speed about its axle in degrees per second after the last physics step, positive rolling forward.", read)
				.Method("GetSlip", Utils::WheelRead<Utils::WheelReading::Slip>, "(self: VehicleWheel) -> (number, number)",
					"The tyre's slip in the last physics step, 0 and 0 in the air: the longitudinal slip ratio (how far the tread's speed differs "
					"from the ground's, relative to it: 0 rolling freely, larger as the wheel spins or locks) and the lateral slip angle in "
					"degrees, in [0, 90], which LongitudinalFriction and LateralFriction turn into grip.",
					read);
			return {};
		}

	}

}
