#include "nanovg_gl.h"
#include "nanovg.h"
#include "nanovg_gl_shader.h"
#include "nanovg_gl_texture.h"
#include <assert.h>
#include <glad/glad.h>
#include <math.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>

static int glnvg__maxi(int a, int b) { return a > b ? a : b; }

static void glnvg__xformToMat3x4(float *m3, float *t) {
  m3[0] = t[0];
  m3[1] = t[1];
  m3[2] = 0.0f;
  m3[3] = 0.0f;
  m3[4] = t[2];
  m3[5] = t[3];
  m3[6] = 0.0f;
  m3[7] = 0.0f;
  m3[8] = t[4];
  m3[9] = t[5];
  m3[10] = 1.0f;
  m3[11] = 0.0f;
}

static NVGcolor glnvg__premulColor(NVGcolor c) {
  c.r *= c.a;
  c.g *= c.a;
  c.b *= c.a;
  return c;
}

struct GLNVGblend {
  GLenum srcRGB;
  GLenum dstRGB;
  GLenum srcAlpha;
  GLenum dstAlpha;
};

enum GLNVGcallType {
  GLNVG_NONE = 0,
  GLNVG_FILL,
  GLNVG_CONVEXFILL,
  GLNVG_STROKE,
  GLNVG_TRIANGLES,
};

struct GLNVGcall {
  int type;
  int image;
  int pathOffset;
  int pathCount;
  int triangleOffset;
  int triangleCount;
  int uniformOffset;
  GLNVGblend blendFunc;
};

struct GLNVGpath {
  int fillOffset;
  int fillCount;
  int strokeOffset;
  int strokeCount;
};

struct GLNVGfragUniforms {
  float scissorMat[12]; // matrices are actually 3 vec4s
  float paintMat[12];
  struct NVGcolor innerCol;
  struct NVGcolor outerCol;
  float scissorExt[2];
  float scissorScale[2];
  float extent[2];
  float radius;
  float feather;
  float strokeMult;
  float strokeThr;
  int texType;
  int type;
};

struct GLNVGcontext {
  GLNVGshader _shader = {};
  std::unordered_map<int, std::shared_ptr<GLNVGtexture>> _textures;
  float _view[2] = {};
  GLuint _vertBuf = {};
  GLuint _vertArr = {};
  GLuint _fragBuf = {};
  int _fragSize = {};
  int _flags = {};

private:
  // Per frame buffers
  std::list<GLNVGcall> _calls;
  std::vector<GLNVGpath> _paths;
  NVGvertex *_verts = {};
  int _nverts = {};
  int _cverts = {};
  unsigned char *_uniforms = {};
  int _cuniforms = {};
  int _nuniforms = {};

  // cached state
  GLuint _boundTexture = {};
  GLuint _stencilMask = {};
  GLenum _stencilFunc = {};
  GLint _stencilFuncRef = {};
  GLuint _stencilFuncMask = {};
  GLNVGblend _blendFunc = {};

public:
  int _dummyTex = {};

public:
  ~GLNVGcontext() {
    if (_fragBuf != 0)
      glDeleteBuffers(1, &_fragBuf);
    if (_vertArr != 0)
      glDeleteVertexArrays(1, &_vertArr);
    if (_vertBuf != 0)
      glDeleteBuffers(1, &_vertBuf);

    free(_uniforms);
  }

  void clear() {
    _nverts = 0;
    _paths.clear();
    _calls.clear();
    _nuniforms = 0;
  }

  void glnvg__checkError(const char *str) {
    GLenum err;
    if ((_flags & NVG_DEBUG) == 0)
      return;
    err = glGetError();
    if (err != GL_NO_ERROR) {
      printf("Error %08x after %s\n", err, str);
      return;
    }
  }

