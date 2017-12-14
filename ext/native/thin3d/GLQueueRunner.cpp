#include "GLQueueRunner.h"
#include "GLRenderManager.h"
#include "base/logging.h"
#include "gfx/gl_common.h"
#include "gfx/gl_debug_log.h"
#include "gfx_es2/gpu_features.h"
#include "math/dataconv.h"

#define TEXCACHE_NAME_CACHE_SIZE 16

// Workaround for Retroarch. Simply declare
//   extern GLuint g_defaultFBO;
// and set is as appropriate. Can adjust the variables in ext/native/base/display.h as
// appropriate.
GLuint g_defaultFBO = 0;

void GLQueueRunner::CreateDeviceObjects() {
	glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropyLevel_);
	glGenVertexArrays(1, &globalVAO_);
}

void GLQueueRunner::DestroyDeviceObjects() {
	if (!nameCache_.empty()) {
		glDeleteTextures((GLsizei)nameCache_.size(), &nameCache_[0]);
		nameCache_.clear();
	}
	glDeleteVertexArrays(1, &globalVAO_);
}

void GLQueueRunner::RunInitSteps(const std::vector<GLRInitStep> &steps) {
	glActiveTexture(GL_TEXTURE0);
	GLuint boundTexture = (GLuint)-1;

	for (int i = 0; i < steps.size(); i++) {
		const GLRInitStep &step = steps[i];
		switch (step.stepType) {
		case GLRInitStepType::CREATE_TEXTURE:
		{
			GLRTexture *tex = step.create_texture.texture;
			glGenTextures(1, &tex->texture);
			glBindTexture(tex->target, tex->texture);
			boundTexture = tex->texture;
			break;
		}
		case GLRInitStepType::CREATE_BUFFER:
		{
			GLRBuffer *buffer = step.create_buffer.buffer;
			glGenBuffers(1, &buffer->buffer);
			glBindBuffer(buffer->target_, buffer->buffer);
			glBufferData(buffer->target_, step.create_buffer.size, nullptr, step.create_buffer.usage);
			break;
		}
		case GLRInitStepType::BUFFER_SUBDATA:
		{
			GLRBuffer *buffer = step.buffer_subdata.buffer;
			glBindBuffer(GL_ARRAY_BUFFER, buffer->buffer);
			glBufferSubData(GL_ARRAY_BUFFER, step.buffer_subdata.offset, step.buffer_subdata.size, step.buffer_subdata.data);
			if (step.buffer_subdata.deleteData)
				delete[] step.buffer_subdata.data;
			break;
		}
		case GLRInitStepType::CREATE_PROGRAM:
		{
			GLRProgram *program = step.create_program.program;
			program->program = glCreateProgram();
			_assert_msg_(G3D, step.create_program.num_shaders > 0, "Can't create a program with zero shaders");
			for (int i = 0; i < step.create_program.num_shaders; i++) {
				_dbg_assert_msg_(G3D, step.create_program.shaders[i]->shader, "Can't create a program with a null shader");
				glAttachShader(program->program, step.create_program.shaders[i]->shader);
			}

			for (auto iter : program->semantics_) {
				glBindAttribLocation(program->program, iter.location, iter.attrib);
			}

#if !defined(USING_GLES2)
			if (step.create_program.support_dual_source) {
				// Dual source alpha
				glBindFragDataLocationIndexed(program->program, 0, 0, "fragColor0");
				glBindFragDataLocationIndexed(program->program, 0, 1, "fragColor1");
			} else if (gl_extensions.VersionGEThan(3, 3, 0)) {
				glBindFragDataLocation(program->program, 0, "fragColor0");
			}
#elif !defined(IOS)
			if (gl_extensions.GLES3) {
				if (gstate_c.featureFlags & GPU_SUPPORTS_DUALSOURCE_BLEND) {
					glBindFragDataLocationIndexedEXT(program->program, 0, 0, "fragColor0");
					glBindFragDataLocationIndexedEXT(program->program, 0, 1, "fragColor1");
				}
			}
#endif
			glLinkProgram(program->program);

			GLint linkStatus = GL_FALSE;
			glGetProgramiv(program->program, GL_LINK_STATUS, &linkStatus);
			if (linkStatus != GL_TRUE) {
				GLint bufLength = 0;
				glGetProgramiv(program->program, GL_INFO_LOG_LENGTH, &bufLength);
				if (bufLength) {
					char* buf = new char[bufLength];
					glGetProgramInfoLog(program->program, bufLength, NULL, buf);
					ELOG("Could not link program:\n %s", buf);
					// We've thrown out the source at this point. Might want to do something about that.
#ifdef _WIN32
					OutputDebugStringUTF8(buf);
#endif
					delete[] buf;
				} else {
					ELOG("Could not link program with %d shaders for unknown reason:", step.create_program.num_shaders);
				}
				break;
			}

			glUseProgram(program->program);

			// Query all the uniforms.
			for (int i = 0; i < program->queries_.size(); i++) {
				auto &x = program->queries_[i];
				assert(x.name);
				*x.dest = glGetUniformLocation(program->program, x.name);
			}

			// Run initializers.
			for (int i = 0; i < program->initialize_.size(); i++) {
				auto &init = program->initialize_[i];
				GLint uniform = *init.uniform;
				if (uniform != -1) {
					switch (init.type) {
					case 0:
						glUniform1i(uniform, init.value);
					}
				}
			}
		}
			break;
		case GLRInitStepType::CREATE_SHADER:
		{
			GLuint shader = glCreateShader(step.create_shader.stage);
			step.create_shader.shader->shader = shader;
			// language_ = language;
			const char *code = step.create_shader.code;
			glShaderSource(shader, 1, &code, nullptr);
			delete[] code;
			glCompileShader(shader);
			GLint success = 0;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
			if (!success) {
#define MAX_INFO_LOG_SIZE 2048
				GLchar infoLog[MAX_INFO_LOG_SIZE];
				GLsizei len = 0;
				glGetShaderInfoLog(shader, MAX_INFO_LOG_SIZE, &len, infoLog);
				infoLog[len] = '\0';
				glDeleteShader(shader);
				shader = 0;
				ILOG("%s Shader compile error:\n%s", step.create_shader.stage == GL_FRAGMENT_SHADER ? "Fragment" : "Vertex", infoLog);
				step.create_shader.shader->valid = false;
			}
			step.create_shader.shader->valid = true;
			break;
		}
		case GLRInitStepType::CREATE_INPUT_LAYOUT:
		{
			GLRInputLayout *layout = step.create_input_layout.inputLayout;
			// Nothing to do unless we want to create vertexbuffer objects (GL 4.5)
			break;
		}
		case GLRInitStepType::CREATE_FRAMEBUFFER:
		{
			boundTexture = (GLuint)-1;
			InitCreateFramebuffer(step);
			break;
		}
		case GLRInitStepType::TEXTURE_SUBDATA:
			break;
		case GLRInitStepType::TEXTURE_IMAGE:
		{
			GLRTexture *tex = step.texture_image.texture;
			CHECK_GL_ERROR_IF_DEBUG();
			if (boundTexture != step.texture_image.texture->texture) {
				glBindTexture(step.texture_image.texture->target, step.texture_image.texture->texture);
				boundTexture = step.texture_image.texture->texture;
			}
			glTexImage2D(tex->target, step.texture_image.level, step.texture_image.internalFormat, step.texture_image.width, step.texture_image.height, 0, step.texture_image.format, step.texture_image.type, step.texture_image.data);
			delete[] step.texture_image.data;
			CHECK_GL_ERROR_IF_DEBUG();
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, step.texture_image.linearFilter ? GL_LINEAR : GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, step.texture_image.linearFilter ? GL_LINEAR : GL_NEAREST);
			break;
		}
		default:
			Crash();
			break;
		}
	}
}

