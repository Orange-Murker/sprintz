//
//  sprintz_8b.hpp
//  Compress
//
//  Created by DB on 12/4/17.
//  Copyright © 2017 D Blalock. All rights reserved.
//

#ifndef sprintz_8b_hpp
#define sprintz_8b_hpp

#include <stdint.h>

int64_t sprintz_compress_delta_8b(const uint8_t* src, uint32_t len, int8_t* dest,
                                  uint16_t ndims, bool write_size=true);
int64_t sprintz_decompress_delta_8b(const int8_t* src, uint8_t* dest);

int64_t sprintz_compress_xff_8b(const uint8_t* src, uint32_t len, int8_t* dest,
                                  uint16_t ndims, bool write_size=true);
int64_t sprintz_decompress_xff_8b(const int8_t* src, uint8_t* dest);

#endif /* sprintz_8b_hpp */