  bool initialize() {

    int align = 4;

    // glnvg__checkError("init");

    if (!_shader.createShader(_flags & NVG_ANTIALIAS)) {
      return 0;
    }

    // glnvg__checkError("uniform locations");
    _shader.getUniforms();

    // Create dynamic vertex array
    glGenVertexArrays(1, &_vertArr);
    glGenBuffers(1, &_vertBuf);

    // Create UBOs
    _shader.blockBind();
    glGenBuffers(1, &_fragBuf);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    _fragSize =
        sizeof(GLNVGfragUniforms) + align - sizeof(GLNVGfragUniforms) % align;

    // Some platforms does not allow to have samples to unset textures.
    // Create empty one which is bound when there's no texture specified.
    {
      auto tex = GLNVGtexture::load(1, 1, NVG_TEXTURE_ALPHA, NULL, 0);
      assert(tex);
      _textures.insert(std::make_pair(tex->id(), tex));
      _dummyTex = tex->id();
      // glnvg__renderCreateTexture(gl, NVG_TEXTURE_ALPHA, 1, 1, 0, NULL);
    }

    // glnvg__checkError("create done");

    glFinish();

    return 1;
  }

  std::shared_ptr<GLNVGtexture> glnvg__findTexture(int id) {
    auto found = _textures.find(id);
    if (found != _textures.end()) {
      return found->second;
    }
    return {};
  }

  GLNVGcall *glnvg__allocCall() {
    _calls.push_back({});
    return &_calls.back();
    // ret = &gl->_calls[gl->_ncalls++];
    // memset(ret, 0, sizeof(GLNVGcall));
    // return ret;
  }

  int glnvg__allocPaths(int n) {
    auto ret = _paths.size();
    _paths.resize(ret + n);
    return ret;
  }

  int glnvg__allocVerts(int n) {
    int ret = 0;
    if (_nverts + n > _cverts) {
      NVGvertex *verts;
      int cverts =
          glnvg__maxi(_nverts + n, 4096) + _cverts / 2; // 1.5x Overallocate
      verts = (NVGvertex *)realloc(_verts, sizeof(NVGvertex) * cverts);
      if (verts == NULL)
        return -1;
      _verts = verts;
      _cverts = cverts;
    }
    ret = _nverts;
    _nverts += n;
    return ret;
  }

  int glnvg__allocFragUniforms(int n) {
    int ret = 0, structSize = _fragSize;
    if (_nuniforms + n > _cuniforms) {
      unsigned char *uniforms;
      int cuniforms = glnvg__maxi(_nuniforms + n, 128) +
                      _cuniforms / 2; // 1.5x Overallocate
      uniforms = (unsigned char *)realloc(_uniforms, structSize * cuniforms);
      if (uniforms == NULL)
        return -1;
      _uniforms = uniforms;
      _cuniforms = cuniforms;
    }
    ret = _nuniforms * structSize;
    _nuniforms += n;
    return ret;
  }

  GLNVGpath &get_path(size_t index) { return _paths[index]; }
  NVGvertex &get_vertex(size_t index) { return _verts[index]; }
  GLNVGfragUniforms *nvg__fragUniformPtr(int i) {
    return (GLNVGfragUniforms *)&_uniforms[i];
  }

  bool glnvg__deleteTexture(int id) {
    auto found = _textures.find(id);
    if (found != _textures.end()) {
      _textures.erase(found);
      return true;
    }
    return false;
  }

