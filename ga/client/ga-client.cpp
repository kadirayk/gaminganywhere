/*
 * Copyright (c) 2013-2014 Chun-Ying Huang
 *
 * This file is part of GamingAnywhere (GA).
 *
 * GA is free software; you can redistribute it and/or modify it
 * under the terms of the 3-clause BSD License as published by the
 * Free Software Foundation: http://directory.fsf.org/wiki/License:BSD_3Clause
 *
 * GA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the 3-clause BSD License along with GA;
 * if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdarg.h>
#include <string.h>

#include <pthread.h>
#include <SDL2/SDL.h>
#ifndef ANDROID
#include <SDL2/SDL_ttf.h>
#endif /* ! ANDROID */
#ifndef WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif /* ! WIN32 */
#if ! defined WIN32 && ! defined __APPLE__ && ! defined ANDROID
#include <X11/Xlib.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

#include "rtspconf.h"
#include "rtspclient.h"

#include "controller.h"
#include "ctrl-sdl.h"

#include "ga-common.h"
#include "ga-conf.h"
#include "ga-avcodec.h"
#include "vconverter.h"

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filereadstream.h"
#include <iostream>
#include <fstream>
#include <ctime>
#include "INIReader.h"

#include <map>
using namespace rapidjson;
using namespace std;

#define	POOLSIZE	16

#define	IDLE_MAXIMUM_THRESHOLD		3600000	/* us */
#define	IDLE_DETECTION_THRESHOLD	 600000 /* us */

#define	WINDOW_TITLE		"Player Channel #%d (%dx%d)"

#define REPLAY_EVENT_CODE 111

#define PRSC_CONF "prsc_conf.ini"

pthread_mutex_t watchdogMutex;
struct timeval watchdogTimer = {0LL, 0LL};

static RTSPThreadParam rtspThreadParam;

static int relativeMouseMode = 0;
static int showCursor = 1;
static int windowSizeX[VIDEO_SOURCE_CHANNEL_MAX];
static int windowSizeY[VIDEO_SOURCE_CHANNEL_MAX];
// support resizable window
static int nativeSizeX[VIDEO_SOURCE_CHANNEL_MAX];
static int nativeSizeY[VIDEO_SOURCE_CHANNEL_MAX];
static map<unsigned int, int> windowId2ch;

// save files
static FILE *savefp_keyts = NULL;

#ifndef ANDROID
#define	DEFAULT_FONT		"FreeSans.ttf"
#define	DEFAULT_FONTSIZE	24
static TTF_Font *defFont = NULL;
#endif

// prsc_conf values
bool replay_from_keypress;
int replay_start_delay;
bool exit_after_replay;

static void
switch_fullscreen() {
	unsigned int flags;
	SDL_Window *w = NULL;
	pthread_mutex_lock(&rtspThreadParam.surfaceMutex[0]);
	if((w = rtspThreadParam.surface[0]) == NULL)
		goto quit;
	flags = SDL_GetWindowFlags(w);
	flags = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ^ SDL_WINDOW_FULLSCREEN_DESKTOP;
	SDL_SetWindowFullscreen(w, flags);
quit:
	pthread_mutex_unlock(&rtspThreadParam.surfaceMutex[0]);
	return;
}

static void
switch_grab_input(SDL_Window *w) {
	SDL_bool grabbed;
	int need_unlock = 0;
	//
	if(w == NULL) {
		pthread_mutex_lock(&rtspThreadParam.surfaceMutex[0]);
		w = rtspThreadParam.surface[0];
		need_unlock = 1;
	}
	if(w != NULL) {
		grabbed = SDL_GetWindowGrab(w);
		if(grabbed == SDL_FALSE)
			SDL_SetWindowGrab(w, SDL_TRUE);
		else
			SDL_SetWindowGrab(w, SDL_FALSE);
	}
	if(need_unlock) {
		pthread_mutex_unlock(&rtspThreadParam.surfaceMutex[0]);
	}
	return;
}

static int
xlat_mouseX(int ch, int x) {
	return (1.0 * nativeSizeX[ch] / windowSizeX[ch]) * x;
}

static int
xlat_mouseY(int ch, int y) {
	return (1.0 * nativeSizeY[ch] / windowSizeY[ch]) * y;
}

