// Control: the script type checker is the one EditorCore file that may include LuauAnalysis (Architecture section 3).
#include <Luau/Frontend.h>

namespace Engine {

	bool TypeCheckerAvailable()
	{
		return true;
	}

}