  void render() {
    if (!_calls.empty()) {

      // Setup require GL state.
      _shader.use();

      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
      glFrontFace(GL_CCW);
      glEnable(GL_BLEND);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_SCISSOR_TEST);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glStencilMask(0xffffffff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
      glStencilFunc(GL_ALWAYS, 0, 0xffffffff);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, 0);
      _boundTexture = 0;
      _stencilMask = 0xffffffff;
      _stencilFunc = GL_ALWAYS;
      _stencilFuncRef = 0;
      _stencilFuncMask = 0xffffffff;
      _blendFunc.srcRGB = GL_INVALID_ENUM;
      _blendFunc.srcAlpha = GL_INVALID_ENUM;
      _blendFunc.dstRGB = GL_INVALID_ENUM;
      _blendFunc.dstAlpha = GL_INVALID_ENUM;

      // Upload ubo for frag shaders
      glBindBuffer(GL_UNIFORM_BUFFER, _fragBuf);
      glBufferData(GL_UNIFORM_BUFFER, _nuniforms * _fragSize, _uniforms,
                   GL_STREAM_DRAW);

      // Upload vertex data
      glBindVertexArray(_vertArr);
      glBindBuffer(GL_ARRAY_BUFFER, _vertBuf);
      glBufferData(GL_ARRAY_BUFFER, _nverts * sizeof(NVGvertex), _verts,
                   GL_STREAM_DRAW);
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(NVGvertex),
                            (const GLvoid *)(size_t)0);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(NVGvertex),
                            (const GLvoid *)(0 + 2 * sizeof(float)));

      // Set view and texture just once per frame.
      _shader.set_texture_and_view(0, _view);

      glBindBuffer(GL_UNIFORM_BUFFER, _fragBuf);

      for (auto &call : _calls) {
        glnvg__blendFuncSeparate(&call.blendFunc);
        if (call.type == GLNVG_FILL)
          glnvg__fill(&call);
        else if (call.type == GLNVG_CONVEXFILL)
          glnvg__convexFill(&call);
        else if (call.type == GLNVG_STROKE)
          glnvg__stroke(&call);
        else if (call.type == GLNVG_TRIANGLES)
          glnvg__triangles(&call);
      }

      glDisableVertexAttribArray(0);
      glDisableVertexAttribArray(1);
      glBindVertexArray(0);
      glDisable(GL_CULL_FACE);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glUseProgram(0);
      glnvg__bindTexture(0);
    }

    // Reset calls
    clear();
  }

