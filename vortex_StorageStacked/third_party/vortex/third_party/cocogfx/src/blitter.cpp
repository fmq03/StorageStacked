#include "blitter.hpp"
#include <assert.h>
#include <cstring>
#include <iostream>

using namespace cocogfx;

namespace {

constexpr uint32_t count_leading_zeros(uint32_t value) {
  return value ? __builtin_clz(value) : 32;
}

constexpr uint32_t log2ceil(uint32_t value) {
  return 32 - count_leading_zeros(value - 1);
}

}

int cocogfx::CopyBuffers(std::vector<uint8_t>& dst_pixels,
                         ePixelFormat dst_format,
                         uint32_t dst_width,
                         uint32_t dst_height,
                         int32_t dst_pitch,
                         int32_t dst_offsetx,
                         int32_t dst_offsety,                
                         const uint8_t* src_pixels,
                         ePixelFormat src_format,
                         uint32_t src_width,
                         uint32_t src_height,
                         int32_t src_pitch,
                         int32_t src_offsetx,
                         int32_t src_offsety,                                     
                         uint32_t width, 
                         uint32_t height) {  
  auto& src_fmtinfo = Format::GetInfo(src_format);
  auto& dst_fmtinfo = Format::GetInfo(dst_format);
  
  if ((src_fmtinfo.Depth || src_fmtinfo.Stencil) ^ (dst_fmtinfo.Depth || dst_fmtinfo.Stencil)) {
    std::cerr << "non-compatible formats" << std::endl;
    return -1;
  }
    
  if (((src_offsetx + width)  > src_width) 
   || ((src_offsety + height) > src_height)) {
    std::cerr << "out of range source region" << std::endl;
    return -1;
  }

  if (((dst_offsetx + width)  > dst_width) 
   || ((dst_offsety + height) > dst_height)) {
    std::cerr << "out of range destination region" << std::endl;
    return -1;
  }

  if (((dst_offsety + height) * dst_pitch) > dst_pixels.size()) {
    std::cerr << "out of range destination buffer" << std::endl;
    return -1;
  }

  auto src_stride = src_fmtinfo.BytePerPixel;
  auto dst_stride = dst_fmtinfo.BytePerPixel;

  auto pbSrc = src_pixels + src_offsetx * src_stride + src_offsety * src_pitch;
  auto pbDst = dst_pixels.data() + dst_offsetx * dst_stride + dst_offsety * dst_pitch;

  auto src_nformat = Format::GetNativeFormat((ePixelFormat)src_format);
  auto dst_nformat = Format::GetNativeFormat((ePixelFormat)dst_format);

  if (src_nformat == dst_nformat) {
    while (height--) {
      memcpy(pbDst, pbSrc, src_stride * width);        
      pbDst += dst_pitch;
      pbSrc += src_pitch;
    }
  } else {
    auto convert_from = Format::GetConvertFrom(src_nformat, true);
    auto convert_to = Format::GetConvertTo(dst_nformat);
    while (height--) {
      uint8_t *pbD = pbDst; 
      for (auto *pbS = pbSrc, 
          *const pbE = pbS + src_stride * width; pbS != pbE;) {
        auto color = convert_from(pbS);
        convert_to(pbD, color);
        pbD += dst_stride;
        pbS += src_stride;
      }      
      pbDst += dst_pitch;
      pbSrc += src_pitch;
    }    
  }

  return 0;
}

int cocogfx::ConvertImage(std::vector<uint8_t>& dst_pixels,
                          ePixelFormat dst_format,
                          const uint8_t* src_pixels,
                          ePixelFormat src_format,
                          uint32_t src_width,
                          uint32_t src_height,                 
                          int32_t src_pitch) {
  uint32_t dst_pitch = Format::GetInfo(dst_format).BytePerPixel * src_width;
  dst_pixels.resize(dst_pitch * src_height);
  return CopyBuffers(    
    dst_pixels,
    dst_format,
    src_width,
    src_height,
    dst_pitch,
    0,
    0,    
    src_pixels,
    src_format,
    src_width,
    src_height,
    src_pitch,
    0,
    0,                                     
    src_width, 
    src_height
  );
}

