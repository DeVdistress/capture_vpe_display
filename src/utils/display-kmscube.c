/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 * Copyright (c) 2012 Vincent Stehlé <v-stehle@ti.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Based on kmscube application by Rob Clark, which was based on a egl cube
 * test app originally written by Arvin Schnell.  And based on display-kms by
 * Rob Clark.  And also based on xbmc eglImageExternal and
 * TIRawVideoEGLImageHandle additions by Rob Clark.
 *
 * This display 'kmscube' draws a rotating 3d cube with EGL on kms display,
 * with video mapped as a texture on the cube faces. This display has the
 * particularity to have no framebuffer, per se, as we expect to render with
 * EGL/GL ES, rather.
 *
 * TODO:
 * - Remove static structs
 * - Factorize kms code with display-kms
 * - Enable from command line only; no auto detect at open like x11/kms
 * - Cleanup display_kmscube options
 * - Implement the "post" functions so that cube faces are updated
 * - Do the necessary cleanup in the close function
 * - Remove the extra level of structure inside display_kmscube
 * - Cleanup commented out code
 * - Revisit disp pointer in struct drm_fb
 * - Better handling of rounding and alignment when allocating single planar
 *   YUV buffers.
 * - Remove unused vertex colors.
 * - Fix warnings
 * - Allow selecting the mode from CLI, like display-kms
 * - Better handling of cropping
 * - Interactive keyboard keys for FOV, distance... with OSD?
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "util.h"

#include <linux/videodev2.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <pthread.h>

#include "esUtil.h"

#define MAX_FACES 6

typedef EGLImageKHR (eglCreateImageKHR_t)(EGLDisplay dpy, EGLContext ctx,
			EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);

typedef EGLBoolean (eglDestroyImageKHR_t)(EGLDisplay dpy, EGLImageKHR image);

typedef void (glEGLImageTargetTexture2DOES_t)(GLenum target, GLeglImageOES image);


#define to_display_kmscube(x) container_of(x, struct display_kmscube, base)
struct display_kmscube {
	struct display base;
	uint32_t bo_flags,
		i;		// This is used to animate the cube.

	// GL.
	struct {
		EGLDisplay display;
		EGLConfig config;
		EGLContext context;
		EGLSurface surface;
		GLuint program;
		GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix, uniform_texture;
		GLuint texture_name;
		eglCreateImageKHR_t *eglCreateImageKHR;
		eglDestroyImageKHR_t *eglDestroyImageKHR;
		glEGLImageTargetTexture2DOES_t *glEGLImageTargetTexture2DOES;
		float distance, fov;
	} gl;

	// GBM.
	struct {
		struct gbm_device *dev;
		struct gbm_surface *surface;
		struct gbm_bo *bo;
	} gbm;

	// DRM.
	struct {
		// Note: fd is in base display
		drmModeModeInfo *mode;
		uint32_t crtc_id;
		uint32_t connector_id;
		drmModePlaneRes *plane_resources;
	} drm;

	//user specified connector id
	uint32_t user_connector_id;

	int num_faces;
	struct buffer *current_buffers[MAX_FACES];
	pthread_t renderThread;
	pthread_mutex_t lock[MAX_FACES];
	pthread_mutex_t init_lock;
};

/* All our buffers are only vid buffers, and they all have an EGLImage. */
#define to_buffer_kmscube(x) container_of(x, struct buffer_kmscube, base)
struct buffer_kmscube {
	struct buffer base;
	uint32_t fb_id;
	EGLImageKHR egl_img;
	GLuint texture_name;
	int face_no;
};

struct drm_fb {
	struct gbm_bo *bo;
	struct display_kmscube *disp_kmsc;
	uint32_t fb_id;
};

static void *render_thread(void *data);
static int create_texture(struct display_kmscube *disp_kmsc, struct buffer *buf);
static int get_texture(struct display_kmscube *disp_kmsc, int face);

