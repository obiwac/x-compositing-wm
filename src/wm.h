// this file contains helpers for the window manager

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <X11/extensions/Xinerama.h>

#if !defined(DEBUGGING)
	#define DEBUGGING 0
#endif

#define WM_NAME "Basic X compositing WM"

// structures and types

typedef void (*wm_keyboard_event_callback_t) (void*, unsigned window, unsigned press, unsigned modifiers, unsigned key);
typedef int  (*wm_click_event_callback_t) (void*, unsigned window, unsigned press, unsigned modifiers, unsigned button, float x, float y);
typedef void (*wm_move_event_callback_t) (void*, unsigned window, unsigned modifiers, float x, float y);

typedef void (*wm_create_event_callback_t) (void*, unsigned window);
typedef void (*wm_modify_event_callback_t) (void*, unsigned window, int visible, float x, float y, float width, float height);
typedef void (*wm_destroy_event_callback_t) (void*, unsigned window);

typedef struct {
	int exists;
	Window window;

	int visible;

	int x, y;
	int width, height;

	// this is extra data that can be allocated by extensions such as a compositor
	void* internal;
} wm_window_t;

typedef struct {
	Display* display;
	int screen;

	Window root_window;

	unsigned width;
	unsigned height;

	wm_window_t* windows;
	int window_count;

	// individual monitor information

	int monitor_count;
	XineramaScreenInfo* monitor_infos;

	// atoms (used for communicating information about the window manager to other clients)

	Atom client_list_atom;

	// list of windows that are blacklisted for events
	// this is mostly useful for non-application windows that the client doesn't care about

	Window* event_blacklisted_windows;
	int event_blacklisted_window_count;

	// event callbacks

	wm_keyboard_event_callback_t keyboard_event_callback;
	wm_click_event_callback_t    click_event_callback;
	wm_move_event_callback_t     move_event_callback;

	wm_create_event_callback_t   create_event_callback;
	wm_modify_event_callback_t   modify_event_callback;
	wm_destroy_event_callback_t  destroy_event_callback;
} wm_t;

// utility functions

static void wm_error(wm_t* wm, const char* message) {
	fprintf(stderr, "[WM_ERROR:%p] %s\n", wm, message);
	exit(1);
}

// don't forget for all the functions dealing with the y coordinate:
// X coordinates start from the top left, whereas AQUA coordinates start from the bottom left (where they should be!)

static inline float wm_width_dimension_to_float (wm_t* wm, int pixels) { return (float) pixels / wm->width  * 2; }
static inline float wm_height_dimension_to_float(wm_t* wm, int pixels) { return (float) pixels / wm->height * 2; }

static inline float wm_x_coordinate_to_float(wm_t* wm, int pixels) { return  wm_width_dimension_to_float (wm, pixels) - 1; }
static inline float wm_y_coordinate_to_float(wm_t* wm, int pixels) { return -wm_height_dimension_to_float(wm, pixels) + 1; }

static inline int wm_float_to_width_dimension (wm_t* wm, float x) { return (int) round(x / 2 * wm->width);  }
static inline int wm_float_to_height_dimension(wm_t* wm, float x) { return (int) round(x / 2 * wm->height); }

static inline int wm_float_to_x_coordinate(wm_t* wm, float x) { return wm_float_to_width_dimension (wm,  x + 1); }
static inline int wm_float_to_y_coordinate(wm_t* wm, float x) { return wm_float_to_height_dimension(wm, -x + 1); }

static int wm_event_blacklisted_window(wm_t* wm, Window window) {
	for (int i = 0; i < wm->event_blacklisted_window_count; i++) {
		if (window == wm->event_blacklisted_windows[i]) {
			return 1;
		}
	}

	return 0;
}

static int wm_find_window_by_xid(wm_t* wm, Window xid) {
	for (int i = 0; i < wm->window_count; i++) {
		wm_window_t* window = &wm->windows[i];

		// it's really super important to verify our window actually exists
		// we could have a window that doesn't exist anymore, but that had the same ID as one that currently exists

		if (window->exists && window->window == xid) {
			return i;
		}
	}

	// this shouldn't ever happen normally
	// we allow it when debugging, because sometimes it's useful to run our WM at the same time as another is running
	// so even if *this* WM has never heard of a certain window, it's possible it's been modified in our previous WM

	#if !defined(DEBUGGING)
		wm_error(wm, "Nonexistant window XID");
	#endif

	return -1;
}