void GLQueueRunner::InitCreateFramebuffer(const GLRInitStep &step) {
	GLRFramebuffer *fbo = step.create_framebuffer.framebuffer;

#ifndef USING_GLES2
	if (!gl_extensions.ARB_framebuffer_object && gl_extensions.EXT_framebuffer_object) {
		fbo_ext_create(step);
	} else if (!gl_extensions.ARB_framebuffer_object) {
		return;
	}
	// If GLES2, we have basic FBO support and can just proceed.
#endif
	CHECK_GL_ERROR_IF_DEBUG();

	// Color texture is same everywhere
	glGenFramebuffers(1, &fbo->handle);
	glGenTextures(1, &fbo->color_texture);

	// Create the surfaces.
	glBindTexture(GL_TEXTURE_2D, fbo->color_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	if (gl_extensions.IsGLES) {
		if (gl_extensions.OES_packed_depth_stencil) {
			ILOG("Creating %i x %i FBO using DEPTH24_STENCIL8", fbo->width, fbo->height);
			// Standard method
			fbo->stencil_buffer = 0;
			fbo->z_buffer = 0;
			// 24-bit Z, 8-bit stencil combined
			glGenRenderbuffers(1, &fbo->z_stencil_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_stencil_buffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, fbo->width, fbo->height);

			// Bind it all together
			glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
		} else {
			ILOG("Creating %i x %i FBO using separate stencil", fbo->width, fbo->height);
			// TEGRA
			fbo->z_stencil_buffer = 0;
			// 16/24-bit Z, separate 8-bit stencil
			glGenRenderbuffers(1, &fbo->z_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_buffer);
			// Don't forget to make sure fbo_standard_z_depth() matches.
			glRenderbufferStorage(GL_RENDERBUFFER, gl_extensions.OES_depth24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16, fbo->width, fbo->height);

			// 8-bit stencil buffer
			glGenRenderbuffers(1, &fbo->stencil_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->stencil_buffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, fbo->width, fbo->height);

			// Bind it all together
			glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_buffer);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->stencil_buffer);
		}
	} else {
		fbo->stencil_buffer = 0;
		fbo->z_buffer = 0;
		// 24-bit Z, 8-bit stencil
		glGenRenderbuffers(1, &fbo->z_stencil_buffer);
		glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_stencil_buffer);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fbo->width, fbo->height);

		// Bind it all together
		glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
	}

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	switch (status) {
	case GL_FRAMEBUFFER_COMPLETE:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}

	// Unbind state we don't need
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	CHECK_GL_ERROR_IF_DEBUG();

	currentDrawHandle_ = fbo->handle;
	currentReadHandle_ = fbo->handle;
}

