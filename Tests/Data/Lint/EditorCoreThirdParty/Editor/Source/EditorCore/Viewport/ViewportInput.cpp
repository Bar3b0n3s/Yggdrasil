// Seeded defect: EditorCore is UI-free and includes no third-party headers of its own (Architecture section 3); GLFW
// belongs to Platform.
#include <GLFW/glfw3.h>

namespace Engine {

	int ViewportKeyCount()
	{
		return GLFW_KEY_LAST;
	}

}
