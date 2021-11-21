// this file contains helpers for the compositing part of the window manager prototype

#include <wm.h>

#include <X11/extensions/Xcomposite.h>

// we need to use Xfixes because, for whatever reason, X (and even Xcomposite, which is even weirder) doesn't include a way to make windows transparent to events

#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

// we use GLEW to help us load most of the OpenGL functions we're using
// it is important that it goes before the 'glx.h' include

#include <GL/glew.h>
#include <GL/glx.h>

// standard library includes

#include <unistd.h>
#include <sys/time.h>

// defines and stuff for GLX

#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092

typedef GLXContext (*glXCreateContextAttribsARB_t) (Display*, GLXFBConfig, GLXContext, Bool, const int*);

typedef void (*glXBindTexImageEXT_t) (Display*, GLXDrawable, int, const int*);
typedef void (*glXReleaseTexImageEXT_t) (Display*, GLXDrawable, int);

typedef void (*glXSwapIntervalEXT_t) (Display*, GLXDrawable, int);

// structures and types

typedef struct {
	wm_t* wm;

	int vsync;
	struct timeval previous_time;

	Window overlay_window;
	Window output_window;

	GLXContext glx_context;

	int glx_config_count;
	GLXFBConfig* glx_configs;

	glXBindTexImageEXT_t glXBindTexImageEXT;
	glXReleaseTexImageEXT_t glXReleaseTexImageEXT;
} cwm_t;

typedef struct {
	GLXPixmap pixmap;
} cwm_window_internal_t;

// functions

