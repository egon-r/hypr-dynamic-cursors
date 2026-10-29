#include "BlurShader.hpp"

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/render/OpenGL.hpp>

#include <cstring>
#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_TEXTURE_BINDING_EXTERNAL_OES
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif

/*
The vertex stage only rasterizes the coverage quad; all motion blur logic lives in the
fragment stage. uv per corner is the sprite mapping of the current position, so the
interpolated v_coord is exactly the uv the sharp cursor draw would use for that fragment.
*/
static constexpr auto VERT_SRC = R"(#version 300 es
uniform mat3 proj;

in vec2 pos;
in vec2 texcoord;

out vec2 v_coord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_coord = texcoord;
}
)";

/*
Mirrors the native `motionBlurSample`: walks the sprite along the displacement,
skips samples outside the sprite, and averages the (premultiplied) texture content.

sourceUV maps sprite uv to raw texture uv: it is the inverse of the projection of the
texture's m_transform, applied per sample after the sprite-space culling, not to the
displacement or the coverage quad. opaque forces the sampled alpha to 1 for RGBX
sources whose alpha channel carries no content. This source is also the template for
the GL_TEXTURE_EXTERNAL_OES variant, generated at first use by declaring the OES
extension directly after the #version line and swapping only the sampler declaration.
*/
static constexpr auto FRAG_SRC = R"(#version 300 es
precision highp float;

in vec2 v_coord;

uniform sampler2D tex;
uniform vec2 deltaUV;
uniform int samples;
uniform bool opaque;
uniform mat3 sourceUV;

layout(location = 0) out vec4 fragColor;

void main() {
    vec4 accumulated = vec4(0.0);

    for (int i = 0; i < 64; ++i) {
        if (i >= samples)
            break;

        float t = float(i) / float(samples);
        vec2  uv = v_coord + deltaUV * t;

        if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0)
            continue;

        vec4 c = texture(tex, (sourceUV * vec3(uv, 1.0)).xy);
        if (opaque)
            c.a = 1.0;

        accumulated += c;
    }

    if (accumulated.a == 0.0)
        fragColor = vec4(0.0);
    else
        fragColor = accumulated / float(samples);
}
)";

namespace {
    struct SBlurProgram {
        GLuint program = 0;
        GLint  uProj = -1, uTex = -1, uDeltaUV = -1, uSamples = -1, uOpaque = -1, uSourceUV = -1;
        GLuint vao = 0, vbo = 0;
        bool   failed = false;
    };

