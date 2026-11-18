#pragma once

// Setting sharp_2d: the textures of flat draws over the 3D scene (the HUD's icons and letters) made two
// or four times larger with their edges kept, so that they stay sharp at a high resolution. The game's pictures are
// small and were shown at 480 lines; enlarged by the graphics card they turn soft and show the 4x4 blocks of their
// compression. Nothing is added that is not there: edges come out clean, details stay as they were.

#include <vector>

#include "pc/nv2a_texture.h"

namespace xport::pc {

// The enlarged texture's levels (BGRA8, the top one first, down to 1x1), or none when it is not worth it or the
// layout is unknown. `scene`: a texture of the 3D scene (setting sharp_textures): it repeats across its edges, and is
// enlarged less than the HUD's; `lower` are the game's own lower levels, kept below the enlarged ones.
std::vector<TextureImage> upscale_texture(const TextureImage& image, bool scene,
                                          const std::vector<TextureImage>& lower = {});
inline std::vector<TextureImage> upscale_2d_texture(const TextureImage& image) { return upscale_texture(image, false); }

}  // namespace xport::pc
