#include "common.hpp"
#include <vector>

namespace cocogfx {

int LoadBMP(const char *filename, 
            std::vector<uint8_t> &pixels,
            uint32_t *width,
            uint32_t *height,
            uint32_t *bpp);

int SaveBMP(const char *filename, 
            const uint8_t* pixels, 
            uint32_t width,
            uint32_t height, 
            uint32_t bpp,
            int32_t pitch);

}