static void
create_overlay(struct RTSPThreadParam *rtspParam, int ch) {
	int w, h;
	AVPixelFormat format;
#if 1	// only support SDL2
	unsigned int renderer_flags = 0;
	int renderer_index = -1;
	SDL_Window *surface = NULL;
	SDL_Renderer *renderer = NULL;
	SDL_Texture *overlay = NULL;
#endif
	struct SwsContext *swsctx = NULL;
	dpipe_t *pipe = NULL;
	dpipe_buffer_t *data = NULL;
	char windowTitle[64];
	char pipename[64];
	//
	pthread_mutex_lock(&rtspParam->surfaceMutex[ch]);
	if(rtspParam->surface[ch] != NULL) {
		pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);
		rtsperror("ga-client: duplicated create window request - image comes too fast?\n");
		return;
	}
	w = rtspParam->width[ch];
	h = rtspParam->height[ch];
	format = rtspParam->format[ch];
	pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);
	// swsctx
	if((swsctx = create_frame_converter(w, h, format, w, h, AV_PIX_FMT_YUV420P)) == NULL) {
		rtsperror("ga-client: cannot create swsscale context.\n");
		exit(-1);
	}
	// pipeline
	snprintf(pipename, sizeof(pipename), "channel-%d", ch);
	if((pipe = dpipe_create(ch, pipename, POOLSIZE, sizeof(AVPicture))) == NULL) {
		rtsperror("ga-client: cannot create pipeline.\n");
		exit(-1);
	}
	for(data = pipe->in; data != NULL; data = data->next) {
		bzero(data->pointer, sizeof(AVPicture));
		if(avpicture_alloc((AVPicture*) data->pointer, AV_PIX_FMT_YUV420P, w, h) != 0) {
			rtsperror("ga-client: per frame initialization failed.\n");
			exit(-1);
		}
	}
	// sdl
	int wflag = 0;
#if 1	// only support SDL2
#ifdef	ANDROID
	wflag = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
#else
	wflag |= SDL_WINDOW_RESIZABLE;
	if(ga_conf_readbool("fullscreen", 0) != 0) {
		wflag |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS;
	}
#endif
	if(relativeMouseMode != 0) {
		wflag |= SDL_WINDOW_INPUT_GRABBED;
	}
	snprintf(windowTitle, sizeof(windowTitle), WINDOW_TITLE, ch, w, h);
	surface = SDL_CreateWindow(windowTitle,
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			w, h, wflag);
#endif
	if(surface == NULL) {
		rtsperror("ga-client: set video mode (create window) failed.\n");
		exit(-1);
	}
	//SDL_SetWindowMaximumSize(surface, w, h);
	SDL_SetWindowMinimumSize(surface, w>>2, h>>2);
	nativeSizeX[ch] = windowSizeX[ch] = w;
	nativeSizeY[ch] = windowSizeY[ch] = h;
	windowId2ch[SDL_GetWindowID(surface)] = ch;
	// move mouse to center
#if 1	// only support SDL2
	SDL_WarpMouseInWindow(surface, w/2, h/2);
#endif
	if(relativeMouseMode != 0) {
		SDL_SetRelativeMouseMode(SDL_TRUE);
		showCursor = 0;
		//SDL_ShowCursor(0);
#if 0		//// XXX: EXPERIMENTAL - switch twice to make it normal?
		switch_grab_input(NULL);
		SDL_SetRelativeMouseMode(SDL_FALSE);
		switch_grab_input(NULL);
		SDL_SetRelativeMouseMode(SDL_TRUE);
#endif		////
		ga_error("ga-client: relative mouse mode enabled.\n");
	}
	//
#if 1	// only support SDL2
	do {	// choose SW or HW renderer?
		// XXX: Windows crashed if there is not a HW renderer!
		int i, n = SDL_GetNumRenderDrivers();
		char renderer_name[64] = "";
		SDL_RendererInfo info;

		ga_conf_readv("video-renderer", renderer_name, sizeof(renderer_name));
		if(strcmp("software", renderer_name) == 0) {
			rtsperror("ga-client: configured to use software renderer.\n");
			renderer_flags = SDL_RENDERER_SOFTWARE;
		}

		for(i = 0; i < n; i++) {
			if(SDL_GetRenderDriverInfo(i, &info) < 0)
				continue;
			if(strcmp(renderer_name, info.name) == 0)
				renderer_index = i;
			rtsperror("ga-client: renderer#%d - %s (%s%s%s%s)%s\n",
				i, info.name,
				info.flags & SDL_RENDERER_SOFTWARE ? "SW" : "",
				info.flags & SDL_RENDERER_ACCELERATED? "HW" : "",
				info.flags & SDL_RENDERER_PRESENTVSYNC ? ",vsync" : "",
				info.flags & SDL_RENDERER_TARGETTEXTURE ? ",texture" : "",
				i != renderer_index ? "" : " *");
			if(renderer_flags != SDL_RENDERER_SOFTWARE && info.flags & SDL_RENDERER_ACCELERATED)
				renderer_flags = SDL_RENDERER_ACCELERATED;
		}
	} while(0);
	//
	renderer = SDL_CreateRenderer(surface, renderer_index, renderer_flags);
			//rtspconf->video_renderer_software ?
			//	SDL_RENDERER_SOFTWARE : renderer_flags);
	if(renderer == NULL) {
		rtsperror("ga-client: create renderer failed.\n");
		exit(-1);
	}
	//
	overlay = SDL_CreateTexture(renderer,
			SDL_PIXELFORMAT_YV12,
			SDL_TEXTUREACCESS_STREAMING,
			w, h);
#endif
	if(overlay == NULL) {
		rtsperror("ga-client: create overlay (textuer) failed.\n");
		exit(-1);
	}
	//
	pthread_mutex_lock(&rtspParam->surfaceMutex[ch]);
	rtspParam->pipe[ch] = pipe;
	rtspParam->swsctx[ch] = swsctx;
	rtspParam->overlay[ch] = overlay;