void new_cwm(cwm_t* cwm, wm_t* wm) {
	memset(cwm, 0, sizeof(*cwm));
	cwm->wm = wm;

	// make it so that our compositing window manager can be recognized as such by other processes

	Window screen_owner = XCreateSimpleWindow(wm->display, wm->root_window, 0, 0, 1, 1, 0, 0, 0);
	Xutf8SetWMProperties(wm->display, screen_owner, "xcompmgr", "xcompmgr", NULL, 0, NULL, NULL, NULL);

	char name[] = "_NET_WM_CM_S##";
	snprintf(name, sizeof(name), "_NET_WM_CM_S%d", cwm->wm->screen);

	Atom atom = XInternAtom(wm->display, name, 0);
	XSetSelectionOwner(wm->display, atom, screen_owner, 0);

	// we want to enable manual redirection, because we want to track damage and flush updates ourselves
	// if we were to pass 'CompositeRedirectAutomatic' instead, the server would handle all that internally

	XCompositeRedirectSubwindows(wm->display, wm->root_window, CompositeRedirectManual);

	// get the overlay window
	// this window allows us to draw what we want on a layer between normal windows and the screensaver without interference

	cwm->overlay_window = XCompositeGetOverlayWindow(wm->display, wm->root_window);

	// explained in more detail in the comment before '#include <X11/extensions/Xfixes.h>'
	// basically, make the overlay transparent to events and pass them on through to lower windows

	XserverRegion region = XFixesCreateRegion(wm->display, NULL, 0);
	XFixesSetWindowShapeRegion(wm->display, cwm->overlay_window, ShapeInput, 0, 0, region);
	XFixesDestroyRegion(wm->display, region);

	// create the output window
	// this window is where the actual drawing is going to happen

	/* const */ int default_visual_attributes[] = {
		GLX_RGBA, GLX_DOUBLEBUFFER,
		GLX_SAMPLE_BUFFERS, 1,
		GLX_SAMPLES, 4,
		GLX_RED_SIZE, 8,
		GLX_GREEN_SIZE, 8,
		GLX_BLUE_SIZE, 8,
		GLX_ALPHA_SIZE, 8,
		GLX_DEPTH_SIZE, 16, 0
	};

	XVisualInfo* default_visual = glXChooseVisual(wm->display, wm->screen, default_visual_attributes);
	if (!default_visual) wm_error(wm, "Failed to get default GLX visual");

	XSetWindowAttributes attributes = {
		.colormap = XCreateColormap(wm->display, wm->root_window, default_visual->visual, AllocNone),
		.border_pixel = 0,
	};

	cwm->output_window = XCreateWindow(
		wm->display, wm->root_window, 0, 0, wm->width, wm->height, 0, default_visual->depth,
		InputOutput, default_visual->visual, CWBorderPixel | CWColormap, &attributes);

	XReparentWindow(wm->display, cwm->output_window, cwm->overlay_window, 0, 0);
	XMapRaised(wm->display, cwm->output_window);

	// get the GLX frame buffer configurations that match our specified attributes
	// generally we'll just be using the first one ('glx_configs[0]')

	const int config_attributes[] = {
		GLX_BIND_TO_TEXTURE_RGBA_EXT, 1,
		GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
		GLX_RENDER_TYPE, GLX_RGBA_BIT,
		GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
		GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
		GLX_X_RENDERABLE, 1,
		GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT, (GLint) GLX_DONT_CARE,
		GLX_BUFFER_SIZE, 32,
//		GLX_SAMPLE_BUFFERS, 1,
//		GLX_SAMPLES, 4,
		GLX_DOUBLEBUFFER, 1,
		GLX_RED_SIZE, 8,
		GLX_GREEN_SIZE, 8,
		GLX_BLUE_SIZE, 8,
		GLX_ALPHA_SIZE, 8,
		GLX_STENCIL_SIZE, 0,
		GLX_DEPTH_SIZE, 16, 0
	};

	cwm->glx_configs = glXChooseFBConfig(wm->display, wm->screen, config_attributes, &cwm->glx_config_count);
	if (!cwm->glx_configs) wm_error(wm, "Failed to get GLX frame buffer configurations");

	// create our OpenGL context
	// we must load the 'glXCreateContextAttribsARB' function ourselves

	const int gl_version_attributes[] = { // we want OpenGL 3.3
		GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
		GLX_CONTEXT_MINOR_VERSION_ARB, 3,
		GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, 0
	};

	glXCreateContextAttribsARB_t glXCreateContextAttribsARB = (glXCreateContextAttribsARB_t) glXGetProcAddressARB((const GLubyte*) "glXCreateContextAttribsARB");
	cwm->glx_context = glXCreateContextAttribsARB(wm->display, cwm->glx_configs[0], NULL, 1, gl_version_attributes);
	if (!cwm->glx_context) wm_error(wm, "Failed to create OpenGL context");

	// load the other two functions we need but don't have

	cwm->glXBindTexImageEXT = (glXBindTexImageEXT_t) glXGetProcAddress((const GLubyte*) "glXBindTexImageEXT");
	cwm->glXReleaseTexImageEXT = (glXReleaseTexImageEXT_t) glXGetProcAddress((const GLubyte*) "glXReleaseTexImageEXT");

	// finally, make the context we just made the OpenGL context of this thread
	glXMakeCurrent(wm->display, cwm->output_window, cwm->glx_context);

	// initialize GLEW
	// this will be needed for most modern OpenGL calls

	glewExperimental = GL_TRUE;
	if (glewInit() != GLEW_OK) wm_error(wm, "Failed to initialize GLEW");

	// enable adaptive vsync (-1 for adaptive vsync, 1 for non-adaptive vsync, 0 for no vsync)
	// this extension seems completely broken on NVIDIA

	// glXSwapIntervalEXT_t glXSwapIntervalEXT = (glXSwapIntervalEXT_t) glXGetProcAddress((const GLubyte*) "glXSwapIntervalEXT");
	// glXSwapIntervalEXT(wm->display, cwm->output_window, 0);

	cwm->vsync = 1;

	// blacklist the overlay and output windows for events

	wm->event_blacklisted_windows = (Window*) realloc(wm->event_blacklisted_windows, (2 + wm->event_blacklisted_window_count) * sizeof(Window));

	wm->event_blacklisted_windows[wm->event_blacklisted_window_count + 0] = cwm->overlay_window;
	wm->event_blacklisted_windows[wm->event_blacklisted_window_count + 1] = cwm->output_window;
	// wm->event_blacklisted_windows[wm->event_blacklisted_window_count + 2] = screen_owner;

	wm->event_blacklisted_window_count += 2;

	// setup the timing code (window managers don't seem to be able to vsync)

	gettimeofday(&cwm->previous_time, 0);
}

uint64_t cwm_swap(cwm_t* cwm) {
	glXSwapBuffers(cwm->wm->display, cwm->output_window);

	// return the time in microseconds between this frame and the last

	struct timeval current_time;
	gettimeofday(&current_time, 0);

	int64_t delta = (current_time.tv_sec - cwm->previous_time.tv_sec) * 1000000 + current_time.tv_usec - cwm->previous_time.tv_usec;

	cwm->previous_time = current_time;
	return delta;
}

