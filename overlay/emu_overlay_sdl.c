/*
 * emu_overlay_sdl.c — SDL2 + SDL_ttf render backend for the emulator overlay.
 *
 * This is a GL<->SDL bridge: menu elements are drawn onto an SDL_Surface using
 * SDL_ttf for text, then the composited surface is uploaded as a GL texture and
 * drawn as a fullscreen quad inside the emulator's GL context.
 *
 * CRITICAL: All GL state must be saved before overlay rendering and restored
 * after, or GLideN64's cached state tracking breaks (black screen on PowerVR).
 */

#include "emu_overlay_sdl.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <GLES3/gl3.h>
#include <png.h>

#include <math.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// GLES3 VAO function pointers — loaded via SDL_GL_GetProcAddress to avoid
// symbol conflicts with emulators that define these as stub function pointers
// (e.g. PPSSPP's gl3stub.c shadows libGLESv2 symbols).
// ---------------------------------------------------------------------------
typedef void(GL_APIENTRY* PFNGLBINDVERTEXARRAYPROC)(GLuint array);
typedef void(GL_APIENTRY* PFNGLDELETEVERTEXARRAYSPROC)(GLsizei n, const GLuint* arrays);
typedef void(GL_APIENTRY* PFNGLGENVERTEXARRAYSPROC)(GLsizei n, GLuint* arrays);

static PFNGLBINDVERTEXARRAYPROC pfn_glBindVertexArray = NULL;
static PFNGLDELETEVERTEXARRAYSPROC pfn_glDeleteVertexArrays = NULL;
static PFNGLGENVERTEXARRAYSPROC pfn_glGenVertexArrays = NULL;

static void ovl_load_gl3_procs(void) {
	if (pfn_glBindVertexArray)
		return; // already loaded
	pfn_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
	pfn_glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glDeleteVertexArrays");
	pfn_glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
	fprintf(stderr, "[OverlaySDL] GL3 procs: BindVAO=%p DeleteVAO=%p GenVAO=%p\n",
			(void*)pfn_glBindVertexArray, (void*)pfn_glDeleteVertexArrays, (void*)pfn_glGenVertexArrays);
}

// ---------------------------------------------------------------------------
// Scale factor & font sizes (matching NextUI's defines.h)
// ---------------------------------------------------------------------------

static int s_scale = 2;

// Match NextUI's common/defines.h: FONT_LARGE=16, FONT_MEDIUM=14, FONT_SMALL=12, FONT_TINY=10
#define FONT_SIZE_LARGE (16 * s_scale)
#define FONT_SIZE_MEDIUM (14 * s_scale)
#define FONT_SIZE_SMALL (12 * s_scale)
#define FONT_SIZE_TINY (10 * s_scale)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static int s_screenW = 0;
static int s_screenH = 0;
static int s_overlayRotation = 0;
static int s_overlayViewportW = 0;
static int s_overlayViewportH = 0;

static TTF_Font* s_fonts[EMU_OVL_FONT_COUNT] = {NULL, NULL, NULL, NULL}; // LARGE, MEDIUM, SMALL, TINY

static SDL_Surface* s_renderSurface = NULL;	 // ARGB8888 compositing surface
static SDL_Surface* s_captureSurface = NULL; // Captured game frame (ARGB8888)

static GLuint s_overlayTexture = 0; // GL texture for uploading composited surface
static GLuint s_texProgram = 0;
static GLint s_texLocTexture = -1;
static GLuint s_texVAO = 0;
static GLuint s_texVBO = 0;

// Pixel conversion buffer (ARGB -> RGBA for GL upload)
static uint8_t* s_uploadBuffer = NULL;

// Icon storage (PNG images for button hints + slot screenshots)
#define MAX_ICONS 16
static SDL_Surface* s_icons[MAX_ICONS];
static int s_iconCount = 0;

// Saved GL state for begin_frame / end_frame
static GLint s_savedViewport[4];
static GLint s_savedScissorBox[4];
static GLboolean s_savedBlend;
static GLboolean s_savedDepthTest;
static GLboolean s_savedCullFace;
static GLboolean s_savedScissorTest;
static GLint s_savedBlendSrcRGB;
static GLint s_savedBlendDstRGB;
static GLint s_savedBlendSrcAlpha;
static GLint s_savedBlendDstAlpha;
static GLint s_savedProgram;
static GLint s_savedVAO;
static GLint s_savedVBO;
static GLint s_savedTex0;
static GLint s_savedActiveTexUnit;
static GLint s_savedUnpackAlignment;

// Optional final presentation rotation for devices whose DRM scanout is
// portrait while the handheld is used in landscape.
static int s_presentRotation = 0;
static int s_presentEnabled = 0;
static int s_presentLogicalW = 0;
static int s_presentLogicalH = 0;
static int s_presentPhysicalW = 0;
static int s_presentPhysicalH = 0;
static GLuint s_presentFBO = 0;
static GLuint s_presentColorTex = 0;
static GLuint s_presentDepthRB = 0;
static GLuint s_presentProgram = 0;
static GLint s_presentLocTexture = -1;
static GLuint s_presentVAO = 0;
static GLuint s_presentVBO = 0;

// ---------------------------------------------------------------------------
// Shader sources
// ---------------------------------------------------------------------------

static const char* s_texVS =
	"#version 300 es\n"
	"in vec2 aPos;\n"
	"in vec2 aTexCoord;\n"
	"out vec2 vTexCoord;\n"
	"void main() {\n"
	"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"    vTexCoord = aTexCoord;\n"
	"}\n";

static const char* s_texFS =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vTexCoord;\n"
	"uniform sampler2D uTexture;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    fragColor = texture(uTexture, vTexCoord);\n"
	"}\n";

