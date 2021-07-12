
#define DEBUGGING 1
#include <cwm.h>

#include <opengl.h>

#include <math.h>
#include <unistd.h>
#include <sys/param.h>

// structures and types

typedef struct {
	unsigned internal_id;

	int exists;
	int visible;

	uint64_t farness;

	float opacity;
	float x, y;
	float width, height;

	float visual_opacity;
	float visual_x, visual_y;
	float visual_width, visual_height;

	float visual_shadow_opacity;
	float visual_shadow_radius;
	float visual_shadow_y_offset;

	int maximized;

	float unmaximized_x, unmaximized_y;
	float unmaximized_width, unmaximized_height;

	int always_on_top; // TODO doesn't always on top mean always focused to X?
	                   //      it appears not, but this still needs to be implemented
					   //      also maybe creating a proper linked list system for windows before implementing will make this easier

	// OpenGL stuff

	int index_count;
	GLuint vao, vbo, ibo;
} window_t;

typedef enum {
	ACTION_NONE = 0,
	ACTION_MOVE, ACTION_RESIZE
} action_t;

typedef struct {
	wm_t wm;
	cwm_t cwm;

	int x_resolution;
	int y_resolution;

	int running;

	window_t* windows;
	int window_count;

	// focused window and current action stuff

	unsigned focused_window_id;
	float focused_window_x, focused_window_y;

	action_t action;

	// monitor configuration info

	int monitor_count;

	float* monitor_xs, *monitor_ys;
	float* monitor_widths, *monitor_heights;

	// OpenGL stuff

	GLuint shader;
	GLuint texture_uniform;

	GLuint opacity_uniform;
	GLuint depth_uniform;

	GLuint position_uniform;
	GLuint size_uniform;

	// shadow stuff

	int shadow_index_count;
	GLuint shadow_vao, shadow_vbo, shadow_ibo;

	GLuint shadow_shader;

	GLuint shadow_strength_uniform;

	GLuint shadow_depth_uniform;
	GLuint shadow_position_uniform;
	GLuint shadow_size_uniform;

	GLuint shadow_spread_uniform;
} my_wm_t;

// useful functions

static unsigned window_internal_id_to_index(my_wm_t* wm, unsigned internal_id) {
	for (int i = 0; i < wm->window_count; i++) {
		window_t* window = &wm->windows[i];

		// it's really super important to verify our window actually exists
		// we could have a window that doesn't exist anymore, but that had the same ID as one that currently exists

		if (window->exists && window->internal_id == internal_id) {
			return i;
		}
	}

	// no window with that internal id was found
	return -1;
}

static void print_window_stack(my_wm_t* wm) { // debugging function
	printf("Window stack (%d windows):\n", wm->window_count);

	for (int i = 0; i < wm->window_count; i++) {
		window_t* window = &wm->windows[i];

		if (window->exists) printf("\t[%d]: internal_id = %d, visible = %d, farness = %lu\n", i, window->internal_id, window->visible, window->farness);
		else printf("\t[%d]: empty space\n", i);
	}

	printf("\n");
}

static void focus_window(my_wm_t* wm, unsigned window_id, int internally) {
	unsigned internal_id = wm->windows[window_id].internal_id;

	if (internally) {
		wm_focus_window(&wm->wm, internal_id);
	}

	for (int i = 0; i < wm->window_count; i++) {
		window_t* window = &wm->windows[i];

		/* if (window->always_on_top) window->farness = 0;
		else */ window->farness++;
	}

	wm->windows[window_id].farness = 0;

	// sort windows
	// this could be a much more efficient system with linked lists (as I believe X does internally), but this is fine for now

	for (int i = 0; i < wm->window_count; i++) {
		for (int j = i + 1; j < wm->window_count; j++) {
			if (wm->windows[i].farness < wm->windows[j].farness) {
				window_t temp;

				memcpy(&temp, &wm->windows[i], sizeof(window_t));
				memcpy(&wm->windows[i], &wm->windows[j], sizeof(window_t));
				memcpy(&wm->windows[j], &temp, sizeof(window_t));
			}
		}
	}

	wm->focused_window_id = window_internal_id_to_index(wm, internal_id);
}

