-- miniaudio (https://github.com/mackron/miniaudio), vendored at 0.11.25. See VENDOR.md.
-- The implementation translation unit is the local miniaudio_vorbis.c (VENDOR.md, "Local additions"): upstream's
-- miniaudio.c (MINIAUDIO_IMPLEMENTATION + miniaudio.h) with stb_vorbis (Vendor/stb/stb_vorbis.c) included around it,
-- the route miniaudio.h documents for Ogg Vorbis decoding. miniaudio.c stays vendored unmodified but is not compiled.
--
-- Consumers include <miniaudio.h> with includedirs { "%{wks.location}/Vendor/miniaudio" } (adjust to the
-- workspace layout) and should use the same configuration defines as this project (see VENDOR.md):
--     defines { "MA_NO_ENCODING" }
--     filter "system:macosx"  defines { "MA_NO_RUNTIME_LINKING" }
-- Executables must link: Linux { "dl", "pthread", "m" };
-- macOS { "CoreFoundation.framework", "CoreAudio.framework", "AudioToolbox.framework" }; Windows: nothing.

project "miniaudio"
	kind "StaticLib"
	language "C"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- Mirrors add_library(miniaudio miniaudio.c miniaudio.h) in upstream CMakeLists.txt, with the local implementation
	-- translation unit in place of miniaudio.c (Ogg Vorbis through stb_vorbis, VENDOR.md).
	files
	{
		"miniaudio.h",
		"miniaudio_vorbis.c",
	}

	-- "../stb" holds stb_vorbis.c, which miniaudio_vorbis.c includes; nothing else of stb is used here.
	includedirs
	{
		".",
		"../stb",
	}

	-- Configuration defines. These must match what consumers see when they include miniaudio.h.
	-- Kept: device IO, decoding (WAV/FLAC/MP3), resource manager, node graph, engine, generation, threading.
	defines
	{
		"MA_NO_ENCODING",	-- The engine never writes audio files; removes the WAV encoder (ma_encoder_*).
	}

	filter "system:windows"
		systemversion "latest"
		-- Backends (WASAPI, DirectSound, WinMM) load their system DLLs at runtime; nothing to link.

	-- Upstream's implementation has exactly one MSVC warning at the default level: C4244 (64-to-32-bit conversion)
	-- in the dr_wav "smpl" chunk parser, ma_dr_wav__read_smpl_to_metadata_obj. It is harmless (see VENDOR.md) and
	-- the source stays unmodified, so it is disabled for this vendored translation unit only.
	filter { "toolset:msc*", "files:miniaudio_vorbis.c" }
		disablewarnings { "4244" }

	filter "system:linux"
		pic "On"
		-- Backends (PulseAudio/PipeWire, ALSA, JACK) are loaded at runtime with dlopen(); no -dev packages
		-- are required to build. Consumers link dl, pthread and m.

	filter "system:macosx"
		-- Link CoreAudio/AudioToolbox/CoreFoundation directly instead of dlopen()-ing them at runtime.
		-- Recommended by upstream (miniaudio.h "2.2. macOS and iOS") for hardened-runtime / notarized apps.
		-- Consumers must link CoreFoundation, CoreAudio and AudioToolbox frameworks.
		defines { "MA_NO_RUNTIME_LINKING" }

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		symbols "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"

	filter {}
