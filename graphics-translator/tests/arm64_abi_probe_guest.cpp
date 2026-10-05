#include "arm64_abi_probe.h"
#include "../../src/winemetal/winemetal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int gpu_probe(HMODULE library) {
#define LOAD_METAL(name) \
  auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(library, #name)); \
  if (!name) return 30
  LOAD_METAL(NSObject_release);
  LOAD_METAL(NSAutoreleasePool_alloc_init);
  LOAD_METAL(WMTCopyAllDevices);
  LOAD_METAL(NSArray_count);
  LOAD_METAL(NSArray_object);
  LOAD_METAL(MTLDevice_newCommandQueue);
  LOAD_METAL(MTLDevice_newBuffer);
  LOAD_METAL(MTLDevice_newSharedEvent);
  LOAD_METAL(MTLCommandQueue_commandBuffer);
  LOAD_METAL(MTLCommandBuffer_blitCommandEncoder);
  LOAD_METAL(MTLBlitCommandEncoder_encodeCommands);
  LOAD_METAL(MTLCommandEncoder_endEncoding);
  LOAD_METAL(MTLCommandBuffer_encodeSignalEvent);
  LOAD_METAL(MTLCommandBuffer_commit);
  LOAD_METAL(MTLCommandBuffer_waitUntilCompleted);
  LOAD_METAL(MTLCommandBuffer_status);
  LOAD_METAL(MTLSharedEvent_signaledValue);
#undef LOAD_METAL
  obj_handle_t pool = NSAutoreleasePool_alloc_init();
  obj_handle_t devices = WMTCopyAllDevices();
  if (!devices || !NSArray_count(devices)) return 31;
  obj_handle_t device = NSArray_object(devices, 0);
  obj_handle_t queue = MTLDevice_newCommandQueue(device, 2);
  WMTBufferInfo info{64, static_cast<WMTResourceOptions>(WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE), {}, 0};
  obj_handle_t buffer = MTLDevice_newBuffer(device, &info);
  obj_handle_t event = MTLDevice_newSharedEvent(device);
  if (!queue || !buffer || !event || !info.memory.get()) return 32;
  std::memset(info.memory.get(), 0, 64);
  obj_handle_t command = MTLCommandQueue_commandBuffer(queue);
  obj_handle_t encoder = MTLCommandBuffer_blitCommandEncoder(command);
  if (!command || !encoder || MTLSharedEvent_signaledValue(event) != 0) return 33;
  wmtcmd_blit_fillbuffer fill{};
  fill.type = WMTBlitCommandFillBuffer;
  fill.buffer = buffer;
  fill.length = 64;
  fill.value = 0xa7;
  wmtcmd_blit_fillbuffer tail = fill;
  fill.length = 32;
  tail.offset = 32;
  tail.length = 32;
  fill.next.set(&tail);
  tail.next.set(nullptr);
#if defined(__i386__)
  uint64_t base = WMTGetGuestWindowBase();
  if ((base & 0xffffffffull) || !base ||
      info.memory.high_part != (base >> 32) || fill.next.high_part != (base >> 32) ||
      fill.next.get() != &tail || tail.next.ptr || tail.next.high_part) return 36;
  WMTConstMemoryPointer encoded{};
  encoded.set(info.memory.get());
  if (encoded.get() != info.memory.get() || encoded.high_part != (base >> 32)) return 37;
  WMTMemoryPointer native_pointer{};
  native_pointer.high_part = static_cast<uint32_t>((base >> 32) + 1);
  native_pointer.ptr = reinterpret_cast<void *>(0x1234);
  if (native_pointer.get_accessible_or_null()) return 38;
  std::printf("{\"memory_codec\":\"PASS\",\"base\":%llu,\"guest_memory\":%u,\"native_memory\":%llu,\"native_next\":%llu,\"null_next\":0}\n",
              static_cast<unsigned long long>(base), static_cast<unsigned>(reinterpret_cast<uintptr_t>(info.memory.get())),
              static_cast<unsigned long long>((uint64_t(info.memory.high_part) << 32) | reinterpret_cast<uintptr_t>(info.memory.ptr)),
              static_cast<unsigned long long>((uint64_t(fill.next.high_part) << 32) | reinterpret_cast<uintptr_t>(fill.next.ptr)));
#endif
  MTLBlitCommandEncoder_encodeCommands(encoder, reinterpret_cast<const wmtcmd_base *>(&fill));
  MTLCommandEncoder_endEncoding(encoder);
  MTLCommandBuffer_encodeSignalEvent(command, event, 0x123456789ull);
  MTLCommandBuffer_commit(command);
  MTLCommandBuffer_waitUntilCompleted(command);
  if (MTLCommandBuffer_status(command) != WMTCommandBufferStatusCompleted ||
      MTLSharedEvent_signaledValue(event) != 0x123456789ull) return 34;
  const auto *bytes = static_cast<const unsigned char *>(info.memory.get());
  for (unsigned index = 0; index < 64; ++index)
    if (bytes[index] != 0xa7) return 35;
  NSObject_release(event);
  NSObject_release(buffer);
  NSObject_release(queue);
  NSObject_release(devices);
  NSObject_release(pool);
  std::printf("{\"gpu_checks\":\"PASS\",\"readback_bytes\":64,\"expected_byte\":167,\"fence_value\":4886718345}\n");
  return 0;
}