static void unfocus_window(my_wm_t* wm) {
	// loop backwards through all the windows and check if they're a valid candidate for refocusing
	// we start at 'wm->window_count - 2', because we want the index and two ignore the last window on the stack (hopefully our focused window)

	for (int i = wm->focused_window_id - 1; i >= 0; i--) {
		if (i != wm->focused_window_id) { // just to make sure, but this shouldn't happen
			window_t* window = &wm->windows[i];

			if (window->exists && window->visible) {
				focus_window(wm, i, 1);
				break;
			}
		}
	}
}

static void maximize_window(my_wm_t* wm, unsigned window_id, int single_monitor) {
	window_t* window = &wm->windows[window_id];

	if (window->maximized) {
		window->maximized = 0;
		wm_move_window(&wm->wm, window->internal_id, window->unmaximized_x, window->unmaximized_y, window->unmaximized_width, window->unmaximized_height);

		return;
	}

	window->unmaximized_x      = window->x;
	window->unmaximized_y      = window->y;

	window->unmaximized_width  = window->width;
	window->unmaximized_height = window->height;

	window->maximized = 1;

	if (single_monitor) {
		// find closest monitor to the centre of the window

		for (int i = 0; i < wm->monitor_count; i++) {
			float monitor_x      = wm->monitor_xs     [i];
			float monitor_y      = wm->monitor_ys     [i];

			float monitor_width  = wm->monitor_widths [i];
			float monitor_height = wm->monitor_heights[i];

			if (window->x >= monitor_x - monitor_width  / 2 && window->x <= monitor_x + monitor_width  / 2 &&
				window->y >= monitor_y - monitor_height / 2 && window->y <= monitor_y + monitor_height / 2) {

				wm_move_window(&wm->wm, window->internal_id, monitor_x, monitor_y, monitor_width, monitor_height);
				return;
			}
		}

		// if we can't find a valid monitor, no worries, we'll just fill the whole screen
	}

	wm_move_window(&wm->wm, window->internal_id, 0.0, 0.0, 2.0, 2.0);
}

// event callback functions

static char* first_argument;

void keyboard_event(my_wm_t* wm, unsigned internal_id, unsigned press, unsigned modifiers, unsigned key) {
	int alt   = modifiers & 0x8;
	int super = modifiers & 0x40;
	
	if (press && super &&         key == 67) wm->running = 0; // Super+F1
	if (press && super &&         key == 24) wm_close_window(&wm->wm, wm->windows[wm->focused_window_id].internal_id); // Super+Q (quit)
	if (press && super &&  alt && key == 41) maximize_window(wm, wm->focused_window_id, 0); // Super+Alt+F (fullfullscreen)
	if (press && super && !alt && key == 41) maximize_window(wm, wm->focused_window_id, 1); // Super+F (fullscreen)
	if (press && super &&         key == 55) wm->cwm.vsync = !wm->cwm.vsync; // Super+V (vsync)

	if (press && super &&  key == 27) { // Super+R (restart)
		execl(first_argument, first_argument, NULL);
		exit(1);
	}

	if (press && super &&  key == 28) { // Super+T (terminal)
		if (!fork()) {
			execl("/usr/local/bin/xterm", "/usr/local/bin/xterm", NULL);
			exit(1);
		}
	}

	if (press && super && !alt && key == 107) { // Super+PrtSc (screenshot of selection to clipboard)
		system("scrot -sf '/tmp/screenshot-selection-aquabsd-%F-%T.png' -e 'xclip -selection clipboard -target image/png -i $f && rm $f' &");
	}

	if (press && super && alt && key == 107) { // Super+Alt+PrtSc (screenshot of window to clipboard)
		system("scrot -u '/tmp/screenshot-selection-aquabsd-$wx$h-%F-%T.png' -e 'xclip -selection clipboard -target image/png -i $f && rm $f' &");
	}
}

