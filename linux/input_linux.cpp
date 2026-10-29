#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "prism/input.h"

namespace prism {

	void initInput() {}

	// For tests: PRISM_LINUX_PULSE presses buttons for player 1 one frame in
	// a given number ("x:20": x every 20 frames).
	static unsigned int gFrame;
	void updateInputPlatform() { gFrame++; }

	static int isPulsed(const char* tButton) {
		static const char* pulse = getenv("PRISM_LINUX_PULSE");
		if (!pulse) return 0;
		const char* colon = strchr(pulse, ':');
		const int period = colon ? atoi(colon + 1) : 0;
		if (period <= 0 || gFrame % period) return 0;
		char list[256];
		snprintf(list, sizeof(list), ",%.*s,", (int)(colon - pulse), pulse);
		char wanted[32];
		snprintf(wanted, sizeof(wanted), ",%s,", tButton);
		return strstr(list, wanted) != NULL;
	}

	// For tests: PRISM_LINUX_HOLD lists the buttons player 1 holds
	// ("up", "a,left"...).
	static int isHeld(int i, const char* tButton) {
		if (i) return 0;
		if (isPulsed(tButton)) return 1;
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

	int isUsingControllerSingle(int i) { return !i && (getenv("PRISM_LINUX_HOLD") || getenv("PRISM_LINUX_PULSE")); }
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