static int init_drm(struct display_kmscube *disp_kmsc)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	int i, area;

	resources = drmModeGetResources(disp_kmsc->base.fd);
	if (!resources) {
		ERROR("drmModeGetResources failed: %s", strerror(errno));
		return -1;
	}

	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(disp_kmsc->base.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED && \
		    connector->connector_id == disp_kmsc->user_connector_id) {
			/* it's connected, let's use this! */
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		ERROR("no connected connector!");
		return -1;
	}

	/* find highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];
		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			disp_kmsc->drm.mode = current_mode;
			area = current_area;
		}
	}

	if (!disp_kmsc->drm.mode) {
		ERROR("could not find mode!");
		return -1;
	}

	/* find encoder: */
	for (i = 0; i < connector->count_encoders; i++) {
		encoder = drmModeGetEncoder(disp_kmsc->base.fd, connector->encoders[i]);
		/* Take the fisrt one, if none is assigned */
		if (!connector->encoder_id)
		{
			connector->encoder_id = encoder->encoder_id;
		}

		if (encoder->encoder_id == connector->encoder_id) {
			/* find the first valid CRTC if not assigned */
			if (!encoder->crtc_id)
			{
				int k;
				for (k = 0; k < resources->count_crtcs; ++k) {
					/* check whether this CRTC works with the encoder */
					if (!(encoder->possible_crtcs & (1 << k)))
						continue;

					encoder->crtc_id = resources->crtcs[k];
					break;
				}

				if (!encoder->crtc_id)
				{
					ERROR("Encoder(%d): no CRTC find!\n", encoder->encoder_id);
					drmModeFreeEncoder(encoder);
					encoder = NULL;
					continue;
				}
			}
			break;
		}
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (!encoder) {
		ERROR("no encoder!");
		return -1;
	}

	disp_kmsc->drm.crtc_id = encoder->crtc_id;
	disp_kmsc->drm.connector_id = connector->connector_id;
	printf("Chosen Connector ID = %d\n", disp_kmsc->drm.connector_id);

	return 0;
}

static int init_gbm(struct display_kmscube *disp_kmsc)
{
	disp_kmsc->gbm.dev = gbm_create_device(disp_kmsc->base.fd);

	disp_kmsc->gbm.surface = gbm_surface_create(disp_kmsc->gbm.dev,
			disp_kmsc->drm.mode->hdisplay, disp_kmsc->drm.mode->vdisplay,
			GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!disp_kmsc->gbm.surface) {
		ERROR("failed to create gbm surface");
		return -1;
	}

	return 0;
}