int cocogfx::GenerateMipmaps(std::vector<uint8_t>& dst_pixels,
                             std::vector<uint32_t>& mip_offsets,
                             const uint8_t* src_pixels,
                             ePixelFormat format,
                             uint32_t src_width,
                             uint32_t src_height,
                             int32_t src_pitch) {
  if (nullptr == src_pixels 
   || 0 == src_width 
   || 0 == src_height 
   || 0 == src_pitch)
    return -1;
  
  uint32_t bpp = Format::GetInfo(format).BytePerPixel;
  uint32_t src_logwidth  = log2ceil(src_width);
  uint32_t src_logheight = log2ceil(src_height);
  uint32_t num_mipmaps   = std::max(src_logwidth, src_logheight) + 1;
  auto convert_from      = Format::GetConvertFrom(format, true);
  auto convert_to        = Format::GetConvertTo(format);

  mip_offsets.resize(num_mipmaps);

  // Calculate mipmaps buffer size and offsets
  uint32_t num_pixels = 0;
  for (uint32_t lod = 0, w = src_width, h = src_height; lod < num_mipmaps;) {
    uint32_t pw = std::max<int>(w, 1);
    uint32_t ph = std::max<int>(h, 1);
    mip_offsets.at(lod) = num_pixels;
    num_pixels += pw * ph;
    w >>= 1;
    h >>= 1;
    ++lod;
  }

  // allocate destination buffer
  dst_pixels.resize(num_pixels * bpp);

  // generate mipmaps  
  {
    // copy level 0
    CopyBuffers(
      dst_pixels, format, src_width, src_height, src_width * bpp,
      0, 0,
      src_pixels, format, src_width, src_height, src_pitch,
      0, 0, src_width, src_height
    );    
    
    // copy lower levels
    
    auto pSrc = src_pixels;
    auto pDst = dst_pixels.data();

    pSrc = pDst;
    pDst += src_width * src_height * bpp;

    for (uint32_t lod = 1, w = (src_width/2), h = (src_height/2); lod < num_mipmaps;) {
      uint32_t pw = std::max<int>(w, 1);
      uint32_t ph = std::max<int>(h, 1);
      for (uint32_t y = 0; y < ph; ++y) {
        uint32_t v0 = 2 * y;
        uint32_t v1 = 2 * y + ((ph > 1) ? 1 : 0);
        
        uint32_t p0 = v0 * 2 * pw;
        uint32_t p1 = v1 * 2 * pw;

        for (uint32_t x = 0; x <pw; ++x) {
          uint32_t u0 = 2 * x;
          uint32_t u1 = 2 * x + ((pw > 1) ? 1 : 0);

          auto c00 = convert_from(pSrc + (u0 + p0) * bpp);
          auto c01 = convert_from(pSrc + (u1 + p0) * bpp);
          auto c10 = convert_from(pSrc + (u0 + p1) * bpp);
          auto c11 = convert_from(pSrc + (u1 + p1) * bpp);

          auto a = (c00.a + c01.a + c10.a + c11.a + 2) >> 2;
          auto r = (c00.r + c01.r + c10.r + c11.r + 2) >> 2;
          auto g = (c00.g + c01.g + c10.g + c11.g + 2) >> 2;
          auto b = (c00.b + c01.b + c10.b + c11.b + 2) >> 2;
                                
          convert_to(pDst + (x + y * pw) * bpp, ColorARGB(a, r, g, b));
        }
      }       
      pSrc = pDst;
      pDst += pw * ph * bpp;
      w >>= 1;
      h >>= 1;  
      ++lod;
    }
    assert(pDst == (dst_pixels.data() + dst_pixels.size()));
  }
 
  for (auto& offset : mip_offsets) {
    offset *= bpp;
  }

  return 0;
}