#if 1	// only support SDL2
	rtspParam->renderer[ch] = renderer;
	rtspParam->windowId[ch] = SDL_GetWindowID(surface);
#endif
	rtspParam->surface[ch] = surface;
	pthread_mutex_unlock(&rtspParam->surfaceMutex[ch]);
	//
	rtsperror("ga-client: window created successfully (%dx%d).\n", w, h);
	// initialize watchdog
	pthread_mutex_lock(&watchdogMutex);
	gettimeofday(&watchdogTimer, NULL);
	pthread_mutex_unlock(&watchdogMutex);
	//
	return;
}

static void
open_audio(struct RTSPThreadParam *rtspParam, AVCodecContext *adecoder) {
	SDL_AudioSpec wanted, spec;
	//
	wanted.freq = rtspconf->audio_samplerate;
	wanted.format = -1;
	if(rtspconf->audio_device_format == AV_SAMPLE_FMT_S16) {
		wanted.format = AUDIO_S16SYS;
	} else {
		rtsperror("ga-client: open audio - unsupported audio device format.\n");
		return;
	}
	wanted.channels = rtspconf->audio_channels;
	wanted.silence = 0;
	wanted.samples = SDL_AUDIO_BUFFER_SIZE;
	wanted.callback = audio_buffer_fill_sdl;
	wanted.userdata = adecoder;
	//
	pthread_mutex_lock(&rtspParam->audioMutex);
	if(rtspParam->audioOpened == true) {
		pthread_mutex_unlock(&rtspParam->audioMutex);
		return;
	}
	if(SDL_OpenAudio(&wanted, &spec) < 0) {
		pthread_mutex_unlock(&rtspParam->audioMutex);
		rtsperror("ga-client: open audio failed - %s\n", SDL_GetError());
		return;
	}
	//
	rtspParam->audioOpened = true;
	//
	SDL_PauseAudio(0);
	pthread_mutex_unlock(&rtspParam->audioMutex);
	rtsperror("ga-client: audio device opened.\n");
	return;
}

// negative x or y means centering-x and centering-y, respectively
static void
render_text(SDL_Renderer *renderer, SDL_Window *window, int x, int y, int line, const char *text) {
#ifdef ANDROID
	// not supported
#else
	SDL_Color textColor = {255, 255, 255};
	SDL_Surface *textSurface = TTF_RenderText_Solid(defFont, text, textColor);
	SDL_Rect dest = {0, 0, 0, 0}, boxRect;
	SDL_Texture *texture;
	int ww, wh;
	//
	if(window == NULL || renderer == NULL) {
		rtsperror("render_text: Invalid window(%p) or renderer(%p) received.\n",
			window, renderer);
		return;
	}
	//
	SDL_GetWindowSize(window, &ww, &wh);
	// centering X/Y?
	if(x >= 0) {	dest.x = x; }
	else {		dest.x = (ww - textSurface->w)/2; }
	if(y >= 0) {	dest.y = y; }
	else {		dest.y = (wh - textSurface->h)/2; }
	//
	dest.y += line * textSurface->h;
	dest.w = textSurface->w;
	dest.h = textSurface->h;
	//
	boxRect.x = dest.x - 6;
	boxRect.y = dest.y - 6;
	boxRect.w = dest.w + 12;
	boxRect.h = dest.h + 12;
	//
	if((texture = SDL_CreateTextureFromSurface(renderer, textSurface)) != NULL) {
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
		SDL_RenderFillRect(renderer, &boxRect);
		SDL_RenderCopy(renderer, texture, NULL, &dest);
		SDL_DestroyTexture(texture);
	} else {
		rtsperror("render_text: failed on creating text texture: %s\n", SDL_GetError());
	}
	//
	SDL_FreeSurface(textSurface);
#endif
	return;
}

#if 1
static void
render_image(struct RTSPThreadParam *rtspParam, int ch) {
	dpipe_buffer_t *data;
	AVPicture *vframe;
	SDL_Rect rect;
#if 1	// only support SDL2
	unsigned char *pixels;
	int pitch;
#endif
	//
	if((data = dpipe_load_nowait(rtspParam->pipe[ch])) == NULL) {
		return;
	}
	vframe = (AVPicture*) data->pointer;
	//
#if 1	// only support SDL2
	if(SDL_LockTexture(rtspParam->overlay[ch], NULL, (void**) &pixels, &pitch) == 0) {
		bcopy(vframe->data[0], pixels, rtspParam->width[ch] * rtspParam->height[ch]);
		bcopy(vframe->data[1], pixels+((pitch*rtspParam->height[ch]*5)>>2), rtspParam->width[ch] * rtspParam->height[ch] / 4);
		bcopy(vframe->data[2], pixels+pitch*rtspParam->height[ch], rtspParam->width[ch] * rtspParam->height[ch] / 4);
		SDL_UnlockTexture(rtspParam->overlay[ch]);
	} else {
		rtsperror("ga-client: lock textture failed - %s\n", SDL_GetError());
	}
#endif
	dpipe_put(rtspParam->pipe[ch], data);
	rect.x = 0;
	rect.y = 0;
	rect.w = rtspParam->width[ch];
	rect.h = rtspParam->height[ch];
#if 1	// only support SDL2
	SDL_RenderCopy(rtspParam->renderer[ch], rtspParam->overlay[ch], NULL, NULL);
	SDL_RenderPresent(rtspParam->renderer[ch]);
#endif
	//
	image_rendered = 1;
	//
	return;
}
#endif