static void wm_sync_window(wm_t* wm, wm_window_t* window) {
	XWindowAttributes attributes;

	XGetWindowAttributes(wm->display, window->window, &attributes);

	window->visible = attributes.map_state == IsViewable;

	window->x = attributes.x;
	window->y = attributes.y;

	window->width  = attributes.width;
	window->height = attributes.height;

	// TODO also get opacity of window here using the '_NET_WM_WINDOW_OPACITY' atom
	//      see if this also is useful for checking if a window actually uses transparency at all (so that programs like OBS don't break)
	//      (although I don't know how much of OBS breaking is due to GLX transparency not being properly implemented by me or due to some other reason where the alpha channel has garbage written in it)
	//      when you re-implement transparency, don't forget to set the 'GLX_TEXTURE_FORMAT_EXT' attribute in 'cwm.h' and uncomment opacity in the shader code in 'main.c'
}

static void wm_update_client_list(wm_t* wm) {
	int existing_window_count = 0;

	for (int i = 0; i < wm->window_count; i++) {
		existing_window_count += wm->windows[i].exists;
	}

	Window client_list[existing_window_count];

	for (int i = 0; i < existing_window_count; i++) {
		client_list[i] = wm->windows[i].window;
	}

	XChangeProperty(wm->display, wm->root_window, wm->client_list_atom, XA_WINDOW, 32, PropModeReplace, (unsigned char*) client_list, existing_window_count);
}

static int wm_error_handler(Display* display, XErrorEvent* event) {
	if (!event->resourceid) return 0; // invalid window

	char buffer[1024];
	XGetErrorText(display, event->error_code, buffer, sizeof(buffer));

	printf("XError code = %d, string = %s, resource ID = 0x%lx\n", event->error_code, buffer, event->resourceid);
	return 0;
}

// exposed wm functions

