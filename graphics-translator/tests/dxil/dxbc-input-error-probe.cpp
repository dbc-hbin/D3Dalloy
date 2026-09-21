#include "airconv_public.h"
#include "DXBCParser/d3d12tokenizedprogramformat.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

using microsoft::D3D10_SB_OPCODE_TYPE;

constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
         (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

std::vector<uint8_t> load(const char *path) {
  std::ifstream file(path, std::ios::binary);
  return file ? std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {}) : std::vector<uint8_t>{};
}

bool corruptVertexInput(std::vector<uint8_t> &container) {
  if (container.size() < 8 * sizeof(uint32_t))
    return false;
  auto *words = reinterpret_cast<uint32_t *>(container.data());
  if (words[0] != fourcc('D', 'X', 'B', 'C'))
    return false;
  const uint32_t chunks = words[7];
  if (container.size() < (8ull + chunks) * sizeof(uint32_t))
    return false;
  for (uint32_t i = 0; i < chunks; ++i) {
    const uint32_t offset = words[8 + i];
    if (offset + 4 * sizeof(uint32_t) > container.size())
      return false;
    auto *chunk = reinterpret_cast<uint32_t *>(container.data() + offset);
    if (chunk[0] != fourcc('S', 'H', 'D', 'R') && chunk[0] != fourcc('S', 'H', 'E', 'X'))
      continue;
    const uint32_t dwords = chunk[1] / sizeof(uint32_t);
    if (dwords < 3 || offset + 2 * sizeof(uint32_t) + chunk[1] > container.size())
      return false;
    auto *code = chunk + 2;
    for (uint32_t at = 2; at < dwords;) {
      const uint32_t token = code[at];
      const auto opcode = DECODE_D3D10_SB_OPCODE_TYPE(token);
      const uint32_t length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
      if (!length || at + length > dwords)
        return false;
      if (opcode == microsoft::D3D10_SB_OPCODE_DCL_INPUT) {
        code[at + 1] &= ~D3D10_SB_OPERAND_TYPE_MASK;
        code[at + 1] |= ENCODE_D3D10_SB_OPERAND_TYPE(microsoft::D3D10_SB_OPERAND_TYPE_OUTPUT);
        return true;
      }
      at += length;
    }
  }
  return false;
}

bool initialize(const std::vector<uint8_t> &bytecode, bool expectSuccess, const char *label) {
  sm50_shader_t shader = nullptr;
  sm50_error_t error = nullptr;
  const int result = SM50Initialize(bytecode.data(), bytecode.size(), &shader, nullptr, &error);
  const bool ok = expectSuccess ? result == 0 && shader != nullptr : result != 0 && shader == nullptr && error != nullptr;
  std::printf("DXBC_INPUT_ERROR_PROBE %s result=%d shader=%p diagnostic=%s\n", label, result, shader,
              error ? SM50GetErrorMessageString(error).c_str() : "none");
  if (shader)
    SM50Destroy(shader);
  if (error)
    SM50FreeError(error);
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: dxbc-input-error-probe <valid-vs.dxbc>\n");
    return 2;
  }
  const auto valid = load(argv[1]);
  auto invalid = valid;
  if (valid.empty() || !corruptVertexInput(invalid)) {
    std::fprintf(stderr, "DXBC_INPUT_ERROR_PROBE_FAIL fixture has no mutable vertex input declaration\n");
    return 1;
  }
  if (!initialize(invalid, false, "invalid-rejected"))
    return 1;
  if (!initialize(valid, true, "process-survived"))
    return 1;
  std::puts("DXBC_INPUT_ERROR_PROBE_PASS");
  return 0;
}