static int init_gl(struct display_kmscube *disp_kmsc)
{
	EGLint major, minor, n;
	GLuint vertex_shader, fragment_shader;
	GLint ret;

	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	static const char *vertex_shader_source =
			"uniform mat4 modelviewMatrix;      \n"
			"uniform mat4 modelviewprojectionMatrix;\n"
			"uniform mat3 normalMatrix;         \n"
			"                                   \n"
			"attribute vec4 in_position;        \n"
			"attribute vec3 in_normal;          \n"
			"attribute vec4 in_color;           \n"
			"attribute vec2 in_texuv;           \n"
			"\n"
			"vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);\n"
			"                                   \n"
			"varying float VaryingLight;        \n"
			"varying vec2 vVaryingTexUV;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_Position = modelviewprojectionMatrix * in_position;\n"
			"    vec3 vEyeNormal = normalMatrix * in_normal;\n"
			"    vec4 vPosition4 = modelviewMatrix * in_position;\n"
			"    vec3 vPosition3 = vPosition4.xyz / vPosition4.w;\n"
			"    vec3 vLightDir = normalize(lightSource.xyz - vPosition3);\n"
			"    VaryingLight = max(0.0, dot(vEyeNormal, vLightDir));\n"
			"    vVaryingTexUV = in_texuv;      \n"
			"}                                  \n";

	static const char *fragment_shader_source =
			"#extension GL_OES_EGL_image_external : require\n"
			"                                   \n"
			"precision mediump float;           \n"
			"                                   \n"
			"uniform samplerExternalOES texture;\n"
			"                                   \n"
			"varying float VaryingLight;        \n"
			"varying vec2 vVaryingTexUV;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    vec4 t = texture2D(texture, vVaryingTexUV);\n"
			"    gl_FragColor = vec4(VaryingLight * t.rgb, 1.0);\n"
			"}                                  \n";

	disp_kmsc->gl.display = eglGetDisplay(disp_kmsc->gbm.dev);

	if (!eglInitialize(disp_kmsc->gl.display, &major, &minor)) {
		ERROR("failed to initialize");
		return -1;
	}

	printf("Using display %p with EGL version %d.%d\n",
			disp_kmsc->gl.display, major, minor);

	printf("EGL Version \"%s\"\n", eglQueryString(disp_kmsc->gl.display, EGL_VERSION));
	printf("EGL Vendor \"%s\"\n", eglQueryString(disp_kmsc->gl.display, EGL_VENDOR));
	printf("EGL Extensions \"%s\"\n", eglQueryString(disp_kmsc->gl.display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		ERROR("failed to bind api EGL_OPENGL_ES_API");
		return -1;
	}

	if (!eglChooseConfig(disp_kmsc->gl.display, config_attribs, &disp_kmsc->gl.config, 1, &n) || n != 1) {
		ERROR("failed to choose config: %d", n);
		return -1;
	}

	disp_kmsc->gl.context = eglCreateContext(disp_kmsc->gl.display, disp_kmsc->gl.config,
			EGL_NO_CONTEXT, context_attribs);
	if (disp_kmsc->gl.context == NULL) {
		ERROR("failed to create context");
		return -1;
	}

	disp_kmsc->gl.surface = eglCreateWindowSurface(disp_kmsc->gl.display,
		disp_kmsc->gl.config, disp_kmsc->gbm.surface, NULL);
	if (disp_kmsc->gl.surface == EGL_NO_SURFACE) {
		ERROR("failed to create egl surface");
		return -1;
	}

	/* connect the context to the surface */
	eglMakeCurrent(disp_kmsc->gl.display, disp_kmsc->gl.surface,
			disp_kmsc->gl.surface, disp_kmsc->gl.context);

	// EGL Image.
	if (!(disp_kmsc->gl.eglCreateImageKHR = eglGetProcAddress("eglCreateImageKHR"))) {
		ERROR("No eglCreateImageKHR?!");
		return -1;
	}

	if (!(disp_kmsc->gl.eglDestroyImageKHR = eglGetProcAddress("eglDestroyImageKHR"))) {
		ERROR("No eglDestroyImageKHR?!");
		return -1;
	}

	if (!(disp_kmsc->gl.glEGLImageTargetTexture2DOES = eglGetProcAddress("glEGLImageTargetTexture2DOES"))) {
		ERROR("No glEGLImageTargetTexture2DOES?!");
		return -1;
	}

	const char *exts = glGetString(GL_EXTENSIONS);
	printf("GL Extensions \"%s\"\n", exts);

	if (!strstr(eglQueryString(disp_kmsc->gl.display, EGL_EXTENSIONS), "EGL_EXT_image_dma_buf_import")) {
		ERROR("No EGL_EXT_image_dma_buf_import extension?!");
		return -1;
	}

	vertex_shader = glCreateShader(GL_VERTEX_SHADER);

	glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
	glCompileShader(vertex_shader);

	glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		ERROR("vertex shader compilation failed!:");
		glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &ret);
		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(vertex_shader, ret, NULL, log);
			ERROR("%s", log);
		}

		return -1;
	}

	fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
	glCompileShader(fragment_shader);

	glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		ERROR("fragment shader compilation failed!:");
		glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(fragment_shader, ret, NULL, log);
			ERROR("%s", log);
		}

		return -1;
	}

	disp_kmsc->gl.program = glCreateProgram();

	glAttachShader(disp_kmsc->gl.program, vertex_shader);
	glAttachShader(disp_kmsc->gl.program, fragment_shader);

	glBindAttribLocation(disp_kmsc->gl.program, 0, "in_position");
	glBindAttribLocation(disp_kmsc->gl.program, 1, "in_normal");
	glBindAttribLocation(disp_kmsc->gl.program, 2, "in_color");
	glBindAttribLocation(disp_kmsc->gl.program, 3, "in_texuv");

	glLinkProgram(disp_kmsc->gl.program);

	glGetProgramiv(disp_kmsc->gl.program, GL_LINK_STATUS, &ret);
	if (!ret) {
		char *log;

		ERROR("program linking failed!:");
		glGetProgramiv(disp_kmsc->gl.program, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetProgramInfoLog(disp_kmsc->gl.program, ret, NULL, log);
			ERROR("%s", log);
		}

		return -1;
	}

	glUseProgram(disp_kmsc->gl.program);

	disp_kmsc->gl.modelviewmatrix = glGetUniformLocation(disp_kmsc->gl.program, "modelviewMatrix");
	disp_kmsc->gl.modelviewprojectionmatrix =
		glGetUniformLocation(disp_kmsc->gl.program, "modelviewprojectionMatrix");
	disp_kmsc->gl.normalmatrix = glGetUniformLocation(disp_kmsc->gl.program, "normalMatrix");
	disp_kmsc->gl.uniform_texture = glGetUniformLocation(disp_kmsc->gl.program, "uniform_texture");

	glViewport(0, 0, disp_kmsc->drm.mode->hdisplay, disp_kmsc->drm.mode->vdisplay);

	/* Textures would be created at runtime by render thread */

	return 0;
}

