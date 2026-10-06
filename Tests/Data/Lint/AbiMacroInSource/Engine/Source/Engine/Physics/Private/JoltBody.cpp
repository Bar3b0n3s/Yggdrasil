// Seeded defects: ABI-relevant configuration macros changed for this translation unit only, before the headers that
// read them. They are set once, for every project, in premake (Architecture section 2.2).
#define JPH_DOUBLE_PRECISION
#undef JPH_PROFILE_ENABLED
#define VULKAN_HPP_NO_EXCEPTIONS
#define GLM_FORCE_LEFT_HANDED
// Control: a per-translation-unit operator switch is allowed.
#define IMGUI_DEFINE_MATH_OPERATORS

namespace Engine {

	int CountBodies()
	{
		return 0;
	}

}