void GLQueueRunner::RunSteps(const std::vector<GLRStep *> &steps) {
	for (int i = 0; i < steps.size(); i++) {
		const GLRStep &step = *steps[i];
		switch (step.stepType) {
		case GLRStepType::RENDER:
			PerformRenderPass(step);
			break;
		case GLRStepType::COPY:
			PerformCopy(step);
			break;
		case GLRStepType::BLIT:
			PerformBlit(step);
			break;
		case GLRStepType::READBACK:
			PerformReadback(step);
			break;
		case GLRStepType::READBACK_IMAGE:
			PerformReadbackImage(step);
			break;
		default:
			Crash();
			break;
		}
		delete steps[i];
	}
}

void GLQueueRunner::LogSteps(const std::vector<GLRStep *> &steps) {

}


void GLQueueRunner::PerformBlit(const GLRStep &step) {
	/*
	// Without FBO_ARB / GLES3, this will collide with bind_for_read, but there's nothing
	// in ES 2.0 that actually separate them anyway of course, so doesn't matter.
	fbo_bind_fb_target(false, dst->handle);
	fbo_bind_fb_target(true, src->handle);
	if (gl_extensions.GLES3 || gl_extensions.ARB_framebuffer_object) {
	glBlitFramebuffer(srcX1, srcY1, srcX2, srcY2, dstX1, dstY1, dstX2, dstY2, aspect, linearFilter == FB_BLIT_LINEAR ? GL_LINEAR : GL_NEAREST);
	CHECK_GL_ERROR_IF_DEBUG();
	#if defined(USING_GLES2) && defined(__ANDROID__)  // We only support this extension on Android, it's not even available on PC.
	return true;
	} else if (gl_extensions.NV_framebuffer_blit) {
	glBlitFramebufferNV(srcX1, srcY1, srcX2, srcY2, dstX1, dstY1, dstX2, dstY2, aspect, linearFilter == FB_BLIT_LINEAR ? GL_LINEAR : GL_NEAREST);
	CHECK_GL_ERROR_IF_DEBUG();
	#endif // defined(USING_GLES2) && defined(__ANDROID__)
	return true;
	} else {
	return false;
	}*/

}