static void draw(struct display_kmscube *disp_kmsc)
{
	int face, tex_name;
	ESMatrix modelview;
	static const GLfloat vVertices[] = {
			// front
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f, // point magenta
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			// back
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, +1.0f, -1.0f, // point yellow
			-1.0f, +1.0f, -1.0f, // point green
			// right
			+1.0f, -1.0f, +1.0f, // point magenta
			+1.0f, -1.0f, -1.0f, // point red
			+1.0f, +1.0f, +1.0f, // point white
			+1.0f, +1.0f, -1.0f, // point yellow
			// left
			-1.0f, -1.0f, -1.0f, // point black
			-1.0f, -1.0f, +1.0f, // point blue
			-1.0f, +1.0f, -1.0f, // point green
			-1.0f, +1.0f, +1.0f, // point cyan
			// top
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			-1.0f, +1.0f, -1.0f, // point green
			+1.0f, +1.0f, -1.0f, // point yellow
			// bottom
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f  // point magenta
	};

	static const GLfloat vColors[] = {
			// front
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f, // magenta
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			// back
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  0.0f, // black
			1.0f,  1.0f,  0.0f, // yellow
			0.0f,  1.0f,  0.0f, // green
			// right
			1.0f,  0.0f,  1.0f, // magenta
			1.0f,  0.0f,  0.0f, // red
			1.0f,  1.0f,  1.0f, // white
			1.0f,  1.0f,  0.0f, // yellow
			// left
			0.0f,  0.0f,  0.0f, // black
			0.0f,  0.0f,  1.0f, // blue
			0.0f,  1.0f,  0.0f, // green
			0.0f,  1.0f,  1.0f, // cyan
			// top
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			0.0f,  1.0f,  0.0f, // green
			1.0f,  1.0f,  0.0f, // yellow
			// bottom
			0.0f,  0.0f,  0.0f, // black
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f  // magenta
	};

	static const GLfloat vNormals[] = {
			// front
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			// back
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			// right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			// left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			// top
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			// bottom
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f  // down
	};

	static const GLfloat vTexUVs[] = {
			// front
			0.0f,  1.0f,
			1.0f,  1.0f,
			0.0f,  0.0f,
			1.0f,  0.0f,
			// back
			0.0f,  1.0f,
			1.0f,  1.0f,
			0.0f,  0.0f,
			1.0f,  0.0f,
			// right
			0.0f,  1.0f,
			1.0f,  1.0f,
			0.0f,  0.0f,
			1.0f,  0.0f,
			// left
			0.0f,  1.0f,
			1.0f,  1.0f,
			0.0f,  0.0f,
			1.0f,  0.0f,
			// top
			0.0f,  1.0f,
			1.0f,  1.0f,
			0.0f,  0.0f,
			1.0f,  0.0f,
			// bottom
			0.0f,  1.0f,
			1.0f,  1.0f,
			0.0f,  0.0f,
			1.0f,  0.0f,
	};

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vVertices);
	glEnableVertexAttribArray(0);

	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, vNormals);
	glEnableVertexAttribArray(1);

	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, vColors);
	glEnableVertexAttribArray(2);

	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, vTexUVs);
	glEnableVertexAttribArray(3);

	esMatrixLoadIdentity(&modelview);
	esTranslate(&modelview, 0.0f, 0.0f, -disp_kmsc->gl.distance);
	esRotate(&modelview, 45.0f + (0.25f * disp_kmsc->i), 1.0f, 0.0f, 0.0f);
	esRotate(&modelview, 45.0f - (0.5f * disp_kmsc->i), 0.0f, 1.0f, 0.0f);
	esRotate(&modelview, 10.0f + (0.15f * disp_kmsc->i), 0.0f, 0.0f, 1.0f);

	GLfloat aspect = (GLfloat)(disp_kmsc->drm.mode->hdisplay) / (GLfloat)(disp_kmsc->drm.mode->vdisplay);

	ESMatrix projection;
	esMatrixLoadIdentity(&projection);
	esPerspective(&projection, disp_kmsc->gl.fov, aspect, 1.0f, 10.0f);

	ESMatrix modelviewprojection;
	esMatrixLoadIdentity(&modelviewprojection);
	esMatrixMultiply(&modelviewprojection, &modelview, &projection);

	float normal[9];
	normal[0] = modelview.m[0][0];
	normal[1] = modelview.m[0][1];
	normal[2] = modelview.m[0][2];
	normal[3] = modelview.m[1][0];
	normal[4] = modelview.m[1][1];
	normal[5] = modelview.m[1][2];
	normal[6] = modelview.m[2][0];
	normal[7] = modelview.m[2][1];
	normal[8] = modelview.m[2][2];

	glUniformMatrix4fv(disp_kmsc->gl.modelviewmatrix, 1, GL_FALSE, &modelview.m[0][0]);
	glUniformMatrix4fv(disp_kmsc->gl.modelviewprojectionmatrix, 1, GL_FALSE, &modelviewprojection.m[0][0]);
	glUniformMatrix3fv(disp_kmsc->gl.normalmatrix, 1, GL_FALSE, normal);

	glEnable(GL_CULL_FACE);

	for(face=0; face<MAX_FACES; face++) {
		glActiveTexture(GL_TEXTURE0);
		tex_name = get_texture(disp_kmsc, face);
		glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_name);
		if (glGetError() != GL_NO_ERROR)
			printf("glBindTexture failed render for face %d!\n", face);
		glUniform1i(disp_kmsc->gl.uniform_texture, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, face * 4, 4);
	}
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
//	struct gbm_device *gbm = gbm_bo_get_device(bo);
	struct display_kmscube *disp_kmsc = fb->disp_kmsc;

	if (fb->fb_id)
		drmModeRmFB(disp_kmsc->base.fd, fb->fb_id);

	free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct display_kmscube *disp_kmsc, struct gbm_bo *bo)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height, stride, handle;
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof(*fb));
	fb->bo = bo;
	fb->disp_kmsc = disp_kmsc;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;

	ret = drmModeAddFB(disp_kmsc->base.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
	if (ret) {
		ERROR("failed to create fb: %s", strerror(errno));
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

static struct omap_bo *
alloc_bo(struct display *disp, uint32_t bpp, uint32_t width, uint32_t height,
		uint32_t *bo_handle, uint32_t *pitch)
{
	struct display_kmscube *disp_kmsc = to_display_kmscube(disp);
	struct omap_bo *bo;
	uint32_t bo_flags = disp_kmsc->bo_flags;

	if ((bo_flags & OMAP_BO_TILED) == OMAP_BO_TILED) {
		bo_flags &= ~OMAP_BO_TILED;
		if (bpp == 8) {
			bo_flags |= OMAP_BO_TILED_8;
		} else if (bpp == 16) {
			bo_flags |= OMAP_BO_TILED_16;
		} else if (bpp == 32) {
			bo_flags |= OMAP_BO_TILED_32;
		}
	}

	bo_flags |= OMAP_BO_WC;

	if (bo_flags & OMAP_BO_TILED) {
		bo = omap_bo_new_tiled(disp->dev, width, height, bo_flags);
	} else {
		bo = omap_bo_new(disp->dev, width * height * bpp / 8, bo_flags);
	}

	if (bo) {
		*bo_handle = omap_bo_handle(bo);
		*pitch = width * bpp / 8;
		if (bo_flags & OMAP_BO_TILED)
			*pitch = ALIGN2(*pitch, PAGE_SHIFT);
	}

	return bo;
}

/* We allocate single planar buffers, always. This, for EGLImage. Also, we
 * create on EGLImageKHR per buffer. */
static struct buffer *
alloc_buffer(struct display *disp, uint32_t fourcc, uint32_t w, uint32_t h)
{
	struct display_kmscube *disp_kmsc = to_display_kmscube(disp);
	struct buffer_kmscube *buf_kmsc;
	struct buffer *buf;
	uint32_t bo_handles[4] = {0}, offsets[4] = {0};

	buf_kmsc = calloc(1, sizeof(*buf_kmsc));
	if (!buf_kmsc) {
		ERROR("allocation failed");
		return NULL;
	}
	buf = &buf_kmsc->base;
	switch(fourcc) {
		case V4L2_PIX_FMT_RGB24:
		case V4L2_PIX_FMT_BGR24:
			fourcc = FOURCC('R','G','2','4');
			break;
		case V4L2_PIX_FMT_BGR32:
		case V4L2_PIX_FMT_RGB32:
			fourcc = FOURCC('A','R','2','4');
			break;
		default:
			break;
	}
	buf->fourcc = fourcc;
	buf->width = w;
	buf->height = h;
	buf->multiplanar = true;

	buf->nbo = 1;

	if (!fourcc)
		fourcc = FOURCC('A','R','2','4');

	switch(fourcc) {
	case FOURCC('A','R','2','4'):
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 32, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);
		break;
	case FOURCC('R','G','2','4'):
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 24, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);
		break;

	case FOURCC('U','Y','V','Y'):
	case FOURCC('Y','U','Y','V'):
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 16, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);
		break;
	case FOURCC('N','V','1','2'):
                if (disp->multiplanar) {
                        buf->nbo = 2;
                        buf->bo[0] = alloc_bo(disp, 8, buf->width, buf->height,
                                        &bo_handles[0], &buf->pitches[0]);
                        buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);
                        buf->bo[1] = alloc_bo(disp, 16, buf->width/2, buf->height/2,
                                        &bo_handles[1], &buf->pitches[1]);
                        buf->fd[1] = omap_bo_dmabuf(buf->bo[1]);
                } else {
                        buf->nbo = 1;
                        buf->bo[0] = alloc_bo(disp, 8, buf->width, buf->height * 3 / 2,
                                        &bo_handles[0], &buf->pitches[0]);
                        buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);
                        bo_handles[1] = bo_handles[0];
                        buf->pitches[1] = buf->pitches[0];
                        offsets[1] = buf->width * buf->height;
                        buf->multiplanar = false;
                }
                break;
	case FOURCC('I','4','2','0'):
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 8, buf->width, (buf->height + buf->height/2),
				&bo_handles[0], &buf->pitches[0]);
		break;
	default:
		ERROR("invalid format: 0x%08x", fourcc);
		goto fail;
	}

	buf_kmsc->face_no = disp_kmsc->num_faces;
	/*
	 * EGL image for this buffer would be created by the render thread
	 * Nothing to be done here
	 */
	return buf;