private:
  void glnvg__blendFuncSeparate(const GLNVGblend *blend) {
    if ((_blendFunc.srcRGB != blend->srcRGB) ||
        (_blendFunc.dstRGB != blend->dstRGB) ||
        (_blendFunc.srcAlpha != blend->srcAlpha) ||
        (_blendFunc.dstAlpha != blend->dstAlpha)) {

      _blendFunc = *blend;
      glBlendFuncSeparate(blend->srcRGB, blend->dstRGB, blend->srcAlpha,
                          blend->dstAlpha);
    }
  }
  void glnvg__fill(GLNVGcall *call) {
    auto paths = &get_path(call->pathOffset);
    int i, npaths = call->pathCount;

    // Draw shapes
    glEnable(GL_STENCIL_TEST);
    glnvg__stencilMask(0xff);
    glnvg__stencilFunc(GL_ALWAYS, 0, 0xff);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    // set bindpoint for solid loc
    glnvg__setUniforms(call->uniformOffset, 0);
    glnvg__checkError("fill simple");

    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
    glDisable(GL_CULL_FACE);
    for (i = 0; i < npaths; i++)
      glDrawArrays(GL_TRIANGLE_FAN, paths[i].fillOffset, paths[i].fillCount);
    glEnable(GL_CULL_FACE);

    // Draw anti-aliased pixels
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glnvg__setUniforms(call->uniformOffset + _fragSize, call->image);
    glnvg__checkError("fill fill");

    if (_flags & NVG_ANTIALIAS) {
      glnvg__stencilFunc(GL_EQUAL, 0x00, 0xff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
      // Draw fringes
      for (i = 0; i < npaths; i++)
        glDrawArrays(GL_TRIANGLE_STRIP, paths[i].strokeOffset,
                     paths[i].strokeCount);
    }

    // Draw fill
    glnvg__stencilFunc(GL_NOTEQUAL, 0x0, 0xff);
    glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
    glDrawArrays(GL_TRIANGLE_STRIP, call->triangleOffset, call->triangleCount);

    glDisable(GL_STENCIL_TEST);
  }

  void glnvg__convexFill(GLNVGcall *call) {
    auto paths = &get_path(call->pathOffset);
    int i, npaths = call->pathCount;

    glnvg__setUniforms(call->uniformOffset, call->image);
    glnvg__checkError("convex fill");

    for (i = 0; i < npaths; i++) {
      glDrawArrays(GL_TRIANGLE_FAN, paths[i].fillOffset, paths[i].fillCount);
      // Draw fringes
      if (paths[i].strokeCount > 0) {
        glDrawArrays(GL_TRIANGLE_STRIP, paths[i].strokeOffset,
                     paths[i].strokeCount);
      }
    }
  }

  void glnvg__stroke(GLNVGcall *call) {
    auto paths = &get_path(call->pathOffset);
    int npaths = call->pathCount, i;

    if (_flags & NVG_STENCIL_STROKES) {

      glEnable(GL_STENCIL_TEST);
      glnvg__stencilMask(0xff);

      // Fill the stroke base without overlap
      glnvg__stencilFunc(GL_EQUAL, 0x0, 0xff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
      glnvg__setUniforms(call->uniformOffset + _fragSize, call->image);
      glnvg__checkError("stroke fill 0");
      for (i = 0; i < npaths; i++)
        glDrawArrays(GL_TRIANGLE_STRIP, paths[i].strokeOffset,
                     paths[i].strokeCount);

      // Draw anti-aliased pixels.
      glnvg__setUniforms(call->uniformOffset, call->image);
      glnvg__stencilFunc(GL_EQUAL, 0x00, 0xff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
      for (i = 0; i < npaths; i++)
        glDrawArrays(GL_TRIANGLE_STRIP, paths[i].strokeOffset,
                     paths[i].strokeCount);

      // Clear stencil buffer.
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glnvg__stencilFunc(GL_ALWAYS, 0x0, 0xff);
      glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
      glnvg__checkError("stroke fill 1");
      for (i = 0; i < npaths; i++)
        glDrawArrays(GL_TRIANGLE_STRIP, paths[i].strokeOffset,
                     paths[i].strokeCount);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

      glDisable(GL_STENCIL_TEST);

      //		glnvg__convertPaint(gl, nvg__fragUniformPtr(gl,
      // call->uniformOffset
      //+ fragSize), paint, scissor, strokeWidth, fringe, 1.0f -
      // 0.5f/255.0f);

    } else {
      glnvg__setUniforms(call->uniformOffset, call->image);
      glnvg__checkError("stroke fill");
      // Draw Strokes
      for (i = 0; i < npaths; i++)
        glDrawArrays(GL_TRIANGLE_STRIP, paths[i].strokeOffset,
                     paths[i].strokeCount);
    }
  }

  void glnvg__triangles(GLNVGcall *call) {
    glnvg__setUniforms(call->uniformOffset, call->image);
    glnvg__checkError("triangles fill");

    glDrawArrays(GL_TRIANGLES, call->triangleOffset, call->triangleCount);
  }

  void glnvg__setUniforms(int uniformOffset, int image) {
    std::shared_ptr<GLNVGtexture> tex = NULL;
    glBindBufferRange(GL_UNIFORM_BUFFER, GLNVG_FRAG_BINDING, _fragBuf,
                      uniformOffset, sizeof(GLNVGfragUniforms));

    if (image != 0) {
      tex = glnvg__findTexture(image);
    }
    // If no image is set, use empty texture
    if (tex == NULL) {
      tex = glnvg__findTexture(_dummyTex);
    }
    glnvg__bindTexture(tex ? tex->handle() : 0);
    glnvg__checkError("tex paint tex");
  }

  void glnvg__bindTexture(GLuint tex) {
    if (_boundTexture != tex) {
      _boundTexture = tex;
      glBindTexture(GL_TEXTURE_2D, tex);
    }
  }

  void glnvg__stencilMask(GLuint mask) {
    if (_stencilMask != mask) {
      _stencilMask = mask;
      glStencilMask(mask);
    }
  }

  void glnvg__stencilFunc(GLenum func, GLint ref, GLuint mask) {
    if ((_stencilFunc != func) || (_stencilFuncRef != ref) ||
        (_stencilFuncMask != mask)) {

      _stencilFunc = func;
      _stencilFuncRef = ref;
      _stencilFuncMask = mask;
      glStencilFunc(func, ref, mask);
    }
  }
};

///
///
///
static int glnvg__renderCreate(void *uptr) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  return gl->initialize();
}

static int glnvg__renderCreateTexture(void *uptr, int type, int w, int h,
                                      int imageFlags,
                                      const unsigned char *data) {
  auto gl = (GLNVGcontext *)uptr;
  auto tex = GLNVGtexture::load(w, h, type, data, imageFlags);
  if (!tex) {
    return 0;
  }
  gl->_textures.insert(std::make_pair(tex->id(), tex));
  return tex->id();
}

static int glnvg__renderDeleteTexture(void *uptr, int image) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  return gl->glnvg__deleteTexture(image);
}

