#include "bmp.hpp"
#include <iostream>

using namespace cocogfx;

#ifdef _WIN32
include<wingdi.h>
#else

#define BF_TYPE 0x4D42 // bitmap file type
#define BI_RGB 0       // no compression
#define BI_RLE8 1      // 8-bit run-length compression
#define BI_RLE4 2      // 4-bit run-length compression
#define BI_BITFIELDS 3 // with RGB masks

// file header
struct __attribute__((__packed__)) BITMAPFILEHEADER {
  uint16_t bfType; // magic number
  uint32_t bfSize;
  uint16_t bfReserved1;
  uint16_t bfReserved2;
  uint32_t bfOffBits; // offset to data
};

// file info
struct __attribute__((__packed__)) BITMAPINFOHEADER {
  uint32_t biSize;
  int32_t biWidth;
  int32_t biHeight;
  uint16_t biPlanes;       // number of color planes
  uint16_t biBitCount;     // number of bits per pixel
  uint32_t biCompression;  // type of compression to use
  uint32_t biSizeImage;    // size of image data
  int32_t biXPelsPerMeter; // X pixels per meter
  int32_t biYPelsPerMeter; // Y pixels per meter
  uint32_t biClrUsed;      // number of colors used
  uint32_t biClrImportant; // number of important colors
};

struct __attribute__((__packed__)) RGBQUAD {
  uint8_t rgbBlue;
  uint8_t rgbGreen;
  uint8_t rgbRed;
  uint8_t rgbReserved;
};

struct __attribute__((__packed__)) BITMAPINFO {
  BITMAPINFOHEADER bmiHeader;
  RGBQUAD bmiColors[1];
};

#endif

int cocogfx::LoadBMP(const char */*filename*/, 
                     std::vector<uint8_t> &/*pixels*/, 
                     uint32_t */*width*/,
                     uint32_t */*height*/,
                     uint32_t */*bpp*/) {        
  std::abort();
  return -1;
}

int cocogfx::SaveBMP(const char *filename, 
                     const uint8_t* pixels, 
                     uint32_t width,
                     uint32_t height, 
                     uint32_t bpp,
                     int32_t pitch) {
  BITMAPFILEHEADER header;
  header.bfSize = 0;
  header.bfType = BF_TYPE;
  header.bfReserved1 = 0;
  header.bfReserved2 = 0;
  header.bfOffBits = 0;

  struct bmp_info_header_t {
    BITMAPINFOHEADER bmiHeader;
    uint32_t bmiColors[3];
  } bmp_info;  

  bmp_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmp_info.bmiHeader.biWidth = width;
  bmp_info.bmiHeader.biHeight = -height;
  bmp_info.bmiHeader.biPlanes = 1;
  bmp_info.bmiHeader.biXPelsPerMeter = 0;
  bmp_info.bmiHeader.biYPelsPerMeter = 0;
  bmp_info.bmiHeader.biClrUsed = 0;
  bmp_info.bmiHeader.biClrImportant = 0;

  uint32_t infoSize;

  if (2 == bpp) {
    bmp_info.bmiHeader.biBitCount = 16;
    bmp_info.bmiHeader.biCompression = BI_BITFIELDS;
    bmp_info.bmiColors[0] = 0xF800;
    bmp_info.bmiColors[1] = 0x07E0;
    bmp_info.bmiColors[2] = 0x001F;
    infoSize = sizeof(bmp_info_header_t);
  }
  if (3 == bpp || 4 == bpp) {
    bmp_info.bmiHeader.biBitCount = 24;
    bmp_info.bmiHeader.biCompression = BI_RGB;
    infoSize = sizeof(BITMAPINFOHEADER);
  } else {
    return -1;
  }

  bmp_info.bmiHeader.biSizeImage = height *
                                   bmp_info.bmiHeader.biWidth *
                                   (bmp_info.bmiHeader.biBitCount / 8);
  header.bfOffBits = sizeof(BITMAPFILEHEADER) + infoSize;
  header.bfSize = header.bfOffBits + bmp_info.bmiHeader.biSizeImage;

  auto pBits = pixels;

  if (pitch < 0) {
    bmp_info.bmiHeader.biHeight *= -1;
    int32_t offset = pitch * (height - 1);
    pBits += offset;
  }

  auto pFile = fopen(filename, "w");
  if (nullptr == pFile) {
    std::cerr << "couldn't open file: " << filename << "!" << std::endl;
    return -1;
  }

  if (fwrite(&header, 1, sizeof(BITMAPFILEHEADER), pFile) !=
      sizeof(BITMAPFILEHEADER)) {
    fclose(pFile);
    return -1;
  }

  if (fwrite(&bmp_info, 1, infoSize, pFile) != infoSize) {
    fclose(pFile);
    return -1;
  }

  if (4 == bpp) {
    for (uint32_t offset = 0; offset < bmp_info.bmiHeader.biSizeImage;
         offset += 3) {
      if (fwrite(pBits + (4 * (offset / 3)), 1, 3, pFile) != 3) {
        fclose(pFile);
        return -1;
      }
    }
  } else {
    if (fwrite(pBits, 1, bmp_info.bmiHeader.biSizeImage, pFile) !=
        bmp_info.bmiHeader.biSizeImage) {
      fclose(pFile);
      return -1;
    }
  }

  fclose(pFile);  

  return 0;  
}