fail:
	// XXX cleanup
	return NULL;
}

static struct buffer **
alloc_buffers(struct display *disp, uint32_t n,
		uint32_t fourcc, uint32_t w, uint32_t h)
{
	struct display_kmscube *disp_kmsc = to_display_kmscube(disp);
	struct buffer **bufs;
	uint32_t i = 0;

	bufs = calloc(n, sizeof(*bufs));
	if (!bufs) {
		ERROR("allocation failed");
		goto fail;
	}

	pthread_mutex_lock(&disp_kmsc->init_lock);
	for (i = 0; i < n; i++) {
		bufs[i] = alloc_buffer(disp, fourcc, w, h);
		if (!bufs[i]) {
			ERROR("allocation failed");
			goto fail;
		}
	}

	/*
	 * All buffers allocated with one call to get_vid_buffers are marked
	 * with the same face number
	 */
	disp_kmsc->num_faces++;
	disp->buf = bufs;
	pthread_mutex_unlock(&disp_kmsc->init_lock);
	return bufs;

fail:
	// XXX cleanup
	return NULL;
}


static void
free_buffers(struct display *disp, uint32_t n)
{
        uint32_t i;
        for (i = 0; i < n; i++) {
                if (disp->buf[i]) {
                        close(disp->buf[i]->fd[0]);
                        omap_bo_del(disp->buf[i]->bo[0]);
                        if(disp->multiplanar){
                                close(disp->buf[i]->fd[1]);
                                omap_bo_del(disp->buf[i]->bo[1]);
                        }
                }
        }
	free(disp->buf);
}