void new_wm(wm_t* wm) {
	memset(wm, 0, sizeof(*wm));

	wm->display = XOpenDisplay(NULL /* default to 'DISPLAY' environment variable */);
	if (!wm->display) wm_error(wm, "Failed to open display");

	// this call is here for debugging, so that errors are reported synchronously as they occur
	XSynchronize(wm->display, DEBUGGING);

	// get screen and root window

	wm->screen = DefaultScreen(wm->display);
	wm->root_window = DefaultRootWindow(wm->display);

	// get width/height of root window

	XWindowAttributes attributes;
	XGetWindowAttributes(wm->display, wm->root_window, &attributes);

	wm->width  = attributes.width;
	wm->height = attributes.height;

	// tell X to send us all 'CreateNotify', 'ConfigureNotify', and 'DestroyNotify' events ('SubstructureNotifyMask' also sends back some other events but we're not using those)

	XSelectInput(wm->display, wm->root_window, SubstructureNotifyMask | PointerMotionMask | ButtonMotionMask | ButtonPressMask | ButtonReleaseMask);

	XGrabKey(wm->display, XKeysymToKeycode(wm->display, XStringToKeysym("F1")), Mod4Mask, wm->root_window, 0, GrabModeAsync, GrabModeAsync);
	XGrabKey(wm->display, XKeysymToKeycode(wm->display, XStringToKeysym("q")), Mod4Mask, wm->root_window, 0, GrabModeAsync, GrabModeAsync);
	XGrabKey(wm->display, XKeysymToKeycode(wm->display, XStringToKeysym("f")), Mod4Mask | Mod1Mask, wm->root_window, 0, GrabModeAsync, GrabModeAsync);
	XGrabKey(wm->display, XKeysymToKeycode(wm->display, XStringToKeysym("f")), Mod4Mask, wm->root_window, 0, GrabModeAsync, GrabModeAsync);
	XGrabKey(wm->display, XKeysymToKeycode(wm->display, XStringToKeysym("t")), Mod4Mask, wm->root_window, 0, GrabModeAsync, GrabModeAsync);
	XGrabKey(wm->display, XKeysymToKeycode(wm->display, XStringToKeysym("v")), Mod4Mask, wm->root_window, 0, GrabModeAsync, GrabModeAsync);
	XGrabKey(wm->display, XKeysymToKeycode(wm->display, XStringToKeysym("r")), Mod4Mask, wm->root_window, 0, GrabModeAsync, GrabModeAsync);

	// setup our atoms (explained in more detail in the 'wm_t' struct)
	// we also need to specify which atoms are supported in '_NET_SUPPORTED'

	wm->client_list_atom = XInternAtom(wm->display, "_NET_CLIENT_LIST", 0);

	Atom supported_list_atom = XInternAtom(wm->display, "_NET_SUPPORTED", 0);
	Atom supported_atoms[] = { supported_list_atom, wm->client_list_atom };

	XChangeProperty(wm->display, wm->root_window, supported_list_atom, XA_ATOM, 32, PropModeReplace, (const unsigned char*) supported_atoms, sizeof(supported_atoms) / sizeof(*supported_atoms));

	// now, we move on to '_NET_SUPPORTING_WM_CHECK'
	// this is a bit weird, but it's all specified by the EWMH spec: https://developer.gnome.org/wm-spec/

	Atom supporting_wm_check_atom = XInternAtom(wm->display, "_NET_SUPPORTING_WM_CHECK", 0);
	Window support_window = XCreateSimpleWindow(wm->display, wm->root_window, 0, 0, 1, 1, 0, 0, 0);

	Window support_window_list[1] = { support_window };

	XChangeProperty(wm->display, wm->root_window, supporting_wm_check_atom, XA_WINDOW, 32, PropModeReplace, (const unsigned char*) support_window_list, 1);
	XChangeProperty(wm->display, support_window,  supporting_wm_check_atom, XA_WINDOW, 32, PropModeReplace, (const unsigned char*) support_window_list, 1);

	Atom name_atom = XInternAtom(wm->display, "_NET_WM_NAME", 0);
	XChangeProperty(wm->display, support_window, name_atom, XA_STRING, 8, PropModeReplace, (const unsigned char*) WM_NAME, sizeof(WM_NAME));

	// get all monitors and their individual resolutions

	wm->monitor_infos = XineramaQueryScreens(wm->display, &wm->monitor_count);

	// TODO REMME, this was just for testing

	// wm->monitor_count = 3; // virtual monitors
	// wm->monitor_infos = (XineramaScreenInfo*) malloc(wm->monitor_count * sizeof(XineramaScreenInfo));

	// // first virtual monitor (1920x1080+0+0)

	// wm->monitor_infos[0].x_org = 0;
	// wm->monitor_infos[0].y_org = 0;

	// wm->monitor_infos[0].width  = 1920;
	// wm->monitor_infos[0].height = 1080;

	// // second virtual monitor (640x1024+1920+56)

	// wm->monitor_infos[1].x_org = 1920;
	// wm->monitor_infos[1].y_org = 56;

	// wm->monitor_infos[1].width  = 640;
	// wm->monitor_infos[1].height = 1024;

	// // third virtual monitor (640x1024+2560+56)

	// wm->monitor_infos[2].x_org = 2560;
	// wm->monitor_infos[2].y_org = 56;

	// wm->monitor_infos[2].width  = 640;
	// wm->monitor_infos[2].height = 1024;

	// create our own error handler so X doesn't crash

	XSetErrorHandler(wm_error_handler);

	// setup event blacklist
	// the first window we wanna blacklist is the root window (TODO really necessary? I think it works just fine without... investigate!)
	// we also wanna blacklist the supporting window from earlier

	wm->event_blacklisted_windows = (Window*) malloc(sizeof(Window));
	wm->event_blacklisted_window_count = 1;

	wm->event_blacklisted_windows[0] = support_window;

	// wm->event_blacklisted_window_count = 1;
	// wm->event_blacklisted_windows[0] = wm->root_window;

	// setup windows

	wm->windows = (wm_window_t*) malloc(1);
	wm->window_count = 0;
}

