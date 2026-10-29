#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "prism/input.h"

namespace prism {

	void initInput() {}
	void updateInputPlatform() {}

	// For tests: PRISM_LINUX_HOLD lists the buttons player 1 holds
	// ("up", "a,left"...).
	static int isHeld(int i, const char* tButton) {
		if (i) return 0;
		static const char* held = getenv("PRISM_LINUX_HOLD");
		if (!held) return 0;
		char list[256];
		snprintf(list, sizeof(list), ",%s,", held);
		char wanted[32];
		snprintf(wanted, sizeof(wanted), ",%s,", tButton);
		return strstr(list, wanted) != NULL;
	}

	int hasPressedASingle(int i) { return isHeld(i, "a"); }
	int hasPressedBSingle(int i) { return isHeld(i, "b"); }
	int hasPressedXSingle(int i) { return isHeld(i, "x"); }
	int hasPressedYSingle(int i) { return isHeld(i, "y"); }
	int hasPressedLeftSingle(int i) { return isHeld(i, "left"); }
	int hasPressedRightSingle(int i) { return isHeld(i, "right"); }
	int hasPressedUpSingle(int i) { return isHeld(i, "up"); }
	int hasPressedDownSingle(int i) { return isHeld(i, "down"); }
	int hasPressedLSingle(int i) { return isHeld(i, "l"); }
	int hasPressedRSingle(int i) { return isHeld(i, "r"); }
	int hasPressedStartSingle(int i) { return isHeld(i, "start"); }
	int hasPressedAbortSingle(int /*i*/) { return 0; }

	float getSingleLeftStickNormalizedX(int /*i*/) { return 0; }
	float getSingleLeftStickNormalizedY(int /*i*/) { return 0; }
	float getSingleLNormalized(int /*i*/) { return 0; }
	float getSingleRNormalized(int /*i*/) { return 0; }

	int hasShotGunSingle(int /*i*/) { return 0; }
	Vector3D getShotPositionSingle(int /*i*/) { return Vector3D(0, 0, 0); }

	int isUsingControllerSingle(int i) { return !i && getenv("PRISM_LINUX_HOLD"); }
	void addControllerRumbleSingle(int /*i*/, Duration /*tDuration*/, int /*tFrequency*/, float /*tAmplitude*/) {}
	void turnControllerRumbleOffSingle(int /*i*/) {}

	int hasPressedRawButton(int /*i*/, ControllerButtonPrism /*tButton*/) { return 0; }
	int hasPressedRawKeyboardKey(KeyboardKeyPrism /*tKey*/) { return 0; }
	int hasPressedKeyboardKeyFlank(KeyboardKeyPrism /*tKey*/) { return 0; }
	int hasPressedKeyboardMultipleKeyFlank(int /*tKeyAmount*/, ...) { return 0; }

	ControllerButtonPrism getButtonForController(int /*i*/, ControllerButtonPrism tTargetButton) { return tTargetButton; }
	void setButtonForController(int /*i*/, ControllerButtonPrism /*tTargetButton*/, ControllerButtonPrism /*tButtonValue*/) {}
	KeyboardKeyPrism getButtonForKeyboard(int /*i*/, ControllerButtonPrism /*tTargetButton*/) { return KEYBOARD_A_PRISM; }
	void setButtonForKeyboard(int /*i*/, ControllerButtonPrism /*tTargetButton*/, KeyboardKeyPrism /*tKeyValue*/) {}

	void waitForCharacterFromUserInput(int /*i*/, void(*/*tCB*/)(void*, const std::string&), void* /*tCaller*/) {}
	void cancelWaitingForCharacterFromUserInput(int /*i*/) {}

	void setInputDelay(int /*tInputDelay*/) {}

	bool isMouseInRectangle(const GeoRectangle2D& /*tRectangle*/) { return false; }
	bool hasPressedMouseLeftSingle(int /*i*/) { return false; }
	bool hasPressedMouseRightSingle(int /*i*/) { return false; }

}
