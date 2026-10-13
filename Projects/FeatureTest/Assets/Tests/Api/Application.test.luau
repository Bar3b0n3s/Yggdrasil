--!strict
-- The Application module (§11.5). Application.Quit is exercised by the QuitApp suite, whose ExpectQuit accepts it.
return Test.Suite("Api.Application", function()
	Test.Case("the application reports its identity, focus and mode", function()
		local editor = Application.IsEditor()
		Test.ExpectEqual(type(editor), "boolean")
		Test.ExpectEqual(type(Application.IsHeadless()), "boolean")
		Test.ExpectEqual(type(Application.IsFocused()), "boolean")
		local platform = Application.GetPlatform()
		Test.Expect(platform == "Windows" or platform == "Linux" or platform == "macOS", "a known platform: " .. platform)
		Test.Expect(string.match(Application.GetVersion(), "^%d+%.%d+%.%d+") ~= nil, "a semantic version: " .. Application.GetVersion())
		-- A test runs with the same answers on every call of the session.
		Test.ExpectEqual(Application.IsEditor(), editor)
		Test.ExpectEqual(Application.GetPlatform(), platform)
	end)

	Test.Case("a test session's view is the project's window size", function()
		Test.ExpectEqual(Application.GetWindowSize(), vector.create(1280, 720, 0))
	end)
end)