// ---------------------------------------------------------------------------
// GL helpers
// ---------------------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src) {
	GLuint sh = glCreateShader(type);
	if (sh == 0) {
		fprintf(stderr, "[OverlaySDL] glCreateShader failed, glError=%d\n", glGetError());
		return 0;
	}
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);

	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		log[0] = '\0';
		GLsizei logLen = 0;
		glGetShaderInfoLog(sh, (GLsizei)(sizeof(log) - 1), &logLen, log);
		log[logLen] = '\0';
		fprintf(stderr, "[OverlaySDL] shader compile error (%s): %s\n",
				type == GL_VERTEX_SHADER ? "VS" : "FS", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint link_program(GLuint vs, GLuint fs) {
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
		fprintf(stderr, "[OverlaySDL] program link error: %s\n", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static int normalize_rotation(int degrees) {
	degrees %= 360;
	if (degrees < 0)
		degrees += 360;
	switch (degrees) {
	case 90:
	case 180:
	case 270:
		return degrees;
	default:
		return 0;
	}
}

static int read_rotation_env(const char* name, int fallback) {
	const char* value = getenv(name);
	if (!value || value[0] == '\0')
		return fallback;
	char* end = NULL;
	long parsed = strtol(value, &end, 10);
	if (end == value)
		return fallback;
	return normalize_rotation((int)parsed);
}

static void present_destroy(void) {
	if (s_presentVAO && pfn_glDeleteVertexArrays) {
		pfn_glDeleteVertexArrays(1, &s_presentVAO);
		s_presentVAO = 0;
	}
	if (s_presentVBO) {
		glDeleteBuffers(1, &s_presentVBO);
		s_presentVBO = 0;
	}
	if (s_presentProgram) {
		glDeleteProgram(s_presentProgram);
		s_presentProgram = 0;
	}
	if (s_presentColorTex) {
		glDeleteTextures(1, &s_presentColorTex);
		s_presentColorTex = 0;
	}
	if (s_presentDepthRB) {
		glDeleteRenderbuffers(1, &s_presentDepthRB);
		s_presentDepthRB = 0;
	}
	if (s_presentFBO) {
		glDeleteFramebuffers(1, &s_presentFBO);
		s_presentFBO = 0;
	}
	s_presentEnabled = 0;
	s_presentRotation = 0;
	s_presentLogicalW = 0;
	s_presentLogicalH = 0;
	s_presentPhysicalW = 0;
	s_presentPhysicalH = 0;
	s_presentLocTexture = -1;
}

static int present_build_program(void) {
	if (s_presentProgram)
		return 0;

	GLuint vs = compile_shader(GL_VERTEX_SHADER, s_texVS);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, s_texFS);
	if (!vs || !fs) {
		if (vs)
			glDeleteShader(vs);
		if (fs)
			glDeleteShader(fs);
		return -1;
	}
	s_presentProgram = link_program(vs, fs);
	glDeleteShader(vs);
	glDeleteShader(fs);
	if (!s_presentProgram)
		return -1;
	s_presentLocTexture = glGetUniformLocation(s_presentProgram, "uTexture");
	return 0;
}

static void present_vertices(float* v, int rotation) {
	float bl_u = 0.0f, bl_v = 0.0f;
	float br_u = 1.0f, br_v = 0.0f;
	float tl_u = 0.0f, tl_v = 1.0f;
	float tr_u = 1.0f, tr_v = 1.0f;

	switch (rotation) {
	case 90:
		bl_u = 1.0f; bl_v = 0.0f;
		br_u = 1.0f; br_v = 1.0f;
		tl_u = 0.0f; tl_v = 0.0f;
		tr_u = 0.0f; tr_v = 1.0f;
		break;
	case 180:
		bl_u = 1.0f; bl_v = 1.0f;
		br_u = 0.0f; br_v = 1.0f;
		tl_u = 1.0f; tl_v = 0.0f;
		tr_u = 0.0f; tr_v = 0.0f;
		break;
	case 270:
		bl_u = 0.0f; bl_v = 1.0f;
		br_u = 0.0f; br_v = 0.0f;
		tl_u = 1.0f; tl_v = 1.0f;
		tr_u = 1.0f; tr_v = 0.0f;
		break;
	default:
		break;
	}

	float verts[24] = {
		-1.0f, -1.0f, bl_u, bl_v,
		 1.0f, -1.0f, br_u, br_v,
		-1.0f,  1.0f, tl_u, tl_v,
		 1.0f, -1.0f, br_u, br_v,
		 1.0f,  1.0f, tr_u, tr_v,
		-1.0f,  1.0f, tl_u, tl_v,
	};
	memcpy(v, verts, sizeof(verts));
}

static void overlay_vertices(float* v, int rotation) {
	// Overlay surfaces are SDL top-down, so the default mapping already flips V.
	float bl_u = 0.0f, bl_v = 1.0f;
	float br_u = 1.0f, br_v = 1.0f;
	float tl_u = 0.0f, tl_v = 0.0f;
	float tr_u = 1.0f, tr_v = 0.0f;

	switch (rotation) {
	case 90:
		bl_u = 1.0f; bl_v = 1.0f;
		br_u = 1.0f; br_v = 0.0f;
		tl_u = 0.0f; tl_v = 1.0f;
		tr_u = 0.0f; tr_v = 0.0f;
		break;
	case 180:
		bl_u = 1.0f; bl_v = 0.0f;
		br_u = 0.0f; br_v = 0.0f;
		tl_u = 1.0f; tl_v = 1.0f;
		tr_u = 0.0f; tr_v = 1.0f;
		break;
	case 270:
		bl_u = 0.0f; bl_v = 0.0f;
		br_u = 0.0f; br_v = 1.0f;
		tl_u = 1.0f; tl_v = 0.0f;
		tr_u = 1.0f; tr_v = 1.0f;
		break;
	default:
		break;
	}

	float verts[24] = {
		-1.0f, -1.0f, bl_u, bl_v,
		 1.0f, -1.0f, br_u, br_v,
		-1.0f,  1.0f, tl_u, tl_v,
		 1.0f, -1.0f, br_u, br_v,
		 1.0f,  1.0f, tr_u, tr_v,
		-1.0f,  1.0f, tl_u, tl_v,
	};
	memcpy(v, verts, sizeof(verts));
}

void overlay_sdl_present_init(int logical_w, int logical_h) {
	ovl_load_gl3_procs();

	int rotation = read_rotation_env("MUPEN64PLUS_PRESENT_ROTATION",
									 read_rotation_env("DISPLAY_ROTATION", 0));
	if (rotation == 0 || logical_w <= 0 || logical_h <= 0) {
		present_destroy();
		return;
	}

	if (s_presentEnabled &&
		s_presentRotation == rotation &&
		s_presentLogicalW == logical_w &&
		s_presentLogicalH == logical_h)
		return;

	present_destroy();

	s_presentRotation = rotation;
	s_presentLogicalW = logical_w;
	s_presentLogicalH = logical_h;
	if (rotation == 90 || rotation == 270) {
		s_presentPhysicalW = logical_h;
		s_presentPhysicalH = logical_w;
	} else {
		s_presentPhysicalW = logical_w;
		s_presentPhysicalH = logical_h;
	}

	GLint savedFBO = 0, savedVAO = 0, savedVBO = 0, savedTex = 0;
	GLint savedUnpackAlign = 4;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFBO);
	if (pfn_glBindVertexArray)
		pfn_glBindVertexArray(0);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedVBO);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);

	if (present_build_program() != 0) {
		fprintf(stderr, "[OverlaySDL] presentation shader init failed; rotation disabled\n");
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFBO);
		glBindBuffer(GL_ARRAY_BUFFER, (GLuint)savedVBO);
		glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
		return;
	}

	glGenTextures(1, &s_presentColorTex);
	glBindTexture(GL_TEXTURE_2D, s_presentColorTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, logical_w, logical_h, 0,
				 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glGenRenderbuffers(1, &s_presentDepthRB);
	glBindRenderbuffer(GL_RENDERBUFFER, s_presentDepthRB);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, logical_w, logical_h);

	glGenFramebuffers(1, &s_presentFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, s_presentFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
						   GL_TEXTURE_2D, s_presentColorTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
							  GL_RENDERBUFFER, s_presentDepthRB);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "[OverlaySDL] presentation FBO incomplete: 0x%x; rotation disabled\n",
				(unsigned int)status);
		present_destroy();
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFBO);
		glBindBuffer(GL_ARRAY_BUFFER, (GLuint)savedVBO);
		glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex);
		glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
		return;
	}

	pfn_glGenVertexArrays(1, &s_presentVAO);
	glGenBuffers(1, &s_presentVBO);
	pfn_glBindVertexArray(s_presentVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_presentVBO);
	glBufferData(GL_ARRAY_BUFFER, 6 * 4 * (GLsizeiptr)sizeof(float), NULL, GL_DYNAMIC_DRAW);
	GLint posLoc = glGetAttribLocation(s_presentProgram, "aPos");
	GLint uvLoc = glGetAttribLocation(s_presentProgram, "aTexCoord");
	glEnableVertexAttribArray(posLoc);
	glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(float), (void*)0);
	glEnableVertexAttribArray(uvLoc);
	glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(float),
						  (void*)(2 * sizeof(float)));

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFBO);
	pfn_glBindVertexArray((GLuint)savedVAO);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)savedVBO);
	glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);

	s_presentEnabled = 1;
	fprintf(stderr, "[OverlaySDL] presentation rotation=%d logical=%dx%d physical=%dx%d\n",
			s_presentRotation, s_presentLogicalW, s_presentLogicalH,
			s_presentPhysicalW, s_presentPhysicalH);
}