int wm_x_resolution(wm_t* wm) { return wm->width;  }
int wm_y_resolution(wm_t* wm) { return wm->height; }

int wm_monitor_count(wm_t* wm) { return wm->monitor_count; }

float wm_monitor_x(wm_t* wm, int monitor_index) { return wm_x_coordinate_to_float(wm, wm->monitor_infos[monitor_index].x_org + wm->monitor_infos[monitor_index].width  / 2); }
float wm_monitor_y(wm_t* wm, int monitor_index) { return wm_y_coordinate_to_float(wm, wm->monitor_infos[monitor_index].y_org + wm->monitor_infos[monitor_index].height / 2); }

float wm_monitor_width (wm_t* wm, int monitor_index) { return wm_width_dimension_to_float (wm, wm->monitor_infos[monitor_index].width ); }
float wm_monitor_height(wm_t* wm, int monitor_index) { return wm_height_dimension_to_float(wm, wm->monitor_infos[monitor_index].height); }

// useful functions for managing windows

void wm_close_window(wm_t* wm, unsigned window_id) {
	// all this fuss is to close a window softly
	// use the 'wm_kill_window' to force-close a window

	XEvent event;

	event.xclient.type = ClientMessage;
	event.xclient.window = wm->windows[window_id].window;
	event.xclient.message_type = XInternAtom(wm->display, "WM_PROTOCOLS", 1);
	event.xclient.format = 32;
	event.xclient.data.l[0] = XInternAtom(wm->display, "WM_DELETE_WINDOW", 0);
	event.xclient.data.l[1] = CurrentTime;

	XSendEvent(wm->display, wm->windows[window_id].window, 0, NoEventMask, &event);
}

void wm_kill_window(wm_t* wm, unsigned window_id) {
	// this function properly kills windows
	// use this sparingly, when force-closing an unresponsive window for example

	XDestroyWindow(wm->display, wm->windows[window_id].window);
}

void wm_move_window(wm_t* wm, unsigned window_id, float x, float y, float width, float height) {
	XMoveResizeWindow(wm->display, wm->windows[window_id].window,
		wm_float_to_x_coordinate(wm, x - width / 2), wm_float_to_y_coordinate(wm, y + height / 2),
		wm_float_to_width_dimension(wm, width), wm_float_to_height_dimension(wm, height));
}

void wm_focus_window(wm_t* wm, unsigned window_id) {
	Window window = wm->windows[window_id].window;

	XSetInputFocus(wm->display, window, RevertToParent, CurrentTime);
	XMapRaised(wm->display, window);
}

// event processing call

