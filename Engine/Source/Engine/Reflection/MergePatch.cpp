#include "EnginePCH.h"
#include "Engine/Reflection/MergePatch.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/Private/ReflectionWalk.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// RFC 7386 section 2, MergePatch(Target, Patch). The recursion follows the patch, whose depth is bounded.
		static Json MergePatchRecursive(const Json& target, const Json& patch)
		{
			if (!patch.is_object())
				return patch;

			static const Json Absent;
			Json result = target.is_object() ? target : Json::object();
			for (auto member = patch.begin(); member != patch.end(); ++member)
			{
				if (member->is_null())
				{
					result.erase(member.key());
					continue;
				}
				const auto existing = result.find(member.key());
				if (existing != result.end())
					*existing = MergePatchRecursive(*existing, *member);
				else
					result[member.key()] = MergePatchRecursive(Absent, *member);
			}
			return result;
		}

		static Json CreateMergePatchRecursive(const Json& source, const Json& target)
		{
			if (!source.is_object() || !target.is_object())
				return target;

			Json patch = Json::object();
			for (auto member = target.begin(); member != target.end(); ++member)
			{
				const auto previous = source.find(member.key());
				if (previous == source.end())
					patch[member.key()] = *member;
				else if (*previous != *member)
					patch[member.key()] = CreateMergePatchRecursive(*previous, *member);
			}
			for (auto member = source.begin(); member != source.end(); ++member)
			{
				if (!target.contains(member.key()))
					patch[member.key()] = nullptr;
			}
			return patch;
		}

	}

	Json ApplyMergePatch(const Json& target, const Json& patch)
	{
		ENGINE_CORE_ASSERT(Utils::ComputeJsonDepth(target) <= MaxJsonDepth && Utils::ComputeJsonDepth(patch) <= MaxJsonDepth,
			"ApplyMergePatch needs inputs nested at most {} levels deep", MaxJsonDepth);
		return Utils::MergePatchRecursive(target, patch);
	}

	Json CreateMergePatch(const Json& source, const Json& target)
	{
		ENGINE_CORE_ASSERT(Utils::ComputeJsonDepth(source) <= MaxJsonDepth && Utils::ComputeJsonDepth(target) <= MaxJsonDepth,
			"CreateMergePatch needs inputs nested at most {} levels deep", MaxJsonDepth);
		return Utils::CreateMergePatchRecursive(source, target);
	}

}