struct KeyPress {
	string value;
	Uint32 timestamp;
	string type;
};

std::vector<KeyPress> keySequence;

std::vector<SDL_Event> eventArray;

vector<KeyPress> replaySequence;

void SerializeEventsToFile(vector<KeyPress> *keySequence, string fileName) {
	StringBuffer s;
	Writer<StringBuffer> writer(s);
	writer.StartArray();
	for (std::vector<KeyPress>::iterator it = keySequence->begin(); it != keySequence->end(); ++it) {
		writer.StartObject();
		writer.Key("value");
		writer.String(it->value.data());
		writer.Key("type");
		writer.String(it->type.data());
		writer.Key("timestamp");
		writer.Uint(it->timestamp);
		writer.EndObject();
	}
	writer.EndArray();

	ofstream file;
	file.open(fileName);
	file << s.GetString();
	file.close();
}

std::vector<KeyPress> ReadReplaySequenceFromFile(string filename) {
	std::vector<KeyPress> eventsFromFile;
	using namespace rapidjson;
	FILE* fp = fopen(filename.c_str(), "rb");
	char readBuffer[65536];
	FileReadStream is(fp, readBuffer, sizeof(readBuffer));
	Document d;
	d.ParseStream(is);
	rapidjson::Value::Array values = d.GetArray();
	for (rapidjson::Value::ConstValueIterator itr = values.Begin(); itr != values.End(); ++itr) {
		const Value& metadata = (*itr);
		KeyPress kp = {};
		for (Value::ConstMemberIterator itrObj = metadata.MemberBegin();
			itrObj != metadata.MemberEnd(); ++itrObj) {
			if (itrObj->value.IsString() && itrObj->name == "value") {
				string val = itrObj->value.GetString();
				kp.value = val;
			}
			else if (itrObj->value.IsString() && itrObj->name == "type") {
				string type = itrObj->value.GetString();
				kp.type = type;
			}
			else if (itrObj->value.IsInt()) {
				int time = itrObj->value.GetInt();
				kp.timestamp = time;
			}
		}
		eventsFromFile.push_back(kp);
	}

	for (vector<KeyPress>::iterator it = eventsFromFile.begin(); it != eventsFromFile.end(); ++it) {
		cout << "val:" << it->value << " " << it->type << " time:" << it->timestamp;
	}

	fclose(fp);
	return eventsFromFile;
}


void AddToKeySequence(SDL_Event *event) {
	Uint32 timestamp = event->key.timestamp;
	string key = SDL_GetKeyName(event->key.keysym.sym);
	string type = "UP";
	if (event->key.type == SDL_KEYDOWN) {
		type = "DOWN";
	}
	KeyPress kp = { key, timestamp, type };
	keySequence.push_back(kp);
	SerializeEventsToFile(&keySequence, "keyPress.json");
}


bool isReplay = false;
//pthread_mutex_t lock;

std::clock_t start;

Uint32 CalculateKeyDelay(KeyPress *currentKey, KeyPress *nextKey) {
	Uint32 currentTimeStamp = currentKey->timestamp;
	Uint32 nextTimeStamp = nextKey->timestamp;
	cout << "current time:" << currentTimeStamp << " next time:" << nextTimeStamp;
	return nextTimeStamp - currentTimeStamp;
}

bool stopReplay = false;

void writeFpsToFile(vector<timeval> *frameTimeStamps, string fileName) {
	ofstream file;
	file.open(fileName);
	for (size_t i = 0; i < frameTimeStamps->size(); ++i) {
		file << frameTimeStamps->at(i).tv_sec << "." << frameTimeStamps->at(i).tv_usec << endl;
	}
	file.close();
}

void SerializeCommandResponseTimesToFile(vector<Command> *commandList, string fileName) {
	StringBuffer s;
	Writer<StringBuffer> writer(s);
	writer.StartArray();
	for (std::vector<Command>::iterator it = commandList->begin(); it != commandList->end(); ++it) {
		writer.StartObject();
		writer.Key("CommandId");
		writer.Int(it->commandId);

		writer.Key("SentTimeStamp");
		string sentTime = to_string(static_cast<long long>(it->sentTimeStamp.tv_sec)) + "." + to_string(static_cast<long long>(it->sentTimeStamp.tv_usec));
		writer.String(sentTime.c_str(), (SizeType)sentTime.length());

		writer.Key("ReceivetimeStamp");
		string receivedTime = to_string(static_cast<long long>(it->receivedTimeStamp.tv_sec)) + "." + to_string(static_cast<long long>(it->receivedTimeStamp.tv_usec));
		writer.String(receivedTime.c_str(), (SizeType)receivedTime.length());

		writer.Key("Delay");
		long microseconds = (it->receivedTimeStamp.tv_sec - it->sentTimeStamp.tv_sec) * 1000000 + (it->receivedTimeStamp.tv_usec - it->sentTimeStamp.tv_usec);
		long milliseconds = microseconds / 1000;
		struct timeval delay;
		delay.tv_sec = microseconds / 1000000;
		delay.tv_usec = microseconds % 1000000;
		string delayTime = to_string(static_cast<long long>(delay.tv_sec)) + "." + to_string(static_cast<long long>(delay.tv_usec));
		writer.String(delayTime.c_str(), (SizeType)delayTime.length());
		writer.EndObject();
	}
	writer.EndArray();

	ofstream file;
	file.open(fileName);
	file << s.GetString();
	file.close();
}

