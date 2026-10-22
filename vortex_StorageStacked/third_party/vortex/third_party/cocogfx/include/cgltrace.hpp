#pragma once

#include "common.hpp"
#include "format.hpp"
#include <vector>
#include <unordered_map>
#include <iostream>

namespace cocogfx {

class CGLTrace {
public:
  enum eTexFilter {
    FILTER_NONE,
    FILTER_NEAREST,
    FILTER_LINEAR,
    FILTER_NEAREST_MIPMAP_NEAREST,
    FILTER_LINEAR_MIPMAP_NEAREST,
    FILTER_NEAREST_MIPMAP_LINEAR,
    FILTER_LINEAR_MIPMAP_LINEAR,
    FILTER_SIZE_,
  };

  enum eTexAddress {
    ADDRESS_WRAP,
    ADDRESS_CLAMP,
    ADDRESS_SIZE_,
  };

  enum eEnvMode {
    ENVMODE_ADD,
    ENVMODE_BLEND,
    ENVMODE_REPLACE,
    ENVMODE_MODULATE,
    ENVMODE_DECAL,
    ENVMODE_SIZE_,
  };

  enum ecompare {
    COMPARE_NEVER,
    COMPARE_LESS,
    COMPARE_EQUAL,
    COMPARE_LEQUAL,
    COMPARE_GREATER,
    COMPARE_NOTEQUAL,
    COMPARE_GEQUAL,
    COMPARE_ALWAYS,
    COMPARE_SIZE_,
  };

  enum eStencilOp {
    STENCIL_KEEP,
    STENCIL_REPLACE,
    STENCIL_INCR,
    STENCIL_DECR,
    STENCIL_ZERO,
    STENCIL_INVERT,
    STENCIL_SIZE_,
    };

  enum eBlendOp {
    BLEND_ZERO,
    BLEND_ONE,
    BLEND_SRC_COLOR,
    BLEND_ONE_MINUS_SRC_COLOR,
    BLEND_SRC_ALPHA,
    BLEND_ONE_MINUS_SRC_ALPHA,
    BLEND_DST_ALPHA,
    BLEND_ONE_MINUS_DST_ALPHA,
    BLEND_DST_COLOR,
    BLEND_ONE_MINUS_DST_COLOR,
    BLEND_SRC_ALPHA_SATURATE,
    BLEND_SIZE_,
  };

  struct pos_t {
    float x;
    float y;
    float z;
    float w;
  };

  struct color_t {
    float r;
    float g;
    float b;
    float a;
  };

  struct texcoord_t {
    float u;
    float v;
  };

  struct viewport_t {
    int   left;
    int   right;
    int   top;
    int   bottom;
    float near;
    float far;
  };

  struct vertex_t {
    pos_t      pos;
    color_t    color;
    texcoord_t texcoord;
  };

  struct primitive_t {
    uint32_t i0;
    uint32_t i1;
    uint32_t i2;
  };

  struct texture_t {
    ePixelFormat format;
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> pixels;

    bool operator==(const texture_t& rhs) const;
  };

  struct states_t {
    bool color_enabled;
    ePixelFormat color_format;
    uint32_t color_writemask;

    bool depth_test;
    bool depth_writemask;
    ePixelFormat depth_format; 
    ecompare depth_func;
    
    bool stencil_test;
    ecompare stencil_func;
    eStencilOp stencil_zpass;
    eStencilOp stencil_zfail;
    eStencilOp stencil_fail;
    uint8_t stencil_ref;
    uint8_t stencil_mask;
    uint8_t stencil_writemask;
    
    bool texture_enabled;
    color_t texture_envcolor;
    eEnvMode texture_envmode;
    eTexFilter texture_minfilter;
    eTexFilter texture_magfilter;
    eTexAddress texture_addressU;
    eTexAddress texture_addressV;

    bool blend_enabled;
    eBlendOp blend_src;
    eBlendOp blend_dst;
  };

  struct drawcall_t {
    states_t states;
    uint32_t texture_id;
    std::unordered_map<uint32_t, vertex_t> vertices;
    std::vector<primitive_t> primitives;
    viewport_t viewport;
  };

  std::vector<drawcall_t> drawcalls;
  std::unordered_map<uint32_t, texture_t> textures;  

  int load(const char* filename);

  int save(const char* filename);
};

}