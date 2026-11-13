#include "prism/drawing.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <set>
#include <algorithm>

#include "prism/log.h"
#include "prism/system.h"
#include "prism/datastructures.h"
#include "prism/memoryhandler.h"
#include "prism/math.h"
#include "prism/geometry.h"

#include "prism3ds.h"
#include "prism3ds_shbin.h"
#include "prism3dsperf.h"

// [3DS] Drawing with citro3d, on the top screen. As on the Vita, sprites are
// kept with their state and drawn sorted by z when the frame ends; the
// game's screen (getScreenSize, 320x240 for Dolmexica) is scaled to fit the
// top screen's 400x240, centered; in the wide mode (800x240) the GPU draws
// it with twice the pixels across, so big sprites keep more of their
// detail.
//
// Each sprite is two triangles in one vertex buffer; sprites in a row with
// the same texture and state are drawn together. The state:
// - color: the vertex color times the texture (or the vertex color alone,
//   "solid"), the texture inverted, plus a color offset;
// - blending: normal, addition and subtraction (the destination times its
//   transparency, plus or minus the source times its alpha, as Windows's
//   shader), or the source alone ("one");
// - texels with alpha 0 are not drawn (in additive blending the
//   destination would be dimmed there), except in "one" blending.

namespace prism {

	u32 gN3DSPalettes[4][256];
	u32 gN3DSClearColor = 0x000000FF;
	int gN3DSPaletteVersions[4];

	typedef struct {
		float a;
		float r;
		float g;
		float b;
		float rOffset;
		float gOffset;
		float bOffset;

		Matrix4D mTransformationMatrix;

		Vector mEffectStack;

		int mIsDisabled;

		BlendType mBlendType;
		int mIsColorSolid;
		int mIsColorInversed;
		float mDestAlpha;
		float mColorFactor;
	} DrawingData;

	struct DrawListSpriteElement {
		TextureData mTexture;
		Position2D mTopLeft;
		Position2D mTopRight;
		Position2D mBottomLeft;
		Position2D mBottomRight;
		PrismRectangle mTexturePosition;

		DrawingData mData;
		float mZ;

		bool operator<(const DrawListSpriteElement& tOther) const {
			return mZ < tOther.mZ;
		}
	};

	typedef struct {
		float x, y, z;
		float u, v;
		float r, g, b, a;
	} Vertex;

	// The top screen: 800x240 in its wide mode (not on the original 2DS),
	// each of the game's pixels twice as wide, else 400x240.
	static int TOP_WIDTH = 400;
	static const int TOP_HEIGHT = 240;
	static const int MAXIMUM_VERTEX_AMOUNT = 6 * 8192;

	// The bottom screen's 320x240, drawn by the GPU when the game puts
	// sprites there (the menus), else left to the text console.
	static const int BOTTOM_WIDTH = 320;
	static const int BOTTOM_HEIGHT = 240;

	static struct {
		C3D_RenderTarget* mTop;
		C3D_RenderTarget* mBottom;
		C3D_Mtx mBottomProjection;
		Matrix4D mBottomFromTop;   // the top screen's transform undone, the bottom's done
		int mScreen;               // where sprites go now (setDrawingScreen)
		int mBottomFramesLeft;     // frames the bottom screen is still the GPU's
		DVLB_s* mShaderDVLB;
		shaderProgram_s mShader;
		int mProjectionLocation;
		C3D_Mtx mProjection;

		Vertex* mVertices;
		int mVertexAmount;

		// The game's screen on the top screen.
		Vector3D mScreenScale;
		Vector3D mScreenOffset;

		int mIsFrameStarted;

		// A white texture, for the background color's rectangle.
		C3D_Tex mWhite;
	} gN3DSDrawingData;

	static struct {
		uint64_t mFrameStartTicks;
		double mRealFramerate = 60;
	} gBookkeepingData;

	// Sorted by z; sprites of the same z in the order drawn.
	static std::multiset<DrawListSpriteElement> gDrawList;
	static std::multiset<DrawListSpriteElement> gBottomDrawList;
	static DrawingData gDrawingData;

	// ==========================================================================

	// Full width: the game at its shape with more of the stage seen at the
	// sides (its full-screen effects widened to them, mugenanimationhandler.cpp).
	// Not used any more: MUGEN's effects are made for its 4:3 screen, and
	// widened they were cut or pulled out of shape. The screen is either
	// stretched (the game's 4:3 screen over the whole top screen, a little
	// wider; the default) or normal (4:3, black bands at the sides).
	static int gIsFullWidth = 0;
	static int gIsStretched = 1;

	static void updateScreenTransform();
	void setN3DSScreenFullWidth(int tIsFullWidth) { gIsFullWidth = tIsFullWidth; }
	int isN3DSScreenFullWidth() { return gIsFullWidth; }
	void setN3DSScreenStretched(int tIsStretched) {
		gIsStretched = tIsStretched;
		updateScreenTransform();
	}
	int isN3DSScreenStretched() { return gIsStretched; }

	float getN3DSScreenSideMargin() {
		if (!gIsFullWidth) return 0;
		return gN3DSDrawingData.mScreenOffset.x / gN3DSDrawingData.mScreenScale.x;
	}

