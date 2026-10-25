#pragma once
#include <stdint.h>

/* The configurator finds this block in the template DLL by its magic and
   overwrites the fields, so the layout is a file format: append only. */
#define DEMAKE_CONFIG_MAGIC "ANYDEMAKE-CFG-v1"

enum demake_sampler_mode {
    DEMAKE_SAMPLER_KEEP = 0,
    DEMAKE_SAMPLER_POINT = 1,
    DEMAKE_SAMPLER_BILINEAR = 2,
};

typedef struct demake_config {
    char magic[16];
    uint32_t size;
    uint32_t mip_skip;            /* top mip levels dropped, 0-4 */
    uint32_t mip_skip_default;    /* also D3D11_USAGE_DEFAULT textures (riskier) */
    uint32_t min_texture_size;    /* never shrink below this width or height */
    uint32_t sampler_mode;        /* enum demake_sampler_mode */
    uint32_t pixelate;            /* block size in pixels at Present, 0/1 = off */
    uint32_t color_bits;          /* bits per channel at Present, 0 = off */
    uint32_t log;                 /* write anydemake.log next to the DLL */
} demake_config;