static int glnvg__renderUpdateTexture(void *uptr, int image, int x, int y,
                                      int w, int h, const unsigned char *data) {
  auto gl = (GLNVGcontext *)uptr;
  auto tex = gl->glnvg__findTexture(image);
  if (!tex)
    return 0;

  tex->update(x, y, w, h, data);
  return 1;
}

static int glnvg__renderGetTextureSize(void *uptr, int image, int *w, int *h) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  auto tex = gl->glnvg__findTexture(image);
  if (tex == NULL)
    return 0;
  *w = tex->width();
  *h = tex->height();
  return 1;
}

static int glnvg__convertPaint(GLNVGcontext *gl, GLNVGfragUniforms *frag,
                               NVGpaint *paint, NVGscissor *scissor,
                               float width, float fringe, float strokeThr) {
  std::shared_ptr<GLNVGtexture> tex = NULL;
  float invxform[6];

  memset(frag, 0, sizeof(*frag));

  frag->innerCol = glnvg__premulColor(paint->innerColor);
  frag->outerCol = glnvg__premulColor(paint->outerColor);

  if (scissor->extent[0] < -0.5f || scissor->extent[1] < -0.5f) {
    memset(frag->scissorMat, 0, sizeof(frag->scissorMat));
    frag->scissorExt[0] = 1.0f;
    frag->scissorExt[1] = 1.0f;
    frag->scissorScale[0] = 1.0f;
    frag->scissorScale[1] = 1.0f;
  } else {
    nvgTransformInverse(invxform, scissor->xform);
    glnvg__xformToMat3x4(frag->scissorMat, invxform);
    frag->scissorExt[0] = scissor->extent[0];
    frag->scissorExt[1] = scissor->extent[1];
    frag->scissorScale[0] = sqrtf(scissor->xform[0] * scissor->xform[0] +
                                  scissor->xform[2] * scissor->xform[2]) /
                            fringe;
    frag->scissorScale[1] = sqrtf(scissor->xform[1] * scissor->xform[1] +
                                  scissor->xform[3] * scissor->xform[3]) /
                            fringe;
  }

  memcpy(frag->extent, paint->extent, sizeof(frag->extent));
  frag->strokeMult = (width * 0.5f + fringe * 0.5f) / fringe;
  frag->strokeThr = strokeThr;

  if (paint->image != 0) {
    tex = gl->glnvg__findTexture(paint->image);
    if (tex == NULL)
      return 0;
    if ((tex->flags() & NVG_IMAGE_FLIPY) != 0) {
      float m1[6], m2[6];
      nvgTransformTranslate(m1, 0.0f, frag->extent[1] * 0.5f);
      nvgTransformMultiply(m1, paint->xform);
      nvgTransformScale(m2, 1.0f, -1.0f);
      nvgTransformMultiply(m2, m1);
      nvgTransformTranslate(m1, 0.0f, -frag->extent[1] * 0.5f);
      nvgTransformMultiply(m1, m2);
      nvgTransformInverse(invxform, m1);
    } else {
      nvgTransformInverse(invxform, paint->xform);
    }
    frag->type = NSVG_SHADER_FILLIMG;

    if (tex->type() == NVG_TEXTURE_RGBA)
      frag->texType = (tex->flags() & NVG_IMAGE_PREMULTIPLIED) ? 0 : 1;
    else
      frag->texType = 2;
    //		printf("frag->texType = %d\n", frag->texType);
  } else {
    frag->type = NSVG_SHADER_FILLGRAD;
    frag->radius = paint->radius;
    frag->feather = paint->feather;
    nvgTransformInverse(invxform, paint->xform);
  }

  glnvg__xformToMat3x4(frag->paintMat, invxform);

  return 1;
}

static void glnvg__renderViewport(void *uptr, float width, float height,
                                  float devicePixelRatio) {
  NVG_NOTUSED(devicePixelRatio);
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  gl->_view[0] = width;
  gl->_view[1] = height;
}