void GLQueueRunner::PerformRenderPass(const GLRStep &step) {
	// Don't execute empty renderpasses.
	if (step.commands.empty()) {
		// Nothing to do.
		return;
	}

	PerformBindFramebufferAsRenderTarget(step);

	glEnable(GL_SCISSOR_TEST);

	/*
#ifndef USING_GLES2
	if (g_Config.iInternalResolution == 0) {
		glLineWidth(std::max(1, (int)(renderWidth_ / 480)));
		glPointSize(std::max(1.0f, (float)(renderWidth_ / 480.f)));
	} else {
		glLineWidth(g_Config.iInternalResolution);
		glPointSize((float)g_Config.iInternalResolution);
	}
#endif
	*/

	glBindVertexArray(globalVAO_);

	GLRFramebuffer *fb = step.render.framebuffer;
	GLRProgram *curProgram = nullptr;
	GLint activeTexture = GL_TEXTURE0;
	glActiveTexture(activeTexture);

	int attrMask = 0;

	// State filtering tracking.
	GLuint curArrayBuffer = (GLuint)-1;
	GLuint curElemArrayBuffer = (GLuint)-1;

	auto &commands = step.commands;
	for (const auto &c : commands) {
		switch (c.cmd) {
		case GLRRenderCommand::DEPTH:
			if (c.depth.enabled) {
				glEnable(GL_DEPTH_TEST);
				glDepthMask(c.depth.write);
				glDepthFunc(c.depth.func);
			} else {
				glDisable(GL_DEPTH_TEST);
			}
			break;
		case GLRRenderCommand::BLEND:
			if (c.blend.enabled) {
				glEnable(GL_BLEND);
				glBlendEquationSeparate(c.blend.funcColor, c.blend.funcAlpha);
				glBlendFuncSeparate(c.blend.srcColor, c.blend.dstColor, c.blend.srcAlpha, c.blend.dstAlpha);
			} else {
				glDisable(GL_BLEND);
			}
			glColorMask(c.blend.mask & 1, (c.blend.mask >> 1) & 1, (c.blend.mask >> 2) & 1, (c.blend.mask >> 3) & 1);
			break;
		case GLRRenderCommand::CLEAR:
			glDisable(GL_SCISSOR_TEST);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			if (c.clear.clearMask & GL_COLOR_BUFFER_BIT) {
				float color[4];
				Uint8x4ToFloat4(color, c.clear.clearColor);
				glClearColor(color[0], color[1], color[2], color[3]);
			}
			if (c.clear.clearMask & GL_DEPTH_BUFFER_BIT) {
#if defined(USING_GLES2)
				glClearDepthf(c.clear.clearZ);
#else
				glClearDepth(c.clear.clearZ);
#endif
			}
			if (c.clear.clearMask & GL_STENCIL_BUFFER_BIT) {
				glClearStencil(c.clear.clearStencil);
			}
			glClear(c.clear.clearMask);
			glEnable(GL_SCISSOR_TEST);
			break;
		case GLRRenderCommand::BLENDCOLOR:
			glBlendColor(c.blendColor.color[0], c.blendColor.color[1], c.blendColor.color[2], c.blendColor.color[3]);
			break;
		case GLRRenderCommand::VIEWPORT:
		{
			float y = c.viewport.vp.y;
			if (!curFramebuffer_)
				y = curFBHeight_ - y - c.viewport.vp.h;

			// TODO: Support FP viewports through glViewportArrays
			glViewport((GLint)c.viewport.vp.x, (GLint)y, (GLsizei)c.viewport.vp.w, (GLsizei)c.viewport.vp.h);
			glDepthRange(c.viewport.vp.minZ, c.viewport.vp.maxZ);
			break;
		}
		case GLRRenderCommand::SCISSOR:
		{
			int y = c.scissor.rc.y;
			if (!curFramebuffer_)
				y = curFBHeight_ - y - c.scissor.rc.h;
			glScissor(c.scissor.rc.x, y, c.scissor.rc.w, c.scissor.rc.h);
			break;
		}
		case GLRRenderCommand::UNIFORM4F:
		{
			int loc = c.uniform4.loc ? *c.uniform4.loc : -1;
			if (c.uniform4.name) {
				loc = curProgram->GetUniformLoc(c.uniform4.name);
			}
			if (loc >= 0) {
				switch (c.uniform4.count) {
				case 1:
					glUniform1f(loc, c.uniform4.v[0]);
					break;
				case 2:
					glUniform2fv(loc, 1, c.uniform4.v);
					break;
				case 3:
					glUniform3fv(loc, 1, c.uniform4.v);
					break;
				case 4:
					glUniform4fv(loc, 1, c.uniform4.v);
					break;
				}
			}
			break;
		}
		case GLRRenderCommand::UNIFORM4I:
		{
			int loc = c.uniform4.loc ? *c.uniform4.loc : -1;
			if (c.uniform4.name) {
				loc = curProgram->GetUniformLoc(c.uniform4.name);
			}
			if (loc >= 0) {
				switch (c.uniform4.count) {
				case 1:
					glUniform1iv(loc, 1, (GLint *)&c.uniform4.v[0]);
					break;
				case 2:
					glUniform2iv(loc, 1, (GLint *)c.uniform4.v);
					break;
				case 3:
					glUniform3iv(loc, 1, (GLint *)c.uniform4.v);
					break;
				case 4:
					glUniform4iv(loc, 1, (GLint *)c.uniform4.v);
					break;
				}
			}
			break;
		}
		case GLRRenderCommand::UNIFORMMATRIX:
		{
			int loc = c.uniform4.loc ? *c.uniform4.loc : -1;
			if (c.uniform4.name) {
				loc = curProgram->GetUniformLoc(c.uniform4.name);
			}
			if (loc >= 0) {
				glUniformMatrix4fv(loc, 1, false, c.uniformMatrix4.m);
			}
			break;
		}
		case GLRRenderCommand::STENCILFUNC:
			if (c.stencilFunc.enabled) {
				glEnable(GL_STENCIL_TEST);
				glStencilFunc(c.stencilFunc.func, c.stencilFunc.ref, c.stencilFunc.compareMask);
			} else {
				glDisable(GL_STENCIL_TEST);
			}
			break;
		case GLRRenderCommand::STENCILOP:
			glStencilOp(c.stencilOp.sFail, c.stencilOp.zFail, c.stencilOp.pass);
			glStencilMask(c.stencilOp.writeMask);
			break;
		case GLRRenderCommand::BINDTEXTURE:
		{
			GLint slot = c.texture.slot;
			if (slot != activeTexture) {
				glActiveTexture(GL_TEXTURE0 + slot);
				activeTexture = slot;
			}
			if (c.texture.texture) {
				glBindTexture(c.texture.texture->target, c.texture.texture->texture);
			} else {
				glBindTexture(GL_TEXTURE_2D, 0);  // Which target? Well we only use this one anyway...
			}
			break;
		}
		case GLRRenderCommand::BIND_FB_TEXTURE:
		{
			GLint slot = c.bind_fb_texture.slot;
			if (slot != activeTexture) {
				glActiveTexture(GL_TEXTURE0 + slot);
				activeTexture = slot;
			}
			if (c.bind_fb_texture.aspect == GL_COLOR_BUFFER_BIT) {
				glBindTexture(GL_TEXTURE_2D, c.bind_fb_texture.framebuffer->color_texture);
			} else {
				// TODO: Depth texturing?
			}
			break;
		}
		case GLRRenderCommand::BINDPROGRAM:
		{
			glUseProgram(c.program.program->program);
			curProgram = c.program.program;
			break;
		}
		case GLRRenderCommand::BIND_INPUT_LAYOUT:
		{
			GLRInputLayout *layout = c.inputLayout.inputLayout;
			int enable, disable;
				enable = layout->semanticsMask_ & ~attrMask;
				disable = (~layout->semanticsMask_) & attrMask;
			for (int i = 0; i < 7; i++) {  // SEM_MAX
				if (enable & (1 << i)) {
					glEnableVertexAttribArray(i);
				}
				if (disable & (1 << i)) {
					glDisableVertexAttribArray(i);
				}
			}
			attrMask = layout->semanticsMask_;
			for (int i = 0; i < layout->entries.size(); i++) {
				auto &entry = layout->entries[i];
				glVertexAttribPointer(entry.location, entry.count, entry.type, entry.normalized, entry.stride, (const void *)(c.inputLayout.offset + entry.offset));
			}
			break;
		}
		case GLRRenderCommand::BIND_BUFFER:
		{
			if (c.bind_buffer.target == GL_ARRAY_BUFFER) {
				GLuint buf = c.bind_buffer.buffer ? c.bind_buffer.buffer->buffer : 0;
				if (buf != curArrayBuffer) {
					glBindBuffer(GL_ARRAY_BUFFER, buf);
					curArrayBuffer = buf;
				}
			} else if (c.bind_buffer.target == GL_ELEMENT_ARRAY_BUFFER) {
				GLuint buf = c.bind_buffer.buffer ? c.bind_buffer.buffer->buffer : 0;
				if (buf != curElemArrayBuffer) {
					glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf);
					curElemArrayBuffer = buf;
				}
			} else {
				GLuint buf = c.bind_buffer.buffer ? c.bind_buffer.buffer->buffer : 0;
				glBindBuffer(c.bind_buffer.target, buf);
			}
			break;
		}
		case GLRRenderCommand::GENMIPS:
			// TODO: Should we include the texture handle in the command?
			glGenerateMipmap(GL_TEXTURE_2D);
			break;
		case GLRRenderCommand::DRAW:
			glDrawArrays(c.draw.mode, c.draw.first, c.draw.count);
			break;
		case GLRRenderCommand::DRAW_INDEXED:
			if (c.drawIndexed.instances == 1) {
				glDrawElements(c.drawIndexed.mode, c.drawIndexed.count, c.drawIndexed.indexType, c.drawIndexed.indices);
			}
			break;
		case GLRRenderCommand::TEXTURESAMPLER:
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, c.textureSampler.wrapS);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, c.textureSampler.wrapT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, c.textureSampler.magFilter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, c.textureSampler.minFilter);
			if (c.textureSampler.anisotropy != 0.0f) {
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, c.textureSampler.anisotropy);
			}
			break;
		case GLRRenderCommand::TEXTURELOD:
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, c.textureLod.minLod);
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, c.textureLod.maxLod);
#ifndef USING_GLES2
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, c.textureLod.lodBias);
#endif
			break;
		case GLRRenderCommand::RASTER:
			if (c.raster.cullEnable) {
				glEnable(GL_CULL_FACE);
				glFrontFace(c.raster.frontFace);
				glCullFace(c.raster.cullFace);
			} else {
				glDisable(GL_CULL_FACE);
			}
			if (c.raster.ditherEnable) {
				glEnable(GL_DITHER);
			} else {
				glDisable(GL_DITHER);
			}
			break;
		default:
			Crash();
			break;
		}
	}

	for (int i = 0; i < 7; i++) {
		if (attrMask & (1 << i)) {
			glDisableVertexAttribArray(i);
		}
	}

	if (activeTexture != GL_TEXTURE0)
		glActiveTexture(GL_TEXTURE0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	glDisable(GL_SCISSOR_TEST);
}