static struct buffer **
get_buffers(struct display *disp, uint32_t n)
{
	MSG("get_buffers not supported!");
	return NULL;
}

static struct buffer **
get_vid_buffers(struct display *disp, uint32_t n,
		uint32_t fourcc, uint32_t w, uint32_t h)
{
	return alloc_buffers(disp, n, fourcc, w, h);
}

static int
post_buffer(struct display *disp, struct buffer *buf)
{
	ERROR("post_buffer not supported!");
	return -1;
}

static int
post_vid_buffer(struct display *disp, struct buffer *buf,
		uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	struct display_kmscube *disp_kmsc = to_display_kmscube(disp);
	struct buffer_kmscube *buf_kmsc = to_buffer_kmscube(buf);
	int face;

	face = buf_kmsc->face_no;
	/*
	 * No gl_ calls here, just update the buffer to be used for the
	 * the specific face to which this buffer belongs
	 */

	pthread_mutex_lock(&disp_kmsc->lock[face]);
	disp_kmsc->current_buffers[face] = buf;
	pthread_mutex_unlock(&disp_kmsc->lock[face]);

	return 0;
}

static void *
render_thread(void *data)
{
	struct display_kmscube *disp_kmsc = to_display_kmscube(data);

	fd_set fds;
	drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};
	struct gbm_bo *bo;
	struct drm_fb *fb;
	int ret, face;
	struct gbm_bo *next_bo;
	int waiting_for_flip = 1;

	if (init_gbm(disp_kmsc)) {
		ERROR("couldn't init gbm");
		return NULL;
	}
	if (init_gl(disp_kmsc)) {
		ERROR("couldn't init gl(es)");
		return NULL;
	}

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(disp_kmsc->gl.display, disp_kmsc->gl.surface);
	bo = gbm_surface_lock_front_buffer(disp_kmsc->gbm.surface);
	fb = drm_fb_get_from_bo(disp_kmsc, bo);

	/* set mode: */
	ret = drmModeSetCrtc(disp_kmsc->base.fd, disp_kmsc->drm.crtc_id, fb->fb_id, 0, 0,
			&disp_kmsc->drm.connector_id, 1, disp_kmsc->drm.mode);
	if (ret) {
		ERROR("failed to set mode: %s\n", strerror(errno));
		return NULL;
	}

	pthread_mutex_unlock(&disp_kmsc->init_lock);

	while(1) {

		for(face = 0; face < MAX_FACES; face++) {
			pthread_mutex_lock(&disp_kmsc->lock[face]);
		}

		FD_ZERO(&fds);
		FD_SET(disp_kmsc->base.fd, &fds);

		// Draw cube.
		draw(disp_kmsc);
		(disp_kmsc->i)++;

		eglSwapBuffers(disp_kmsc->gl.display, disp_kmsc->gl.surface);
		next_bo = gbm_surface_lock_front_buffer(disp_kmsc->gbm.surface);
		fb = drm_fb_get_from_bo(disp_kmsc, next_bo);

		for(face = 0; face < MAX_FACES; face++) {
			pthread_mutex_unlock(&disp_kmsc->lock[face]);
		}

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */

		ret = drmModePageFlip(disp_kmsc->base.fd, disp_kmsc->drm.crtc_id, fb->fb_id,
				DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
		if (ret) {
			ERROR("failed to queue page flip: %s\n", strerror(errno));
			return NULL;
		}

		while (waiting_for_flip) {
			ret = select(disp_kmsc->base.fd + 1, &fds, NULL, NULL, NULL);
			if (ret < 0) {
				ERROR("select err: %s\n", strerror(errno));
				return NULL;
			} else if (ret == 0) {
				ERROR("select timeout!\n");
				return NULL;
			} else if (FD_ISSET(0, &fds)) {
				ERROR("user interrupted!\n");
				break;
			}
			drmHandleEvent(disp_kmsc->base.fd, &evctx);
		}

		/* release last buffer to render on again: */
		gbm_surface_release_buffer(disp_kmsc->gbm.surface, bo);
		bo = next_bo;

		waiting_for_flip = 1;

	}
	return data;
}