static void glnvg__renderCancel(void *uptr) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  gl->clear();
}

static GLenum glnvg_convertBlendFuncFactor(int factor) {
  if (factor == NVG_ZERO)
    return GL_ZERO;
  if (factor == NVG_ONE)
    return GL_ONE;
  if (factor == NVG_SRC_COLOR)
    return GL_SRC_COLOR;
  if (factor == NVG_ONE_MINUS_SRC_COLOR)
    return GL_ONE_MINUS_SRC_COLOR;
  if (factor == NVG_DST_COLOR)
    return GL_DST_COLOR;
  if (factor == NVG_ONE_MINUS_DST_COLOR)
    return GL_ONE_MINUS_DST_COLOR;
  if (factor == NVG_SRC_ALPHA)
    return GL_SRC_ALPHA;
  if (factor == NVG_ONE_MINUS_SRC_ALPHA)
    return GL_ONE_MINUS_SRC_ALPHA;
  if (factor == NVG_DST_ALPHA)
    return GL_DST_ALPHA;
  if (factor == NVG_ONE_MINUS_DST_ALPHA)
    return GL_ONE_MINUS_DST_ALPHA;
  if (factor == NVG_SRC_ALPHA_SATURATE)
    return GL_SRC_ALPHA_SATURATE;
  return GL_INVALID_ENUM;
}

static GLNVGblend
glnvg__blendCompositeOperation(NVGcompositeOperationState op) {
  GLNVGblend blend;
  blend.srcRGB = glnvg_convertBlendFuncFactor(op.srcRGB);
  blend.dstRGB = glnvg_convertBlendFuncFactor(op.dstRGB);
  blend.srcAlpha = glnvg_convertBlendFuncFactor(op.srcAlpha);
  blend.dstAlpha = glnvg_convertBlendFuncFactor(op.dstAlpha);
  if (blend.srcRGB == GL_INVALID_ENUM || blend.dstRGB == GL_INVALID_ENUM ||
      blend.srcAlpha == GL_INVALID_ENUM || blend.dstAlpha == GL_INVALID_ENUM) {
    blend.srcRGB = GL_ONE;
    blend.dstRGB = GL_ONE_MINUS_SRC_ALPHA;
    blend.srcAlpha = GL_ONE;
    blend.dstAlpha = GL_ONE_MINUS_SRC_ALPHA;
  }
  return blend;
}

static void glnvg__renderFlush(void *uptr) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  gl->render();
}

static int glnvg__maxVertCount(const NVGpath *paths, int npaths) {
  int i, count = 0;
  for (i = 0; i < npaths; i++) {
    count += paths[i].nfill;
    count += paths[i].nstroke;
  }
  return count;
}

static void glnvg__vset(NVGvertex *vtx, float x, float y, float u, float v) {
  vtx->x = x;
  vtx->y = y;
  vtx->u = u;
  vtx->v = v;
}

