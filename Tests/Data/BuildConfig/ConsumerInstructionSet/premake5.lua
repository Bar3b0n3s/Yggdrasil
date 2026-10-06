-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/ConsumerInstructionSet
--
-- Defect: the Tests project is compiled for AVX2 while JoltPhysics is built for SSE4.2. Jolt/Core/Core.h
-- then defines JPH_USE_AVX2 (and more) in Tests only, so Jolt's inline vector code differs between the library and
-- this consumer, and JPH_VERSION_ID does not encode it.

include "../../../.."

workspace (WorkspaceName)

project "Tests"
	filter "toolset:msc*"
		buildoptions { "/arch:AVX2" }

	filter { "toolset:not msc*", "architecture:x86_64" }
		buildoptions { "-mavx2", "-mfma" }

	filter {}
