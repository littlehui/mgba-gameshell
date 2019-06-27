/* Copyright (c) 2013-2018 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "feature/gui/gui-runner.h"
#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/audio.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/gui.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/menu.h>

#include <switch.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define AUTO_INPUT 0x4E585031
#define SAMPLES 0x400
#define BUFFER_SIZE 0x1000
#define N_BUFFERS 4
#define ANALOG_DEADZONE 0x4000

TimeType __nx_time_type = TimeType_UserSystemClock;

static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;

static const GLfloat _offsets[] = {
	0.f, 0.f,
	1.f, 0.f,
	1.f, 1.f,
	0.f, 1.f,
};

static const GLchar* const _gles2Header =
	"#version 100\n"
	"precision mediump float;\n";

static const char* const _vertexShader =
	"attribute vec2 offset;\n"
	"uniform vec2 dims;\n"
	"uniform vec2 insize;\n"
	"varying vec2 texCoord;\n"

	"void main() {\n"
	"	vec2 ratio = insize / 256.0;\n"
	"	vec2 scaledOffset = offset * dims;\n"
	"	gl_Position = vec4(scaledOffset.x * 2.0 - dims.x, scaledOffset.y * -2.0 + dims.y, 0.0, 1.0);\n"
	"	texCoord = offset * ratio;\n"
	"}";

static const char* const _fragmentShader =
	"varying vec2 texCoord;\n"
	"uniform sampler2D tex;\n"
	"uniform vec4 color;\n"

	"void main() {\n"
	"	vec4 texColor = vec4(texture2D(tex, texCoord).rgb, 1.0);\n"
	"	texColor *= color;\n"
	"	gl_FragColor = texColor;\n"
	"}";

static GLuint program;
static GLuint vbo;
static GLuint vao;
static GLuint pbo;
static GLuint texLocation;
static GLuint dimsLocation;
static GLuint insizeLocation;
static GLuint colorLocation;
static GLuint tex;

static color_t* frameBuffer;
static struct mAVStream stream;
static struct mSwitchRumble {
	struct mRumble d;
	int up;
	int down;
	HidVibrationValue value;
} rumble;
static struct mRotationSource rotation = {0};
static int audioBufferActive;
static struct GBAStereoSample audioBuffer[N_BUFFERS][SAMPLES] __attribute__((__aligned__(0x1000)));
static AudioOutBuffer audoutBuffer[N_BUFFERS];
static int enqueuedBuffers;
static bool frameLimiter = true;
static unsigned framecount = 0;
static unsigned framecap = 10;
static u32 vibrationDeviceHandles[4];
static HidVibrationValue vibrationStop = { .freq_low = 160.f, .freq_high = 320.f };

static enum ScreenMode {
	SM_PA,
	SM_AF,
	SM_SF,
	SM_MAX
} screenMode = SM_PA;

static bool initEgl() {
	s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!s_display) {
		goto _fail0;
	}

	eglInitialize(s_display, NULL, NULL);

	EGLConfig config;
	EGLint numConfigs;
	static const EGLint attributeList[] = {
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_NONE
	};
	eglChooseConfig(s_display, attributeList, &config, 1, &numConfigs);
	if (!numConfigs) {
		goto _fail1;
	}

	s_surface = eglCreateWindowSurface(s_display, config, nwindowGetDefault(), NULL);
	if (!s_surface) {
		goto _fail1;
	}

	EGLint contextAttributeList[] = {
		EGL_CONTEXT_CLIENT_VERSION, 3,
		EGL_NONE
	};
	s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttributeList);
	if (!s_context) {
		goto _fail2;
	}

	eglMakeCurrent(s_display, s_surface, s_surface, s_context);
	return true;

_fail2:
	eglDestroySurface(s_display, s_surface);
	s_surface = NULL;
_fail1:
	eglTerminate(s_display);
	s_display = NULL;
_fail0:
	return false;
}

static void deinitEgl() {
	if (s_display) {
		eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (s_context) {
			eglDestroyContext(s_display, s_context);
		}
		if (s_surface) {
			eglDestroySurface(s_display, s_surface);
		}
		eglTerminate(s_display);
	}
}

static void _mapKey(struct mInputMap* map, uint32_t binding, int nativeKey, enum GBAKey key) {
	mInputBindKey(map, binding, __builtin_ctz(nativeKey), key);
}

static void _drawStart(void) {
	glClear(GL_COLOR_BUFFER_BIT);
}

static void _drawEnd(void) {
	if (frameLimiter || framecount >= framecap) {
		eglSwapBuffers(s_display, s_surface);
		framecount = 0;
	}
}

static uint32_t _pollInput(const struct mInputMap* map) {
	int keys = 0;
	hidScanInput();
	u32 padkeys = hidKeysHeld(CONTROLLER_P1_AUTO);
	keys |= mInputMapKeyBits(map, AUTO_INPUT, padkeys, 0);

	JoystickPosition jspos;
	hidJoystickRead(&jspos, CONTROLLER_P1_AUTO, JOYSTICK_LEFT);

	int l = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_LSTICK_LEFT));
	int r = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_LSTICK_RIGHT));
	int u = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_LSTICK_UP));
	int d = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_LSTICK_DOWN));

	if (l == -1) {
		l = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_DLEFT));
	}
	if (r == -1) {
		r = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_DRIGHT));
	}
	if (u == -1) {
		u = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_DUP));
	}
	if (d == -1) {
		d = mInputMapKey(map, AUTO_INPUT, __builtin_ctz(KEY_DDOWN));
	}

	if (jspos.dx < -ANALOG_DEADZONE && l != -1) {
		keys |= 1 << l;
	}
	if (jspos.dx > ANALOG_DEADZONE && r != -1) {
		keys |= 1 << r;
	}
	if (jspos.dy < -ANALOG_DEADZONE && d != -1) {
		keys |= 1 << d;
	}
	if (jspos.dy > ANALOG_DEADZONE && u != -1) {
		keys |= 1 << u;
	}
	return keys;
}

static enum GUICursorState _pollCursor(unsigned* x, unsigned* y) {
	hidScanInput();
	if (hidTouchCount() < 1) {
		return GUI_CURSOR_NOT_PRESENT;
	}
	touchPosition touch;
	hidTouchRead(&touch, 0);
	*x = touch.px;
	*y = touch.py;
	return GUI_CURSOR_DOWN;
}


static void _setup(struct mGUIRunner* runner) {
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_A, GBA_KEY_A);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_B, GBA_KEY_B);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_PLUS, GBA_KEY_START);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_MINUS, GBA_KEY_SELECT);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_DUP, GBA_KEY_UP);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_DDOWN, GBA_KEY_DOWN);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_DLEFT, GBA_KEY_LEFT);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_DRIGHT, GBA_KEY_RIGHT);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_L, GBA_KEY_L);
	_mapKey(&runner->core->inputMap, AUTO_INPUT, KEY_R, GBA_KEY_R);

	runner->core->setVideoBuffer(runner->core, frameBuffer, 256);
	runner->core->setPeripheral(runner->core, mPERIPH_RUMBLE, &rumble.d);
	runner->core->setPeripheral(runner->core, mPERIPH_ROTATION, &rotation);
	runner->core->setAVStream(runner->core, &stream);

	unsigned mode;
	if (mCoreConfigGetUIntValue(&runner->config, "screenMode", &mode) && mode < SM_MAX) {
		screenMode = mode;
	}
}

static void _gameLoaded(struct mGUIRunner* runner) {
	u32 samplerate = audoutGetSampleRate();

	double ratio = GBAAudioCalculateRatio(1, 60.0, 1);
	blip_set_rates(runner->core->getAudioChannel(runner->core, 0), runner->core->frequency(runner->core), samplerate * ratio);
	blip_set_rates(runner->core->getAudioChannel(runner->core, 1), runner->core->frequency(runner->core), samplerate * ratio);

	mCoreConfigGetUIntValue(&runner->config, "fastForwardCap", &framecap);

	unsigned mode;
	if (mCoreConfigGetUIntValue(&runner->config, "screenMode", &mode) && mode < SM_MAX) {
		screenMode = mode;
	}

	rumble.up = 0;
	rumble.down = 0;
}

static void _gameUnloaded(struct mGUIRunner* runner) {
	HidVibrationValue values[4];
	memcpy(&values[0], &vibrationStop, sizeof(rumble.value));
	memcpy(&values[1], &vibrationStop, sizeof(rumble.value));
	memcpy(&values[2], &vibrationStop, sizeof(rumble.value));
	memcpy(&values[3], &vibrationStop, sizeof(rumble.value));
	hidSendVibrationValues(vibrationDeviceHandles, values, 4);
}

static void _drawTex(struct mGUIRunner* runner, unsigned width, unsigned height, bool faded) {
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(program);
	glBindVertexArray(vao);
	float aspectX = width / (float) runner->params.width;
	float aspectY = height / (float) runner->params.height;
	float max = 1.f;
	switch (screenMode) {
	case SM_PA:
		if (aspectX > aspectY) {
			max = floor(1.0 / aspectX);
		} else {
			max = floor(1.0 / aspectY);
		}
		break;
	case SM_AF:
		if (aspectX > aspectY) {
			max = 1.0 / aspectX;
		} else {
			max = 1.0 / aspectY;
		}
		break;
	case SM_SF:
		aspectX = 1.0;
		aspectY = 1.0;
		break;
	}

	aspectX *= max;
	aspectY *= max;

	glUniform1i(texLocation, 0);
	glUniform2f(dimsLocation, aspectX, aspectY);
	glUniform2f(insizeLocation, width, height);
	if (!faded) {
		glUniform4f(colorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
	} else {
		glUniform4f(colorLocation, 0.8f, 0.8f, 0.8f, 0.8f);		
	}

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glBindVertexArray(0);
	glUseProgram(0);
}

static void _prepareForFrame(struct mGUIRunner* runner) {
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
	frameBuffer = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, 256 * 256 * 4, GL_MAP_WRITE_BIT);
	if (frameBuffer) {
		runner->core->setVideoBuffer(runner->core, frameBuffer, 256);
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

static void _drawFrame(struct mGUIRunner* runner, bool faded) {
	++framecount;
	if (!frameLimiter && framecount < framecap) {
		return;
	}

	unsigned width, height;
	runner->core->desiredVideoDimensions(runner->core, &width, &height);

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, height, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	_drawTex(runner, width, height, faded);

	HidVibrationValue values[4];
	if (rumble.up) {
		rumble.value.amp_low = rumble.up / (float) (rumble.up + rumble.down);
		rumble.value.amp_high = rumble.up / (float) (rumble.up + rumble.down);
		memcpy(&values[0], &rumble.value, sizeof(rumble.value));
		memcpy(&values[1], &rumble.value, sizeof(rumble.value));
		memcpy(&values[2], &rumble.value, sizeof(rumble.value));
		memcpy(&values[3], &rumble.value, sizeof(rumble.value));
	} else {
		memcpy(&values[0], &vibrationStop, sizeof(rumble.value));
		memcpy(&values[1], &vibrationStop, sizeof(rumble.value));
		memcpy(&values[2], &vibrationStop, sizeof(rumble.value));
		memcpy(&values[3], &vibrationStop, sizeof(rumble.value));
	}
	hidSendVibrationValues(vibrationDeviceHandles, values, 4);
	rumble.up = 0;
	rumble.down = 0;
}

static void _drawScreenshot(struct mGUIRunner* runner, const color_t* pixels, unsigned width, unsigned height, bool faded) {
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

	_drawTex(runner, width, height, faded);
}

static uint16_t _pollGameInput(struct mGUIRunner* runner) {
	return _pollInput(&runner->core->inputMap);
}

static void _incrementScreenMode(struct mGUIRunner* runner) {
	UNUSED(runner);
	screenMode = (screenMode + 1) % SM_MAX;
	mCoreConfigSetUIntValue(&runner->config, "screenMode", screenMode);
}

static void _setFrameLimiter(struct mGUIRunner* runner, bool limit) {
	UNUSED(runner);
	if (!frameLimiter && limit) {
		while (enqueuedBuffers > 1) {
			AudioOutBuffer* releasedBuffers;
			u32 audoutNReleasedBuffers;
			audoutWaitPlayFinish(&releasedBuffers, &audoutNReleasedBuffers, 100000000);
			enqueuedBuffers -= audoutNReleasedBuffers;
		}
	}
	frameLimiter = limit;
	eglSwapInterval(s_surface, limit);
}

static bool _running(struct mGUIRunner* runner) {
	UNUSED(runner);
	return appletMainLoop();
}

static void _postAudioBuffer(struct mAVStream* stream, blip_t* left, blip_t* right) {
	UNUSED(stream);
	AudioOutBuffer* releasedBuffers;
	u32 audoutNReleasedBuffers;
	audoutGetReleasedAudioOutBuffer(&releasedBuffers, &audoutNReleasedBuffers);
	enqueuedBuffers -= audoutNReleasedBuffers;
	if (!frameLimiter && enqueuedBuffers >= N_BUFFERS) {
		blip_clear(left);
		blip_clear(right);
		return;
	}
	if (enqueuedBuffers >= N_BUFFERS - 1 && R_SUCCEEDED(audoutWaitPlayFinish(&releasedBuffers, &audoutNReleasedBuffers, 10000000))) {
		enqueuedBuffers -= audoutNReleasedBuffers;
	}

	struct GBAStereoSample* samples = audioBuffer[audioBufferActive];
	blip_read_samples(left, &samples[0].left, SAMPLES, true);
	blip_read_samples(right, &samples[0].right, SAMPLES, true);
	audoutAppendAudioOutBuffer(&audoutBuffer[audioBufferActive]);
	audioBufferActive += 1;
	audioBufferActive %= N_BUFFERS;
	++enqueuedBuffers;
}

void _setRumble(struct mRumble* rumble, int enable) {
	struct mSwitchRumble* sr = (struct mSwitchRumble*) rumble;
	if (enable) {
		++sr->up;
	} else {
		++sr->down;
	}
}

int32_t _readTiltX(struct mRotationSource* source) {
	UNUSED(source);
	SixAxisSensorValues sixaxis;
	hidSixAxisSensorValuesRead(&sixaxis, CONTROLLER_P1_AUTO, 1);
	return sixaxis.accelerometer.x * 3e8f;
}

int32_t _readTiltY(struct mRotationSource* source) {
	UNUSED(source);
	SixAxisSensorValues sixaxis;
	hidSixAxisSensorValuesRead(&sixaxis, CONTROLLER_P1_AUTO, 1);
	return sixaxis.accelerometer.y * -3e8f;
}

int32_t _readGyroZ(struct mRotationSource* source) {
	UNUSED(source);
	SixAxisSensorValues sixaxis;
	hidSixAxisSensorValuesRead(&sixaxis, CONTROLLER_P1_AUTO, 1);
	return sixaxis.gyroscope.z * -1.1e9f;
}

static int _batteryState(void) {
	u32 charge;
	int state = 0;
	if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) {
		state = (charge + 12) / 25;
	} else {
		return BATTERY_NOT_PRESENT;
	}
	ChargerType type;
	if (R_SUCCEEDED(psmGetChargerType(&type)) && type) {
		state |= BATTERY_CHARGING;
	}
	return state;
}

int main(int argc, char* argv[]) {
	socketInitializeDefault();
	nxlinkStdio();
	initEgl();
	romfsInit();
	audoutInitialize();
	psmInitialize();

	struct GUIFont* font = GUIFontCreate();

	u32 width = 1280;
	u32 height = 720;

	glViewport(0, 0, width, height);
	glClearColor(0.f, 0.f, 0.f, 1.f);

	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glGenBuffers(1, &pbo);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, 256 * 256 * 4, NULL, GL_STREAM_DRAW);
	frameBuffer = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, 256 * 256 * 4, GL_MAP_WRITE_BIT);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	program = glCreateProgram();
	GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
	GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	const GLchar* shaderBuffer[2];

	shaderBuffer[0] = _gles2Header;

	shaderBuffer[1] = _vertexShader;
	glShaderSource(vertexShader, 2, shaderBuffer, NULL);

	shaderBuffer[1] = _fragmentShader;
	glShaderSource(fragmentShader, 2, shaderBuffer, NULL);

	glAttachShader(program, vertexShader);
	glAttachShader(program, fragmentShader);

	glCompileShader(fragmentShader);

	GLint success;
	glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetShaderInfoLog(fragmentShader, sizeof(msg), NULL, msg);
		puts(msg);
	}

	glCompileShader(vertexShader);

	glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
	if (!success) {
		GLchar msg[512];
		glGetShaderInfoLog(vertexShader, sizeof(msg), NULL, msg);
		puts(msg);
	}
	glLinkProgram(program);

	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);

	texLocation = glGetUniformLocation(program, "tex");
	colorLocation = glGetUniformLocation(program, "color");
	dimsLocation = glGetUniformLocation(program, "dims");
	insizeLocation = glGetUniformLocation(program, "insize");
	GLuint offsetLocation = glGetAttribLocation(program, "offset");

	glGenBuffers(1, &vbo);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(_offsets), _offsets, GL_STATIC_DRAW);
	glVertexAttribPointer(offsetLocation, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(offsetLocation);
	glBindVertexArray(0);

	rumble.d.setRumble = _setRumble;
	rumble.value.freq_low = 120.0;
	rumble.value.freq_high = 180.0;
	hidInitializeVibrationDevices(&vibrationDeviceHandles[0], 2, CONTROLLER_HANDHELD, TYPE_HANDHELD | TYPE_JOYCON_PAIR);
	hidInitializeVibrationDevices(&vibrationDeviceHandles[2], 2, CONTROLLER_PLAYER_1, TYPE_HANDHELD | TYPE_JOYCON_PAIR);

	u32 handles[4];
	hidGetSixAxisSensorHandles(&handles[0], 2, CONTROLLER_PLAYER_1, TYPE_JOYCON_PAIR);
	hidGetSixAxisSensorHandles(&handles[2], 1, CONTROLLER_PLAYER_1, TYPE_PROCONTROLLER);
	hidGetSixAxisSensorHandles(&handles[3], 1, CONTROLLER_HANDHELD, TYPE_HANDHELD);
	hidStartSixAxisSensor(handles[0]);
	hidStartSixAxisSensor(handles[1]);
	hidStartSixAxisSensor(handles[2]);
	hidStartSixAxisSensor(handles[3]);
	rotation.readTiltX = _readTiltX;
	rotation.readTiltY = _readTiltY;
	rotation.readGyroZ = _readGyroZ;

	stream.videoDimensionsChanged = NULL;
	stream.postVideoFrame = NULL;
	stream.postAudioFrame = NULL;
	stream.postAudioBuffer = _postAudioBuffer;

	memset(audioBuffer, 0, sizeof(audioBuffer));
	audioBufferActive = 0;
	enqueuedBuffers = 0;
	size_t i;
	for (i = 0; i < N_BUFFERS; ++i) {
		audoutBuffer[i].next = NULL;
		audoutBuffer[i].buffer = audioBuffer[i];
		audoutBuffer[i].buffer_size = BUFFER_SIZE;
		audoutBuffer[i].data_size = BUFFER_SIZE;
		audoutBuffer[i].data_offset = 0;
	}

	struct mGUIRunner runner = {
		.params = {
			width, height,
			font, "/",
			_drawStart, _drawEnd,
			_pollInput, _pollCursor,
			_batteryState,
			NULL, NULL,
		},
		.keySources = (struct GUIInputKeys[]) {
			{
				.name = "Controller Input",
				.id = AUTO_INPUT,
				.keyNames = (const char*[]) {
					"A",
					"B",
					"X",
					"Y",
					"L Stick",
					"R Stick",
					"L",
					"R",
					"ZL",
					"ZR",
					"+",
					"-",
					"Left",
					"Up",
					"Right",
					"Down",
					"L Left",
					"L Up",
					"L Right",
					"L Down",
					"R Left",
					"R Up",
					"R Right",
					"R Down",
					"SL",
					"SR"
				},
				.nKeys = 26
			},
			{ .id = 0 }
		},
		.configExtra = (struct GUIMenuItem[]) {
			{
				.title = "Screen mode",
				.data = "screenMode",
				.submenu = 0,
				.state = SM_PA,
				.validStates = (const char*[]) {
					"Pixel-Accurate",
					"Aspect-Ratio Fit",
					"Stretched",
				},
				.nStates = 3
			},
			{
				.title = "Fast forward cap",
				.data = "fastForwardCap",
				.submenu = 0,
				.state = 7,
				.validStates = (const char*[]) {
					"2", "3", "4", "5", "6", "7", "8", "9",
					"10", "11", "12", "13", "14", "15",
					"20", "30"
				},
				.stateMappings = (const struct GUIVariant[]) {
					GUI_V_U(2),
					GUI_V_U(3),
					GUI_V_U(4),
					GUI_V_U(5),
					GUI_V_U(6),
					GUI_V_U(7),
					GUI_V_U(8),
					GUI_V_U(9),
					GUI_V_U(10),
					GUI_V_U(11),
					GUI_V_U(12),
					GUI_V_U(13),
					GUI_V_U(14),
					GUI_V_U(15),
					GUI_V_U(20),
					GUI_V_U(30),
				},
				.nStates = 16
			},
		},
		.nConfigExtra = 2,
		.setup = _setup,
		.teardown = NULL,
		.gameLoaded = _gameLoaded,
		.gameUnloaded = _gameUnloaded,
		.prepareForFrame = _prepareForFrame,
		.drawFrame = _drawFrame,
		.drawScreenshot = _drawScreenshot,
		.paused = _gameUnloaded,
		.unpaused = _gameLoaded,
		.incrementScreenMode = _incrementScreenMode,
		.setFrameLimiter = _setFrameLimiter,
		.pollGameInput = _pollGameInput,
		.running = _running
	};
	mGUIInit(&runner, "switch");

	_mapKey(&runner.params.keyMap, AUTO_INPUT, KEY_A, GUI_INPUT_SELECT);
	_mapKey(&runner.params.keyMap, AUTO_INPUT, KEY_B, GUI_INPUT_BACK);
	_mapKey(&runner.params.keyMap, AUTO_INPUT, KEY_X, GUI_INPUT_CANCEL);
	_mapKey(&runner.params.keyMap, AUTO_INPUT, KEY_DUP, GUI_INPUT_UP);
	_mapKey(&runner.params.keyMap, AUTO_INPUT, KEY_DDOWN, GUI_INPUT_DOWN);
	_mapKey(&runner.params.keyMap, AUTO_INPUT, KEY_DLEFT, GUI_INPUT_LEFT);
	_mapKey(&runner.params.keyMap, AUTO_INPUT, KEY_DRIGHT, GUI_INPUT_RIGHT);

	audoutStartAudioOut();

	if (argc > 1) {
		size_t i;
		for (i = 0; runner.keySources[i].id; ++i) {
			mInputMapLoad(&runner.params.keyMap, runner.keySources[i].id, mCoreConfigGetInput(&runner.config));
		}
		mGUIRun(&runner, argv[1]);
	} else {
		mGUIRunloop(&runner);
	}

	mGUIDeinit(&runner);

	audoutStopAudioOut();
	GUIFontDestroy(font);

	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glDeleteBuffers(1, &pbo);

	glDeleteTextures(1, &tex);
	glDeleteBuffers(1, &vbo);
	glDeleteProgram(program);
	glDeleteVertexArrays(1, &vao);

	hidStopSixAxisSensor(handles[0]);
	hidStopSixAxisSensor(handles[1]);
	hidStopSixAxisSensor(handles[2]);
	hidStopSixAxisSensor(handles[3]);

	psmExit();
	audoutExit();
	deinitEgl();
	socketExit();
	return 0;
}