static cwm_window_internal_t* cwm_get_window_internal(cwm_t* cwm, wm_window_t* window) {
	cwm_window_internal_t* window_internal = (cwm_window_internal_t*) window->internal;

	if (!window_internal) {
		window->internal = (void*) malloc(sizeof(cwm_window_internal_t));
		memset(window->internal, 0, sizeof(cwm_window_internal_t));
	}

	return (cwm_window_internal_t*) window->internal;
}

// event handler functions

void cwm_create_event(cwm_t* cwm, unsigned window_index) {
	wm_window_t* window = &cwm->wm->windows[window_index];
	cwm_window_internal_t* window_internal = cwm_get_window_internal(cwm, window);
}

void cwm_modify_event(cwm_t* cwm, unsigned window_index) {
	wm_window_t* window = &cwm->wm->windows[window_index];
	cwm_window_internal_t* window_internal = cwm_get_window_internal(cwm, window);

	// check to see if we already have a pixmap
	// delete it if so since we're likely gonna need to update it

	if (window_internal->pixmap) {
		glXDestroyPixmap(cwm->wm->display, window_internal->pixmap);
		window_internal->pixmap = 0;
	}
}

void cwm_destroy_event(cwm_t* cwm, unsigned window_index) {
	wm_window_t* window = &cwm->wm->windows[window_index];
	cwm_window_internal_t* window_internal = cwm_get_window_internal(cwm, window);

	free(window_internal);
}

// rendering functions

#define glXGetFBConfigAttribChecked(a, b, attr, c) \
	if (glXGetFBConfigAttrib((a), (b), (attr), (c))) { \
		fprintf(stderr, "WARNING Cannot get FBConfig attribute " #attr "\n"); \
	}

void cwm_bind_window_texture(cwm_t* cwm, unsigned window_index) {
	wm_window_t* window = &cwm->wm->windows[window_index];
	cwm_window_internal_t* window_internal = cwm_get_window_internal(cwm, window);

	if (!window->exists)  return;
	if (!window->visible) return;

	// TODO 'XGrabServer'/'XUngrabServer' necessary?
	// it seems to make things 10x faster for whatever reason
	// which is actually good for recording using OBS with XSHM

	if (!cwm->vsync) XGrabServer(cwm->wm->display);
	// glXWaitX(); // same as 'XSync', but a tad more efficient

	// update the window's pixmap

	if (!window_internal->pixmap) {
		XWindowAttributes attribs;
		XGetWindowAttributes(cwm->wm->display, window->window, &attribs);

		int format;
		GLXFBConfig config;

		for (int i = 0; i < cwm->glx_config_count; i++) {
			config = cwm->glx_configs[i];

			int has_alpha;
			glXGetFBConfigAttribChecked(cwm->wm->display, config, GLX_BIND_TO_TEXTURE_RGBA_EXT, &has_alpha);

			XVisualInfo* visual = glXGetVisualFromFBConfig(cwm->wm->display, config);
			int visual_depth = visual->depth;
			free(visual);

			if (attribs.depth != visual_depth) {
				continue;
			}

			// found the config we want, break

			format = has_alpha ? GLX_TEXTURE_FORMAT_RGBA_EXT : GLX_TEXTURE_FORMAT_RGB_EXT;
			break;
		}

		const int pixmap_attributes[] = {
			GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
			GLX_TEXTURE_FORMAT_EXT, format, 0 // GLX_TEXTURE_FORMAT_RGB_EXT
		};

		Pixmap x_pixmap = XCompositeNameWindowPixmap(cwm->wm->display, window->window);
		window_internal->pixmap = glXCreatePixmap(cwm->wm->display, config, x_pixmap, pixmap_attributes);

		XFreePixmap(cwm->wm->display, x_pixmap);
	}

	cwm->glXBindTexImageEXT(cwm->wm->display, window_internal->pixmap, GLX_FRONT_LEFT_EXT, NULL);
}

void cwm_unbind_window_texture(cwm_t* cwm, unsigned window_index) {
	wm_window_t* window = &cwm->wm->windows[window_index];
	cwm_window_internal_t* window_internal = cwm_get_window_internal(cwm, window);

	cwm->glXReleaseTexImageEXT(cwm->wm->display, window_internal->pixmap, GLX_FRONT_LEFT_EXT);
	if (!cwm->vsync) XUngrabServer(cwm->wm->display);
}
