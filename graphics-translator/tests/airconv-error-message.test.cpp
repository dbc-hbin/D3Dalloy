// Link against the production libairconv.a and its native LLVM dependencies.
#include "airconv_public.h"
#include <array>
#include <cstdio>
#include <cstring>

static bool check(sm50_error_t error) {
  for (size_t capacity : {size_t(0), size_t(1), size_t(5), size_t(256)}) {
    std::array<char, 257> buffer;
    buffer.fill('?');
    const auto copied = SM50GetErrorMessage(error, buffer.data(), capacity);
    if (buffer[capacity] != '?' || (!capacity && copied) ||
        (capacity && (copied >= capacity || buffer[copied] != '\0' || copied != std::strlen(buffer.data())))) {
      std::fprintf(stderr, "ERROR_MESSAGE_FAIL capacity=%zu copied=%zu\n", capacity, copied);
      return false;
    }
  }
  if (SM50GetErrorMessage(error, nullptr, 256) != 0) return false;
  const auto message = SM50GetErrorMessageString(error);
  if (message.empty() || message.find('\0') != std::string::npos) return false;
  // Wine's logger consumes a C string: an embedded NUL would hide this newline
  // and accumulate repeated diagnostics until STATUS_BUFFER_OVERFLOW.
  const auto line = "err:   " + message + '\n';
  return std::strlen(line.c_str()) == line.size() && std::strchr(line.c_str(), '\n');
}

int main() {
  // A valid DXBC chunk table with an empty DXIL program reaches reportError(),
  // whose native buffer contains an explicit NUL. No shader fixture is needed.
  const uint32_t dxil[] = {0x43425844, 0, 0, 0, 0, 1, 44, 1, 36, 0x4c495844, 0};
  sm50_shader_t shader{};
  sm50_error_t error{};
  if (!SM50Initialize(dxil, sizeof(dxil), &shader, nullptr, &error) || shader || !error) return 1;
  bool okay = check(error);
  SM50FreeError(error);

  // The legacy error path has no explicit terminator in its native buffer.
  error = nullptr;
  if (!SM50Initialize(nullptr, 0, nullptr, nullptr, &error) || !error) return 1;
  okay = check(error) && okay;
  SM50FreeError(error);
  okay = check(nullptr) && okay;
  if (!okay) return 1;
  std::puts("ERROR_MESSAGE_PASS dxil=1 legacy=1 null_error=1 capacities=4");
  return 0;
}