int click_event(my_wm_t* wm, unsigned internal_id, unsigned press, unsigned modifiers, unsigned button, float x, float y) {
	int window_index;

	if (press) {
		if (internal_id == -1) return 0;
		window_index = window_internal_id_to_index(wm, internal_id);

		focus_window(wm, window_index, 1);

		wm->focused_window_x = wm->windows[wm->focused_window_id].x - x;
		wm->focused_window_y = wm->windows[wm->focused_window_id].y - y;

	} else if (wm->action) { // releasing and were already doing something
		window_t* window = &wm->windows[wm->focused_window_id];

		window->opacity = 1.0;
		wm_move_window(&wm->wm, window->internal_id, window->x, window->y, window->width, window->height);

		wm->action = ACTION_NONE;
		return 0;
	}

	if (modifiers & Mod4Mask && press) {
		if      (button == 1) wm->action = ACTION_MOVE;
		else if (button == 3) wm->action = ACTION_RESIZE;

		wm->windows[wm->focused_window_id].opacity = 0.9;

		return 0;
	}

	return 1;
}

void move_event(my_wm_t* wm, unsigned internal_id, unsigned modifiers, float x, float y) {
	if (wm->action && !wm->windows[wm->focused_window_id].maximized) {
		window_t* window = &wm->windows[wm->focused_window_id];

		if (wm->action == ACTION_MOVE) {
			window->x = x + wm->focused_window_x;
			window->y = y + wm->focused_window_y;
		}

		else if (wm->action == ACTION_RESIZE) {
			window->width  = 2 * fabs(x - window->x);
			window->height = 2 * fabs(y - window->y);

			// this is not ideal for performance in certain applications, but it looks hella cool for apps that work correctly nonetheless
			// basically the problem is that you're calling a 'ConfigureNotify' event each time,
			// which means the compositor needs to create a new pixmap for each frame
			// not good...

			wm_move_window(&wm->wm, window->internal_id, window->x, window->y, window->width, window->height);
		}
	}
}

void create_event(my_wm_t* wm, unsigned internal_id) {
	cwm_create_event(&wm->cwm, internal_id);

	int window_index = 0;

	for (; window_index < wm->window_count; window_index++) {
		if (!wm->windows[window_index].exists) {
			goto got_space;
		}
	}

	wm->windows = (window_t*) realloc(wm->windows, ++wm->window_count * sizeof(*wm->windows));
	window_index = wm->window_count - 1;

got_space: {}

	window_t* window = &wm->windows[window_index];
	memset(window, 0, sizeof(*window));

	window->internal_id = internal_id;
	window->exists = 1;
	window->opacity = 1.0;

	gl_create_vao_vbo_ibo(&window->vao, &window->vbo, &window->ibo);
}

void modify_event(my_wm_t* wm, unsigned internal_id, int visible, float x, float y, float width, float height) {
	cwm_modify_event(&wm->cwm, internal_id);

	int window_index = window_internal_id_to_index(wm, internal_id);
	window_t* window = &wm->windows[window_index];

	int was_visible = window->visible;
	window->visible = visible;

	window->x = x;
	window->y = y;

	window->width  = width;
	window->height = height;

	// regenerate vertex attributes and indices

	#define TAU 6.283185

	#define CORNER_RESOLUTION 8
	#define CORNER_RADIUS 3 // pixels

	float x_radius = 4 * (float) CORNER_RADIUS / wm->x_resolution / width;
	float y_radius = 4 * (float) CORNER_RADIUS / wm->y_resolution / height;

	// loop through all the vertex pairs

	GLfloat vertex_positions[CORNER_RESOLUTION * 2 * 4 + 4];
	GLubyte indices[CORNER_RESOLUTION * 2 * 6 + 3];

	int prev_index_pair[2] = { -1 };

	for (int i = 0; i < CORNER_RESOLUTION * 2 + 1; i++) {
		// calculate indices

		int index_pair[2] = { i * 2, i * 2 + 1 };

		if (prev_index_pair[0] >= 0) {
			indices[i * 6 + 0] = prev_index_pair[0];
			indices[i * 6 + 1] = prev_index_pair[1];
			indices[i * 6 + 2] =      index_pair[1];
			indices[i * 6 + 3] = prev_index_pair[0];
			indices[i * 6 + 4] =      index_pair[1];
			indices[i * 6 + 5] =      index_pair[0];
		}

		memcpy(prev_index_pair, index_pair, sizeof(prev_index_pair));

		// calculate vertices

		float theta = (float) (i - (i > CORNER_RESOLUTION / 2)) / CORNER_RESOLUTION * TAU / 2;

		float corner_x = cos(theta) * x_radius;
		float corner_y = sin(theta) * y_radius;

		float x = (i <= CORNER_RESOLUTION / 2 ? 0.5 - x_radius : -0.5 + x_radius) + corner_x;
		float y = 0.5 - y_radius + corner_y;

		vertex_positions[i * 4 + 0] =  x;
		vertex_positions[i * 4 + 1] =  y;

		vertex_positions[i * 4 + 2] =  x;
		vertex_positions[i * 4 + 3] = -y;
	}

	window->index_count = sizeof(indices) / sizeof(*indices);
	gl_set_vao_vbo_ibo_data(window->vao, window->vbo, sizeof(vertex_positions), vertex_positions, window->ibo, sizeof(indices), indices);

	if (window->visible && !was_visible) {
		window->opacity = 1.0;
		window->visual_opacity = 0.0;

		wm_move_window(&wm->wm, window->internal_id, window->x, window->y, window->width, window->height);

		window->visual_x = window->x;
		window->visual_y = window->y;

		window->visual_width  = window->width  * 0.9;
		window->visual_height = window->height * 0.9;

	 	focus_window(wm, window_index, 0);
	}

	else if (window_index == wm->focused_window_id && !window->visible && was_visible) {
		unfocus_window(wm);
	}
}