	static void updateScreenTransform() {
		const ScreenSize sz = getScreenSize();
		const float scale = fminf(400 / (float)sz.x, TOP_HEIGHT / (float)sz.y);
		float scaleX = scale * TOP_WIDTH / 400;
		if (gIsStretched) scaleX = TOP_WIDTH / (float)sz.x;
		gN3DSDrawingData.mScreenScale = Vector3D(scaleX, scale, 1);
		gN3DSDrawingData.mScreenOffset = Vector3D(floorf((TOP_WIDTH - sz.x * scaleX) / 2), floorf((TOP_HEIGHT - sz.y * scale) / 2), 0);
		// The bottom screen: the game's screen over all of it.
		Matrix4D m = createScaleMatrix4D(Vector3D(BOTTOM_WIDTH / (double)sz.x, BOTTOM_HEIGHT / (double)sz.y, 1));
		m = matMult4D(m, createScaleMatrix4D(Vector3D(1.0 / scaleX, 1.0 / scale, 1)));
		m = matMult4D(m, createTranslationMatrix4D(vecScale(gN3DSDrawingData.mScreenOffset, -1)));
		gN3DSDrawingData.mBottomFromTop = m;
	}

	void setDrawingScreen(int tScreen) { gN3DSDrawingData.mScreen = tScreen; }
	int getDrawingScreen() { return gN3DSDrawingData.mScreen; }
	int isN3DSBottomScreenInUse() { return gN3DSDrawingData.mBottomFramesLeft > 0; }

	// The bottom screen's average color, read from its framebuffer (the
	// GPU's picture copied there) once the frame after the request is done.
	static int gIsBottomAverageRequested;
	static int gHasBottomAverage;
	static Vector3D gBottomAverage;
	static int gBottomAverageLeftOut[4];   // x0, y0, x1, y1 on the bottom screen

	void requestDrawingScreenAverageColor(int tScreen, const GeoRectangle2D& tLeftOut) {
		if (tScreen != 1) return;
		const ScreenSize sz = getScreenSize();
		gBottomAverageLeftOut[0] = int(tLeftOut.mTopLeft.x * BOTTOM_WIDTH / sz.x);
		gBottomAverageLeftOut[1] = int(tLeftOut.mTopLeft.y * BOTTOM_HEIGHT / sz.y);
		gBottomAverageLeftOut[2] = int(tLeftOut.mBottomRight.x * BOTTOM_WIDTH / sz.x);
		gBottomAverageLeftOut[3] = int(tLeftOut.mBottomRight.y * BOTTOM_HEIGHT / sz.y);
		gIsBottomAverageRequested = 1;
		gHasBottomAverage = 0;
	}

	int getDrawingScreenAverageColor(int tScreen, Vector3D* oColor) {
		if (tScreen != 1 || !gHasBottomAverage) return 0;
		*oColor = gBottomAverage;
		return 1;
	}