void overlay_sdl_present_bind_target(void) {
	if (!s_presentEnabled)
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, s_presentFBO);
	glViewport(0, 0, s_presentLogicalW, s_presentLogicalH);
}

int overlay_sdl_present_active(void) {
	return s_presentEnabled;
}

void overlay_sdl_present_draw(void) {
	if (!s_presentEnabled) {
		return;
	}

	GLint savedFramebuffer = 0;
	GLint savedViewport[4] = {0, 0, 0, 0};
	GLint savedProgram = 0;
	GLint savedVAO = 0;
	GLint savedVBO = 0;
	GLint savedTex = 0;
	GLint savedActiveTexUnit = 0;
	GLint savedBlendSrcRGB = GL_ONE, savedBlendDstRGB = GL_ZERO;
	GLint savedBlendSrcAlpha = GL_ONE, savedBlendDstAlpha = GL_ZERO;
	GLboolean savedBlend = glIsEnabled(GL_BLEND);
	GLboolean savedDepth = glIsEnabled(GL_DEPTH_TEST);
	GLboolean savedCull = glIsEnabled(GL_CULL_FACE);
	GLboolean savedScissor = glIsEnabled(GL_SCISSOR_TEST);

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFramebuffer);
	glGetIntegerv(GL_VIEWPORT, savedViewport);
	glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedVBO);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexUnit);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
	glGetIntegerv(GL_BLEND_SRC_RGB, &savedBlendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB, &savedBlendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDstAlpha);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, s_presentPhysicalW, s_presentPhysicalH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(s_presentProgram);
	pfn_glBindVertexArray(s_presentVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_presentVBO);

	float verts[24];
	present_vertices(verts, s_presentRotation);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_presentColorTex);
	if (s_presentLocTexture >= 0)
		glUniform1i(s_presentLocTexture, 0);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFramebuffer);
	glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
	if (savedBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (savedDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (savedCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if (savedScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	glBlendFuncSeparate((GLenum)savedBlendSrcRGB, (GLenum)savedBlendDstRGB,
						(GLenum)savedBlendSrcAlpha, (GLenum)savedBlendDstAlpha);
	glUseProgram((GLuint)savedProgram);
	pfn_glBindVertexArray((GLuint)savedVAO);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)savedVBO);
	glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex);
	glActiveTexture((GLenum)savedActiveTexUnit);

	overlay_sdl_present_bind_target();
}

void overlay_sdl_present_swap(OverlaySdlSwapFn swap_fn) {
	if (!swap_fn)
		return;
	if (s_presentEnabled)
		overlay_sdl_present_draw();
	swap_fn();
}

// ---------------------------------------------------------------------------
// init / destroy
// ---------------------------------------------------------------------------