void destroy_event(my_wm_t* wm, unsigned internal_id) {
	cwm_destroy_event(&wm->cwm, internal_id);

	unsigned window_index = window_internal_id_to_index(wm, internal_id);
	window_t* window = &wm->windows[window_index];

	window->exists = 0;
}

// main functions

static void render_window(my_wm_t* wm, unsigned window_id, float delta) {
	window_t* window = &wm->windows[window_id];

	if (!window->exists ) return;
	if (!window->visible) return;

	// calculate visual window coordinates and size (animation)

	delta = MIN(0.1, delta); // just make sure this doesn't get too crazy
	                         // TODO see if this actually does anything

	window->visual_opacity += (window->opacity - window->visual_opacity) * delta * 10;

	window->visual_x += (window->x - window->visual_x) * delta * 20;
	window->visual_y += (window->y - window->visual_y) * delta * 20;

	// TODO for some reason, when disabling vsync, windows take a real long time before starting their "appearing" animation
	//      maybe this is because of this?

	window->visual_width  += (window->width  - window->visual_width ) * delta * 30;
	window->visual_height += (window->height - window->visual_height) * delta * 30;

	float x = window->visual_x;
	float y = window->visual_y;

	float width  = window->visual_width;
	float height = window->visual_height;

	// check if window coordinates and size are pixel aligned
	// rounding here instead of simply flooring to preserve proper subpixel rendering when animating

	int width_pixels  = (int) round(width  / 2 * wm->x_resolution);
	int height_pixels = (int) round(height / 2 * wm->y_resolution);

	if (width_pixels  % 2) x += 0.5 / wm->x_resolution * 2; // if width odd, add half a pixel to x
	if (height_pixels % 2) y += 0.5 / wm->y_resolution * 2; // if height odd, subtract half a pixel to y

	// calculate window depth

	float depth = 1.0 - (float) window_id / wm->window_count;

	// draw the window contents

	glUseProgram(wm->shader);
	glUniform1i(wm->texture_uniform, 0);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glActiveTexture(GL_TEXTURE0);
	cwm_bind_window_texture(&wm->cwm, window->internal_id);

	// actually draw the window

	glUniform1f(wm->opacity_uniform, window->visual_opacity);
	glUniform1f(wm->depth_uniform, depth);

	glUniform2f(wm->position_uniform, x, y);
	glUniform2f(wm->size_uniform, width, height);

	glBindVertexArray(window->vao);
	glDrawElements(GL_TRIANGLES, window->index_count, GL_UNSIGNED_BYTE, NULL);

	cwm_unbind_window_texture(&wm->cwm, window->internal_id);

	// draw the shadow
	// we do this after drawing the window contents so we can take advantage of alpha sorting

	glUseProgram(wm->shadow_shader);

	float shadow_opacity = 0.15 + 0.1 * (window_id == wm->focused_window_id);

	// TODO do I really want to disable shadows on maximized windows?

	// if (window->maximized) { // we want to phase out the shadow much slower when maximizing our window
	// 	window->visual_shadow_opacity -= window->visual_shadow_opacity * delta * 10;
	// }

	// else {
		window->visual_shadow_opacity += (shadow_opacity - window->visual_shadow_opacity) * delta * 30;
	// }

	glUniform1f(wm->shadow_strength_uniform, window->visual_opacity * window->visual_shadow_opacity);

	float shadow_radius = (float) (64 + (64 * (window_id == wm->focused_window_id))); // pixels
	window->visual_shadow_radius += (shadow_radius - window->visual_shadow_radius) * delta * 20;

	float spread_x = 4 * window->visual_shadow_radius / wm->x_resolution;
	float spread_y = 4 * window->visual_shadow_radius / wm->y_resolution;

	glUniform2f(wm->shadow_spread_uniform, spread_x, spread_y);

	float y_offset = -spread_y / 32 - spread_y / 16 * (window_id == wm->focused_window_id);
	window->visual_shadow_y_offset += (y_offset - window->visual_shadow_y_offset) * delta * 10;

	glUniform1f(wm->shadow_depth_uniform, depth);
	glUniform2f(wm->shadow_position_uniform, x, y /* + window->visual_shadow_y_offset / 2 */);
	glUniform2f(wm->shadow_size_uniform, width, height);

	glBindVertexArray(wm->shadow_vao);
	glDrawElements(GL_TRIANGLES, wm->shadow_index_count, GL_UNSIGNED_BYTE, NULL);
}