    GLuint compile(GLenum type, const char* src) {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[1024];
            glGetShaderInfoLog(shader, 1024, nullptr, log);
            Log::logger->log(Log::ERR, "[dynamic-cursors] failed to compile blur shader: {}", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    /*
     Builds the GL_TEXTURE_EXTERNAL_OES variant out of FRAG_SRC: EGLImage textures can only
     be sampled through samplerExternalOES, whose extension must be declared directly after
     the #version line. Nothing else differs from the plain variant.
    */
    bool buildExternalFragSrc(std::string& out) {
        static constexpr std::string_view VERSION    = "#version 300 es";
        static constexpr std::string_view EXTENSION  = "\n#extension GL_OES_EGL_image_external_essl3 : require";
        static constexpr std::string_view SAMPLER    = "uniform sampler2D tex;";
        static constexpr std::string_view EXTSAMPLER = "uniform samplerExternalOES tex;";

        out = FRAG_SRC;

        if (!out.starts_with(VERSION)) {
            Log::logger->log(Log::ERR, "[dynamic-cursors] blur shader: unexpected #version line, cannot build external variant");
            return false;
        }
        const auto sampler = out.find(SAMPLER);
        if (sampler == std::string::npos) {
            Log::logger->log(Log::ERR, "[dynamic-cursors] blur shader: sampler declaration not found, cannot build external variant");
            return false;
        }

        out.insert(VERSION.size(), EXTENSION);
        out.replace(sampler + EXTENSION.size(), SAMPLER.size(), EXTSAMPLER);
        return true;
    }

    bool ensureProgram(SBlurProgram& p, bool external) {
        if (p.program != 0)
            return true;
        if (p.failed)
            return false;

        std::string fragStorage;
        const char* fragSrc = FRAG_SRC;
        if (external) {
            if (!buildExternalFragSrc(fragStorage)) {
                p.failed = true;
                return false;
            }
            fragSrc = fragStorage.c_str();
        }

        const auto VERT = compile(GL_VERTEX_SHADER, VERT_SRC);
        const auto FRAG = compile(GL_FRAGMENT_SHADER, fragSrc);
        if (VERT == 0 || FRAG == 0) {
            if (VERT != 0)
                glDeleteShader(VERT);
            if (FRAG != 0)
                glDeleteShader(FRAG);
            p.failed = true;
            return false;
        }

        p.program = glCreateProgram();
        // bind the attribute locations before linking, they are fixed in the vao setup
        glBindAttribLocation(p.program, 0, "pos");
        glBindAttribLocation(p.program, 1, "texcoord");
        glAttachShader(p.program, VERT);
        glAttachShader(p.program, FRAG);
        glLinkProgram(p.program);
        glDeleteShader(VERT);
        glDeleteShader(FRAG);

        GLint ok = GL_FALSE;
        glGetProgramiv(p.program, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[1024];
            glGetProgramInfoLog(p.program, 1024, nullptr, log);
            Log::logger->log(Log::ERR, "[dynamic-cursors] failed to link blur shader: {}", log);
            glDeleteProgram(p.program);
            p.program = 0;
            p.failed  = true;
            return false;
        }

        p.uProj     = glGetUniformLocation(p.program, "proj");
        p.uTex      = glGetUniformLocation(p.program, "tex");
        p.uDeltaUV  = glGetUniformLocation(p.program, "deltaUV");
        p.uSamples  = glGetUniformLocation(p.program, "samples");
        p.uOpaque   = glGetUniformLocation(p.program, "opaque");
        p.uSourceUV = glGetUniformLocation(p.program, "sourceUV");

        glGenVertexArrays(1, &p.vao);
        glGenBuffers(1, &p.vbo);
        glBindVertexArray(p.vao);
        glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
        // 4 corners, pos.xy + texcoord.xy each; storage is allocated once here,
        // draws only update it with glBufferSubData
        glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return true;
    }

    static SBlurProgram         program;    // sampler2D variant, RGBA/RGBX sources
    static SBlurProgram         programExt; // samplerExternalOES variant, EXTERNAL (EGLImage) sources
    static GLuint               blurFramebuffer = 0;
    static SP<Render::ITexture> blurTexture;
    static bool                 sizeWarned = false;
} // namespace

bool drawCursorBlurred(const SCursorBlurDraw& draw) {
    // supported sources: plain RGBA/RGBX 2D textures and EXTERNAL (EGLImage) textures;
    // anything else keeps the native sharp cursor path.
    GLenum target   = GL_TEXTURE_2D;
    bool   external = false;
    bool   opaque   = false;
    switch (draw.tex->m_type) {
        case Render::TEXTURE_RGBA: break;
        case Render::TEXTURE_RGBX:
            opaque = true; // no alpha channel in the content, force it in the shader
            break;
        case Render::TEXTURE_EXTERNAL:
            external = true;
            target   = GL_TEXTURE_EXTERNAL_OES;
            break;
        default: return false;
    }

    const auto& DAMAGE = g_pHyprRenderer->m_renderData.damage;
    if (DAMAGE.empty())
        return true;

    struct SState {
        GLint     shader, vao, buffer, active, texture, sourceMin, sourceMag, framebuffer, readFramebuffer;
        GLint     texture2D;
        GLint     viewport[4];
        GLfloat   clearColor[4];
        GLint     scissor[4], srcRGB, dstRGB, srcAlpha, dstAlpha, equationRGB, equationAlpha;
        GLboolean blendEnabled, scissorEnabled;
        GLenum    target         = GL_TEXTURE_2D;
        GLuint    source         = 0;
        bool      filtersChanged = false;

        SState(GLuint sourceTexture, GLenum sourceTarget) : target(sourceTarget), source(sourceTexture) {
            glGetIntegerv(GL_CURRENT_PROGRAM, &shader);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2D);
            glGetIntegerv(target == GL_TEXTURE_EXTERNAL_OES ? GL_TEXTURE_BINDING_EXTERNAL_OES : GL_TEXTURE_BINDING_2D, &texture);
            glBindTexture(target, source);
            glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &sourceMin);
            glGetTexParameteriv(target, GL_TEXTURE_MAG_FILTER, &sourceMag);
            glGetIntegerv(GL_SCISSOR_BOX, scissor);
            glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
            glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
            glGetIntegerv(GL_BLEND_EQUATION_RGB, &equationRGB);
            glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &equationAlpha);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
            glGetIntegerv(GL_VIEWPORT, viewport);
            glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
            blendEnabled   = glIsEnabled(GL_BLEND);
            scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        }
        ~SState() {
            if (filtersChanged) {
                // the pass itself rebinds unit 0 (texture allocation, program setup), so
                // rebind the source before restoring its params, then the caller's binding
                glBindTexture(target, source);
                glTexParameteri(target, GL_TEXTURE_MIN_FILTER, sourceMin);
                glTexParameteri(target, GL_TEXTURE_MAG_FILTER, sourceMag);
            }
            glBindTexture(target, texture);
            if (target != GL_TEXTURE_2D)
                glBindTexture(GL_TEXTURE_2D, texture2D);
            glActiveTexture(active);
            glUseProgram(shader);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
            glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
            glBlendEquationSeparate(equationRGB, equationAlpha);
            if (blendEnabled)
                glEnable(GL_BLEND);
            else
                glDisable(GL_BLEND);
            if (scissorEnabled)
                glEnable(GL_SCISSOR_TEST);
            else
                glDisable(GL_SCISSOR_TEST);
        }
    };

    const int width  = (static_cast<int>(draw.quad.w) + 63) & ~63;
    const int height = (static_cast<int>(draw.quad.h) + 63) & ~63;
    {
        SState state(draw.tex->m_texID, target);

        auto& blurProgram = external ? programExt : program;
        if (!ensureProgram(blurProgram, external))
            return false;

        if (!blurTexture) {
            blurTexture = g_pHyprRenderer->createTexture();
            glGenFramebuffers(1, &blurFramebuffer);
        }
        if (blurTexture->m_size.x < width || blurTexture->m_size.y < height) {
            GLint maxTextureSize = 0;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
            if (width > maxTextureSize || height > maxTextureSize) {
                if (!sizeWarned) {
                    Log::logger->log(Log::ERR, "[dynamic-cursors] blur fbo {}x{} exceeds GL_MAX_TEXTURE_SIZE {}, falling back to sharp cursor", width, height, maxTextureSize);
                    sizeWarned = true;
                }
                return false;
            }
            blurTexture->allocate({std::max(width, static_cast<int>(blurTexture->m_size.x)), std::max(height, static_cast<int>(blurTexture->m_size.y))});
            blurTexture->bind();
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, blurTexture->m_size.x, blurTexture->m_size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            blurTexture->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            blurTexture->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            blurTexture->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            blurTexture->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, blurFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTexture->m_texID, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        glViewport(0, 0, blurTexture->m_size.x, blurTexture->m_size.y);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(blurProgram.program);
        const Mat3x3 localProjection(
            std::array<float, 9>{static_cast<float>(2.0 / blurTexture->m_size.x), 0.F, -1.F, 0.F, static_cast<float>(2.0 / blurTexture->m_size.y), -1.F, 0.F, 0.F, 1.F});
        auto projection = localProjection.copy().transpose().getMatrix();
        glUniformMatrix3fv(blurProgram.uProj, 1, GL_FALSE, projection.data());
        glUniform1i(blurProgram.uTex, 0);
        glUniform2f(blurProgram.uDeltaUV, draw.deltaUV.x, draw.deltaUV.y);
        glUniform1i(blurProgram.uSamples, std::clamp(draw.samples, 1, 64));
        glUniform1i(blurProgram.uOpaque, opaque ? 1 : 0);

        // sprite uv -> raw texture uv: projectBox projects raw uv through the texture's
        // eTransform into sprite space, so the sampler needs its inverse. The transform is
        // an orthogonal 90 deg step, its inverse is the transposed 2x2 with -R^T * t as
        // translation.
        const Mat3x3 fwd = Mat3x3::identity().projectBox(CBox{0, 0, 1, 1}, draw.tex->m_transform);
        const auto   m   = fwd.getMatrix();
        const Mat3x3 sourceUV(std::array<float, 9>{m[0], m[3], -(m[0] * m[2] + m[3] * m[5]), m[1], m[4], -(m[1] * m[2] + m[4] * m[5]), 0.F, 0.F, 1.F});
        auto         sourceUVMatrix = sourceUV.copy().transpose().getMatrix();
        glUniformMatrix3fv(blurProgram.uSourceUV, 1, GL_FALSE, sourceUVMatrix.data());

        glBindTexture(target, draw.tex->m_texID);
        const GLint minFilter = draw.nearest ? GL_NEAREST : static_cast<GLint>(draw.tex->minFilter);
        const GLint magFilter = draw.nearest ? GL_NEAREST : static_cast<GLint>(draw.tex->magFilter);
        if (minFilter != state.sourceMin) {
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
            state.filtersChanged = true;
        }
        if (magFilter != state.sourceMag) {
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
            state.filtersChanged = true;
        }
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        // strip order TL, BL, TR, BR
        const Vector2D CORNERS[4] = {
            {draw.quad.x, draw.quad.y}, {draw.quad.x, draw.quad.y + draw.quad.h}, {draw.quad.x + draw.quad.w, draw.quad.y}, {draw.quad.x + draw.quad.w, draw.quad.y + draw.quad.h}};
        std::array<float, 16> verts = {};
        for (size_t i = 0; i < 4; ++i) {
            verts[i * 4 + 0] = CORNERS[i].x - draw.quad.x;
            verts[i * 4 + 1] = CORNERS[i].y - draw.quad.y;
            verts[i * 4 + 2] = draw.uvs[i].x;
            verts[i * 4 + 3] = draw.uvs[i].y;
        }

        glBindVertexArray(blurProgram.vao);
        glBindBuffer(GL_ARRAY_BUFFER, blurProgram.vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts.data());

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    // The smear is already transformed. The native pass applies the same color
    // management as a sharp cursor, including HDR and ICC output conversion.
    blurTexture->m_imageDescription                  = draw.tex->m_imageDescription ? draw.tex->m_imageDescription : NColorManagement::getDefaultImageDescription();
    const bool nearest                               = g_pHyprRenderer->m_renderData.useNearestNeighbor;
    g_pHyprRenderer->m_renderData.useNearestNeighbor = draw.nearest;
    g_pHyprRenderer->draw(CTexPassElement::SRenderData{.tex = blurTexture, .box = CBox{draw.quad.pos(), blurTexture->m_size}}, DAMAGE);
    g_pHyprRenderer->m_renderData.useNearestNeighbor = nearest;

    return true;
}

void destroyCursorBlurRenderer() {
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();
    blurTexture.reset();
    if (blurFramebuffer)
        glDeleteFramebuffers(1, &blurFramebuffer);
    blurFramebuffer = 0;
    // every cached program variant with its vao/vbo, each exactly once
    for (auto* p : {&program, &programExt}) {
        if (p->vbo)
            glDeleteBuffers(1, &p->vbo);
        if (p->vao)
            glDeleteVertexArrays(1, &p->vao);
        if (p->program)
            glDeleteProgram(p->program);
        *p = {};
    }
    sizeWarned = false;
}