static void glnvg__renderFill(void *uptr, NVGpaint *paint,
                              NVGcompositeOperationState compositeOperation,
                              NVGscissor *scissor, float fringe,
                              const float *bounds, const NVGpath *paths,
                              int npaths) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  GLNVGcall *call = gl->glnvg__allocCall();
  if (call == NULL)
    return;

  call->type = GLNVG_FILL;
  call->triangleCount = 4;
  call->pathOffset = gl->glnvg__allocPaths(npaths);
  if (call->pathOffset == -1)
    return;
  call->pathCount = npaths;
  call->image = paint->image;
  call->blendFunc = glnvg__blendCompositeOperation(compositeOperation);

  if (npaths == 1 && paths[0].convex) {
    call->type = GLNVG_CONVEXFILL;
    call->triangleCount =
        0; // Bounding box fill quad not needed for convex fill
  }

  // Allocate vertices for all the paths.
  int maxverts = glnvg__maxVertCount(paths, npaths) + call->triangleCount;
  int offset = gl->glnvg__allocVerts(maxverts);
  if (offset == -1)
    return;

  for (int i = 0; i < npaths; i++) {
    auto copy = &gl->get_path(call->pathOffset + i);
    const NVGpath *path = &paths[i];
    memset(copy, 0, sizeof(GLNVGpath));
    if (path->nfill > 0) {
      copy->fillOffset = offset;
      copy->fillCount = path->nfill;
      memcpy(&gl->get_vertex(offset), path->fill,
             sizeof(NVGvertex) * path->nfill);
      offset += path->nfill;
    }
    if (path->nstroke > 0) {
      copy->strokeOffset = offset;
      copy->strokeCount = path->nstroke;
      memcpy(&gl->get_vertex(offset), path->stroke,
             sizeof(NVGvertex) * path->nstroke);
      offset += path->nstroke;
    }
  }

  // Setup uniforms for draw calls
  if (call->type == GLNVG_FILL) {
    // Quad
    call->triangleOffset = offset;
    auto quad = &gl->get_vertex(call->triangleOffset);
    glnvg__vset(&quad[0], bounds[2], bounds[3], 0.5f, 1.0f);
    glnvg__vset(&quad[1], bounds[2], bounds[1], 0.5f, 1.0f);
    glnvg__vset(&quad[2], bounds[0], bounds[3], 0.5f, 1.0f);
    glnvg__vset(&quad[3], bounds[0], bounds[1], 0.5f, 1.0f);

    call->uniformOffset = gl->glnvg__allocFragUniforms(2);
    if (call->uniformOffset == -1)
      return;
    // Simple shader for stencil
    auto frag = gl->nvg__fragUniformPtr(call->uniformOffset);
    memset(frag, 0, sizeof(*frag));
    frag->strokeThr = -1.0f;
    frag->type = NSVG_SHADER_SIMPLE;
    // Fill shader
    glnvg__convertPaint(
        gl, gl->nvg__fragUniformPtr(call->uniformOffset + gl->_fragSize), paint,
        scissor, fringe, fringe, -1.0f);
  } else {
    call->uniformOffset = gl->glnvg__allocFragUniforms(1);
    if (call->uniformOffset == -1)
      return;
    // Fill shader
    glnvg__convertPaint(gl, gl->nvg__fragUniformPtr(call->uniformOffset), paint,
                        scissor, fringe, fringe, -1.0f);
  }
}

static void glnvg__renderStroke(void *uptr, NVGpaint *paint,
                                NVGcompositeOperationState compositeOperation,
                                NVGscissor *scissor, float fringe,
                                float strokeWidth, const NVGpath *paths,
                                int npaths) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  GLNVGcall *call = gl->glnvg__allocCall();
  int i, maxverts, offset;

  if (call == NULL)
    return;

  call->type = GLNVG_STROKE;
  call->pathOffset = gl->glnvg__allocPaths(npaths);
  if (call->pathOffset == -1)
    return;
  call->pathCount = npaths;
  call->image = paint->image;
  call->blendFunc = glnvg__blendCompositeOperation(compositeOperation);

  // Allocate vertices for all the paths.
  maxverts = glnvg__maxVertCount(paths, npaths);
  offset = gl->glnvg__allocVerts(maxverts);
  if (offset == -1)
    return;

  for (i = 0; i < npaths; i++) {
    auto copy = &gl->get_path(call->pathOffset + i);
    const NVGpath *path = &paths[i];
    memset(copy, 0, sizeof(GLNVGpath));
    if (path->nstroke) {
      copy->strokeOffset = offset;
      copy->strokeCount = path->nstroke;
      memcpy(&gl->get_vertex(offset), path->stroke,
             sizeof(NVGvertex) * path->nstroke);
      offset += path->nstroke;
    }
  }

  if (gl->_flags & NVG_STENCIL_STROKES) {
    // Fill shader
    call->uniformOffset = gl->glnvg__allocFragUniforms(2);
    if (call->uniformOffset == -1)
      return;

    glnvg__convertPaint(gl, gl->nvg__fragUniformPtr(call->uniformOffset), paint,
                        scissor, strokeWidth, fringe, -1.0f);
    glnvg__convertPaint(
        gl, gl->nvg__fragUniformPtr(call->uniformOffset + gl->_fragSize), paint,
        scissor, strokeWidth, fringe, 1.0f - 0.5f / 255.0f);

  } else {
    // Fill shader
    call->uniformOffset = gl->glnvg__allocFragUniforms(1);
    if (call->uniformOffset == -1)
      return;
    glnvg__convertPaint(gl, gl->nvg__fragUniformPtr(call->uniformOffset), paint,
                        scissor, strokeWidth, fringe, -1.0f);
  }
}

