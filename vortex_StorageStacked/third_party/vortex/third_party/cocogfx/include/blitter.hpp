//
// Copyright (c) Blaise Tine.  All rights reserved.
//
//
// Use of this sample source code is subject to the terms of the Microsoft
// license agreement under which you licensed this sample source code. If
// you did not accept the terms of the license agreement, you are not
// authorized to use this sample source code. For the terms of the license,
// please see the license agreement between you and Microsoft or, if applicable,
// see the LICENSE.RTF on your install media or the root of your tools
// installation.
// THE SAMPLE SOURCE CODE IS PROVIDED "AS IS", WITH NO WARRANTIES OR
// INDEMNITIES.
//
#pragma once

#include "common.hpp"
#include "format.hpp"
#include  <vector>

namespace cocogfx {

int CopyBuffers(std::vector<uint8_t>& dst_pixels,
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
                uint32_t height);

int ConvertImage(std::vector<uint8_t>& dst_pixels,
                 ePixelFormat dst_format,
                 const uint8_t* src_pixels,
                 ePixelFormat src_format,
                 uint32_t src_width,
                 uint32_t src_height,                   
                 int32_t src_pitch);

int GenerateMipmaps(std::vector<uint8_t>& dst_pixels,
                    std::vector<uint32_t>& mip_offsets,
                    const uint8_t* src_pixels,
                    ePixelFormat src_format,
                    uint32_t src_width,
                    uint32_t src_height,
                    int32_t src_pitch);

}