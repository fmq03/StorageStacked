#include "imageutil.hpp"
#include "blitter.hpp"
#include "tga.hpp"
#include "png.hpp"
#include "bmp.hpp"
#include <iostream>
#include <iomanip>

using namespace cocogfx;

static std::string getFileExt(const std::string& str) {
  auto i = str.rfind('.');
  if (i != std::string::npos) {
    return str.substr(i+1);
  }
  return("");
}

static bool iequals(const std::string& a, const std::string& b) {
  auto sz = a.size();
  if (b.size() != sz)
    return false;
  for (size_t i = 0; i < sz; ++i) {
    if (tolower(a[i]) != tolower(b[i]))
      return false;
  }
  return true;
}

int cocogfx::LoadImage(const char *filename,
                       ePixelFormat format, 
                       std::vector<uint8_t> &pixels, 
                       uint32_t *width,
                       uint32_t *height) {
  uint32_t img_width;
  uint32_t img_height;
  uint32_t img_bpp;

  auto ext = getFileExt(filename);
  if (iequals(ext, "tga")) {
    int ret = LoadTGA(filename, pixels, &img_width, &img_height, &img_bpp);
    if (ret)
      return ret;
  } else 
  if (iequals(ext, "png")) {
    int ret = LoadPNG(filename, pixels, &img_width, &img_height, &img_bpp);
    if (ret)
      return ret;
  } else 
  if (iequals(ext, "bmp")) {
    int ret = LoadBMP(filename, pixels, &img_width, &img_height, &img_bpp);
    if (ret)
      return ret;
  } else {
    std::cerr << "invalid file extension: " << ext << "!" << std::endl;
    return -1;
  }

  ePixelFormat img_format;  
  switch (img_bpp) {
  case 1: 
    img_format = FORMAT_A8;
    break;
  case 2: 
    img_format = FORMAT_R5G6B5;
    break;
  case 3: 
    img_format = FORMAT_R8G8B8; 
    break;
  case 4: 
    img_format = FORMAT_A8R8G8B8; 
    break;
  default:
    std::abort();            
  }

  if (img_format != format) {
    // format conversion to RGBA
    std::vector<uint8_t> staging;    
    int ret = ConvertImage(staging, format, pixels.data(), img_format, img_width, img_height, img_width * img_bpp);
    if (ret)
      return ret;
    pixels.swap(staging);
  }

  *width  = img_width;
  *height = img_height;
  
  return 0;
}

int cocogfx::SaveImage(const char *filename,
                       ePixelFormat format,
                       const uint8_t* pixels, 
                       uint32_t width,
                       uint32_t height,
                       int32_t pitch) {
  uint32_t bpp = Format::GetInfo(format).BytePerPixel;
  auto ext = getFileExt(filename);
  if (iequals(ext, "tga")) {
    return SaveTGA(filename, pixels, width, height, bpp, pitch);
  } else 
  if (iequals(ext, "png")) {
    return SavePNG(filename, pixels, width, height, bpp, pitch);
  } else 
  if (iequals(ext, "bmp")) {
    return SaveBMP(filename, pixels, width, height, bpp, pitch);
  } else {
    std::cerr << "invalid file extension: " << ext << "!" << std::endl;
    return -1;
  }

  return 0;
}

void cocogfx::DumpImage(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t bpp) {
  assert(width * height * bpp == pixels.size());
  const uint8_t* pixel_bytes = pixels.data();
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t pixel32 = 0;
      for (uint32_t b = 0; b < bpp; ++b) {
        uint32_t pixel8 = *pixel_bytes++;
        pixel32 |= pixel8 << (b * 8);
      }
      if (x) std::cout << ", ";
      std::cout << std::hex << std::setw(bpp * 2) << std::setfill('0') << pixel32;
    }
    std::cout << std::endl;
  }
}

int cocogfx::CompareImages(const char* filename1, 
                           const char* filename2, 
                           cocogfx::ePixelFormat format,
                           uint32_t tolerance) {
  int ret;
  std::vector<uint8_t> image1_bits;  
  uint32_t image1_width; 
  uint32_t image1_height;

  std::vector<uint8_t> image2_bits;  
  uint32_t image2_width; 
  uint32_t image2_height;
    
  ret = cocogfx::LoadImage(filename1, format, image1_bits, &image1_width, &image1_height);  
  if (ret)
    return ret;

  ret = cocogfx::LoadImage(filename2, format, image2_bits, &image2_width, &image2_height);  
  if (ret)
    return ret;

  if (image1_bits.size() != image2_bits.size())
    return -1;

  if (image1_width != image2_width)
    return -1;

  if (image1_height != image2_height)
    return -1;

  int errors = 0;
  {
    auto convert_from = Format::GetConvertFrom(format, true);
    auto bpp = Format::GetInfo(format).BytePerPixel;
    auto pixels1 = image1_bits.data();
    auto pixels2 = image2_bits.data();
    for (uint32_t y = 0; y < image1_height; ++y) {
      for (uint32_t x = 0; x < image1_width; ++x) {
        auto color1 = convert_from(pixels1);
        auto color2 = convert_from(pixels2);
        if (abs(color1.r - color2.r) > tolerance 
         || abs(color1.g - color2.g) > tolerance
         || abs(color1.b - color2.b) > tolerance
         || abs(color1.a - color2.a) > tolerance) {
          printf("Error: pixel mismatch at (%d, %d), actual=0x%x, expected=0x%x\n", x, y, color1.value, color2.value);
          ++errors;
        }
        pixels1 += bpp;
        pixels2 += bpp;    
      }
    }
  }

  return errors;
}