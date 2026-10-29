#include "prism/input.h"

#include <3ds.h>

#include "prism/system.h"

// [3DS] The console's buttons as player 1's controller. Prism's A, B, X and
// Y are the 3DS's A, B, X and Y; ZL and ZR also count as L and R. The
// Circle Pad moves as the D-pad. The touch screen is the mouse (the bottom
// screen's 320x240 on the game's screen).

namespace prism {

	static struct {
		u32 mHeld;
		circlePosition mCircle;
		touchPosition mTouch;
	} gPrismN3DSInputData;

	static int evaluateButtonA(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_A); }
	static int evaluateButtonB(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_B); }
	static int evaluateButtonX(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_X); }
	static int evaluateButtonY(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_Y); }
	static int evaluateButtonL(int i) { return !i && (gPrismN3DSInputData.mHeld & (KEY_L | KEY_ZL)); }
	static int evaluateButtonR(int i) { return !i && (gPrismN3DSInputData.mHeld & (KEY_R | KEY_ZR)); }
	static int evaluateButtonLeft(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_LEFT); }
	static int evaluateButtonRight(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_RIGHT); }
	static int evaluateButtonUp(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_UP); }
	static int evaluateButtonDown(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_DOWN); }
	static int evaluateButtonStart(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_START); }

	typedef int(*InputEvaluationFunction)(int);
	static InputEvaluationFunction gN3DSButtonMapping[] = {
		evaluateButtonA,
		evaluateButtonB,
		evaluateButtonX,
		evaluateButtonY,
		evaluateButtonL,
		evaluateButtonR,
		evaluateButtonLeft,
		evaluateButtonRight,
		evaluateButtonUp,
		evaluateButtonDown,
		evaluateButtonStart,
	};

	static ControllerButtonPrism gButtonMapping[MAXIMUM_CONTROLLER_AMOUNT][CONTROLLER_BUTTON_AMOUNT_PRISM];

	static void resetButtonMapping() {
		for (int i = 0; i < MAXIMUM_CONTROLLER_AMOUNT; i++)
			for (int j = 0; j < CONTROLLER_BUTTON_AMOUNT_PRISM; j++)
				gButtonMapping[i][j] = (ControllerButtonPrism)j;
	}

	void initInput() {
		resetButtonMapping();
		gPrismN3DSInputData.mHeld = 0;
	}

	void updateInputPlatform() {
		hidScanInput();
		// (KEY_LEFT and the others are the D-pad's and the Circle Pad's.)
		gPrismN3DSInputData.mHeld = hidKeysHeld();
		hidCircleRead(&gPrismN3DSInputData.mCircle);
		if (gPrismN3DSInputData.mHeld & KEY_TOUCH) hidTouchRead(&gPrismN3DSInputData.mTouch);
	}

	static int pressed(int i, ControllerButtonPrism tButton) {
		return gN3DSButtonMapping[gButtonMapping[i][tButton]](i);
	}

	int hasPressedASingle(int i) { return pressed(i, CONTROLLER_A_PRISM); }
	int hasPressedBSingle(int i) { return pressed(i, CONTROLLER_B_PRISM); }
	int hasPressedXSingle(int i) { return pressed(i, CONTROLLER_X_PRISM); }
	int hasPressedYSingle(int i) { return pressed(i, CONTROLLER_Y_PRISM); }
	int hasPressedLeftSingle(int i) { return pressed(i, CONTROLLER_LEFT_PRISM); }
	int hasPressedRightSingle(int i) { return pressed(i, CONTROLLER_RIGHT_PRISM); }
	int hasPressedUpSingle(int i) { return pressed(i, CONTROLLER_UP_PRISM); }
	int hasPressedDownSingle(int i) { return pressed(i, CONTROLLER_DOWN_PRISM); }
	int hasPressedLSingle(int i) { return pressed(i, CONTROLLER_L_PRISM); }
	int hasPressedRSingle(int i) { return pressed(i, CONTROLLER_R_PRISM); }
	int hasPressedStartSingle(int i) { return pressed(i, CONTROLLER_START_PRISM); }
	int hasPressedN3DSSelectSingle(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_SELECT); }

	// No quitting by buttons: the HOME Menu closes the game.
	int hasPressedAbortSingle(int /*i*/) { return 0; }

	float getSingleLeftStickNormalizedX(int i) { return i ? 0 : gPrismN3DSInputData.mCircle.dx / 156.0f; }
	float getSingleLeftStickNormalizedY(int i) { return i ? 0 : -gPrismN3DSInputData.mCircle.dy / 156.0f; }
	float getSingleLNormalized(int i) { return evaluateButtonL(i) ? 1.0f : 0.0f; }
	float getSingleRNormalized(int i) { return evaluateButtonR(i) ? 1.0f : 0.0f; }

	int hasShotGunSingle(int /*i*/) { return 0; }
	Vector3D getShotPositionSingle(int /*i*/) { return Vector3D(0, 0, 0); }

	int isUsingControllerSingle(int i) { return !i; }
	void addControllerRumbleSingle(int /*i*/, Duration /*tDuration*/, int /*tFrequency*/, float /*tAmplitude*/) {}
	void turnControllerRumbleOffSingle(int /*i*/) {}

	int hasPressedRawButton(int i, ControllerButtonPrism tButton) { return gN3DSButtonMapping[tButton](i); }
	int hasPressedRawKeyboardKey(KeyboardKeyPrism /*tKey*/) { return 0; }
	int hasPressedKeyboardKeyFlank(KeyboardKeyPrism /*tKey*/) { return 0; }
	int hasPressedKeyboardMultipleKeyFlank(int /*tKeyAmount*/, ...) { return 0; }

	ControllerButtonPrism getButtonForController(int i, ControllerButtonPrism tTargetButton) { return gButtonMapping[i][tTargetButton]; }
	void setButtonForController(int i, ControllerButtonPrism tTargetButton, ControllerButtonPrism tButtonValue) { gButtonMapping[i][tTargetButton] = tButtonValue; }
	KeyboardKeyPrism getButtonForKeyboard(int /*i*/, ControllerButtonPrism /*tTargetButton*/) { return KEYBOARD_A_PRISM; }
	void setButtonForKeyboard(int /*i*/, ControllerButtonPrism /*tTargetButton*/, KeyboardKeyPrism /*tKeyValue*/) {}

	void waitForCharacterFromUserInput(int /*i*/, void(*/*tCB*/)(void*, const std::string&), void* /*tCaller*/) {}
	void cancelWaitingForCharacterFromUserInput(int /*i*/) {}

	void setInputDelay(int /*tInputDelay*/) {}

	// The touch screen, mapped onto the game's screen.
	static Vector2D getTouchPositionLogical() {
		const auto sz = getScreenSize();
		return Vector2D(gPrismN3DSInputData.mTouch.px * sz.x / 320.0, gPrismN3DSInputData.mTouch.py * sz.y / 240.0);
	}

	bool isMouseInRectangle(const GeoRectangle2D& tRectangle) {
		const auto pos = getTouchPositionLogical();
		return pos.x >= tRectangle.mTopLeft.x && pos.x <= tRectangle.mBottomRight.x && pos.y >= tRectangle.mTopLeft.y && pos.y <= tRectangle.mBottomRight.y;
	}

	bool hasPressedMouseLeftSingle(int i) { return !i && (gPrismN3DSInputData.mHeld & KEY_TOUCH); }
	bool hasPressedMouseRightSingle(int /*i*/) { return false; }

}