static int
get_texture(struct display_kmscube *disp_kmsc, int face)
{

	struct buffer *buf;
	struct buffer_kmscube *buf_kmsc;
	int num_faces;

	num_faces = disp_kmsc->num_faces ? disp_kmsc->num_faces : 1;
	face = face % num_faces;
	buf = disp_kmsc->current_buffers[face];

	if(buf == NULL) {
		return 0;
	}

	buf_kmsc = to_buffer_kmscube(buf);
	if(buf_kmsc->texture_name == 0) {
		create_texture(disp_kmsc, buf);
	}

	return buf_kmsc->texture_name;
}

static int
create_texture(struct display_kmscube *disp_kmsc, struct buffer *buf)
{

	struct buffer_kmscube *buf_kmsc = to_buffer_kmscube(buf);
	// Create EGLImage and return.
	// TODO: cropping attributes when this will be supported.
	EGLint attr[20];
	bool isRGB;
	int dfd = omap_bo_dmabuf(buf->bo[0]);
	struct gbm_import_fd_data gbm_dmabuf = {
		.fd     = dfd,
		.width  = buf->width,
		.height = buf->height,
		.stride = buf->pitches[0],
		.format = GBM_FORMAT_ARGB8888
	};

	if(buf->fourcc == FOURCC('Y','U','Y','V')) {
		EGLint __attr[] = {
			EGL_WIDTH,			buf->width,
			EGL_HEIGHT,			buf->height,
			EGL_LINUX_DRM_FOURCC_EXT,	DRM_FORMAT_YUYV,
			EGL_DMA_BUF_PLANE0_FD_EXT,	dfd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,	0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT,	buf->pitches[0],
			EGL_NONE
		};
		memcpy(attr, __attr, sizeof(__attr));
		isRGB = false;
	} else if(buf->fourcc == FOURCC('U','Y','V','Y')) {
		EGLint __attr[] = {
			EGL_WIDTH,			buf->width,
			EGL_HEIGHT,			buf->height,
			EGL_LINUX_DRM_FOURCC_EXT,	DRM_FORMAT_UYVY,
			EGL_DMA_BUF_PLANE0_FD_EXT,	dfd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,	0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT,	buf->pitches[0],
			EGL_NONE
		};
		memcpy(attr, __attr, sizeof(__attr));
		isRGB = false;
	} else if(buf->fourcc == FOURCC('N','V','1','2')) {
		/*
		 * Please note that multiple dmabuf fds are not supported,
		 * and therefore, for NV12,
		 * - fd[0] must be equal to fd[1] and
		 * - pitch[0] must be equal to pitch[1]
		 * In this example, a single buffer of size (W * H * 3) / 2
		 * is allocated, and a single DMABuf fd is generated.
		 */
		EGLint __attr[] = {
			EGL_WIDTH,			buf->width,
			EGL_HEIGHT,			buf->height,
			EGL_LINUX_DRM_FOURCC_EXT,	DRM_FORMAT_NV12,
			EGL_DMA_BUF_PLANE0_FD_EXT,	dfd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,	0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT,	buf->pitches[0],
			EGL_DMA_BUF_PLANE1_FD_EXT,	dfd,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT,	0,
			EGL_DMA_BUF_PLANE1_PITCH_EXT,	buf->pitches[0],
			EGL_NONE
		};
		memcpy(attr, __attr, sizeof(__attr));
		isRGB = false;
	} else if(buf->fourcc == FOURCC('A','R','2','4')) {
		disp_kmsc->gbm.bo  = gbm_bo_import(disp_kmsc->gbm.dev, GBM_BO_IMPORT_FD, &gbm_dmabuf,
				GBM_BO_USE_SCANOUT);
		if(!disp_kmsc->gbm.bo){
			ERROR("gbm_bo_import failed\n");
			return -1;
		}

		isRGB = true;
	} else {
		return -1;
	}
	if(isRGB == false) {
		buf_kmsc->egl_img =
			disp_kmsc->gl.eglCreateImageKHR(disp_kmsc->gl.display, EGL_NO_CONTEXT,
					EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	} else {
		EGLint attrib_list = EGL_NONE;
		buf_kmsc->egl_img =
			disp_kmsc->gl.eglCreateImageKHR(disp_kmsc->gl.display, EGL_NO_CONTEXT,
					EGL_NATIVE_PIXMAP_KHR, disp_kmsc->gbm.bo, &attrib_list);
	}
	if (buf_kmsc->egl_img == EGL_NO_IMAGE_KHR) {
		ERROR("eglCreateImageKHR failed!\n");
		goto fail;
	}

	// Texture.
	glGenTextures(1, &buf_kmsc->texture_name);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buf_kmsc->texture_name);

	if (glGetError() != GL_NO_ERROR) {
		ERROR("glBindTexture!");
		goto fail;
	}

        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// Update video texture / EGL Image.
        disp_kmsc->gl.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, buf_kmsc->egl_img);

	if (glGetError() != GL_NO_ERROR) {
		ERROR("glEGLImageTargetTexture2DOES!\n");
		goto fail;
	}
	return 0;

