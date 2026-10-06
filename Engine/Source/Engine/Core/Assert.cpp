#include "EnginePCH.h"
#include "Engine/Core/Assert.h"

#include "Engine/Core/FatalError.h"

// M1 contract stub (Roadmap rule 3): stream A implements the handler registry, the default handler and the message
// format. Until then a failed assertion terminates through FatalError with the formatted message.

namespace Engine {

	AssertHandler SetAssertHandler(AssertHandler /*handler*/)
	{
		return &DefaultAssertHandler;
	}

	AssertHandler GetAssertHandler()
	{
		return &DefaultAssertHandler;
	}

	void DefaultAssertHandler(const AssertInfo& info)
	{
		FatalError(FatalErrorKind::Assert, info.Message);
	}

	std::string FormatAssertInfo(const AssertInfo& /*info*/)
	{
		return {};
	}

	std::string_view AssertKindToString(AssertKind /*kind*/)
	{
		return {};
	}

	namespace Detail {

		void ReportAssertFailure(AssertKind /*kind*/, bool /*isClient*/, std::string_view /*expression*/, std::string_view message,
			const std::source_location& /*location*/)
		{
			FatalError(FatalErrorKind::Assert, message);
		}

	}

}