void *replayEvents(void *ptr) {
	//pthread_mutex_lock(&lock);
	isReplay = false;
	stopReplay = true;
	//pthread_mutex_unlock(&lock);
	keySequence.clear(); // clear keys stored until replay
	std::vector<KeyPress> keysFromFile = ReadReplaySequenceFromFile("keyPress.json");
	for (vector<KeyPress>::iterator it = keysFromFile.begin(); it != keysFromFile.end(); ++it) {
		SDL_Event event = {};
		event.user.code = REPLAY_EVENT_CODE;
		if (it->type == "UP") {
			event.type = SDL_KEYUP;
		}
		else {
			event.type = SDL_KEYDOWN;
		}
		event.key.keysym.sym = SDL_GetKeyFromName(it->value.data());
		event.key.timestamp = it->timestamp;
		Uint32 delay = 0;
		if (it + 1 < keysFromFile.end()) {
			int nextEventIndex = (it - keysFromFile.begin()) + 1;
			KeyPress nextKey = keysFromFile[nextEventIndex];
			delay = CalculateKeyDelay(&*it, &nextKey);
		}
		SDL_PushEvent(&event);
		Sleep(delay);
	}

	//write response times to file
	SerializeCommandResponseTimesToFile(&commandList, "responseTime.json");
	writeFpsToFile(&frameTimeStamps, "fps.log");

	// exit client after replay
	if (exit_after_replay) {
		SDL_Event quit_event = {};
		quit_event.type = SDL_QUIT;
		SDL_PushEvent(&quit_event);
	}
	return NULL;
}

static unsigned char commandIdCounter = 0;