void GLQueueRunner::PerformCopy(const GLRStep &step) {
	GLuint srcTex = 0;
	GLuint dstTex = 0;
	GLuint target = GL_TEXTURE_2D;

	const GLRect2D &srcRect = step.copy.srcRect;
	const GLOffset2D &dstPos = step.copy.dstPos;

	GLRFramebuffer *src = step.copy.src;
	GLRFramebuffer *dst = step.copy.src;

	int srcLevel = 0;
	int dstLevel = 0;
	int srcZ = 0;
	int dstZ = 0;
	int depth = 1;

	switch (step.copy.aspectMask) {
	case GL_COLOR_BUFFER_BIT:
		srcTex = src->color_texture;
		dstTex = dst->color_texture;
		break;
	case GL_DEPTH_BUFFER_BIT:
		_assert_msg_(G3D, false, "Depth copies not yet supported - soon");
		target = GL_RENDERBUFFER;
		/*
		srcTex = src->depth.texture;
		dstTex = src->depth.texture;
		*/
		break;
	}
#if defined(USING_GLES2)
#ifndef IOS
	glCopyImageSubDataOES(
		srcTex, target, srcLevel, srcRect.x, srcRect.y, srcZ,
		dstTex, target, dstLevel, dstPos.x, dstPos.y, dstZ,
		srcRect.w, srcRect.h, depth);
#endif
#else
	if (gl_extensions.ARB_copy_image) {
		glCopyImageSubData(
			srcTex, target, srcLevel, srcRect.x, srcRect.y, srcZ,
			dstTex, target, dstLevel, dstPos.x, dstPos.y, dstZ,
			srcRect.w, srcRect.h, depth);
	} else if (gl_extensions.NV_copy_image) {
		// Older, pre GL 4.x NVIDIA cards.
		glCopyImageSubDataNV(
			srcTex, target, srcLevel, srcRect.x, srcRect.y, srcZ,
			dstTex, target, dstLevel, dstPos.x, dstPos.y, dstZ,
			srcRect.w, srcRect.h, depth);
	}
#endif
}