int main(int argc, char* argv[]) {
	first_argument = argv[0];

	my_wm_t _wm;
	my_wm_t* wm = &_wm;
	memset(wm, 0, sizeof(*wm));

	// create a compositing window manager

	new_wm(&wm->wm);
	new_cwm(&wm->cwm, &wm->wm);

	wm->x_resolution = wm_x_resolution(&wm->wm);
	wm->y_resolution = wm_y_resolution(&wm->wm);

	// get info about the monitor configuration

	wm->monitor_count = wm_monitor_count(&wm->wm);

	wm->monitor_xs      = (float*) malloc(wm->monitor_count * sizeof(float));
	wm->monitor_ys      = (float*) malloc(wm->monitor_count * sizeof(float));

	wm->monitor_widths  = (float*) malloc(wm->monitor_count * sizeof(float));
	wm->monitor_heights = (float*) malloc(wm->monitor_count * sizeof(float));

	for (int i = 0; i < wm->monitor_count; i++) {
		wm->monitor_xs     [i] = wm_monitor_x     (&wm->wm, i);
		wm->monitor_ys     [i] = wm_monitor_y     (&wm->wm, i);

		wm->monitor_widths [i] = wm_monitor_width (&wm->wm, i);
		wm->monitor_heights[i] = wm_monitor_height(&wm->wm, i);
	}

	// register all the event callbacks
	// ideally, there would be proper functions to do this

	wm->wm.keyboard_event_callback = (wm_keyboard_event_callback_t) keyboard_event;
	wm->wm.click_event_callback    = (wm_click_event_callback_t)    click_event;
	wm->wm.move_event_callback     = (wm_move_event_callback_t)     move_event;

	wm->wm.create_event_callback   = (wm_create_event_callback_t)   create_event;
	wm->wm.modify_event_callback   = (wm_modify_event_callback_t)   modify_event;
	wm->wm.destroy_event_callback  = (wm_destroy_event_callback_t)  destroy_event;

	// run any startup programs here
	
	// system("code-oss");

	// OpenGL stuff

	const char* vertex_shader_source = "#version 330\n"
		"layout(location = 0) in vec2 vertex_position;"
		"out vec2 local_position;"

		"uniform float depth;"
		"uniform vec2 position;"
		"uniform vec2 size;"

		"void main(void) {"
		"	local_position = vertex_position;"
		"	gl_Position = vec4(vertex_position * size + position, depth, 1.0);"
		"}";

	const char* fragment_shader_source = "#version 330\n"
		"in vec2 local_position;"
		"out vec4 fragment_colour;"

		"uniform float opacity;"
		"uniform sampler2D texture_sampler;"

		"void main(void) {"
		"   vec4 colour = texture(texture_sampler, local_position * vec2(1.0, -1.0) + vec2(0.5));"
		"	float alpha = opacity /* * (1.0 - colour.a) */;"

		"	fragment_colour = vec4(colour.rgb, alpha);"
		"}";

	wm->shader = gl_create_shader_program(vertex_shader_source, fragment_shader_source);
	wm->texture_uniform = glGetUniformLocation(wm->shader, "texture_sampler");

	wm->opacity_uniform = glGetUniformLocation(wm->shader, "opacity");
	wm->depth_uniform = glGetUniformLocation(wm->shader, "depth");

	wm->position_uniform = glGetUniformLocation(wm->shader, "position");
	wm->size_uniform = glGetUniformLocation(wm->shader, "size");

	// shadow stuff

	const GLubyte shadow_indices[] = { 0, 1, 2, 0, 2, 3 };

	const GLfloat shadow_vertex_positions[] = {
		-0.5,  0.5,
		-0.5, -0.5,
		 0.5, -0.5,
		 0.5,  0.5,
	};

	gl_create_vao_vbo_ibo(&wm->shadow_vao, &wm->shadow_vbo, &wm->shadow_ibo);

	wm->shadow_index_count = sizeof(shadow_indices) / sizeof(*shadow_indices);
	gl_set_vao_vbo_ibo_data(wm->shadow_vao, wm->shadow_vbo, sizeof(shadow_vertex_positions), shadow_vertex_positions, wm->shadow_ibo, sizeof(shadow_indices), shadow_indices);

	const char* shadow_vertex_shader_source = "#version 330\n"
		"layout(location = 0) in vec2 vertex_position;"

		"out vec2 map_position;"

		"uniform float depth;"
		"uniform vec2 position;"
		"uniform vec2 size;"
		"uniform vec2 spread;"

		"void main(void) {"
		"	map_position = vertex_position * (size + spread);"
		"	gl_Position = vec4(map_position + position, depth, 1.0);"
		"}";

	const char* shadow_fragment_shader_source = "#version 330\n"
		"in vec2 map_position;"
		"out vec4 fragment_colour;"

		"uniform float strength;"

		"uniform vec2 size;"
		"uniform vec2 spread;"

		"void main(void) {"

		// find distance from point on shadow plane to window bounds

		"	float dx = (2 * abs(map_position.x) - size.x + spread.x / 8) / spread.x;"
		"	float dy = (2 * abs(map_position.y) - size.y + spread.y / 8) / spread.y;"

		"	if (map_position.y > 0) dy *= 1.5;"
		"	if (map_position.y < 0) dy /= 1.2;"

		"	dx = clamp(dx, 0, 1);"
		"	dy = clamp(dy, 0, 1);"

		// calculate the shadow colour

		"	float value = 1.0 - clamp(length(vec2(dx, dy)), 0, 1);"
		"	fragment_colour = vec4(0.0, 0.0, 0.0, value * value) * strength;"
		"}";

	wm->shadow_shader = gl_create_shader_program(shadow_vertex_shader_source, shadow_fragment_shader_source);

	wm->shadow_strength_uniform = glGetUniformLocation(wm->shadow_shader, "strength");

	wm->shadow_depth_uniform = glGetUniformLocation(wm->shadow_shader, "depth");
	wm->shadow_position_uniform = glGetUniformLocation(wm->shadow_shader, "position");
	wm->shadow_size_uniform = glGetUniformLocation(wm->shadow_shader, "size");

	wm->shadow_spread_uniform = glGetUniformLocation(wm->shadow_shader, "spread");

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// main loop

	float average_delta = 0.0;

	wm->running = 1;
	while (wm->running) {
		while (wm_process_events(&wm->wm, wm));

		// glClearColor(0.4, 0.2, 0.4, 1.0);
		// gruvbox background colour (#292828)
		glClearColor(0.16015625, 0.15625, 0.15625, 1.);
		
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// render our windows

		for (int i = 0; i < wm->window_count; i++) {
			render_window(wm, i, average_delta);
		}

		float delta = (float) cwm_swap(&wm->cwm) / 1000000;

		average_delta += delta;
		average_delta /= 2;

		// printf("average fps %f\n", 1 / average_delta);
	}
}