	static void readBottomAverageColor() {
		u16 width = 0, height = 0;
		u8* framebuffer = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height);
		if (!framebuffer || !width || !height) return;
		const int is565 = gfxGetScreenFormat(GFX_BOTTOM) == GSP_RGB565_OES;
		const u32 bytesPerPixel = is565 ? 2 : 3;
		const u32 size = u32(width) * height * bytesPerPixel;
		GSPGPU_InvalidateDataCache(framebuffer, size);
		double r = 0, g = 0, b = 0;
		u32 amount = 0;
		// The screen turned on its side: a column of 240 pixels after another,
		// from the bottom up.
		for (int x = 0; x < BOTTOM_WIDTH; x += 3) {
			for (int y = 0; y < BOTTOM_HEIGHT; y += 3) {
				if (x >= gBottomAverageLeftOut[0] && x < gBottomAverageLeftOut[2] && y >= gBottomAverageLeftOut[1] && y < gBottomAverageLeftOut[3]) continue;
				const u8* pixel = framebuffer + (u32(x) * height + (height - 1 - y)) * bytesPerPixel;
				if (is565) {
					const u16 c = u16(pixel[0] | (pixel[1] << 8));
					r += ((c >> 11) & 31) * 255 / 31;
					g += ((c >> 5) & 63) * 255 / 63;
					b += (c & 31) * 255 / 31;
				}
				else {
					b += pixel[0];
					g += pixel[1];
					r += pixel[2];
				}
				amount++;
			}
		}
		if (!amount) return;
		gBottomAverage = Vector3D(r / (255.0 * amount), g / (255.0 * amount), b / (255.0 * amount));
		gHasBottomAverage = 1;
		gIsBottomAverageRequested = 0;
	}

	void initDrawing() {
		C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4);

		u8 model = CFG_MODEL_2DS;
		if (R_SUCCEEDED(cfguInit())) {
			CFGU_GetSystemModel(&model);
			cfguExit();
		}
		if (model != CFG_MODEL_2DS) {
			TOP_WIDTH = 800;
			gfxSetWide(true);
		}

		gN3DSDrawingData.mTop = C3D_RenderTargetCreate(TOP_HEIGHT, TOP_WIDTH, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
		C3D_RenderTargetSetOutput(gN3DSDrawingData.mTop, GFX_TOP, GFX_LEFT,
			GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
			| GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)
			| GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

		gN3DSDrawingData.mBottom = C3D_RenderTargetCreate(BOTTOM_HEIGHT, BOTTOM_WIDTH, GPU_RB_RGBA8, C3D_DEPTHTYPE(-1));
		// In the bottom screen's own format: the text console made it RGB565
		// (copied as RGB8 it showed stripes).
		const int isBottom565 = gfxGetScreenFormat(GFX_BOTTOM) == GSP_RGB565_OES;
		C3D_RenderTargetSetOutput(gN3DSDrawingData.mBottom, GFX_BOTTOM, GFX_LEFT,
			GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
			| GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(isBottom565 ? GX_TRANSFER_FMT_RGB565 : GX_TRANSFER_FMT_RGB8)
			| GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
		Mtx_OrthoTilt(&gN3DSDrawingData.mBottomProjection, 0.0f, BOTTOM_WIDTH, BOTTOM_HEIGHT, 0.0f, 0.0f, 1.0f, true);

		gN3DSDrawingData.mShaderDVLB = DVLB_ParseFile((u32*)prism3ds_shbin, prism3ds_shbin_size);
		shaderProgramInit(&gN3DSDrawingData.mShader);
		shaderProgramSetVsh(&gN3DSDrawingData.mShader, &gN3DSDrawingData.mShaderDVLB->DVLE[0]);
		C3D_BindProgram(&gN3DSDrawingData.mShader);
		gN3DSDrawingData.mProjectionLocation = shaderInstanceGetUniformLocation(gN3DSDrawingData.mShader.vertexShader, "projection");

		C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
		AttrInfo_Init(attrInfo);
		AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
		AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);
		AttrInfo_AddLoader(attrInfo, 2, GPU_FLOAT, 4);

		gN3DSDrawingData.mVertices = (Vertex*)linearAlloc(MAXIMUM_VERTEX_AMOUNT * sizeof(Vertex));
		C3D_BufInfo* bufInfo = C3D_GetBufInfo();
		BufInfo_Init(bufInfo);
		BufInfo_Add(bufInfo, gN3DSDrawingData.mVertices, sizeof(Vertex), 3, 0x210);

		// The top screen's pixels, x to the right and y down.
		Mtx_OrthoTilt(&gN3DSDrawingData.mProjection, 0.0f, TOP_WIDTH, TOP_HEIGHT, 0.0f, 0.0f, 1.0f, true);

		C3D_TexInit(&gN3DSDrawingData.mWhite, 8, 8, GPU_RGBA8);
		memset(gN3DSDrawingData.mWhite.data, 0xFF, gN3DSDrawingData.mWhite.size);
		C3D_TexFlush(&gN3DSDrawingData.mWhite);

		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_CullFace(GPU_CULL_NONE);
		for (int i = 0; i < 4; i++) gN3DSPaletteVersions[i] = 0;

		updateScreenTransform();
		gDrawingData.mEffectStack = new_vector();
		gDrawingData.mIsDisabled = 0;
		setDrawingParametersToIdentity();
		gDrawList.clear();
	}

	void shutdownN3DSDrawing() {
		if (!gN3DSDrawingData.mTop) return;
		if (gN3DSDrawingData.mIsFrameStarted) C3D_FrameEnd(0);
		C3D_RenderTargetDelete(gN3DSDrawingData.mTop);
		if (gN3DSDrawingData.mBottom) C3D_RenderTargetDelete(gN3DSDrawingData.mBottom);
		gN3DSDrawingData.mBottom = NULL;
		C3D_TexDelete(&gN3DSDrawingData.mWhite);
		gN3DSDrawingData.mTop = NULL;
		shaderProgramFree(&gN3DSDrawingData.mShader);
		DVLB_Free(gN3DSDrawingData.mShaderDVLB);
		linearFree(gN3DSDrawingData.mVertices);
		C3D_Fini();
	}

	// ==========================================================================
	// The sprites.

	static int isCulledOutsideScreen(const Position& tPos, const PrismRectangle& tTexturePosition) {
		const auto sizeX = abs(tTexturePosition.bottomRight.x - tTexturePosition.topLeft.x) + 1;
		const auto sizeY = abs(tTexturePosition.bottomRight.y - tTexturePosition.topLeft.y) + 1;
		const Matrix4D& m = gDrawingData.mTransformationMatrix;
		const Position corners[4] = {
			rotateScaleTranslatePositionByMatrix4D(m, tPos),
			rotateScaleTranslatePositionByMatrix4D(m, tPos + Vector3D(sizeX, 0, 0)),
			rotateScaleTranslatePositionByMatrix4D(m, tPos + Vector3D(0, sizeY, 0)),
			rotateScaleTranslatePositionByMatrix4D(m, tPos + Vector3D(sizeX, sizeY, 0)),
		};
		double minX = corners[0].x, maxX = corners[0].x, minY = corners[0].y, maxY = corners[0].y;
		for (int i = 1; i < 4; i++) {
			minX = fmin(minX, corners[i].x);
			maxX = fmax(maxX, corners[i].x);
			minY = fmin(minY, corners[i].y);
			maxY = fmax(maxY, corners[i].y);
		}
		static const double CULL_EPSILON = 1e-5;
		return maxX < -CULL_EPSILON || minX > TOP_WIDTH + CULL_EPSILON || maxY < -CULL_EPSILON || minY > TOP_HEIGHT + CULL_EPSILON;
	}

	void drawSprite(const TextureData& tTexture, const Position& tPos, const PrismRectangle& tTexturePosition) {
		if (gDrawingData.mIsDisabled) return;
		if (!gN3DSDrawingData.mScreen && isCulledOutsideScreen(tPos, tTexturePosition)) return;

		const auto sizeX = abs(tTexturePosition.bottomRight.x - tTexturePosition.topLeft.x) + 1;
		const auto sizeY = abs(tTexturePosition.bottomRight.y - tTexturePosition.topLeft.y) + 1;
		drawSpriteNoRectangle(tTexture, tPos, Vector3D(tPos.x + sizeX, tPos.y, tPos.z), Vector3D(tPos.x, tPos.y + sizeY, tPos.z), Vector3D(tPos.x + sizeX, tPos.y + sizeY, tPos.z), tTexturePosition);
	}

	void drawSpriteNoRectangle(const TextureData& tTexture, const Position& tTopLeft, const Position& tTopRight, const Position& tBottomLeft, const Position& tBottomRight, const PrismRectangle& tTexturePosition) {
		if (gDrawingData.mIsDisabled) return;

		if (tTexture.mTextureSize.x < 0 || tTexture.mTextureSize.y < 0) {
			logError("Called with invalid textureSize");
			return;
		}

		DrawListSpriteElement e;
		e.mTexture = tTexture;
		e.mTopLeft = tTopLeft.xy();
		e.mTopRight = tTopRight.xy();
		e.mBottomLeft = tBottomLeft.xy();
		e.mBottomRight = tBottomRight.xy();
		e.mTexturePosition = tTexturePosition;
		e.mData = gDrawingData;
		e.mZ = tTopLeft.z;
		if (gN3DSDrawingData.mScreen == 1) {
			e.mData.mTransformationMatrix = matMult4D(gN3DSDrawingData.mBottomFromTop, e.mData.mTransformationMatrix);
			gBottomDrawList.insert(e);
			return;
		}
		gDrawList.insert(e);
	}

	// ==========================================================================
	// The frame.

	// For the load log: this frame's wait in C3D_FrameBegin, when its drawing
	// began, its sprites and batches.
	static uint64_t gFrameWaitTicks, gFrameDrawStart;
	static int gFrameSprites, gFrameBatches;

	// Whether this frame is late (frame pacing, below): not waiting for the screen.
	static int gIsBehindScreen;

	void startDrawing() {
		const uint64_t waitStart = getN3DSPerfTicks();
		C3D_FrameBegin(gIsBehindScreen ? 0 : C3D_FRAME_SYNCDRAW);
		gFrameDrawStart = getN3DSPerfTicks();
		gFrameWaitTicks = gFrameDrawStart - waitStart;
		gFrameSprites = gFrameBatches = 0;
		gN3DSDrawingData.mIsFrameStarted = 1;
		advanceN3DSTextureFrame();
		// The last frame's done (C3D_FrameBegin waited for it).
		if (gIsBottomAverageRequested && gN3DSDrawingData.mBottomFramesLeft == 3) readBottomAverageColor();
#ifdef N3DS_LINEAR_DIAG
		// The linear memory's guards, twice a second (3ds/lineardiag_3ds.cpp).
		static unsigned int diagnosedFrames = 0;
		if (!(++diagnosedFrames % 30)) checkN3DSLinearGuards("a frame's start");
#endif
		// In full width, the game over the whole screen; otherwise black
		// around the game's screen (as the PC's window, which is just its
		// size): its background color and sprites only inside it.
		C3D_RenderTargetClear(gN3DSDrawingData.mTop, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(gN3DSDrawingData.mTop);
		const ScreenSize sz = getScreenSize();
		int gameX = (int)gN3DSDrawingData.mScreenOffset.x, gameY = (int)gN3DSDrawingData.mScreenOffset.y;
		int gameW = (int)(sz.x * gN3DSDrawingData.mScreenScale.x), gameH = (int)(sz.y * gN3DSDrawingData.mScreenScale.y);
		if (gIsFullWidth) {
			gameX = 0;
			gameW = TOP_WIDTH;
		}
		// (The target is the screen turned on its side.)
		C3D_SetScissor(GPU_SCISSOR_NORMAL, TOP_HEIGHT - (gameY + gameH), TOP_WIDTH - (gameX + gameW), TOP_HEIGHT - gameY, TOP_WIDTH - gameX);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gN3DSDrawingData.mProjectionLocation, &gN3DSDrawingData.mProjection);
		gN3DSDrawingData.mVertexAmount = 0;
		gDrawList.clear();
		gBottomDrawList.clear();
	}

	// The state a sprite is drawn with: sprites in a row with the same one
	// are drawn together.
	typedef struct {
		C3D_Tex* mTex;
		BlendType mBlendType;
		int mIsColorSolid;
		int mIsColorInversed;
		u32 mColorOffset;           // PalFX's add: what adds
		u32 mColorOffsetNegative;   // and what takes away
		u8 mDestAlpha;
	} DrawState;

	static bool isSameDrawState(const DrawState& a, const DrawState& b) {
		return a.mTex == b.mTex && a.mBlendType == b.mBlendType && a.mIsColorSolid == b.mIsColorSolid
			&& a.mIsColorInversed == b.mIsColorInversed && a.mColorOffset == b.mColorOffset && a.mColorOffsetNegative == b.mColorOffsetNegative && a.mDestAlpha == b.mDestAlpha;
	}

	static u8 toByte(float tValue) {
		if (tValue <= 0) return 0;
		if (tValue >= 1) return 255;
		return (u8)(tValue * 255 + 0.5f);
	}

	static void applyDrawState(const DrawState& tState) {
		C3D_TexBind(0, tState.mTex);

		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		int stage = 1;
		if (tState.mIsColorSolid) {
			// The vertex color, with the texture's alpha.
			C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
		}
		else {
			// As MUGEN's PalFX: the color (inverted or not) times mul, then
			// plus add (one stage for what adds, one for what takes away).
			C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvOpRgb(env, tState.mIsColorInversed ? GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR : GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
			C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
			if (tState.mColorOffset) {
				env = C3D_GetTexEnv(stage++);
				C3D_TexEnvInit(env);
				C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_RGB, GPU_ADD);
				C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
				C3D_TexEnvColor(env, tState.mColorOffset);
			}
			if (tState.mColorOffsetNegative) {
				env = C3D_GetTexEnv(stage++);
				C3D_TexEnvInit(env);
				C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_RGB, GPU_SUBTRACT);
				C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
				C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
				C3D_TexEnvColor(env, tState.mColorOffsetNegative);
			}
		}
		for (; stage < 3; stage++) C3D_TexEnvInit(C3D_GetTexEnv(stage));

		// "One" draws every texel, the transparent color too (MUGEN's mask = 0:
		// a storyboard's white paper, a title screen's background).
		C3D_AlphaTest(tState.mBlendType != BLEND_TYPE_ONE, GPU_GREATER, 0);

		switch (tState.mBlendType) {
		case BLEND_TYPE_ADDITION:
			C3D_BlendingColor((u32)tState.mDestAlpha << 24);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_CONSTANT_ALPHA, GPU_ZERO, GPU_ONE);
			break;
		case BLEND_TYPE_SUBTRACTION:
			C3D_BlendingColor((u32)tState.mDestAlpha << 24);
			C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_CONSTANT_ALPHA, GPU_ZERO, GPU_ONE);
			break;
		case BLEND_TYPE_ONE:
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
			break;
		case BLEND_TYPE_NORMAL:
		default:
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
			break;
		}
	}

	static void setVertex(Vertex* tVertex, const Position2D& tPos, float tU, float tV, const DrawingData& tData) {
		const Position p = rotateScaleTranslatePositionByMatrix4D(tData.mTransformationMatrix, tPos.xyz(0));
		tVertex->x = floorf(p.x);
		tVertex->y = floorf(p.y);
		tVertex->z = 0.5f;
		tVertex->u = tU;
		tVertex->v = tV;
		// Between 0 and 1: the GPU takes others as they come (a PalFX's
		// negative mul, Kyouki's hits, made the one hit green, not black).
		tVertex->r = std::min(std::max(tData.r, 0.0f), 1.0f);
		tVertex->g = std::min(std::max(tData.g, 0.0f), 1.0f);
		tVertex->b = std::min(std::max(tData.b, 0.0f), 1.0f);
		tVertex->a = std::min(std::max(tData.a, 0.0f), 1.0f);
	}

	static int gBatchStart;

	static void flushBatch() {
		const int amount = gN3DSDrawingData.mVertexAmount - gBatchStart;
		if (amount > 0) {
			C3D_DrawArrays(GPU_TRIANGLES, gBatchStart, amount);
			gFrameBatches++;
		}
		gBatchStart = gN3DSDrawingData.mVertexAmount;
	}

	static void drawSortedSprite(const DrawListSpriteElement& e, DrawState* tCurrentState, int* tHasState) {
		if (gN3DSDrawingData.mVertexAmount + 6 > MAXIMUM_VERTEX_AMOUNT) return;

		prepareN3DSTexture(e.mTexture);
		Texture texture = (Texture)e.mTexture.mTexture->mData;
		if (!texture->mIsInitialized) return;   // left out for want of memory

		DrawState state;
		state.mTex = &texture->mTex;
		state.mBlendType = e.mData.mBlendType;
		state.mIsColorSolid = e.mData.mIsColorSolid;
		state.mIsColorInversed = e.mData.mIsColorInversed;
		state.mColorOffset = (u32)toByte(e.mData.rOffset) | ((u32)toByte(e.mData.gOffset) << 8) | ((u32)toByte(e.mData.bOffset) << 16);
		state.mColorOffsetNegative = (u32)toByte(-e.mData.rOffset) | ((u32)toByte(-e.mData.gOffset) << 8) | ((u32)toByte(-e.mData.bOffset) << 16);
		state.mDestAlpha = toByte(e.mData.mDestAlpha);

		if (!*tHasState || !isSameDrawState(state, *tCurrentState)) {
			flushBatch();
			applyDrawState(state);
			*tCurrentState = state;
			*tHasState = 1;
		}

		// The texture's part, in its texels (a reversed rectangle mirrors).
		const float texWidth = (float)texture->mTex.width;
		const float texHeight = (float)texture->mTex.height;
		const PrismRectangle& r = e.mTexturePosition;
		float u1, u2, v1, v2;
		if (r.topLeft.x < r.bottomRight.x) {
			u1 = r.topLeft.x / texWidth;
			u2 = (r.bottomRight.x + 1) / texWidth;
		}
		else {
			u1 = (r.topLeft.x + 1) / texWidth;
			u2 = r.bottomRight.x / texWidth;
		}
		if (r.topLeft.y < r.bottomRight.y) {
			v1 = r.topLeft.y / texHeight;
			v2 = (r.bottomRight.y + 1) / texHeight;
		}
		else {
			v1 = (r.topLeft.y + 1) / texHeight;
			v2 = r.bottomRight.y / texHeight;
		}

		gFrameSprites++;
		Vertex* v = gN3DSDrawingData.mVertices + gN3DSDrawingData.mVertexAmount;
		setVertex(&v[0], e.mTopLeft, u1, v1, e.mData);
		setVertex(&v[1], e.mBottomLeft, u1, v2, e.mData);
		setVertex(&v[2], e.mTopRight, u2, v1, e.mData);
		v[3] = v[2];
		v[4] = v[1];
		setVertex(&v[5], e.mBottomRight, u2, v2, e.mData);
		gN3DSDrawingData.mVertexAmount += 6;
	}

	// The background color, over the game's screen (the rest stays black,
	// unless in full width).
	static void drawBackgroundColor() {
		if (gN3DSClearColor == 0x000000FF) return;

		DrawState state;
		memset(&state, 0, sizeof(state));
		state.mTex = &gN3DSDrawingData.mWhite;
		state.mBlendType = BLEND_TYPE_NORMAL;
		state.mIsColorSolid = 1;
		state.mDestAlpha = 255;
		applyDrawState(state);

		const ScreenSize sz = getScreenSize();
		float x0 = gN3DSDrawingData.mScreenOffset.x, y0 = gN3DSDrawingData.mScreenOffset.y;
		float x1 = x0 + sz.x * gN3DSDrawingData.mScreenScale.x, y1 = y0 + sz.y * gN3DSDrawingData.mScreenScale.y;
		if (gIsFullWidth) {
			x0 = 0;
			x1 = TOP_WIDTH;
		}
		const float r = (gN3DSClearColor >> 24) / 255.0f, g = ((gN3DSClearColor >> 16) & 0xFF) / 255.0f, b = ((gN3DSClearColor >> 8) & 0xFF) / 255.0f;
		const float corners[6][2] = { { x0, y0 }, { x0, y1 }, { x1, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };

		Vertex* v = gN3DSDrawingData.mVertices + gN3DSDrawingData.mVertexAmount;
		for (int i = 0; i < 6; i++) {
			v[i].x = corners[i][0];
			v[i].y = corners[i][1];
			v[i].z = 0.5f;
			v[i].u = v[i].v = 0;
			v[i].r = r;
			v[i].g = g;
			v[i].b = b;
			v[i].a = 1;
		}
		gN3DSDrawingData.mVertexAmount += 6;
		flushBatch();
	}

	// The bottom screen's sprites, if any; a few black frames once they are
	// gone (the text console's again then).
	static void drawBottomScreen() {
		if (!gN3DSDrawingData.mBottom) return;
		if (gBottomDrawList.empty()) {
			if (gN3DSDrawingData.mBottomFramesLeft <= 0) return;
			gN3DSDrawingData.mBottomFramesLeft--;
		}
		else gN3DSDrawingData.mBottomFramesLeft = 3;
		C3D_RenderTargetClear(gN3DSDrawingData.mBottom, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(gN3DSDrawingData.mBottom);
		C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gN3DSDrawingData.mProjectionLocation, &gN3DSDrawingData.mBottomProjection);
		DrawState state;
		int hasState = 0;
		for (const auto& e : gBottomDrawList) {
			drawSortedSprite(e, &state, &hasState);
		}
		flushBatch();
		gBottomDrawList.clear();
	}

	void stopDrawing() {
		if (!gN3DSDrawingData.mIsFrameStarted) return;

		gBatchStart = gN3DSDrawingData.mVertexAmount;
		DrawState state;
		int hasState = 0;
		drawBackgroundColor();
		for (const auto& e : gDrawList) {
			drawSortedSprite(e, &state, &hasState);
		}
		flushBatch();
		gDrawList.clear();
		drawBottomScreen();
		issueN3DSTextureReadAhead();

		GSPGPU_FlushDataCache(gN3DSDrawingData.mVertices, gN3DSDrawingData.mVertexAmount * sizeof(Vertex));
		const uint64_t drawTicks = getN3DSPerfTicks() - gFrameDrawStart;
		C3D_FrameEnd(0);
		gN3DSDrawingData.mIsFrameStarted = 0;
		addN3DSPerfFrame(gFrameWaitTicks, drawTicks, gFrameSprites, gFrameBatches);
	}

	void waitForRendering() {}

	void waitForScreen() {
		// C3D_FrameBegin waits for the screen (60 FPS).
		const uint64_t now = getSystemTicks();
		if (gBookkeepingData.mFrameStartTicks && now > gBookkeepingData.mFrameStartTicks) {
			gBookkeepingData.mRealFramerate = 1000.0 / (double)(now - gBookkeepingData.mFrameStartTicks);
		}
		gBookkeepingData.mFrameStartTicks = now;
	}

	// ==========================================================================
	// Frame pacing: the game keeps its 60 frames a second when a frame takes
	// longer than the screen's 16.7 ms. Each frame's time is counted against
	// the screen's start; a frame behind it does not wait for the screen
	// (C3D_FrameBegin waits for the next one even when late: a 20 ms frame
	// took 33.3), and one over half a frame behind is not drawn (never two
	// in a row: at least 30 pictures a second). Over 100 ms behind (a
	// load, a pause), counted from there again rather than hurried after.
	static const double PACING_FRAME_MS = 1000.0 / 60.0;
	static uint64_t gPacingStart;
	static uint32_t gPacingFrames;
	static int gWasPictureSkipped;

	bool isSkippingDrawing() {
		const uint64_t now = getN3DSPerfTicks();
		if (!gPacingStart) {
			gPacingStart = now;
			gPacingFrames = 0;
		}
		gPacingFrames++;
		double behind = getN3DSPerfMilliseconds(now - gPacingStart) - gPacingFrames * PACING_FRAME_MS;
		if (behind > 100) {
			gPacingStart = now;
			gPacingFrames = 0;
			behind = 0;
		}
		gIsBehindScreen = behind > 1;
		const int isSkipping = behind > PACING_FRAME_MS / 2 && !gWasPictureSkipped;
		gWasPictureSkipped = isSkipping;
		if (isSkipping) addN3DSPerfSkippedFrame();
		return isSkipping;
	}

	void setDrawingFrameSkippingEnabled(bool /*tIsEnabled*/) {}
	void resetDrawingFrameStartTime() {
		gPacingStart = 0;
		gWasPictureSkipped = 0;
		gIsBehindScreen = 0;
	}
	void updateDrawingFrameStartTime(float /*tTimeDelta*/) {}

	// ==========================================================================
	// Text with the bitmap font (as on the Vita).

	void drawMultilineText(const char* tText, const char* tFullText, const Position& tPosition, const Vector3D& tFontSize, Color tColor, const Vector3D& tBreakSize, const Vector3D& tTextBoxSize) {
		int current = 0;

		setDrawingBaseColor(tColor);

		TextureData fontData = getFontTexture();
		Position pos = tPosition;

		while (tText[current] != '\0') {
			FontCharacterData charData = getFontCharacterData(tText[current]);

			PrismRectangle tTexturePosition;
			tTexturePosition.topLeft.x = (int)(fontData.mTextureSize.x * charData.mFilePositionX1);
			tTexturePosition.topLeft.y = (int)(fontData.mTextureSize.y * charData.mFilePositionY1);
			tTexturePosition.bottomRight.x = (int)(fontData.mTextureSize.x * charData.mFilePositionX2);
			tTexturePosition.bottomRight.y = (int)(fontData.mTextureSize.y * charData.mFilePositionY2);

			auto dx = (float)abs(tTexturePosition.bottomRight.x - tTexturePosition.topLeft.x);
			auto dy = (float)abs(tTexturePosition.bottomRight.y - tTexturePosition.topLeft.y);
			Vector3D scale = Vector3D(1 / dx, 1 / dy, 1);
			scaleDrawing3D(vecScale3D(tFontSize, scale), pos);

			drawSprite(fontData, pos, tTexturePosition);

			pos.x += tFontSize.x + tBreakSize.x;
			current++;

			if (hasToLinebreak(tFullText, current, tPosition, pos, tFontSize, tBreakSize, tTextBoxSize)) {
				pos.x = tPosition.x - (tFontSize.x + tBreakSize.x);
				pos.y += tFontSize.y + tBreakSize.y;
			}
		}

		setDrawingParametersToIdentity();
	}

	void drawTruetypeText(const char* /*tText*/, TruetypeFont /*tFont*/, const Position& /*tPosition*/, const Vector3DI& /*tTextSize*/, const Vector3D& /*tColor*/, float /*tTextBoxWidth*/, const GeoRectangle2D& /*tDrawRectangle*/) {
		// UNSUPPORTED
	}

	// ==========================================================================
	// The drawing state.

	void scaleDrawing(float tFactor, const Position& tScalePosition) {
		scaleDrawing3D(Vector3D(tFactor, tFactor, 1), tScalePosition);
	}

	void scaleDrawing2D(const Vector2D& tFactor, const Position2D& tScalePosition) {
		scaleDrawing3D(tFactor.xyz(1.0), tScalePosition.xyz(0.0));
	}

	void scaleDrawing3D(const Vector3D& tFactor, const Position& tScalePosition) {
		const auto scalePosition2D = Vector3D(tScalePosition.x, tScalePosition.y, 0);
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createTranslationMatrix4D(scalePosition2D));
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createScaleMatrix4D(Vector3D(tFactor.x, tFactor.y, tFactor.z)));
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createTranslationMatrix4D(vecScale(scalePosition2D, -1)));
	}

	void setDrawingBaseColorOffsetAdvanced(float r, float g, float b) {
		gDrawingData.rOffset = r;
		gDrawingData.gOffset = g;
		gDrawingData.bOffset = b;
	}

	void setDrawingBaseColor(Color tColor) {
		getRGBFromColor(tColor, &gDrawingData.r, &gDrawingData.g, &gDrawingData.b);
	}

	void setDrawingBaseColorAdvanced(float r, float g, float b) {
		gDrawingData.r = r;
		gDrawingData.g = g;
		gDrawingData.b = b;
	}

	void setDrawingColorSolidity(int tIsSolid) { gDrawingData.mIsColorSolid = tIsSolid; }
	void setDrawingColorInversed(int tIsInversed) { gDrawingData.mIsColorInversed = tIsInversed; }
	void setDrawingColorFactor(float tColorFactor) { gDrawingData.mColorFactor = tColorFactor; }
	void setDrawingTransparency(float tAlpha) { gDrawingData.a = tAlpha; }
	void setDrawingDestinationTransparency(float tAlpha) { gDrawingData.mDestAlpha = tAlpha; }

	void setDrawingRotationZ(float tAngle, const Position2D& tPosition) {
		setDrawingRotationZ(tAngle, tPosition.xyz(0.0));
	}

	void setDrawingRotationZ(float tAngle, const Position& tPosition) {
		tAngle = (2 * M_PI - tAngle);
		const auto rotationPosition2D = Vector3D(tPosition.x, tPosition.y, 0);
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createTranslationMatrix4D(rotationPosition2D));
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createRotationZMatrix4D(tAngle));
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createTranslationMatrix4D(vecScale(rotationPosition2D, -1)));
	}

	void setDrawingParametersToIdentity() {
		setDrawingBaseColor(COLOR_WHITE);
		setDrawingTransparency(1.0);
		setDrawingDestinationTransparency(1.0);
		setDrawingBlendType(BLEND_TYPE_NORMAL);
		setDrawingColorSolidity(0);
		setDrawingColorInversed(0);
		setDrawingColorFactor(1.0);
		setDrawingBaseColorOffsetAdvanced(0, 0, 0);

		// The game's screen onto the top screen.
		updateScreenTransform();
		gDrawingData.mTransformationMatrix = makeIdentityMatrix4D();
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createTranslationMatrix4D(gN3DSDrawingData.mScreenOffset));
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createScaleMatrix4D(gN3DSDrawingData.mScreenScale));
	}

	void setDrawingBlendType(BlendType tBlendType) { gDrawingData.mBlendType = tBlendType; }

	typedef struct {
		float mAngle;
		Position mCenter;
	} RotationZEffect;

	typedef struct {
		Vector3D mTranslation;
	} TranslationEffect;

	void pushDrawingTranslation(const Vector3D& tTranslation) {
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createTranslationMatrix4D(tTranslation));

		TranslationEffect* e = (TranslationEffect*)allocMemory(sizeof(TranslationEffect));
		e->mTranslation = tTranslation;
		vector_push_back_owned(&gDrawingData.mEffectStack, e);
	}

	void pushDrawingRotationZ(float tAngle, const Vector3D& tCenter) {
		setDrawingRotationZ(tAngle, tCenter);

		RotationZEffect* e = (RotationZEffect*)allocMemory(sizeof(RotationZEffect));
		e->mAngle = tAngle;
		e->mCenter = tCenter;
		vector_push_back_owned(&gDrawingData.mEffectStack, e);
	}

	void popDrawingRotationZ() {
		int ind = vector_size(&gDrawingData.mEffectStack) - 1;
		RotationZEffect* e = (RotationZEffect*)vector_get(&gDrawingData.mEffectStack, ind);
		setDrawingRotationZ(-e->mAngle, e->mCenter);
		vector_remove(&gDrawingData.mEffectStack, ind);
	}

	void popDrawingTranslation() {
		int ind = vector_size(&gDrawingData.mEffectStack) - 1;
		TranslationEffect* e = (TranslationEffect*)vector_get(&gDrawingData.mEffectStack, ind);
		gDrawingData.mTransformationMatrix = matMult4D(gDrawingData.mTransformationMatrix, createTranslationMatrix4D(vecScale(e->mTranslation, -1)));
		vector_remove(&gDrawingData.mEffectStack, ind);
	}

	void disableDrawing() { gDrawingData.mIsDisabled = 1; }
	void enableDrawing() { gDrawingData.mIsDisabled = 0; }

	// ==========================================================================
	// Palettes: kept as RGBA8 texels (paletted textures are converted with
	// them, texture_3ds.cpp). An ARGB palette's bytes are A, R, G, B; an
	// RGB one's R, G, B, with color 0 transparent (as the sprite reader's
	// own conversions read them).

	void setPaletteFromARGB256Buffer(int tPaletteID, const Buffer& tBuffer) {
		const u8* src = (const u8*)tBuffer.mData;
		u32* dst = gN3DSPalettes[tPaletteID & 3];
		for (int i = 0; i < 256; i++) {
			const u8* c = src + 4 * i;
			dst[i] = ((u32)c[1] << 24) | ((u32)c[2] << 16) | ((u32)c[3] << 8) | c[0];
		}
		gN3DSPaletteVersions[tPaletteID & 3]++;
	}

	void setPaletteFromBGR256WithFirstValueTransparentBuffer(int tPaletteID, const Buffer& tBuffer) {
		const u8* src = (const u8*)tBuffer.mData;
		u32* dst = gN3DSPalettes[tPaletteID & 3];
		for (int i = 0; i < 256; i++) {
			const u8* c = src + 3 * i;
			dst[i] = ((u32)c[0] << 24) | ((u32)c[1] << 16) | ((u32)c[2] << 8) | (i == 0 ? 0 : 0xFF);
		}
		gN3DSPaletteVersions[tPaletteID & 3]++;
	}

	float getRealFramerate() {
		return gBookkeepingData.mRealFramerate;
	}


	// The top screen as shown (a frame or two before), as a 24-bit BMP; 0 if
	// it could not be written.
	int saveN3DSScreenshot(const char* tPath) {
		u16 fbWidth = 0, fbHeight = 0;
		const u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbWidth, &fbHeight);
		if (!fb || !fbWidth || !fbHeight) return 0;
		const GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
		const int bytesPerPixel = format == GSP_RGBA8_OES ? 4 : (format == GSP_BGR8_OES ? 3 : 2);
		const int width = fbHeight, height = fbWidth;   // stored turned: columns of 240
		const int rowSize = (width * 3 + 3) & ~3;
		FILE* file = fopen(tPath, "wb");
		if (!file) return 0;
		// Written to the SD in a few large pieces, not one per row.
		static char buffer[64 * 1024];
		setvbuf(file, buffer, _IOFBF, sizeof(buffer));
		u8 header[54] = { 'B', 'M' };
		const u32 dataSize = (u32)rowSize * height, fileSize = 54 + dataSize;
		memcpy(header + 2, &fileSize, 4);
		const u32 dataOffset = 54, infoSize = 40;
		memcpy(header + 10, &dataOffset, 4);
		memcpy(header + 14, &infoSize, 4);
		const int32_t w32 = width, h32 = height;
		memcpy(header + 18, &w32, 4);
		memcpy(header + 22, &h32, 4);
		const u16 planes = 1, bits = 24;
		memcpy(header + 26, &planes, 2);
		memcpy(header + 28, &bits, 2);
		memcpy(header + 34, &dataSize, 4);
		fwrite(header, 1, sizeof(header), file);
		static u8 row[800 * 3 + 4];
		for (int y = height - 1; y >= 0; y--) {   // BMP rows: bottom first
			memset(row, 0, sizeof(row));
			for (int x = 0; x < width && x < 800; x++) {
				const u8* p = fb + ((size_t)x * fbWidth + (fbWidth - 1 - y)) * bytesPerPixel;
				u8 r, g, b;
				if (bytesPerPixel == 4) { r = p[3]; g = p[2]; b = p[1]; }
				else if (bytesPerPixel == 3) { b = p[0]; g = p[1]; r = p[2]; }
				else { const u16 c = (u16)(p[0] | (p[1] << 8)); r = (u8)((c >> 11) << 3); g = (u8)(((c >> 5) & 63) << 2); b = (u8)((c & 31) << 3); }
				row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
			}
			fwrite(row, 1, rowSize, file);
		}
		fclose(file);
		return 1;
	}
}