class Callback final : public AbiCallback {
  ULONG refs_ = 1;
public:
  unsigned calls = 0;
  uint64_t expected_address = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!IsEqualIID(iid, IID_IUnknown)) return E_NOINTERFACE;
    *object = static_cast<AbiCallback *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
  HRESULT STDMETHODCALLTYPE Check(AbiPacket packet, uint64_t *reply) override {
    if (!reply || packet.tag != 0x41424931 || packet.delta != -37 ||
        packet.token != 0xfedcba9876543210ull || packet.address != expected_address || packet.value != 6.25)
      return E_INVALIDARG;
    ++calls;
    *reply = packet.token ^ packet.address;
    return S_OK;
  }
};

int main(int argc, char **argv) {
  auto image = [](HMODULE module) {
    if (!module) module = GetModuleHandleW(nullptr);
#if defined(__i386__)
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(reinterpret_cast<const unsigned char *>(module) + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
      return false;
#endif
    char path[MAX_PATH];
    if (!GetModuleFileNameA(module, path, sizeof(path))) return false;
    std::printf("ABI_IMAGE:%s\n", path);
    return true;
  };
  if (!image(nullptr)) return 3;
  std::printf("{\"guest_entry\":true,\"pointer_bits\":%u}\n", static_cast<unsigned>(sizeof(void *) * 8));
  std::fflush(stdout);
  if (argc != 4 && (argc != 5 || std::strcmp(argv[4], "--gpu"))) {
    std::fprintf(stderr, "usage: abi-guest.exe expected-host-major minor patch [--gpu]\n");
    return 2;
  }
  HMODULE library = LoadLibraryW(L"abi-callee.dll");
  if (!library) return 10;
  if (!image(library)) return 4;
  auto create = reinterpret_cast<AbiCreate>(GetProcAddress(library, "AbiProbeCreate"));
  auto arithmetic = reinterpret_cast<AbiArithmetic>(GetProcAddress(library, "AbiProbeArithmetic"));
  if (!create || !arithmetic) return 11;
  Callback callback;
  AbiPacket packet{0x41424931, -37, 0xfedcba9876543210ull,
                   static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&callback)), 6.25};
  callback.expected_address = packet.address;
  if (arithmetic(packet.token, packet.delta, packet.value, &callback) != -30.75) return 12;
  AbiObject *object = nullptr;
  if (FAILED(create(&object)) || !object) return 13;
  {
    auto retained = object->Retain(0x41424931, object);
    if (retained.ptr() != object || object->AddRef() != 3 || object->Release() != 2) return 18;
  }
  IUnknown *identity = nullptr;
  HRESULT queried = object->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&identity));
  if (FAILED(queried) || identity != static_cast<IUnknown *>(object)) return 14;
  if (identity->Release() != 1) return 16;
  AbiPacket reply{};
  if (FAILED(object->Exchange(packet, &callback, &reply)) || callback.calls != 1 ||
      reply.tag != packet.tag || reply.delta != packet.delta || reply.token != (packet.token ^ packet.address) ||
      reply.address != packet.address || reply.value != -30.75)
    return 15;
  if (object->Release() != 0) return 17;
  FreeLibrary(library);

  // Actual owned Wine Unix-call 111; this is not a local stand-in for the bridge.
  HMODULE metal = LoadLibraryW(L"winemetal.dll");
  if (!metal) return 20;
  if (!image(metal)) return 5;
  using OSVersion = decltype(&WMTGetOSVersion);
  auto version = reinterpret_cast<OSVersion>(GetProcAddress(metal, "WMTGetOSVersion"));
  if (!version) return 21;
  uint64_t major = 0, minor = 0, patch = 0;
  version(&major, &minor, &patch);
  if (major != std::strtoull(argv[1], nullptr, 10) || minor != std::strtoull(argv[2], nullptr, 10) ||
      patch != std::strtoull(argv[3], nullptr, 10)) return 22;
  std::printf("{\"abi_checks\":\"PASS\",\"pointer_bits\":%u,\"packet_bytes\":%u,"
              "\"callback_calls\":%u,\"unix_os_version\":[%llu,%llu,%llu],"
              "\"strict_no_rosetta_proven\":false}\n",
              static_cast<unsigned>(sizeof(void *) * 8), static_cast<unsigned>(sizeof(AbiPacket)), callback.calls,
              static_cast<unsigned long long>(major), static_cast<unsigned long long>(minor),
              static_cast<unsigned long long>(patch));
  std::fflush(stdout);
  if (argc == 5) {
    int gpu_result = gpu_probe(metal);
    if (gpu_result) return gpu_result;
  }
  FreeLibrary(metal);
  return 0;
}