static int ovl_sdl_init(int screen_w, int screen_h) {
	ovl_load_gl3_procs();
	s_screenW = screen_w;
	s_screenH = screen_h;
	s_overlayRotation = read_rotation_env("MUPEN64PLUS_OVERLAY_ROTATION", 0);
	if (s_overlayRotation == 90 || s_overlayRotation == 270) {
		s_overlayViewportW = screen_h;
		s_overlayViewportH = screen_w;
	} else {
		s_overlayViewportW = screen_w;
		s_overlayViewportH = screen_h;
	}
	if (s_overlayRotation != 0) {
		fprintf(stderr, "[OverlaySDL] overlay rotation=%d logical=%dx%d physical=%dx%d\n",
				s_overlayRotation, s_screenW, s_screenH,
				s_overlayViewportW, s_overlayViewportH);
	}

	// Scale factor: only the Brick (1024x768) is tall enough for 3x; 720p-class
	// screens (MLP1 960x720, Smart Pro / TG5050 1280x720) use 2x.
	if (screen_w <= 1024 && screen_h >= 768)
		s_scale = 3;
	else
		s_scale = 2;

	// Initialize SDL_ttf
	if (!TTF_WasInit()) {
		if (TTF_Init() < 0) {
			fprintf(stderr, "[OverlaySDL] TTF_Init failed: %s\n", TTF_GetError());
			return -1;
		}
	}

	// Reset icon storage
	for (int i = 0; i < MAX_ICONS; i++)
		s_icons[i] = NULL;
	s_iconCount = 0;

	// Load font
	const char* font_path = getenv("EMU_OVERLAY_FONT");
	if (!font_path || font_path[0] == '\0') {
		fprintf(stderr, "[OverlaySDL] EMU_OVERLAY_FONT not set\n");
		return -1;
	}

	// Match the launcher's font metrics: when the active Jawaka/Catastrophe
	// stylesheet supplies ui_font.size, use it as the LARGE tier and derive the
	// smaller tiers from the 16:14:12:10 ratios. Otherwise fall back to the
	// scale-based tiers (NextUI/MinUI launch path, no stylesheet).
	int large = emu_ovl_stylesheet_font_size(FONT_SIZE_LARGE);
	int font_sizes[EMU_OVL_FONT_COUNT] = {
		large,
		large * EMU_OVL_FONT_TIER_MEDIUM / EMU_OVL_FONT_TIER_LARGE,
		large * EMU_OVL_FONT_TIER_SMALL / EMU_OVL_FONT_TIER_LARGE,
		large * EMU_OVL_FONT_TIER_TINY / EMU_OVL_FONT_TIER_LARGE};
	const char* bump_env = getenv("CAT_FONT_BUMP");
	if (bump_env && bump_env[0] != '\0') {
		char* end = NULL;
		long bump = strtol(bump_env, &end, 10);
		if (end != bump_env) {
			if (bump < -8)
				bump = -8;
			if (bump > 12)
				bump = 12;
			for (int i = 0; i < EMU_OVL_FONT_COUNT; i++) {
				font_sizes[i] += (int)bump;
				if (font_sizes[i] < 8)
					font_sizes[i] = 8;
			}
		}
	}
	for (int i = 0; i < EMU_OVL_FONT_COUNT; i++) {
		s_fonts[i] = TTF_OpenFont(font_path, font_sizes[i]);
		if (!s_fonts[i]) {
			fprintf(stderr, "[OverlaySDL] TTF_OpenFont(%s, %d) failed: %s\n",
					font_path, font_sizes[i], TTF_GetError());
			for (int j = 0; j < i; j++) {
				TTF_CloseFont(s_fonts[j]);
				s_fonts[j] = NULL;
			}
			return -1;
		}
	}

	// Create render surface (ARGB8888)
	s_renderSurface = SDL_CreateRGBSurfaceWithFormat(
		0, screen_w, screen_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s_renderSurface) {
		fprintf(stderr, "[OverlaySDL] SDL_CreateRGBSurfaceWithFormat failed: %s\n",
				SDL_GetError());
		return -1;
	}

	// Create capture surface (ARGB8888)
	s_captureSurface = SDL_CreateRGBSurfaceWithFormat(
		0, screen_w, screen_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s_captureSurface) {
		fprintf(stderr, "[OverlaySDL] SDL_CreateRGBSurfaceWithFormat (capture) failed: %s\n",
				SDL_GetError());
		return -1;
	}

	// Allocate upload buffer for ARGB -> RGBA conversion
	s_uploadBuffer = (uint8_t*)malloc((size_t)screen_w * (size_t)screen_h * 4);
	if (!s_uploadBuffer) {
		fprintf(stderr, "[OverlaySDL] malloc upload buffer failed\n");
		return -1;
	}

	// --- GL setup ---
	// Save GL state during init (same as OverlayGL.cpp pattern)
	// Note: skip querying GL_VERTEX_ARRAY_BINDING — can crash on Mali drivers
	// when the render manager's VAO state is unexpected. Just reset to 0.
	GLint savedVAO = 0, savedVBO = 0, savedTex = 0, savedUnpackAlign = 4;
	pfn_glBindVertexArray(0);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedVBO);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);

	// Build textured fullscreen quad shader
	{
		GLuint vs = compile_shader(GL_VERTEX_SHADER, s_texVS);
		GLuint fs = compile_shader(GL_FRAGMENT_SHADER, s_texFS);
		if (!vs || !fs) {
			if (vs)
				glDeleteShader(vs);
			if (fs)
				glDeleteShader(fs);
			// Restore GL state before returning
			pfn_glBindVertexArray(savedVAO);
			glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
			glBindTexture(GL_TEXTURE_2D, savedTex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
			return -1;
		}
		s_texProgram = link_program(vs, fs);
		glDeleteShader(vs);
		glDeleteShader(fs);
		if (!s_texProgram) {
			pfn_glBindVertexArray(savedVAO);
			glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
			glBindTexture(GL_TEXTURE_2D, savedTex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
			return -1;
		}
		s_texLocTexture = glGetUniformLocation(s_texProgram, "uTexture");
	}

	// VAO/VBO for textured fullscreen quad (6 verts * 4 floats: pos.xy + uv.xy)
	pfn_glGenVertexArrays(1, &s_texVAO);
	glGenBuffers(1, &s_texVBO);
	pfn_glBindVertexArray(s_texVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_texVBO);
	glBufferData(GL_ARRAY_BUFFER, 6 * 4 * (GLsizeiptr)sizeof(float), NULL, GL_DYNAMIC_DRAW);
	GLint posLoc = glGetAttribLocation(s_texProgram, "aPos");
	GLint uvLoc = glGetAttribLocation(s_texProgram, "aTexCoord");
	glEnableVertexAttribArray(posLoc);
	glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(float), (void*)0);
	glEnableVertexAttribArray(uvLoc);
	glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(float),
						  (void*)(2 * sizeof(float)));
	pfn_glBindVertexArray(0);

	// Overlay texture
	glGenTextures(1, &s_overlayTexture);
	glBindTexture(GL_TEXTURE_2D, s_overlayTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Restore GL state
	pfn_glBindVertexArray(savedVAO);
	glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
	glBindTexture(GL_TEXTURE_2D, savedTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);

	return 0;
}