static void glnvg__renderTriangles(
    void *uptr, NVGpaint *paint, NVGcompositeOperationState compositeOperation,
    NVGscissor *scissor, const NVGvertex *verts, int nverts, float fringe) {
  GLNVGcontext *gl = (GLNVGcontext *)uptr;
  GLNVGcall *call = gl->glnvg__allocCall();
  GLNVGfragUniforms *frag;

  if (call == NULL)
    return;

  call->type = GLNVG_TRIANGLES;
  call->image = paint->image;
  call->blendFunc = glnvg__blendCompositeOperation(compositeOperation);

  // Allocate vertices for all the paths.
  call->triangleOffset = gl->glnvg__allocVerts(nverts);
  if (call->triangleOffset == -1)
    return;
  call->triangleCount = nverts;

  memcpy(&gl->get_vertex(call->triangleOffset), verts,
         sizeof(NVGvertex) * nverts);

  // Fill shader
  call->uniformOffset = gl->glnvg__allocFragUniforms(1);
  if (call->uniformOffset == -1)
    return;
  frag = gl->nvg__fragUniformPtr(call->uniformOffset);
  glnvg__convertPaint(gl, frag, paint, scissor, 1.0f, fringe, -1.0f);
  frag->type = NSVG_SHADER_IMG;
}

static void glnvg__renderDelete(void *uptr) {
  auto gl = (GLNVGcontext *)uptr;
  if (!gl)
    return;

  delete gl;
}

NVGcontext *nvgCreateGL3(int flags) {
  auto gl = new GLNVGcontext;
  if (!gl)
    goto error;

  NVGparams params;
  memset(&params, 0, sizeof(params));
  params.renderCreate = glnvg__renderCreate;
  params.renderCreateTexture = glnvg__renderCreateTexture;
  params.renderDeleteTexture = glnvg__renderDeleteTexture;
  params.renderUpdateTexture = glnvg__renderUpdateTexture;
  params.renderGetTextureSize = glnvg__renderGetTextureSize;
  params.renderViewport = glnvg__renderViewport;
  params.renderCancel = glnvg__renderCancel;
  params.renderFlush = glnvg__renderFlush;
  params.renderFill = glnvg__renderFill;
  params.renderStroke = glnvg__renderStroke;
  params.renderTriangles = glnvg__renderTriangles;
  params.renderDelete = glnvg__renderDelete;
  params.userPtr = gl;
  params.edgeAntiAlias = flags & NVG_ANTIALIAS ? 1 : 0;

  gl->_flags = flags;

  NVGcontext *ctx = NULL;
  ctx = nvgCreateInternal(&params);
  if (ctx == NULL)
    goto error;

  return ctx;

error:
  // 'gl' is freed by nvgDeleteInternal.
  if (ctx != NULL)
    nvgDeleteInternal(ctx);
  return NULL;
}

void nvgDeleteGL3(NVGcontext *ctx) { nvgDeleteInternal(ctx); }

int nvglCreateImageFromHandleGL3(NVGcontext *ctx, GLuint textureId, int w,
                                 int h, int imageFlags) {
  auto gl = (GLNVGcontext *)nvgInternalParams(ctx)->userPtr;
  auto tex = GLNVGtexture::fromHandle(textureId, w, h, imageFlags);
  if (!tex)
    return 0;
  gl->_textures.insert(std::make_pair(tex->id(), tex));
  return tex->id();
}

GLuint nvglImageHandleGL3(NVGcontext *ctx, int image) {
  GLNVGcontext *gl = (GLNVGcontext *)nvgInternalParams(ctx)->userPtr;
  auto tex = gl->glnvg__findTexture(image);
  return tex->handle();
}
