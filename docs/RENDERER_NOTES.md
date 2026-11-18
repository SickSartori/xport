# Renderer notes

What the renderer does on purpose, what was tried and dropped, and how the defects were found. For whoever changes
`renderer_d3d11.cpp`, `nv2a_vsh.cpp` or `nv2a_combiner.cpp` next. Written November 2026 (xport 0.2.3).

## References

- **xemu** (`hw/xbox/nv2a/pgraph/glsl/vsh-prog.c`, `vsh.c`): the NV2A emulated in OpenGL. The first place to look for
  how the hardware behaves.
- **SantiBros/SpiderMan2-2004-NX**: another static recompilation of Spider-Man 2 (Xbox), to the Switch with OpenGL.
  `gl/glsl.cpp`, `docs/TECHNICAL.md` and its `CHANGELOG.md` describe the same game's GPU habits (shadow volumes,
  multisample, depth read as colour).

## Vertex positions

- A vertex program leaves its position in **screen space** (pixels, z already divided by w, w kept). The vertex
  shaders multiply x, y and z back by w and let Direct3D clip in homogeneous space (`clip_position`). xemu does the
  same. Tried and dropped: drawing the colourless draws (stencil shadow volumes) with w = 1, as if the NV2A
  rasterized screen positions without clipping. On a rooftop it drew light wedges where the current way is right.
- Screen x and y are cut to **1/16 pixel**, the NV2A's 4 fractional bits (xemu's `roundScreenCoords`), in the shaders
  and in `to_render_vertex` of each port (the CPU's vertices). Nearly equal vertices of two meshes land on the same spot.
- The vertex programs' registers and the final position are **`precise`** (as SpiderMan2-NX's GLSL): the same mesh
  drawn by two programs (a second pass with LEQUAL and no depth write, a stencil shadow volume) gets the same depth.
- A program still compiling (100 to 400 ms the first time) is drawn by the CPU meanwhile: for a few frames a scene can
  mix CPU and GPU vertices.
- Vertices behind the camera reach the near plane clipped as on every renderer above. A camera inside a roof shows
  the street through it (Spider-Man 2, a rooftop with the camera low): not changed.

## Depth and stencil

- **Polygon offset**: enabled by `0x338` (`SET_POLY_OFFSET_FILL_ENABLE`), not `0x380` (the line width). The slope is
  per pixel of the target, so it is multiplied by the internal resolution (`g_scale`).
- **Multisample 2X/4X**: the game's screen space is half as wide (and high); the ports scale it back with
  `pixels_x`/`pixels_y` (Spider-Man 2 fills its shadow volumes so, and darkens the screen in halves).
- **A depth buffer read as colour** (`refresh_depth_colour`): A, R, G are the depth's three bytes, high first; B (the
  stencil) is left 0. Spider-Man 2 fades its shadows this way. **Colour drawn into a depth buffer**
  (`colour_to_depth`): Midnight Club 3's headlight volumes.
- **Signed blend equations** (`0xF005`, `0xF006`) switch the target to float (`make_float_target`). Tried and dropped:
  clamping that float target to 0..1 after every other blend, as an 8-bit target would. No visible gain in Midnight
  Club 3, and a copy and a full-target pass per blend.

## Texture lookups

- Forced anisotropy only replaces mipmapped linear filters; nearest stays nearest (the depth reads, the ramps).
- **Dependent lookups** (stage modes AR and GB, `dependent()` in the generated pixel shader): a byte b reads texel
  floor(b * size / 255) at its centre, as the NV2A's coordinate b / 255 with a nearest filter, and the renderer gives
  them a linear filter. For AR the next byte, green, is the fraction between rows.
- **After a depth read as colour** (`texture_scale[stage].z` set by the renderer, `dependent_depth()`), the lookup is
  averaged over 8 distances from about half to twice the pixel's own: Spider-Man 2's shadow fade, a short span that
  showed as a light arc around the camera, is spread over about three times the distance. A port's choice.

## Effects

- **Ambient occlusion**: the sample pairs across an outline are left out of the average, not counted as open; counted
  as open they lit a pale box around Midnight Club 3's pedestrians and lamp posts at night. The "before the effects"
  frame dump already contains the AO (it is applied when the 3D scene ends): look at `AO_VIEW` before ruling it out.

## Shaders

- Combiner (pixel) shaders not in the cache are compiled on two helper threads; a draw waits for its shader while the
  frame's waits stay within 8 ms (`kCompileWaitMs`), and is left out past that until the shader is ready (the log's
  "draws waited for a shader"). Alternating runs with an empty cache, Midnight Club 3: 31 and 33 fps synchronous
  against 43 and 42 in the 10 s of entering the city, 28 and 31 against 33 and 39 at the start; nothing missing seen
  in the frames of the start. Vertex programs compile on a thread of their own while the CPU draws with them. Both are kept in `shader_cache` and read
  from there the next time. With an empty cache: Midnight Club 3's start and city about 70 combiners and 12 programs,
  Spider-Man 2's city about 35 and 30.
- Compiled alone, one after the other (Spider-Man 2's sources, `d3dcompiler_47`): combiners 6 ms on average and 33 at
  most, vertex programs 120 ms on average and 630 at most. Optimisation level 0 is no faster than level 1 (it seemed so
  in game, where the runs differ by more than that: measure compiles outside the game).

## Finding a defect

- **F9** in the game opens the debug window for one second (one line per draw). With `<PREFIX>DUMP_DRAW=<texture>`
  the first matching draw of the window is saved before and after (`dump-before/after-NNNNN.bmp`); outside the window
  the vertices stay on the GPU, so the game runs as usual until the key. `<PREFIX>DEBUG_PROBE=x,y` logs every draw
  that changes a pixel. The window's draws use the CPU's vertices.
- To test a change, alternate it **within one run** (a temporary switch on `GetTickCount64() / 700`, logged when it
  flips) and compare frames dumped close together while the camera is still: two runs of the same input script end in
  different places. Two frames of the same phase give the noise floor.
- A target dumped at the end of a frame can already hold half of the next one (the main surfaces alternate): for what
  a pass reads, dump its draw (`DUMP_DRAW` saves the texture it samples too).
- Shadows in Spider-Man 2: before/after of the fade quad (`<PREFIX>DUMP_DRAW=017E4000`, the depth buffer it reads)
  give the darkening per pixel (after / before): about 0.6 in full shadow, 1 where there is none.
