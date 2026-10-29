#include "prism/screeneffect.h"

#include "prism/drawing.h"

#include "prism3ds.h"

// [3DS] The color the top screen is cleared to each frame (a storyboard
// scene's clearcolor: Kung Fu Man's story is drawn on white).

namespace prism {

	static u32 toClearColor(float tR, float tG, float tB) {
		auto byte = [](float v) { return (u32)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255 + 0.5f); };
		return (byte(tR) << 24) | (byte(tG) << 16) | (byte(tB) << 8) | 0xFF;
	}

	void setScreenColor(Color tColor) {
		float r, g, b;
		getRGBFromColor(tColor, &r, &g, &b);
		gN3DSClearColor = toClearColor(r, g, b);
		disableDrawing();
	}

	void setScreenBackgroundColorRGB(float tR, float tG, float tB) {
		gN3DSClearColor = toClearColor(tR, tG, tB);
	}

	void unsetScreenColor() {
		enableDrawing();
	}

}