int wm_process_events(wm_t* wm, void* thing) {
	int events_left = XPending(wm->display);

	if (events_left) {
		XEvent event;
		XNextEvent(wm->display, &event);

		int type = event.type;

		if (type == KeyPress || type == KeyRelease) {
			if (wm->keyboard_event_callback) {
				wm->keyboard_event_callback(thing, wm_find_window_by_xid(wm, event.xkey.window),
					event.xkey.type == KeyPress, event.xkey.state, event.xkey.keycode);
			}
		}

		else if (type == ButtonPress || type == ButtonRelease) {
			if (wm->click_event_callback) {
				unsigned window = -1;

				if (!wm_event_blacklisted_window(wm, event.xbutton.window)) {
					window = wm_find_window_by_xid(wm, event.xbutton.window);
				}

				if (wm->click_event_callback(thing, window,
						event.xbutton.type == ButtonPress, event.xbutton.state, event.xbutton.button,
						wm_x_coordinate_to_float(wm, event.xbutton.x_root), wm_y_coordinate_to_float(wm, event.xbutton.y_root))) {

					// pass the event on to the client
					XAllowEvents(wm->display, ReplayPointer, CurrentTime);
				}

				else {
					// if we shouldn't pass the event on to the client, we still need to sync the pointer or else we hang
					XAllowEvents(wm->display, SyncPointer, CurrentTime);
				}
			}
		}

		else if (type == MotionNotify) {
			if (wm->move_event_callback) {
				wm->move_event_callback(thing, wm_find_window_by_xid(wm, event.xmotion.subwindow),
					event.xmotion.state,
					wm_x_coordinate_to_float(wm, event.xmotion.x_root), wm_y_coordinate_to_float(wm, event.xmotion.y_root));
			}
		}

		// window notification events

		else if (type == CreateNotify) {
			Window x_window = event.xcreatewindow.window;
			if (wm_event_blacklisted_window(wm, x_window)) goto done;

			wm_window_t* window = (wm_window_t*) 0;
			int window_index;

			// search for an empty space in the window list

			for (window_index = 0; window_index < wm->window_count; window_index++) {
				if (!wm->windows[window_index].exists) {
					window = &wm->windows[window_index];
					break;
				}
			}

			// if no empty space found, add one

			if (!window) {
				wm->windows = (wm_window_t*) realloc(wm->windows, (wm->window_count + 1) * sizeof(wm_window_t));
				window_index = wm->window_count;

				window = &wm->windows[window_index];
				wm->window_count++;
			}

			memset(window, 0, sizeof(*window));

			window->exists = 1;
			window->window = x_window;

			if (wm->create_event_callback) {
				wm->create_event_callback(thing, window_index);
			}

			// set up some other stuff for the window
			// this is saying we want focus change and button events from the window

			XSelectInput(wm->display, x_window, FocusChangeMask);
			XGrabButton(wm->display, AnyButton, AnyModifier, x_window, 1, ButtonPressMask | ButtonReleaseMask | ButtonMotionMask, GrabModeSync, GrabModeSync, 0, 0);

			wm_update_client_list(wm);
		}

		// TODO 'VisibilityNotify'?

		else if (type == ConfigureNotify || type == MapNotify /* show window */ || type == UnmapNotify /* hide window */) {
			Window x_window;

			if (type == ConfigureNotify) x_window = event.xconfigure.window;
			else if (type == MapNotify) x_window = event.xmap.window;
			else if (type == UnmapNotify) x_window = event.xunmap.window;

			if (wm_event_blacklisted_window(wm, x_window)) goto done;

			int window_index = wm_find_window_by_xid(wm, x_window);
			if (window_index < 0) goto done;
			wm_window_t* window = &wm->windows[window_index];

			int was_visible = window->visible;
			wm_sync_window(wm, window);

			// if window wasn't visible before but is now, center it to the cursor position

			if (window->visible && !was_visible && !window->x && !window->y) {
				__attribute__((unused)) Window rw, cw; // root_return, child_return
				__attribute__((unused)) int wx, wy; // win_x_return, win_y_return
				__attribute__((unused)) unsigned int mask; // mask_return

				int x, y;
				XQueryPointer(wm->display, window->window, &rw, &cw, &x, &y, &wx, &wy, &mask);

				window->x = x - window->width  / 2;
				window->y = y - window->height / 2;

				XMoveWindow(wm->display, window->window, window->x, window->y);
			}

			if (wm->modify_event_callback) {
				wm->modify_event_callback(thing, window_index, window->visible,
					wm_x_coordinate_to_float(wm, window->x + window->width / 2), wm_y_coordinate_to_float(wm, window->y + window->height / 2),
					wm_width_dimension_to_float (wm, window->width), wm_height_dimension_to_float(wm, window->height));
			}
		}

		// else if (type == FocusIn) {
		// 	Window x_window = event.xfocus.window;
		// 	if (wm_event_blacklisted_window(wm, x_window)) goto done;

		// 	int window_index = wm_find_window_by_xid(wm, x_window);
		// 	if (window_index < 0) goto done;

		// 	if (wm->focus_event_callback) {
		// 		wm->focus_event_callback(thing, window_index);
		// 	}
		// }

		else if (type == DestroyNotify) {
			Window x_window = event.xdestroywindow.window;
			if (!x_window) goto done;

			int window_index = wm_find_window_by_xid(wm, x_window);
			if (window_index < 0) goto done;
			wm_window_t* window = &wm->windows[window_index];

			if (wm->destroy_event_callback) {
				wm->destroy_event_callback(thing, window_index);
			}

			// remove the window from our list
			wm->windows[window_index].exists = 0;

			wm_update_client_list(wm);
		}
	}

done:
	return events_left;
}