fail:
	return -1;
}

static void
close_kmscube(struct display *disp)
{
}

void
disp_kmscube_usage(void)
{
	MSG("KMSCUBE Display Options:");
	MSG("\t--distance <float>\tset cube distance (default 8.0)");
	MSG("\t--fov <float>\tset field of vision (default 45.0)");
	MSG("\t--kmscube\tEnable display kmscube (default: disabled)");
	MSG("\t--connector <connector_id>\tset the connector ID (default: LCD)");
}

struct display *
disp_kmscube_open(int argc, char **argv)
{
	struct display_kmscube *disp_kmsc = NULL;
	struct display *disp;
	int ret, i, enabled = 0;
	float fov = 45, distance = 8, connector_id = 4;

	/* note: set args to NULL after we've parsed them so other modules know
	 * that it is already parsed (since the arg parsing is decentralized)
	 */
	for (i = 1; i < argc; i++) {
		if (!argv[i]) {
			continue;
		}
		if (!strcmp("--distance", argv[i])) {
			argv[i++] = NULL;
			if (sscanf(argv[i], "%f", &distance) != 1) {
				ERROR("invalid arg: %s", argv[i]);
				goto fail;
			}
		} else if (!strcmp("--fov", argv[i])) {
			argv[i++] = NULL;
			if (sscanf(argv[i], "%f", &fov) != 1) {
				ERROR("invalid arg: %s", argv[i]);
				goto fail;
			}
		} else if (!strcmp("--connector", argv[i])) {
			argv[i++] = NULL;
			if (sscanf(argv[i], "%f", &connector_id) != 1) {
				ERROR("invalid arg: %s", argv[i]);
				goto fail;
			}
		} else if (!strcmp("--kmscube", argv[i])) {
			enabled = 1;
		} else {
			/* ignore */
			continue;
		}
		argv[i] = NULL;
	}

	// If not explicitely enabled from command line, fail (and fallback to disp-kms).
	if (!enabled)
		goto fail;

	disp_kmsc = calloc(1, sizeof(*disp_kmsc));
	if (!disp_kmsc) {
		ERROR("allocation failed");
		goto fail;
	}
	disp_kmsc->gl.distance = distance;
	disp_kmsc->gl.fov = fov;
	disp_kmsc->user_connector_id = connector_id;
	disp = &disp_kmsc->base;

	for (i=0; i<MAX_FACES; i++) {
		pthread_mutex_init(&disp_kmsc->lock[i], NULL);
	}
	pthread_mutex_init(&disp_kmsc->init_lock, NULL);

	disp->fd = drmOpen("omapdrm", NULL);
	if (disp->fd < 0) {
		ERROR("could not open drm device: %s (%d)", strerror(errno), errno);
		goto fail;
	}

	disp->dev = omap_device_new(disp->fd);
	if (!disp->dev) {
		ERROR("couldn't create device");
		goto fail;
	}

	disp->get_buffers = get_buffers;
	disp->get_vid_buffers = get_vid_buffers;
	disp->post_buffer = post_buffer;
	disp->post_vid_buffer = post_vid_buffer;
	disp->close = close_kmscube;
	disp->disp_free_buf = free_buffers;


	if (init_drm(disp_kmsc)) {
		ERROR("couldn't init drm");
		goto fail;
	}

	disp_kmsc->drm.plane_resources = drmModeGetPlaneResources(disp->fd);
	if (!disp_kmsc->drm.plane_resources) {
		ERROR("drmModeGetPlaneResources failed: %s", strerror(errno));
		goto fail;
	}

	disp->width = 0;
	disp->height = 0;
	disp->multiplanar = false;

	/*
	 * Initialize everything from a render thread.
	 */
	pthread_mutex_lock(&disp_kmsc->init_lock);

	ret = pthread_create(&disp_kmsc->renderThread, NULL,
			render_thread, disp);
	if(ret) {
		ERROR("Failed creating Render Thread: %s", strerror(errno));
	}

	/*
	 * Wait till initialization is complete from render thread
	 */
	pthread_mutex_lock(&disp_kmsc->init_lock);
	MSG("Display initialized, Render thread created");
	pthread_mutex_unlock(&disp_kmsc->init_lock);

	return disp;

fail:
	// XXX cleanup
	return NULL;
}