void GLQueueRunner::PerformReadback(const GLRStep &pass) {

}

void GLQueueRunner::PerformReadbackImage(const GLRStep &pass) {

}

void GLQueueRunner::PerformBindFramebufferAsRenderTarget(const GLRStep &pass) {
	if (pass.render.framebuffer) {
		curFBWidth_ = pass.render.framebuffer->width;
		curFBHeight_ = pass.render.framebuffer->height;
	} else {
		curFBWidth_ = targetWidth_;
		curFBHeight_ = targetHeight_;
	}

	curFB_ = pass.render.framebuffer;
	if (curFB_) {
		// Without FBO_ARB / GLES3, this will collide with bind_for_read, but there's nothing
		// in ES 2.0 that actually separate them anyway of course, so doesn't matter.
		fbo_bind_fb_target(false, curFB_->framebuf);
	} else {
		fbo_unbind();
		// Backbuffer is now bound.
	}

	// GLenum attachments[3] = { GL_COLOR_ATTACHMENT0, GL_STENCIL_ATTACHMENT, GL_DEPTH_ATTACHMENT };
	// glInvalidateFramebuffer(GL_FRAMEBUFFER, 3, attachments);
}

void GLQueueRunner::CopyReadbackBuffer(int width, int height, Draw::DataFormat srcFormat, Draw::DataFormat destFormat, int pixelStride, uint8_t *pixels) {

}