void
ProcessEvent(SDL_Event *event) {
	// start replay 5 sec after start
	if (replay_from_keypress) {
		if (!stopReplay) {
			double duration = (std::clock() - start) / (double)CLOCKS_PER_SEC;
			if (duration>replay_start_delay) {
				isReplay = true;
			}
		} else {
			isReplay = false;
		}
	}

	if (isReplay) {
		pthread_t replayThread;
		pthread_create(&replayThread, NULL, replayEvents, NULL);
	}
	sdlmsg_t m;
	map<unsigned int,int>::iterator mi;
	int ch;
	struct timeval tv;
	//
	switch(event->type) {
	case SDL_KEYUP:
		if (event->key.keysym.sym != SDLK_F6) {
			if (!isReplay && event->user.code != REPLAY_EVENT_CODE) {
				eventArray.push_back(*event);
				AddToKeySequence(event);
			}
		}
		if(event->key.keysym.sym == SDLK_BACKQUOTE
		&& relativeMouseMode != 0) {
			showCursor = 1 - showCursor;
			//SDL_ShowCursor(showCursor);
			switch_grab_input(NULL);
#if 1
			if(showCursor)
				SDL_SetRelativeMouseMode(SDL_FALSE);
			else
				SDL_SetRelativeMouseMode(SDL_TRUE);
#endif
		}
		// switch between fullscreen?
		if((event->key.keysym.sym == SDLK_RETURN)
		&& (event->key.keysym.mod & KMOD_ALT)) {
			// do nothing
		} else
		//
		if(rtspconf->ctrlenable) {
			if (event->key.keysym.sym == SDLK_F8) {
				m.which = 199; // PRSC start measuring encoding quality
			}
			else if (event->key.keysym.sym == SDLK_F7) {
				m.which = 198; // PRSC stop measuring encoding quality
			}
			else {
				m.which = commandIdCounter++;
				gettimeofday(&tv, NULL);
				Command cmd = { commandIdCounter, tv, 0 };
				commandList.push_back(cmd);
			}
		sdlmsg_keyboard(&m, 0,
			event->key.keysym.scancode,
			event->key.keysym.sym,
			event->key.keysym.mod,
			0/*event->key.keysym.unicode*/);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_keyboard_t));
		}
		if(savefp_keyts != NULL) {
			gettimeofday(&tv, NULL);
			ga_save_printf(savefp_keyts, "KEY-UP: %u.%06u scan 0x%04x sym 0x%04x mod 0x%04x\n",
				tv.tv_sec, tv.tv_usec,
				event->key.keysym.scancode,
				event->key.keysym.sym,
				event->key.keysym.mod);
		}
		break;
	case SDL_KEYDOWN:
		if (event->key.keysym.sym == SDLK_F6) {
			isReplay = true;
			return;
		}
		else {
			if (!isReplay && event->user.code != REPLAY_EVENT_CODE) {
				eventArray.push_back(*event);
				AddToKeySequence(event);
			}
			else if (event->user.code != REPLAY_EVENT_CODE) {
				cout << "down key: " << SDL_GetKeyName(event->key.keysym.sym) << " downtime:" << event->key.timestamp;
			}
		}
		// switch between fullscreen?
		if((event->key.keysym.sym == SDLK_RETURN)
		&& (event->key.keysym.mod & KMOD_ALT)) {
			switch_fullscreen();
		} else
		//
		if(rtspconf->ctrlenable) {
			m.which = commandIdCounter++;
			gettimeofday(&tv, NULL);
			Command cmd = { commandIdCounter, tv, 0 };
			commandList.push_back(cmd);
		sdlmsg_keyboard(&m, 1,
			event->key.keysym.scancode,
			event->key.keysym.sym,
			event->key.keysym.mod,
			0/*event->key.keysym.unicode*/);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_keyboard_t));
		}
		if(savefp_keyts != NULL) {
			gettimeofday(&tv, NULL);
			ga_save_printf(savefp_keyts, "KEY-DN: %u.%06u scan 0x%04x sym 0x%04x mod 0x%04x\n",
				tv.tv_sec, tv.tv_usec,
				event->key.keysym.scancode,
				event->key.keysym.sym,
				event->key.keysym.mod);
		}
		break;
	case SDL_MOUSEBUTTONUP:
		mi = windowId2ch.find(event->button.windowID);
		if(mi != windowId2ch.end() && rtspconf->ctrlenable) {
			ch = mi->second;
			sdlmsg_mousekey(&m, 0, event->button.button,
				xlat_mouseX(ch, event->button.x),
				xlat_mouseY(ch, event->button.y));
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
	case SDL_MOUSEBUTTONDOWN:
		mi = windowId2ch.find(event->button.windowID);
		if(mi != windowId2ch.end() && rtspconf->ctrlenable) {
			ch = mi->second;
			sdlmsg_mousekey(&m, 1, event->button.button,
				xlat_mouseX(ch, event->button.x),
				xlat_mouseY(ch, event->button.y));
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
	case SDL_MOUSEMOTION:
		mi = windowId2ch.find(event->motion.windowID);
		if(mi != windowId2ch.end() && rtspconf->ctrlenable && rtspconf->sendmousemotion) {
			ch = mi->second;
			sdlmsg_mousemotion(&m,
				xlat_mouseX(ch, event->motion.x),
				xlat_mouseY(ch, event->motion.y),
				xlat_mouseX(ch, event->motion.xrel),
				xlat_mouseY(ch, event->motion.yrel),
				event->motion.state,
				relativeMouseMode == 0 ? 0 : 1);
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
#if 1	// only support SDL2
	case SDL_MOUSEWHEEL:
		if(rtspconf->ctrlenable && rtspconf->sendmousemotion) {
			sdlmsg_mousewheel(&m, event->motion.x, event->motion.y);
			ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
#ifdef ANDROID
#define	DEBUG_FINGER(etf)	\
	rtsperror("XXX DEBUG: finger-event(%d) - x=%d y=%d dx=%d dy=%d p=%d\n",\
		(etf).type, (etf).x, (etf).y, (etf).dx, (etf).dy, (etf).pressure);
	case SDL_FINGERDOWN:
		// window size has not been registered
		if(nativeSizeX[0] == 0)
			break;
		//DEBUG_FINGER(event->tfinger);
		if(rtspconf->ctrlenable) {
		unsigned short mapx, mapy;
		mapx = (unsigned short) (1.0 * (nativeSizeX[0]-1) * event->tfinger.x / 32767.0);
		mapy = (unsigned short) (1.0 * (nativeSizeY[0]-1) * event->tfinger.y / 32767.0);
		sdlmsg_mousemotion(&m, mapx, mapy, 0, 0, 0, 0);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		//
		sdlmsg_mousekey(&m, 1, SDL_BUTTON_LEFT, mapx, mapy);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
	case SDL_FINGERUP:
		// window size has not been registered
		if(nativeSizeX[0] == 0)
			break;
		//DEBUG_FINGER(event->tfinger);
		if(rtspconf->ctrlenable) {
		unsigned short mapx, mapy;
		mapx = (unsigned short) (1.0 * (nativeSizeX[0]-1) * event->tfinger.x / 32767.0);
		mapy = (unsigned short) (1.0 * (nativeSizeY[0]-1) * event->tfinger.y / 32767.0);
		sdlmsg_mousemotion(&m, mapx, mapy, 0, 0, 0, 0);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		//
		sdlmsg_mousekey(&m, 0, SDL_BUTTON_LEFT, mapx, mapy);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
	case SDL_FINGERMOTION:
		// window size has not been registered
		if(nativeSizeX[0] == 0)
			break;
		//DEBUG_FINGER(event->tfinger);
		if(rtspconf->ctrlenable) {
		unsigned short mapx, mapy;
		mapx = (unsigned short) (1.0 * (nativeSizeX[0]-1) * event->tfinger.x / 32767.0);
		mapy = (unsigned short) (1.0 * (nativeSizeY[0]-1) * event->tfinger.y / 32767.0);
		sdlmsg_mousemotion(&m, mapx, mapy, 0, 0, 0, 0);
		ctrl_client_sendmsg(&m, sizeof(sdlmsg_mouse_t));
		}
		break;
#undef	DEBUG_FINGER
#endif	/* ANDROID */
	case SDL_WINDOWEVENT:
		if(event->window.event == SDL_WINDOWEVENT_CLOSE) {
			rtspThreadParam.running = false;
			return;
		} else if(event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
			mi = windowId2ch.find(event->window.windowID);
			if(mi != windowId2ch.end()) {
				int w, h, ch = mi->second;
				char title[64];
				w = event->window.data1;
				h = event->window.data2;
				windowSizeX[ch] = w;
				windowSizeY[ch] = h;
				snprintf(title, sizeof(title), WINDOW_TITLE, ch, w, h);
				SDL_SetWindowTitle(rtspThreadParam.surface[ch], title);
				rtsperror("event window #%d(%x) resized: w=%d h=%d\n",
					ch, event->window.windowID, w, h);
			}
		}
		break;
	case SDL_USEREVENT:
		if(event->user.code == SDL_USEREVENT_RENDER_IMAGE) {
			long long ch = (long long) event->user.data2;
			render_image((struct RTSPThreadParam*) event->user.data1, (int) ch & 0x0ffffffff);
			break;
		}
		if(event->user.code == SDL_USEREVENT_CREATE_OVERLAY) {
			long long ch = (long long) event->user.data2;
			create_overlay((struct RTSPThreadParam*) event->user.data1, (int) ch & 0x0ffffffff);
			break;
		}
		if(event->user.code == SDL_USEREVENT_OPEN_AUDIO) {
			open_audio(
				(struct RTSPThreadParam*) event->user.data1,
				(AVCodecContext*) event->user.data2);
			break;
		}
		if(event->user.code == SDL_USEREVENT_RENDER_TEXT) {
			//SDL_SetAlpha()
			SDL_SetRenderDrawColor(rtspThreadParam.renderer[0], 0, 0, 0, 192/*SDL_ALPHA_OPAQUE/2*/);
			//SDL_RenderFillRect(rtspThreadParam.renderer[0], NULL);
			render_text(rtspThreadParam.renderer[0],
				rtspThreadParam.surface[0],
				-1, -1, 0, (const char *) event->user.data1);
			SDL_RenderPresent(rtspThreadParam.renderer[0]);
			break;
		}
		break;
#endif /* SDL_VERSION_ATLEAST(2,0,0) */
	case SDL_QUIT:
		rtspThreadParam.running = false;
		return;
	default:
		// do nothing
		break;
	}
	return;
}

boolean checkTimeout = false;

static void *
watchdog_thread(void *args) {
	static char idlemsg[128];
	struct timeval tv;
	SDL_Event evt;
	//
	rtsperror("watchdog: launched, waiting for audio/video frames ...\n");
	//
	while(true) {
#ifdef WIN32
		Sleep(1000);
#else
		sleep(1);
#endif
		pthread_mutex_lock(&watchdogMutex);
		gettimeofday(&tv, NULL);
		if(watchdogTimer.tv_sec != 0) {
			long long d;
			d = tvdiff_us(&tv, &watchdogTimer);
			if (checkTimeout) {
				if (d > IDLE_MAXIMUM_THRESHOLD) {
					rtspThreadParam.running = false;
					break;
				}
				else if (d > IDLE_DETECTION_THRESHOLD) {
					// update message and show
					snprintf(idlemsg, sizeof(idlemsg),
						"Audio/video stall detected, waiting for %d second(s) to terminate ...",
						(int)(IDLE_MAXIMUM_THRESHOLD - d) / 1000000);
					//
					bzero(&evt, sizeof(evt));
					evt.user.type = SDL_USEREVENT;
					evt.user.timestamp = time(0);
					evt.user.code = SDL_USEREVENT_RENDER_TEXT;
					evt.user.data1 = idlemsg;
					evt.user.data2 = NULL;
					SDL_PushEvent(&evt);
					//
					rtsperror("watchdog: %s\n", idlemsg);
				}
				else {
					// do nothing
				}
			}
		} else {
			rtsperror("watchdog: initialized, but no frames received ...\n");
		}
		pthread_mutex_unlock(&watchdogMutex);
	}
	//
	rtsperror("watchdog: terminated.\n");
	exit(-1);
	//
	return NULL;
}

void init_configuration() {
	INIReader reader(PRSC_CONF);
	if (reader.ParseError() < 0) {
		std::cout << "Can't load '"<< PRSC_CONF << "'\n";
		return;
	}
	replay_from_keypress = reader.GetBoolean("ga-client", "replay_from_keypress", false);
	replay_start_delay = reader.GetInteger("ga-client", "replay_start_delay", 5);
	exit_after_replay = reader.GetBoolean("ga-client", "exit_after_replay", false);
}

int
main(int argc, char *argv[]) {
	int i;
	SDL_Event event;
	pthread_t rtspthread;
	pthread_t ctrlthread;
	pthread_t watchdog;
	char savefile_keyts[128];

	init_configuration();
	//start timer to replay commands
	start = std::clock();

	//
#ifdef ANDROID
	if(ga_init("/sdcard/ga/android.conf", NULL) < 0) {
		rtsperror("cannot load configuration file '%s'\n", argv[1]);
		return -1;
	}
#else
	if(argc < 3) {
		rtsperror("usage: %s config url\n", argv[0]);
		return -1;
	}
	//
	if(ga_init(argv[1], argv[2]) < 0) {
		rtsperror("cannot load configuration file '%s'\n", argv[1]);
		return -1;
	}
#endif
	// enable logging
	ga_openlog();
	//
	if(ga_conf_readbool("control-relative-mouse-mode", 0) != 0) {
		rtsperror("*** Relative mouse mode enabled.\n");
		relativeMouseMode = 1;
	}
	//
	if(ga_conf_readv("save-key-timestamp", savefile_keyts, sizeof(savefile_keyts)) != NULL) {
		savefp_keyts = ga_save_init_txt(savefile_keyts);
		rtsperror("*** SAVEFILE: key timestamp saved fo '%s'\n",
			savefp_keyts ? savefile_keyts : "NULL");
	}
	//
	rtspconf = rtspconf_global();
	if(rtspconf_parse(rtspconf) < 0) {
		rtsperror("parse configuration failed.\n");
		return -1;
	}
	//
#if ! defined WIN32 && ! defined __APPLE__ && ! defined ANDROID
	if(XInitThreads() == 0) {
		rtsperror("XInitThreads() failed, client terminated.\n");
		return -1;
	}
#endif
#ifndef ANDROID
	// init fonts
	if(TTF_Init() != 0) {
		rtsperror("cannot initialize SDL_ttf: %s\n", SDL_GetError());
		return -1;
	}
	if((defFont = TTF_OpenFont(DEFAULT_FONT, DEFAULT_FONTSIZE)) == NULL) {
		rtsperror("open font '%s' failed: %s\n",
			DEFAULT_FONT, SDL_GetError());
		return -1;
	}
#endif
	//
	rtspconf_resolve_server(rtspconf, rtspconf->servername);
	rtsperror("Remote server @ %s[%s]:%d\n",
		rtspconf->servername,
		inet_ntoa(rtspconf->sin.sin_addr),
		rtspconf->serverport);
	//
	if(SDL_Init(SDL_INIT_EVERYTHING) < 0) {
		rtsperror("SDL init failed: %s\n", SDL_GetError());
		return -1;
	}
	if(rtspconf->video_renderer_software == 0) {
		ga_error("SDL: prefer opengl hardware renderer.\n");
		SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
	}
#if 0	// only support SDL2
	// enable keyboard repeat?
	SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
#endif
	// launch controller?
	do if(rtspconf->ctrlenable) {
		if(ctrl_queue_init(32768, sizeof(sdlmsg_t)) < 0) {
			rtsperror("Cannot initialize controller queue, controller disabled.\n");
			rtspconf->ctrlenable = 0;
			break;
		}
		if(pthread_create(&ctrlthread, NULL, ctrl_client_thread, rtspconf) != 0) {
			rtsperror("Cannot create controller thread, controller disabled.\n");
			rtspconf->ctrlenable = 0;
			break;
		}
		pthread_detach(ctrlthread);
	} while(0);
	// launch watchdog
	pthread_mutex_init(&watchdogMutex, NULL);
	if(ga_conf_readbool("enable-watchdog", 1) == 1) {
		if(pthread_create(&watchdog, NULL, watchdog_thread, NULL) != 0) {
			rtsperror("Cannot create watchdog thread.\n");
			return -1;
		}
		pthread_detach(watchdog);
	} else {
		ga_error("watchdog disabled.\n");
	}
	//
	bzero(&rtspThreadParam, sizeof(rtspThreadParam));
	for(i = 0; i < VIDEO_SOURCE_CHANNEL_MAX; i++) {
		pthread_mutex_init(&rtspThreadParam.surfaceMutex[i], NULL);
	}
	pthread_mutex_init(&rtspThreadParam.audioMutex, NULL);
	rtspThreadParam.url = strdup(argv[2]);
	rtspThreadParam.running = true;
	if(pthread_create(&rtspthread, NULL, rtsp_thread, &rtspThreadParam) != 0) {
		rtsperror("Cannot create rtsp client thread.\n");
		return -1;
	}
	pthread_detach(rtspthread);
	//
	while(rtspThreadParam.running) {
		if(SDL_WaitEvent(&event)) {
			ProcessEvent(&event);
		}
	}
	//
	rtspThreadParam.quitLive555 = 1;
	rtsperror("terminating ...\n");
	//
#ifndef ANDROID
	pthread_cancel(rtspthread);
	if(rtspconf->ctrlenable)
		pthread_cancel(ctrlthread);
	pthread_cancel(watchdog);
#endif
	//SDL_WaitThread(thread, &status);
	//
	if(savefp_keyts != NULL) {
		ga_save_close(savefp_keyts);
		savefp_keyts = NULL;
	}
	SDL_Quit();
	ga_deinit();
	exit(0);
	//
	return 0;
}

