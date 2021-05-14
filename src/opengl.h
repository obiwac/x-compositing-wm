// this file contains OpenGL helpers for the window manager

// shaders

static void gl_compile_shader_and_check_for_errors /* lmao */ (GLuint shader, const char* source) {
	glShaderSource(shader, 1, &source, 0);
	glCompileShader(shader);

	GLint log_length;
	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);

	char* log_buffer = (char*) malloc(log_length); // 'log_length' includes null character
	glGetShaderInfoLog(shader, log_length, NULL, log_buffer);

	if (log_length) {
		fprintf(stderr, "[SHADER_ERROR] %s\n", log_buffer);
		exit(1); // no real need to free 'log_buffer' here
	}

	free(log_buffer);
}

GLuint gl_create_shader_program(const char* vertex_source, const char* fragment_source) {
	GLuint program = glCreateProgram();

	GLuint vertex_shader   = glCreateShader(GL_VERTEX_SHADER);
	GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

	gl_compile_shader_and_check_for_errors(vertex_shader,   vertex_source);
	gl_compile_shader_and_check_for_errors(fragment_shader, fragment_source);

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);

	glLinkProgram(program);

	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);

	return program;
}

// VAO / VBO / IBO

void gl_create_vao_vbo_ibo(GLuint* vao, GLuint* vbo, GLuint* ibo) {
	glGenVertexArrays(1, vao);
	glBindVertexArray(*vao);

	glGenBuffers(1, vbo);
	glBindBuffer(GL_ARRAY_BUFFER, *vbo);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	glGenBuffers(1, ibo);
}

void gl_set_vao_vbo_ibo_data(GLuint vao, GLuint vbo, GLsizeiptr vbo_size, const void* vbo_data, GLuint ibo, GLsizeiptr ibo_size, const void* ibo_data) {
	glBindVertexArray(vao);
	
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, vbo_size, vbo_data, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibo_size, ibo_data, GL_STATIC_DRAW);
}