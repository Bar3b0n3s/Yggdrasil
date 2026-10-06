-- premake 5.0.0's xcode4 generator writes `includedirs` only to USER_HEADER_SEARCH_PATHS, which clang searches for
-- quoted includes alone (-iquote). Vendored libraries include their own headers with angle brackets (<Jolt/Jolt.h>,
-- <nvrhi/nvrhi.h>, <spdlog/spdlog.h>, <Luau/NotNull.h>), so those directories are mirrored into HEADER_SEARCH_PATHS
-- (-I). That is what the gmake, ninja and Visual Studio generators already do with `includedirs`, so every generator
-- now resolves includes the same way.

if _ACTION == "xcode4" then
	local xcode = require("xcode")

	premake.override(xcode, "overrideSettings", function(base, settings, overrides)
		local userPaths = settings["USER_HEADER_SEARCH_PATHS"]
		if type(userPaths) == "table" and #userPaths > 0 and settings["HEADER_SEARCH_PATHS"] == nil then
			settings["HEADER_SEARCH_PATHS"] = table.join(userPaths, { "$(inherited)" })
		end
		base(settings, overrides)
	end)
end