static void ovl_sdl_destroy(void) {
	present_destroy();

	for (int i = 0; i < EMU_OVL_FONT_COUNT; i++) {
		if (s_fonts[i]) {
			TTF_CloseFont(s_fonts[i]);
			s_fonts[i] = NULL;
		}
	}

	// Free loaded icons
	for (int i = 0; i < s_iconCount; i++) {
		if (s_icons[i]) {
			SDL_FreeSurface(s_icons[i]);
			s_icons[i] = NULL;
		}
	}
	s_iconCount = 0;

	if (s_renderSurface) {
		SDL_FreeSurface(s_renderSurface);
		s_renderSurface = NULL;
	}
	if (s_captureSurface) {
		SDL_FreeSurface(s_captureSurface);
		s_captureSurface = NULL;
	}

	free(s_uploadBuffer);
	s_uploadBuffer = NULL;

	if (s_texProgram) {
		glDeleteProgram(s_texProgram);
		s_texProgram = 0;
	}
	if (s_texVAO) {
		pfn_glDeleteVertexArrays(1, &s_texVAO);
		s_texVAO = 0;
	}
	if (s_texVBO) {
		glDeleteBuffers(1, &s_texVBO);
		s_texVBO = 0;
	}
	if (s_overlayTexture) {
		glDeleteTextures(1, &s_overlayTexture);
		s_overlayTexture = 0;
	}
}

// ---------------------------------------------------------------------------
// capture_frame — grab current GL framebuffer into SDL surface
// ---------------------------------------------------------------------------

static void ovl_sdl_capture_frame(void) {
	if (!s_captureSurface)
		return;

	int w = s_overlayViewportW > 0 ? s_overlayViewportW : s_screenW;
	int h = s_overlayViewportH > 0 ? s_overlayViewportH : s_screenH;

	// Temporary buffer for glReadPixels (RGBA, bottom-up)
	size_t row_bytes = (size_t)w * 4;
	uint8_t* gl_pixels = (uint8_t*)malloc(row_bytes * (size_t)h);
	if (!gl_pixels)
		return;

	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gl_pixels);

	// Copy into capture surface, flipping vertically and converting RGBA -> ARGB
	if (SDL_LockSurface(s_captureSurface) != 0) {
		fprintf(stderr, "[OverlaySDL] SDL_LockSurface failed: %s\n", SDL_GetError());
		free(gl_pixels);
		return;
	}
	uint8_t* dst_base = (uint8_t*)s_captureSurface->pixels;
	int dst_pitch = s_captureSurface->pitch;

	for (int y = 0; y < s_screenH; y++) {
		// GL row 0 is bottom, SDL row 0 is top => flip
		int src_y = (int)((int64_t)y * h / s_screenH);
		const uint8_t* src_row = gl_pixels + (size_t)(h - 1 - src_y) * row_bytes;
		uint8_t* dst_row = dst_base + (size_t)y * (size_t)dst_pitch;

		for (int x = 0; x < s_screenW; x++) {
			int src_x = (int)((int64_t)x * w / s_screenW);
			uint8_t r = src_row[src_x * 4 + 0];
			uint8_t g = src_row[src_x * 4 + 1];
			uint8_t b = src_row[src_x * 4 + 2];
			// Force alpha to 255 — GL framebuffer alpha is often 0 for opaque
			// game content, which would make saved screenshots invisible when
			// loaded and drawn with SDL_BLENDMODE_BLEND
			uint32_t* dst_px = (uint32_t*)(dst_row + x * 4);
			*dst_px = 0xFF000000u | ((uint32_t)r << 16) |
					  ((uint32_t)g << 8) | (uint32_t)b;
		}
	}

	SDL_UnlockSurface(s_captureSurface);
	free(gl_pixels);
}

// ---------------------------------------------------------------------------
// draw_captured_frame — blit captured frame with dimming
// ---------------------------------------------------------------------------

static void ovl_sdl_draw_captured_frame(float dim) {
	if (!s_captureSurface || !s_renderSurface)
		return;

	// Blit captured frame onto render surface
	SDL_SetSurfaceBlendMode(s_captureSurface, SDL_BLENDMODE_NONE);
	SDL_BlitSurface(s_captureSurface, NULL, s_renderSurface, NULL);

	// Apply dimming: overlay a semi-transparent black rect
	// dim=0.4 means final brightness is 40%, so overlay alpha = 60% = 153/255
	if (dim < 1.0f) {
		int alpha = (int)((1.0f - dim) * 255.0f + 0.5f);
		if (alpha > 255)
			alpha = 255;
		if (alpha < 0)
			alpha = 0;

		SDL_Surface* dimSurf = SDL_CreateRGBSurfaceWithFormat(
			0, 1, 1, 32, SDL_PIXELFORMAT_ARGB8888);
		if (dimSurf) {
			SDL_FillRect(dimSurf, NULL,
						 SDL_MapRGBA(dimSurf->format, 0, 0, 0, (Uint8)alpha));
			SDL_SetSurfaceBlendMode(dimSurf, SDL_BLENDMODE_BLEND);
			SDL_Rect dst_rect = {0, 0, s_screenW, s_screenH};
			SDL_BlitScaled(dimSurf, NULL, s_renderSurface, &dst_rect);
			SDL_FreeSurface(dimSurf);
		}
	}
}

// ---------------------------------------------------------------------------
// draw_rect — filled rectangle with alpha support
// ---------------------------------------------------------------------------

static void ovl_sdl_draw_rect(int x, int y, int w, int h, uint32_t color) {
	if (!s_renderSurface)
		return;

	// Extract ARGB components
	uint8_t a = (uint8_t)((color >> 24) & 0xFF);
	uint8_t r = (uint8_t)((color >> 16) & 0xFF);
	uint8_t g = (uint8_t)((color >> 8) & 0xFF);
	uint8_t b = (uint8_t)((color) & 0xFF);

	if (a == 255) {
		// Fully opaque: direct fill
		SDL_Rect rect = {x, y, w, h};
		SDL_FillRect(s_renderSurface, &rect,
					 SDL_MapRGBA(s_renderSurface->format, r, g, b, 255));
	} else {
		// Semi-transparent: use a temp surface and blit with blend
		SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormat(
			0, 1, 1, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!tmp)
			return;

		SDL_FillRect(tmp, NULL, SDL_MapRGBA(tmp->format, r, g, b, a));
		SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_BLEND);

		SDL_Rect dst_rect = {x, y, w, h};
		SDL_BlitScaled(tmp, NULL, s_renderSurface, &dst_rect);
		SDL_FreeSurface(tmp);
	}
}

// ---------------------------------------------------------------------------
// draw_rounded_rect — anti-aliased rounded rectangle (pill when radius == h/2)
// ---------------------------------------------------------------------------
//
// Fills the interior as 1-3 SDL_FillRect bands, then walks only the corner
// bounding boxes (radius × radius each) and blends per-pixel coverage based on
// the signed distance from each pixel center to the corner arc center. Gives
// ~4-pixel-wide soft edges at radius >= 20, matching NextUI's pill look.

