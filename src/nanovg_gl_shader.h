#pragma once
#include <glad/glad.h>
#include <memory>

enum GLNVGuniformLoc {
  GLNVG_LOC_VIEWSIZE,
  GLNVG_LOC_TEX,
  GLNVG_LOC_FRAG,
  GLNVG_MAX_LOCS
};

enum GLNVGshaderType {
  NSVG_SHADER_FILLGRAD,
  NSVG_SHADER_FILLIMG,
  NSVG_SHADER_SIMPLE,
  NSVG_SHADER_IMG
};

enum GLNVGuniformBindings {
  GLNVG_FRAG_BINDING = 0,
};

class GLNVGshader {
  GLuint prog = 0;
  GLuint frag = 0;
  GLuint vert = 0;
  GLint loc[GLNVG_MAX_LOCS];

  GLNVGshader();
public:
  ~GLNVGshader();
  static std::shared_ptr<GLNVGshader> create(bool useAntiAlias);
  void use();
  void getUniforms();
  void blockBind();
  void set_texture_and_view(int texture, const float view[2]);
};
