// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

namespace owned_rr {
constexpr unsigned Width = 128, Height = 128, Frames = 8;
struct Frame {
  std::vector<uint16_t> color, motion, diffuse, specular, normals;
  std::vector<float> depth, roughness, hit;
  float world[16], projection[16];
  float jitterX, jitterY;
  bool reset;
};
inline bool saveInputs(const char *directory, const char *prefix, unsigned frame, const Frame &scene,
                       const void *world, const void *projection) {
  char path[4096]; std::snprintf(path,sizeof(path),"%s/%s-%u.inputs",directory,prefix,frame);
  FILE *file = std::fopen(path,"wb"); if (!file) return false;
  const uint32_t reset = scene.reset;
  const struct { const void *bytes; size_t length; } chunks[] = {
    {scene.color.data(),scene.color.size()*2}, {scene.motion.data(),scene.motion.size()*2},
    {scene.diffuse.data(),scene.diffuse.size()*2}, {scene.specular.data(),scene.specular.size()*2},
    {scene.normals.data(),scene.normals.size()*2}, {scene.depth.data(),scene.depth.size()*4},
    {scene.roughness.data(),scene.roughness.size()*4}, {scene.hit.data(),scene.hit.size()*4},
    {world,64}, {projection,64}, {&scene.jitterX,4}, {&scene.jitterY,4}, {&reset,4}};
  bool valid = true;
  for (const auto &chunk : chunks)
    if (std::fwrite(chunk.bytes,1,chunk.length,file) != chunk.length) valid = false;
  return std::fclose(file) == 0 && valid;
}
inline Frame makeFrame(unsigned frame) {
  Frame result;
  result.color.resize(Width * Height * 4);
  result.motion.resize(Width * Height * 2);
  result.diffuse.resize(Width * Height * 4);
  result.specular.resize(Width * Height * 4);
  result.normals.resize(Width * Height * 4);
  result.depth.resize(Width * Height);
  result.roughness.resize(Width * Height);
  result.hit.resize(Width * Height);
  // NGX row-major left-multiplication matrices. Nonidentity rotation,
  // translation and perspective distinguish a byte copy from a transpose.
  const float world[16] = {.8f,0,.6f,0, 0,1,0,0, -.6f,0,.8f,0, .125f,-.25f,.5f,1};
  const float projection[16] = {1.5f,0,0,0, 0,1.25f,0,0, .125f,-.0625f,0,1, 0,0,.1f,0};
  for (unsigned i = 0; i < 16; ++i) { result.world[i] = world[i]; result.projection[i] = projection[i]; }
  result.world[12] += frame * .125f;
  result.projection[8] += frame * .0625f;
  result.jitterX = frame & 1 ? .25f : -.25f;
  result.jitterY = frame & 2 ? .125f : -.125f;
  result.reset = frame == 1 || frame == 5;
  for (unsigned y = 0; y < Height; ++y) {
    for (unsigned x = 0; x < Width; ++x) {
      const unsigned index = y * Width + x;
      const unsigned sceneX = (x + frame * 2) % Width;
      const bool tile = ((sceneX / 16) ^ (y / 24)) & 1;
      const bool noise = ((x * 17 + y * 31 + frame * 13) % 7) < 3;
      const uint16_t noisy = noise ? 0x3800 : 0x3400; // .5 or .25.
      const uint16_t red = tile ? 0x3a00 : 0x3600; // .75 or .375.
      result.color[index * 4] = red;
      result.color[index * 4 + 1] = noisy;
      result.color[index * 4 + 2] = tile ? noisy : 0x3000;
      result.color[index * 4 + 3] = 0x3c00;
      result.diffuse[index * 4] = red;
      result.diffuse[index * 4 + 1] = 0x3600;
      result.diffuse[index * 4 + 2] = tile ? 0x3800 : 0x3000;
      result.diffuse[index * 4 + 3] = 0x3c00;
      result.specular[index * 4] = result.specular[index * 4 + 1] = result.specular[index * 4 + 2] = 0x3400;
      result.specular[index * 4 + 3] = 0x3c00;
      result.normals[index * 4] = 0;
      result.normals[index * 4 + 1] = 0;
      result.normals[index * 4 + 2] = 0x3c00; // World-space +Z.
      result.normals[index * 4 + 3] = tile ? 0x3800 : 0x3400; // Packed roughness.
      result.roughness[index] = tile ? .5f : .25f;
      result.depth[index] = .1f / (tile ? 2.f : 4.f); // Reversed infinite projection.
      result.hit[index] = tile ? 1.5f : 3.f;
      result.motion[index * 2] = 0x4000; // +2 input pixels backward camera pan.
      result.motion[index * 2 + 1] = 0;
    }
  }
  return result;
}
} // namespace owned_rr
