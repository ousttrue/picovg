#include "nanovg_gl_texture.h"
#include "nanovg.h"
#include "nanovg_gl.h"
#include <glad/glad.h>

GLNVGtexture::GLNVGtexture() {
  static int s_textureId = 0;
  _id = ++s_textureId;
}

GLNVGtexture::~GLNVGtexture() {
  if (_handle != 0 && (_flags & NVG_IMAGE_NODELETE) == 0) {
    glDeleteTextures(1, &_handle);
  }
}

void GLNVGtexture::bind() { glBindTexture(GL_TEXTURE_2D, _handle); }
void GLNVGtexture::unbind() { glBindTexture(GL_TEXTURE_2D, 0); }

std::shared_ptr<GLNVGtexture>
GLNVGtexture::load(int w, int h, int type, const void *data, int imageFlags) {
  auto tex = std::shared_ptr<GLNVGtexture>(new GLNVGtexture);
  glGenTextures(1, &tex->_handle);
  tex->bind();
  tex->_width = w;
  tex->_height = h;
  tex->_type = type;
  tex->_flags = imageFlags;
  glBindTexture(GL_TEXTURE_2D, tex->_handle);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, tex->_width);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

  if (type == NVG_TEXTURE_RGBA)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 data);
  else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, w, h, 0, GL_RED, GL_UNSIGNED_BYTE,
                 data);

  if (imageFlags & NVG_IMAGE_GENERATE_MIPMAPS) {
    if (imageFlags & NVG_IMAGE_NEAREST) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                      GL_NEAREST_MIPMAP_NEAREST);
    } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                      GL_LINEAR_MIPMAP_LINEAR);
    }
  } else {
    if (imageFlags & NVG_IMAGE_NEAREST) {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    } else {
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
  }

  if (imageFlags & NVG_IMAGE_NEAREST) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  } else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }

  if (imageFlags & NVG_IMAGE_REPEATX)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
  else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);

  if (imageFlags & NVG_IMAGE_REPEATY)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
  else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

  // The new way to build mipmaps on GLES and GL3
  if (imageFlags & NVG_IMAGE_GENERATE_MIPMAPS) {
    glGenerateMipmap(GL_TEXTURE_2D);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  tex->unbind();
  return tex;
}

std::shared_ptr<GLNVGtexture> GLNVGtexture::fromHandle(GLuint textureId, int w,
                                                       int h, int imageFlags) {
  auto tex = std::shared_ptr<GLNVGtexture>(new GLNVGtexture);
  tex->_type = NVG_TEXTURE_RGBA;
  tex->_handle = textureId;
  tex->_flags = imageFlags;
  tex->_width = w;
  tex->_height = h;
  return tex;
}

void GLNVGtexture::update(int x, int y, int w, int h, const void *data) {
  bind();

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, _width);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, y);

  if (_type == NVG_TEXTURE_RGBA)
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                    data);
  else
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE,
                    data);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

  unbind();
}