static inline uint32_t ovl_blend_argb(uint32_t dst, uint8_t sr, uint8_t sg,
									   uint8_t sb, uint8_t sa) {
	if (sa == 255)
		return 0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | sb;
	uint32_t dr = (dst >> 16) & 0xFF;
	uint32_t dg = (dst >> 8) & 0xFF;
	uint32_t db = dst & 0xFF;
	uint32_t da = (dst >> 24) & 0xFF;
	uint32_t inv = 255 - sa;
	uint32_t nr = (sr * sa + dr * inv + 127) / 255;
	uint32_t ng = (sg * sa + dg * inv + 127) / 255;
	uint32_t nb = (sb * sa + db * inv + 127) / 255;
	uint32_t na = sa + (da * inv + 127) / 255;
	if (na > 255) na = 255;
	return (na << 24) | (nr << 16) | (ng << 8) | nb;
}

static void ovl_sdl_draw_rounded_rect(int x, int y, int w, int h, int radius,
									  uint32_t color) {
	if (!s_renderSurface || w <= 0 || h <= 0)
		return;
	if (radius < 0)
		radius = h / 2;
	if (radius > h / 2)
		radius = h / 2;
	if (radius > w / 2)
		radius = w / 2;
	if (radius < 0)
		radius = 0;

	uint8_t ca = (uint8_t)((color >> 24) & 0xFF);
	uint8_t cr = (uint8_t)((color >> 16) & 0xFF);
	uint8_t cg = (uint8_t)((color >> 8) & 0xFF);
	uint8_t cb = (uint8_t)((color) & 0xFF);

	Uint32 packed = SDL_MapRGBA(s_renderSurface->format, cr, cg, cb, ca);

	// Interior bands (no rounding in these regions):
	//   middle strip: full-width, y+radius .. y+h-radius
	//   top strip:    x+radius .. x+w-radius, y .. y+radius
	//   bottom strip: x+radius .. x+w-radius, y+h-radius .. y+h
	if (h - 2 * radius > 0) {
		SDL_Rect mid = {x, y + radius, w, h - 2 * radius};
		SDL_FillRect(s_renderSurface, &mid, packed);
	}
	int mid_w = w - 2 * radius;
	if (radius > 0 && mid_w > 0) {
		SDL_Rect top_mid = {x + radius, y, mid_w, radius};
		SDL_Rect bot_mid = {x + radius, y + h - radius, mid_w, radius};
		SDL_FillRect(s_renderSurface, &top_mid, packed);
		SDL_FillRect(s_renderSurface, &bot_mid, packed);
	}

	if (radius == 0)
		return;

	// Anti-aliased corners via per-pixel coverage.
	if (SDL_MUSTLOCK(s_renderSurface))
		SDL_LockSurface(s_renderSurface);

	uint32_t* pixels = (uint32_t*)s_renderSurface->pixels;
	int pitch = s_renderSurface->pitch / 4;
	int sw = s_renderSurface->w;
	int sh = s_renderSurface->h;

	float r_f = (float)radius;
	float r_in2 = (r_f - 0.5f) * (r_f - 0.5f);
	float r_out2 = (r_f + 0.5f) * (r_f + 0.5f);

	for (int cy = 0; cy < radius; cy++) {
		int py_top = y + cy;
		int py_bot = y + h - 1 - cy;
		float fdy = (float)cy + 0.5f - r_f;
		float fdy2 = fdy * fdy;
		for (int cx = 0; cx < radius; cx++) {
			float fdx = (float)cx + 0.5f - r_f;
			float dist2 = fdx * fdx + fdy2;
			if (dist2 > r_out2)
				continue;
			uint8_t alpha;
			if (dist2 < r_in2) {
				alpha = ca;
			} else {
				float dist = sqrtf(dist2);
				float cov = r_f + 0.5f - dist;
				if (cov < 0.0f) cov = 0.0f;
				if (cov > 1.0f) cov = 1.0f;
				alpha = (uint8_t)((float)ca * cov + 0.5f);
			}
			if (alpha == 0)
				continue;

			int px_left = x + cx;
			int px_right = x + w - 1 - cx;

			if (py_top >= 0 && py_top < sh) {
				if (px_left >= 0 && px_left < sw) {
					uint32_t* p = &pixels[py_top * pitch + px_left];
					*p = ovl_blend_argb(*p, cr, cg, cb, alpha);
				}
				if (px_right != px_left && px_right >= 0 && px_right < sw) {
					uint32_t* p = &pixels[py_top * pitch + px_right];
					*p = ovl_blend_argb(*p, cr, cg, cb, alpha);
				}
			}
			if (py_bot != py_top && py_bot >= 0 && py_bot < sh) {
				if (px_left >= 0 && px_left < sw) {
					uint32_t* p = &pixels[py_bot * pitch + px_left];
					*p = ovl_blend_argb(*p, cr, cg, cb, alpha);
				}
				if (px_right != px_left && px_right >= 0 && px_right < sw) {
					uint32_t* p = &pixels[py_bot * pitch + px_right];
					*p = ovl_blend_argb(*p, cr, cg, cb, alpha);
				}
			}
		}
	}

	if (SDL_MUSTLOCK(s_renderSurface))
		SDL_UnlockSurface(s_renderSurface);
}

// ---------------------------------------------------------------------------
// draw_text — TTF rendered text
// ---------------------------------------------------------------------------

static TTF_Font* get_font(int font_id) {
	if (font_id >= 0 && font_id < EMU_OVL_FONT_COUNT && s_fonts[font_id])
		return s_fonts[font_id];
	// Fallback to small
	return s_fonts[EMU_OVL_FONT_SMALL];
}

static void ovl_sdl_draw_text(const char* text, int x, int y, uint32_t color, int font_id) {
	if (!text || !*text || !s_renderSurface)
		return;

	TTF_Font* font = get_font(font_id);
	if (!font)
		return;

	// Extract ARGB -> SDL_Color (RGBA)
	SDL_Color sdl_color;
	sdl_color.r = (uint8_t)((color >> 16) & 0xFF);
	sdl_color.g = (uint8_t)((color >> 8) & 0xFF);
	sdl_color.b = (uint8_t)((color) & 0xFF);
	sdl_color.a = (uint8_t)((color >> 24) & 0xFF);

	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font, text, sdl_color);
	if (!text_surf)
		return;

	SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_BLEND);
	SDL_Rect dst_rect = {x, y, text_surf->w, text_surf->h};
	SDL_BlitSurface(text_surf, NULL, s_renderSurface, &dst_rect);
	SDL_FreeSurface(text_surf);
}