GLuint GLQueueRunner::AllocTextureName() {
	if (nameCache_.empty()) {
		nameCache_.resize(TEXCACHE_NAME_CACHE_SIZE);
		glGenTextures(TEXCACHE_NAME_CACHE_SIZE, &nameCache_[0]);
	}
	u32 name = nameCache_.back();
	nameCache_.pop_back();
	return name;
}

// On PC, we always use GL_DEPTH24_STENCIL8. 
// On Android, we try to use what's available.

#ifndef USING_GLES2
void GLQueueRunner::fbo_ext_create(const GLRInitStep &step) {
	GLRFramebuffer *fbo = step.create_framebuffer.framebuffer;

	// Color texture is same everywhere
	glGenFramebuffersEXT(1, &fbo->handle);
	glGenTextures(1, &fbo->color_texture);

	// Create the surfaces.
	glBindTexture(GL_TEXTURE_2D, fbo->color_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	fbo->stencil_buffer = 0;
	fbo->z_buffer = 0;
	// 24-bit Z, 8-bit stencil
	glGenRenderbuffersEXT(1, &fbo->z_stencil_buffer);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_STENCIL_EXT, fbo->width, fbo->height);
	//glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, width, height);

	// Bind it all together
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo->handle);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, fbo->color_texture, 0);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);

	GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
	switch (status) {
	case GL_FRAMEBUFFER_COMPLETE_EXT:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED_EXT:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}
	// Unbind state we don't need
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	currentDrawHandle_ = fbo->handle;
	currentReadHandle_ = fbo->handle;
}
#endif

