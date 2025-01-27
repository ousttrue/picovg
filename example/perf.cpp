#include "perf.h"
#include "nanovg.h"
#include <glad/glad.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#elif !defined(__MINGW32__)
#include <iconv.h>
#endif

// timer query support
#ifndef GL_ARB_timer_query
#define GL_TIME_ELAPSED 0x88BF
// typedef void (APIENTRY *pfnGLGETQUERYOBJECTUI64V)(GLuint id, GLenum pname,
// GLuint64* params); pfnGLGETQUERYOBJECTUI64V glGetQueryObjectui64v = 0;
#endif

void GPUtimer::startGPUTimer() {
  if (!supported)
    return;
  glBeginQuery(GL_TIME_ELAPSED, queries[cur % GPU_QUERY_COUNT]);
  cur++;
}

int GPUtimer::stopGPUTimer(float *times, int maxTimes) {
  NVG_NOTUSED(times);
  NVG_NOTUSED(maxTimes);
  GLint available = 1;
  int n = 0;
  if (!supported)
    return 0;

  glEndQuery(GL_TIME_ELAPSED);
  while (available && ret <= cur) {
    // check for results if there are any
    glGetQueryObjectiv(queries[ret % GPU_QUERY_COUNT],
                       GL_QUERY_RESULT_AVAILABLE, &available);
    if (available) {
      /*			GLuint64 timeElapsed = 0;
                              glGetQueryObjectui64v(queries[ret %
         GPU_QUERY_COUNT], GL_QUERY_RESULT, &timeElapsed); ret++; if (n <
         maxTimes) { times[n] = (float)((double)timeElapsed * 1e-9); n++;
                              }*/
    }
  }
  return n;
}

PerfGraph::PerfGraph(int style, const char *name) {
  this->_style = style;
  this->_name = name;
}

PerfGraph::~PerfGraph() {
  printf("Average %s Time: %.2f ms\n", _name.c_str(),
         getGraphAverage() * 1000.0f);
  // printf("          CPU Time: %.2f ms\n", cpuGraph.getGraphAverage() *
  // 1000.0f); printf("          GPU Time: %.2f ms\n",
  // gpuGraph.getGraphAverage() * 1000.0f);
}

void PerfGraph::updateGraph(float frameTime) {
  _head = (_head + 1) % GRAPH_HISTORY_COUNT;
  _values[_head] = frameTime;
}

float PerfGraph::getGraphAverage() {
  float avg = 0;
  for (int i = 0; i < GRAPH_HISTORY_COUNT; i++) {
    avg += _values[i];
  }
  return avg / (float)GRAPH_HISTORY_COUNT;
}

void PerfGraph::renderGraph(NVGcontext *vg, float x, float y) {

  float avg = getGraphAverage();

  float w = 200;
  float h = 35;

  nvgBeginPath(vg);
  nvgRect(vg, x, y, w, h);
  nvgFillColor(vg, nvgRGBA(0, 0, 0, 128));
  nvgFill(vg);

  nvgBeginPath(vg);
  nvgMoveTo(vg, x, y + h);
  if (_style == GRAPH_RENDER_FPS) {
    for (int i = 0; i < GRAPH_HISTORY_COUNT; i++) {
      float v = 1.0f / (0.00001f + _values[(_head + i) % GRAPH_HISTORY_COUNT]);
      float vx, vy;
      if (v > 80.0f)
        v = 80.0f;
      vx = x + ((float)i / (GRAPH_HISTORY_COUNT - 1)) * w;
      vy = y + h - ((v / 80.0f) * h);
      nvgLineTo(vg, vx, vy);
    }
  } else if (_style == GRAPH_RENDER_PERCENT) {
    for (int i = 0; i < GRAPH_HISTORY_COUNT; i++) {
      float v = _values[(_head + i) % GRAPH_HISTORY_COUNT] * 1.0f;
      float vx, vy;
      if (v > 100.0f)
        v = 100.0f;
      vx = x + ((float)i / (GRAPH_HISTORY_COUNT - 1)) * w;
      vy = y + h - ((v / 100.0f) * h);
      nvgLineTo(vg, vx, vy);
    }
  } else {
    for (int i = 0; i < GRAPH_HISTORY_COUNT; i++) {
      float v = _values[(_head + i) % GRAPH_HISTORY_COUNT] * 1000.0f;
      float vx, vy;
      if (v > 20.0f)
        v = 20.0f;
      vx = x + ((float)i / (GRAPH_HISTORY_COUNT - 1)) * w;
      vy = y + h - ((v / 20.0f) * h);
      nvgLineTo(vg, vx, vy);
    }
  }
  nvgLineTo(vg, x + w, y + h);
  nvgFillColor(vg, nvgRGBA(255, 192, 0, 128));
  nvgFill(vg);

  nvgFontFace(vg, "sans");

  if (_name[0] != '\0') {
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGBA(240, 240, 240, 192));
    nvgText(vg, x + 3, y + 3, _name.c_str(), NULL);
  }

  if (_style == GRAPH_RENDER_FPS) {
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGBA(240, 240, 240, 255));
    char str[64];
    sprintf(str, "%.2f FPS", 1.0f / avg);
    nvgText(vg, x + w - 3, y + 3, str, NULL);

    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_BASELINE);
    nvgFillColor(vg, nvgRGBA(240, 240, 240, 160));
    sprintf(str, "%.2f ms", avg * 1000.0f);
    nvgText(vg, x + w - 3, y + h - 3, str, NULL);
  } else if (_style == GRAPH_RENDER_PERCENT) {
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGBA(240, 240, 240, 255));
    char str[64];
    sprintf(str, "%.1f %%", avg * 1.0f);
    nvgText(vg, x + w - 3, y + 3, str, NULL);
  } else {
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGBA(240, 240, 240, 255));
    char str[64];
    sprintf(str, "%.2f ms", avg * 1000.0f);
    nvgText(vg, x + w - 3, y + 3, str, NULL);
  }
}