// ---------------------------------------------------------------------------
// text_width / text_height
// ---------------------------------------------------------------------------

static int ovl_sdl_text_width(const char* text, int font_id) {
	if (!text || !*text)
		return 0;

	TTF_Font* font = get_font(font_id);
	if (!font)
		return 0;

	int w = 0;
	TTF_SizeUTF8(font, text, &w, NULL);
	return w;
}

static int ovl_sdl_text_height(int font_id) {
	TTF_Font* font = get_font(font_id);
	if (!font)
		return 0;
	return TTF_FontHeight(font);
}

// ---------------------------------------------------------------------------
// begin_frame — save GL state, clear render surface
// ---------------------------------------------------------------------------

static void ovl_sdl_begin_frame(void) {
	// Save GL state (must restore ALL state overlay touches to avoid
	// corrupting GLideN64's CachedFunctions state -- especially on PowerVR)
	glGetIntegerv(GL_VIEWPORT, s_savedViewport);
	glGetIntegerv(GL_SCISSOR_BOX, s_savedScissorBox);
	s_savedBlend = glIsEnabled(GL_BLEND);
	s_savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
	s_savedCullFace = glIsEnabled(GL_CULL_FACE);
	s_savedScissorTest = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_BLEND_SRC_RGB, &s_savedBlendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB, &s_savedBlendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &s_savedBlendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &s_savedBlendDstAlpha);
	glGetIntegerv(GL_CURRENT_PROGRAM, &s_savedProgram);
	// Skip querying GL_VERTEX_ARRAY_BINDING — crashes on Mali drivers when
	// the render manager leaves VAOs in unexpected state. Just reset to 0.
	s_savedVAO = 0;
	pfn_glBindVertexArray(0);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_savedVBO);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &s_savedActiveTexUnit);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &s_savedTex0);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &s_savedUnpackAlignment);

	// Clear the render surface to fully transparent
	if (s_renderSurface) {
		SDL_FillRect(s_renderSurface, NULL, 0x00000000);
	}
}

// ---------------------------------------------------------------------------
// end_frame — upload surface to GL, draw fullscreen quad, restore GL state
// ---------------------------------------------------------------------------

static void convert_argb_to_rgba(const uint8_t* src, uint8_t* dst, int w, int h, int src_pitch) {
	for (int y = 0; y < h; y++) {
		const uint32_t* src_row = (const uint32_t*)(src + (size_t)y * (size_t)src_pitch);
		uint32_t* dst_row = (uint32_t*)(dst + (size_t)y * (size_t)w * 4);

		for (int x = 0; x < w; x++) {
			uint32_t argb = src_row[x];
			// ARGB: A[31:24] R[23:16] G[15:8] B[7:0]
			// RGBA: R[31:24] G[23:16] B[15:8] A[7:0] (as uint32 on big-endian)
			// But GL_RGBA + GL_UNSIGNED_BYTE reads bytes as R,G,B,A regardless of endian
			uint8_t a = (uint8_t)((argb >> 24) & 0xFF);
			uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
			uint8_t g = (uint8_t)((argb >> 8) & 0xFF);
			uint8_t b = (uint8_t)((argb) & 0xFF);
			uint8_t* p = (uint8_t*)&dst_row[x];
			p[0] = r;
			p[1] = g;
			p[2] = b;
			p[3] = a;
		}
	}
}

static void ovl_sdl_end_frame(void) {
	if (!s_renderSurface || !s_uploadBuffer)
		return;

	// Convert ARGB8888 surface to RGBA byte order for GL upload
	SDL_LockSurface(s_renderSurface);
	convert_argb_to_rgba(
		(const uint8_t*)s_renderSurface->pixels,
		s_uploadBuffer,
		s_screenW, s_screenH,
		s_renderSurface->pitch);
	SDL_UnlockSurface(s_renderSurface);

	// Set overlay GL state
	glViewport(0, 0,
			   s_overlayViewportW > 0 ? s_overlayViewportW : s_screenW,
			   s_overlayViewportH > 0 ? s_overlayViewportH : s_screenH);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// Upload composited surface as GL texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_overlayTexture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_screenW, s_screenH, 0,
				 GL_RGBA, GL_UNSIGNED_BYTE, s_uploadBuffer);

	float verts[24];
	overlay_vertices(verts, s_overlayRotation);

	glUseProgram(s_texProgram);
	glUniform1i(s_texLocTexture, 0);

	pfn_glBindVertexArray(s_texVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_texVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)sizeof(verts), verts);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	pfn_glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	// Restore all saved GL state
	glViewport(s_savedViewport[0], s_savedViewport[1],
			   s_savedViewport[2], s_savedViewport[3]);
	glScissor(s_savedScissorBox[0], s_savedScissorBox[1],
			  s_savedScissorBox[2], s_savedScissorBox[3]);

	if (s_savedBlend)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);
	if (s_savedDepthTest)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	if (s_savedCullFace)
		glEnable(GL_CULL_FACE);
	else
		glDisable(GL_CULL_FACE);
	if (s_savedScissorTest)
		glEnable(GL_SCISSOR_TEST);
	else
		glDisable(GL_SCISSOR_TEST);

	glBlendFuncSeparate(s_savedBlendSrcRGB, s_savedBlendDstRGB,
						s_savedBlendSrcAlpha, s_savedBlendDstAlpha);

	// Restore object bindings (critical for GLideN64's cached state on PowerVR)
	glUseProgram(s_savedProgram);
	pfn_glBindVertexArray(s_savedVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_savedVBO);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_savedTex0);
	glActiveTexture(s_savedActiveTexUnit);
	glPixelStorei(GL_UNPACK_ALIGNMENT, s_savedUnpackAlignment);
}

// ---------------------------------------------------------------------------
// Icon loading/drawing (PNG images for button hints)
// ---------------------------------------------------------------------------