GLenum GLQueueRunner::fbo_get_fb_target(bool read, GLuint **cached) {
	bool supportsBlit = gl_extensions.ARB_framebuffer_object;
	if (gl_extensions.IsGLES) {
		supportsBlit = (gl_extensions.GLES3 || gl_extensions.NV_framebuffer_blit);
	}

	// Note: GL_FRAMEBUFFER_EXT and GL_FRAMEBUFFER have the same value, same with _NV.
	if (supportsBlit) {
		if (read) {
			*cached = &currentReadHandle_;
			return GL_READ_FRAMEBUFFER;
		} else {
			*cached = &currentDrawHandle_;
			return GL_DRAW_FRAMEBUFFER;
		}
	} else {
		*cached = &currentDrawHandle_;
		return GL_FRAMEBUFFER;
	}
}

void GLQueueRunner::fbo_bind_fb_target(bool read, GLuint name) {
	CHECK_GL_ERROR_IF_DEBUG();
	GLuint *cached;
	GLenum target = fbo_get_fb_target(read, &cached);
	if (*cached != name) {
		if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
			glBindFramebuffer(target, name);
		} else {
#ifndef USING_GLES2
			glBindFramebufferEXT(target, name);
#endif
		}
		*cached = name;
	}
	CHECK_GL_ERROR_IF_DEBUG();
}

void GLQueueRunner::fbo_unbind() {
	CHECK_GL_ERROR_IF_DEBUG();
#ifndef USING_GLES2
	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFBO);
	} else if (gl_extensions.EXT_framebuffer_object) {
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, g_defaultFBO);
	}
#else
	glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFBO);
#endif

#ifdef IOS
	bindDefaultFBO();
#endif

	currentDrawHandle_ = 0;
	currentReadHandle_ = 0;
	CHECK_GL_ERROR_IF_DEBUG();
}

GLRFramebuffer::~GLRFramebuffer() {
	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		if (handle) {
			glBindFramebuffer(GL_FRAMEBUFFER, handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFBO);
			glDeleteFramebuffers(1, &handle);
		}
		if (z_stencil_buffer)
			glDeleteRenderbuffers(1, &z_stencil_buffer);
		if (z_buffer)
			glDeleteRenderbuffers(1, &z_buffer);
		if (stencil_buffer)
			glDeleteRenderbuffers(1, &stencil_buffer);
	} else if (gl_extensions.EXT_framebuffer_object) {
#ifndef USING_GLES2
		if (handle) {
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, handle);
			glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER_EXT, 0);
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, g_defaultFBO);
			glDeleteFramebuffersEXT(1, &handle);
		}
		if (z_stencil_buffer)
			glDeleteRenderbuffers(1, &z_stencil_buffer);
		if (z_buffer)
			glDeleteRenderbuffers(1, &z_buffer);
		if (stencil_buffer)
			glDeleteRenderbuffers(1, &stencil_buffer);
#endif
	}

	glDeleteTextures(1, &color_texture);
}