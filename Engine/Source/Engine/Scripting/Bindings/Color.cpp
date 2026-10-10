#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace Engine {

	namespace Utils {

		static int ColorNew(ScriptCall& call)
		{
			const float red = Lua::Check<float>(call, 1);
			const float green = Lua::Check<float>(call, 2);
			const float blue = Lua::Check<float>(call, 3);
			const float alpha = Lua::IsNoneOrNil(call, 4) ? 1.0f : Lua::Check<float>(call, 4);
			Lua::Push(call, glm::vec4(red, green, blue, alpha));
			return 1;
		}

		static int ColorHexDigit(char value)
		{
			if (value >= '0' && value <= '9')
				return value - '0';
			if (value >= 'a' && value <= 'f')
				return value - 'a' + 10;
			if (value >= 'A' && value <= 'F')
				return value - 'A' + 10;
			return -1;
		}

		static int ColorFromHex(ScriptCall& call)
		{
			const std::string text = Lua::Check<std::string>(call, 1);
			if ((text.size() != 7 && text.size() != 9) || text.front() != '#')
				return Lua::RaiseError(call, "expected #RRGGBB or #RRGGBBAA");
			glm::vec4 value(1.0f);
			const int channels = text.size() == 9 ? 4 : 3;
			for (int channel = 0; channel < channels; ++channel)
			{
				const size_t offset = 1 + static_cast<size_t>(channel) * 2;
				const int high = ColorHexDigit(text[offset]);
				const int low = ColorHexDigit(text[offset + 1]);
				if (high < 0 || low < 0)
					return Lua::RaiseError(call, "expected hexadecimal digits in #RRGGBB or #RRGGBBAA");
				value[channel] = static_cast<float>(high * 16 + low) / 255.0f;
			}
			Lua::Push(call, value);
			return 1;
		}

		static int ColorLerp(ScriptCall& call)
		{
			const glm::vec4 from = Lua::Check<glm::vec4>(call, 1);
			const glm::vec4 to = Lua::Check<glm::vec4>(call, 2);
			const double amount = std::clamp(Lua::Check<double>(call, 3), 0.0, 1.0);
			glm::vec4 value(0.0f);
			for (int channel = 0; channel < 4; ++channel)
			{
				// Double interpolation keeps opposite HDR endpoints from overflowing float before cancellation;
				// explicit operations give the same evaluation order on every standard library.
				const double start = from[channel];
				const double end = to[channel];
				value[channel] = amount == 0.0 ? from[channel] : (amount == 1.0 ? to[channel] : static_cast<float>(start + (end - start) * amount));
			}
			Lua::Push(call, value);
			return 1;
		}

		template<int Channel>
		static int ColorGet(ScriptCall& call)
		{
			Lua::Push(call, Lua::Check<glm::vec4>(call, 1)[Channel]);
			return 1;
		}

		template<int Channel>
		static int ColorSet(ScriptCall& call)
		{
			// Validate the whole candidate before borrowing storage. A rejected assignment changes no channel.
			glm::vec4 candidate = Lua::Check<glm::vec4>(call, 1);
			candidate[Channel] = Lua::Check<float>(call, 2);
			Lua::CheckColorStorage(call, 1) = candidate;
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterColor(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions pure{ .Environments = ScriptApiEnvironment::All, .Mutates = false, .SetterMutates = false };
			api.Type("Color", "A local linear RGBA float value. Channels are finite and unclamped, allowing HDR values.")
				.Constructor("New", &Utils::ColorNew, "(r: number, g: number, b: number, a: number?) -> Color",
					"Creates a linear color; alpha defaults to one. Channels must fit finite engine floats.", pure)
				.Constructor("FromHex", &Utils::ColorFromHex, "(hex: string) -> Color",
					"Reads #RRGGBB or #RRGGBBAA, case-insensitively, as linear channel bytes divided by 255; absent alpha is one.", pure)
				.Constructor("Lerp", &Utils::ColorLerp, "(a: Color, b: Color, t: number) -> Color",
					"Interpolates all four linear channels with t clamped to [0, 1], producing a new color.", pure)
				.Property("r", &Utils::ColorGet<0>, &Utils::ColorSet<0>, "number", "The local linear red channel, finite and unclamped.", pure)
				.Property("g", &Utils::ColorGet<1>, &Utils::ColorSet<1>, "number", "The local linear green channel, finite and unclamped.", pure)
				.Property("b", &Utils::ColorGet<2>, &Utils::ColorSet<2>, "number", "The local linear blue channel, finite and unclamped.", pure)
				.Property("a", &Utils::ColorGet<3>, &Utils::ColorSet<3>, "number", "The local alpha channel, finite and unclamped.", pure);
			return {};
		}

	}

}