static int ovl_sdl_load_icon(const char* path, int target_height) {
	if (!path || s_iconCount >= MAX_ICONS || target_height <= 0)
		return -1;

	SDL_Surface* raw = SDL_LoadBMP(path);
	if (!raw) {
		fprintf(stderr, "[OverlaySDL] SDL_LoadBMP(%s) failed: %s\n", path, SDL_GetError());
		return -1;
	}

	// Scale to target height preserving aspect ratio
	int scaled_w = (int)((float)raw->w * (float)target_height / (float)raw->h + 0.5f);
	SDL_Surface* argb = SDL_CreateRGBSurfaceWithFormat(
		0, scaled_w, target_height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!argb) {
		SDL_FreeSurface(raw);
		return -1;
	}

	SDL_SetSurfaceBlendMode(raw, SDL_BLENDMODE_NONE);
	SDL_Rect dst = {0, 0, scaled_w, target_height};
	SDL_BlitScaled(raw, NULL, argb, &dst);
	SDL_FreeSurface(raw);

	int id = s_iconCount++;
	s_icons[id] = argb;
	return id;
}

static int ovl_sdl_load_icon_rgba(const uint32_t* pixels, int w, int h,
								  int target_height) {
	if (!pixels || s_iconCount >= MAX_ICONS || target_height <= 0)
		return -1;

	SDL_Surface* raw = SDL_CreateRGBSurfaceFrom(
		(void*)pixels, w, h, 32, w * 4,
		0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	if (!raw)
		return -1;

	int scaled_w = (int)((float)w * (float)target_height / (float)h + 0.5f);
	SDL_Surface* argb = SDL_CreateRGBSurfaceWithFormat(
		0, scaled_w, target_height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!argb) {
		SDL_FreeSurface(raw);
		return -1;
	}

	SDL_SetSurfaceBlendMode(raw, SDL_BLENDMODE_NONE);
	SDL_Rect dst = {0, 0, scaled_w, target_height};
	SDL_BlitScaled(raw, NULL, argb, &dst);
	SDL_FreeSurface(raw);

	int id = s_iconCount++;
	s_icons[id] = argb;
	return id;
}

static void ovl_sdl_draw_icon(int icon_id, int x, int y) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id] || !s_renderSurface)
		return;

	SDL_SetSurfaceBlendMode(s_icons[icon_id], SDL_BLENDMODE_BLEND);
	SDL_Rect dst = {x, y, s_icons[icon_id]->w, s_icons[icon_id]->h};
	SDL_BlitSurface(s_icons[icon_id], NULL, s_renderSurface, &dst);
}

static int ovl_sdl_icon_width(int icon_id) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id])
		return 0;
	return s_icons[icon_id]->w;
}

static int ovl_sdl_icon_height(int icon_id) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id])
		return 0;
	return s_icons[icon_id]->h;
}

static void ovl_sdl_free_icon(int icon_id) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id])
		return;
	SDL_FreeSurface(s_icons[icon_id]);
	s_icons[icon_id] = NULL;
}

// ---------------------------------------------------------------------------
static int path_has_suffix(const char* path, const char* suffix) {
	size_t path_len;
	size_t suffix_len;
	if (!path || !suffix)
		return 0;
	path_len = strlen(path);
	suffix_len = strlen(suffix);
	return path_len >= suffix_len &&
		strcmp(path + path_len - suffix_len, suffix) == 0;
}

static int ovl_sdl_save_captured_frame_png(const char* path) {
	if (!path || !s_captureSurface)
		return -1;

	FILE* fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "[OverlaySDL] fopen(%s) failed\n", path);
		return -1;
	}

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png) {
		fclose(fp);
		return -1;
	}
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, NULL);
		fclose(fp);
		return -1;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		fclose(fp);
		unlink(path);
		return -1;
	}

	png_init_io(png, fp);
	png_set_IHDR(png, info, s_captureSurface->w, s_captureSurface->h, 8,
				 PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
				 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);

	uint8_t* row = (uint8_t*)malloc((size_t)s_captureSurface->w * 4);
	if (!row) {
		png_destroy_write_struct(&png, &info);
		fclose(fp);
		unlink(path);
		return -1;
	}

	if (SDL_LockSurface(s_captureSurface) != 0) {
		fprintf(stderr, "[OverlaySDL] SDL_LockSurface failed: %s\n", SDL_GetError());
		free(row);
		png_destroy_write_struct(&png, &info);
		fclose(fp);
		unlink(path);
		return -1;
	}
	for (int y = 0; y < s_captureSurface->h; y++) {
		uint8_t* src = (uint8_t*)s_captureSurface->pixels +
					   (size_t)y * (size_t)s_captureSurface->pitch;
		for (int x = 0; x < s_captureSurface->w; x++) {
			Uint8 r, g, b, a;
			Uint32 px;
			memcpy(&px, src + (size_t)x * 4, sizeof(px));
			SDL_GetRGBA(px, s_captureSurface->format, &r, &g, &b, &a);
			row[x * 4 + 0] = r;
			row[x * 4 + 1] = g;
			row[x * 4 + 2] = b;
			row[x * 4 + 3] = a ? a : 255;
		}
		png_write_row(png, row);
	}
	SDL_UnlockSurface(s_captureSurface);

	free(row);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(fp);
	return 0;
}

// ---------------------------------------------------------------------------
// save_captured_frame — write captured frame as BMP or PNG
// ---------------------------------------------------------------------------

static int ovl_sdl_save_captured_frame(const char* path) {
	if (!path || !s_captureSurface)
		return -1;
	if (path_has_suffix(path, ".png")) {
		if (ovl_sdl_save_captured_frame_png(path) != 0) {
			fprintf(stderr, "[OverlaySDL] PNG save failed: %s\n", path);
			return -1;
		}
		return 0;
	}
	if (SDL_SaveBMP(s_captureSurface, path) != 0) {
		fprintf(stderr, "[OverlaySDL] SDL_SaveBMP(%s) failed: %s\n", path, SDL_GetError());
		return -1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Backend struct
// ---------------------------------------------------------------------------

static EmuOvlRenderBackend s_backend = {
	ovl_sdl_init,
	ovl_sdl_destroy,
	ovl_sdl_draw_rect,
	ovl_sdl_draw_rounded_rect,
	ovl_sdl_draw_text,
	ovl_sdl_text_width,
	ovl_sdl_text_height,
	ovl_sdl_begin_frame,
	ovl_sdl_end_frame,
	ovl_sdl_capture_frame,
	ovl_sdl_draw_captured_frame,
	ovl_sdl_load_icon,
	ovl_sdl_load_icon_rgba,
	ovl_sdl_free_icon,
	ovl_sdl_draw_icon,
	ovl_sdl_icon_width,
	ovl_sdl_icon_height,
	ovl_sdl_save_captured_frame};

EmuOvlRenderBackend* overlay_sdl_get_backend(void) {
	return &s_backend;
}
