#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <cstdint>
#include <optional>
#include <string_view>

// The joint proxies' methods (M37, Docs/Decisions/0054-m37-joints.md decision 8; Architecture §11.5): HingeJoint:GetAngle,
// SliderJoint:GetPosition, DistanceJoint:GetDistance and each type's GetForce (and, but on DistanceJoint, GetTorque): reads of
// the play session's joint (PhysicsSystem::GetJointInfo) as the last physics step left it, and a teleport since, usable in
// every callback, the fixed step's included. Every field is read and written through the generic proxy; a write takes effect
// at the next physics pre-step (§9.2).

namespace Engine {

	namespace Utils {

		// Which reading a joint method returns.
		enum class JointReading : uint8_t
		{
			Angle,
			Position,
			Distance,
			Force,
			Torque
		};

		// The registry name of the component of `type` (the proxy a method receives).
		constexpr std::string_view GetJointProxyName(PhysicsJointType type)
		{
			switch (type)
			{
				case PhysicsJointType::Hinge:    return "HingeJoint";
				case PhysicsJointType::Slider:   return "SliderJoint";
				case PhysicsJointType::Distance: return "DistanceJoint";
				case PhysicsJointType::Fixed:    return "FixedJoint";
			}
			return "";
		}

		// The reading `Reading` of the receiver's joint in force: a joint component of `Type` of a live entity (a read of the
		// physics system's state, refused for an entity pending destruction as RigidBody's are). Errors: the receiver's;
		// "physics is unavailable" without the session's physics
		// (an edit evaluation); "entity has no joint in force" for a refused, broken, disabled or not yet created joint.
		template<PhysicsJointType Type, JointReading Reading>
		static int JointRead(ScriptCall& call)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponent(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != GetJointProxyName(Type))
				Lua::RaiseError(call, "wrong component receiver");
			const PhysicsSystem* physics = host.GetPhysics();
			if (physics == nullptr)
				Lua::RaiseError(call, "physics is unavailable");
			const std::optional<PhysicsJointReport> report = physics->GetJointInfo(proxy.Entity.ID);
			if (!report.has_value() || !report->Created)
				Lua::RaiseError(call, "entity has no joint in force");
			switch (Reading)
			{
				case JointReading::Angle:    Lua::Push(call, report->Angle); break;
				case JointReading::Position: Lua::Push(call, report->Position); break;
				case JointReading::Distance: Lua::Push(call, report->Distance); break;
				case JointReading::Force:    Lua::Push(call, report->Force); break;
				case JointReading::Torque:   Lua::Push(call, report->Torque); break;
			}
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterJoints(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			constexpr std::string_view ForceDescription =
				"The force the joint applied over the last collision step of the last physics step, in newtons, as the break test measures it "
				"against BreakForce.";
			constexpr std::string_view TorqueDescription =
				"The torque the joint applied over the last collision step of the last physics step, in newton metres, as the break test "
				"measures it against BreakTorque.";
			api.Type("HingeJoint")
				.Method("GetAngle", Utils::JointRead<PhysicsJointType::Hinge, Utils::JointReading::Angle>, "(self: HingeJoint) -> number",
					"The hinge's angle in degrees from the pose it was made in, positive by the right-hand rule about Axis, after the last "
					"physics step.",
					read)
				.Method("GetForce", Utils::JointRead<PhysicsJointType::Hinge, Utils::JointReading::Force>, "(self: HingeJoint) -> number", ForceDescription, read)
				.Method("GetTorque", Utils::JointRead<PhysicsJointType::Hinge, Utils::JointReading::Torque>, "(self: HingeJoint) -> number", TorqueDescription,
					read);
			api.Type("SliderJoint")
				.Method("GetPosition", Utils::JointRead<PhysicsJointType::Slider, Utils::JointReading::Position>, "(self: SliderJoint) -> number",
					"The slider's position in metres along Axis from the pose it was made in, after the last physics step.", read)
				.Method("GetForce", Utils::JointRead<PhysicsJointType::Slider, Utils::JointReading::Force>, "(self: SliderJoint) -> number", ForceDescription,
					read)
				.Method("GetTorque", Utils::JointRead<PhysicsJointType::Slider, Utils::JointReading::Torque>, "(self: SliderJoint) -> number",
					TorqueDescription, read);
			api.Type("DistanceJoint")
				.Method("GetDistance", Utils::JointRead<PhysicsJointType::Distance, Utils::JointReading::Distance>, "(self: DistanceJoint) -> number",
					"The distance in metres between the joint's two anchors after the last physics step.", read)
				.Method("GetForce", Utils::JointRead<PhysicsJointType::Distance, Utils::JointReading::Force>, "(self: DistanceJoint) -> number",
					ForceDescription, read);
			api.Type("FixedJoint")
				.Method("GetForce", Utils::JointRead<PhysicsJointType::Fixed, Utils::JointReading::Force>, "(self: FixedJoint) -> number", ForceDescription, read)
				.Method("GetTorque", Utils::JointRead<PhysicsJointType::Fixed, Utils::JointReading::Torque>, "(self: FixedJoint) -> number", TorqueDescription,
					read);
			return {};
		}

	}

}
