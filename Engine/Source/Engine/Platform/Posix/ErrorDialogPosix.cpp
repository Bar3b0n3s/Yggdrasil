#include "EnginePCH.h"
#include "Engine/Platform/ErrorDialog.h"

// The error dialog on macOS: CFUserNotificationDisplayAlert, a modal stop alert that needs no window and no running
// application event loop. Linux has no standard dialog under X11 (Architecture §4.6), so it shows none: the message
// stays in the log and on stderr.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#if defined(ENGINE_PLATFORM_MACOS)
		#include <CoreFoundation/CoreFoundation.h>
	#endif

namespace Engine {

	#if defined(ENGINE_PLATFORM_MACOS)

	namespace {

		// A CFString built from UTF-8 text, released on destruction. Text that is not valid UTF-8 is read as Mac Roman,
		// which accepts every byte sequence, so a garbled message is still shown rather than dropped.
		class ScopedCFString
		{
		public:
			explicit ScopedCFString(std::string_view text)
			{
				const auto* bytes = reinterpret_cast<const UInt8*>(text.data());
				const auto length = static_cast<CFIndex>(text.size());
				m_String = CFStringCreateWithBytes(kCFAllocatorDefault, bytes, length, kCFStringEncodingUTF8, false);
				if (m_String == nullptr)
					m_String = CFStringCreateWithBytes(kCFAllocatorDefault, bytes, length, kCFStringEncodingMacRoman, false);
			}

			~ScopedCFString()
			{
				if (m_String != nullptr)
					CFRelease(m_String);
			}

			ScopedCFString(const ScopedCFString&) = delete;
			ScopedCFString& operator=(const ScopedCFString&) = delete;

			[[nodiscard]] CFStringRef Get() const { return m_String; }
		private:
			CFStringRef m_String = nullptr;
		};

	}

	bool ShowErrorDialog(std::string_view title, std::string_view message)
	{
		const ScopedCFString titleString(title);
		const ScopedCFString messageString(message);
		if (titleString.Get() == nullptr || messageString.Get() == nullptr)
			return false;

		// A timeout of 0 waits until the user dismisses the alert; a null button title shows the default "OK".
		CFOptionFlags response = 0;
		const SInt32 result = CFUserNotificationDisplayAlert(0.0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr,
			titleString.Get(), messageString.Get(), nullptr, nullptr, nullptr, &response);
		return result == 0;
	}

	#else

	bool ShowErrorDialog(std::string_view /*title*/, std::string_view /*message*/)
	{
		return false;
	}

	#endif

}

#endif
