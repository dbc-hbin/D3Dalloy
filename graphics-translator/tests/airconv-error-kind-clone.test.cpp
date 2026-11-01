// Link against production airconv; no error API mocks.
#include "dxil_converter.hpp"
#include "dxbc_root_signature.hpp"
#include "../../src/winemetal/airconv_thunks.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstdio>
#include <cstring>

static_assert(unix_sm50_get_error_kind == 319 && unix_sm50_clone_error == 320);
static_assert(sizeof(sm50_get_error_kind_params) == 16 && sizeof(sm50_clone_error_params) == 24);
static_assert(offsetof(sm50_clone_error_params, clone) == 8 && offsetof(sm50_clone_error_params, ret) == 16);

static bool clones(sm50_error_t original, SM50_ERROR_KIND kind, bool original_first) {
  const auto message = SM50GetErrorMessageString(original);
  sm50_error_t copy{}, second{};
  if (SM50GetErrorKind(original) != kind || SM50CloneError(original, &copy) || !copy || copy == original ||
      SM50CloneError(copy, &second) || !second || second == copy)
    return false;
  if (original_first) SM50FreeError(original);
  else SM50FreeError(copy);
  const auto survivor = original_first ? copy : original;
  bool okay = SM50GetErrorKind(survivor) == kind && SM50GetErrorMessageString(survivor) == message &&
      SM50GetErrorKind(second) == kind && SM50GetErrorMessageString(second) == message;
  char short_message[5];
  okay &= SM50GetErrorMessage(second, short_message, sizeof(short_message)) == 4 && short_message[4] == '\0';
  SM50FreeError(survivor);
  okay &= SM50GetErrorMessageString(second) == message;
  SM50FreeError(second);
  return okay;
}

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  sm50_error_t sentinel = (sm50_error_t)(uintptr_t)1;
  if (SM50GetErrorKind(nullptr) != SM50_ERROR_UNKNOWN || SM50CloneError(nullptr, &sentinel) || sentinel ||
      !SM50CloneError(nullptr, nullptr)) return 1;
  auto bytes = llvm::MemoryBuffer::getFile(argv[1]);
  if (!bytes) return 1;
  auto parsed = d3dalloy::dxil::ModuleInfo::Parse((*bytes)->getBufferStart(), (*bytes)->getBufferSize());
  if (!parsed) { llvm::logAllUnhandledErrors(parsed.takeError(), llvm::errs()); return 1; }
  auto &module = *(*parsed)->module;
  const std::string name(4096, 'R');
  auto *resources = module.getNamedMetadata("dx.resources")->getOperand(0);
  for (auto &list : resources->operands())
    if (auto *entries = llvm::dyn_cast_or_null<llvm::MDNode>(list.get()))
      for (auto &entry : entries->operands())
        llvm::cast<llvm::MDNode>(entry.get())->replaceOperandWith(2, llvm::MDString::get(module.getContext(), name));
  llvm::SmallVector<char, 0> bitcode;
  llvm::raw_svector_ostream stream(bitcode);
  llvm::WriteBitcodeToFile(module, stream);
  std::vector<uint8_t> container(44 + bitcode.size());
  const uint32_t header[] = {0x43425844, 0, 0, 0, 0, 1, uint32_t(container.size()), 1, 36,
                             0x4c495844, uint32_t(bitcode.size())};
  std::memcpy(container.data(), header, sizeof(header));
  std::memcpy(container.data() + sizeof(header), bitcode.data(), bitcode.size());
  sm50_shader_t shader{};
  sm50_error_t error{};
  if (SM50Initialize(container.data(), container.size(), &shader, nullptr, &error)) {
    llvm::errs() << SM50GetErrorMessageString(error) << '\n'; return 1;
  }
  d3dalloy::RawRootSignatureDesc root{D3D_ROOT_SIGNATURE_VERSION_1_1, 0, 0, 0, 0, 0};
  SM50_SHADER_ROOT_SIGNATURE_DATA args{};
  args.type = SM50_SHADER_ROOT_SIGNATURE;
  args.bytecode = &root;
  args.bytecode_length = sizeof(root);
  bool okay = true;
  for (bool original_first : {false, true}) {
    sm50_bitcode_t output{};
    if (!SM50Compile(shader, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&args, "main", &output, &error) || !error || output)
      return 1;
    const auto message = SM50GetErrorMessageString(error);
    okay &= message.size() > 4096 && message.find(name) != std::string::npos;
    okay &= clones(error, SM50_ERROR_ROOT_BINDING, original_first);
    error = nullptr;
    if (!SM50Initialize(nullptr, 0, nullptr, nullptr, &error) || !error) return 1;
    okay &= clones(error, SM50_ERROR_UNKNOWN, original_first);
  }
  SM50Destroy(shader);
  if (!okay) return 1;
  std::puts("ERROR_KIND_CLONE_PASS typed=1 unknown=1 long_message=1 clone_of_clone=1 free_orders=2 abi=1");
  return 0;
}
