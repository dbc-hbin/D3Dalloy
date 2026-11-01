// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "d3d12.h"
#include "dxgi.h"
#include "winemetal.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>

namespace dxmt::sharing {

enum class Kind : uint32_t { Heap = 1, Buffer = 2, Texture = 3, Fence = 4 };
constexpr uint64_t kPayloadOffset = 0x10000;
constexpr uint32_t kSlotCount = 64;
constexpr uint64_t kMagic = 0x5245485354584d44ull;     // "DMXTSHER"
constexpr uint64_t kLinkMagic = 0x4b4e494c54584d44ull; // "DMXTLINK"
constexpr uint32_t kVersion = 3;
constexpr uint32_t kCastFormatWordCount = 4;
constexpr uint32_t kTextureStorageSwizzledVolume = 1;
/* Object-namespace prefix of a main shared section; distinct from upstream DXMT's so the two never open each other. */
constexpr wchar_t kSharePrefix[] = L"Local\\d3dalloy.share.";

struct Slot {
  uint32_t pid;
  uint32_t reserved;
  char name[64];
};

struct AdapterMetadata {
  LUID luid;
  char tag[33];
};

struct Header {
  uint64_t magic;
  uint32_t version;
  Kind kind;
  uint8_t token[16];
  uint64_t payload_offset;
  uint64_t payload_size;
  uint64_t identity;
  D3D12_HEAP_DESC heap_desc;
  D3D12_HEAP_PROPERTIES heap_props;
  D3D12_HEAP_FLAGS heap_flags;
  D3D12_FENCE_FLAGS fence_flags;
  D3D12_RESOURCE_DESC resource_desc;
  volatile LONG lock;
  uint32_t reserved;
  Slot slots[kSlotCount];
  // v2 appends identity without moving the v1 payload, descriptors or rendezvous slots.
  AdapterMetadata adapter;
  uint32_t format_list_explicit;
  D3D12_BARRIER_LAYOUT initial_layout;
  uint32_t texture_storage;
  uint64_t castable_formats[kCastFormatWordCount];
};
static_assert(sizeof(Header) <= kPayloadOffset);
static_assert(offsetof(Header, token) == 16);
static_assert(offsetof(Header, slots) == 200);
static_assert(offsetof(Header, adapter) == 4808);
static_assert(offsetof(Header, format_list_explicit) == 4852);
static_assert(offsetof(Header, castable_formats) == 4864);
static_assert(offsetof(Header, texture_storage) == 4860);
static_assert(sizeof(Header) == 4896);
static_assert(sizeof(AdapterMetadata) == 44);

struct LinkHeader {
  uint64_t magic;
  uint8_t token[16];
  WCHAR section_name[80];
  uint32_t version;
  Kind kind;
  // The named NT handle retains this identity even after its creator releases the COM object.
  AdapterMetadata adapter;
};
static_assert(offsetof(LinkHeader, token) == 8);
static_assert(offsetof(LinkHeader, version) == 184);

inline std::array<char, 33>
TokenHex(const uint8_t token[16]) {
  static const char digits[] = "0123456789abcdef";
  std::array<char, 33> hex{};
  for (unsigned i = 0; i < 16; ++i) {
    hex[i * 2] = digits[token[i] >> 4];
    hex[i * 2 + 1] = digits[token[i] & 15];
  }
  return hex;
}

/* A kernel-validated section name, never an application-supplied HANDLE value or pointer. */
inline std::wstring
SectionLeafName(HANDLE handle) {
  // Both NtQueryObject responses start with a native UNICODE_STRING.
  // Keep the wire prefix local: winternl.h conflicts with util_d3dkmt.h's declarations.
  struct ObjectString {
    USHORT Length, MaximumLength;
    WCHAR *Buffer;
  };
  constexpr ULONG kObjectNameInformation = 1, kObjectTypeInformation = 2;
  using QueryObjectFn = LONG(WINAPI *)(HANDLE, ULONG, void *, ULONG, ULONG *);
  static const auto NtQueryObject = reinterpret_cast<QueryObjectFn>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
  if (!NtQueryObject)
    return {};
  alignas(8) uint8_t buffer[1024];
  ULONG length = 0;
  if (NtQueryObject(handle, kObjectTypeInformation, buffer, sizeof(buffer), &length))
    return {};
  auto *type = reinterpret_cast<ObjectString *>(buffer);
  if (type->Length != 7 * sizeof(WCHAR) || wcsncmp(type->Buffer, L"Section", 7))
    return {};
  if (NtQueryObject(handle, kObjectNameInformation, buffer, sizeof(buffer), &length))
    return {};
  auto *name = reinterpret_cast<ObjectString *>(buffer);
  if (!name->Buffer || !name->Length)
    return {};
  std::wstring full(name->Buffer, name->Length / sizeof(WCHAR));
  auto slash = full.rfind(L'\\');
  return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

inline bool
EndsWithToken(std::wstring_view leaf, const std::array<char, 33> &hex) {
  constexpr size_t length = 32;
  if (leaf.size() < length + 1 || leaf[leaf.size() - length - 1] != L'.')
    return false;
  for (size_t i = 0; i < length; ++i)
    if (leaf[leaf.size() - length + i] != WCHAR(hex[i]))
      return false;
  return true;
}

/* Reuse the existing per-user salted hash; bind the immutable adapter and object type to the section token. */
inline bool
AdapterTag(const uint8_t token[16], Kind kind, LUID luid, std::wstring_view leaf, std::wstring_view target, char out[33]) {
  WCHAR input[256];
  const auto hex = TokenHex(token);
  int length = swprintf(input, 256, L"d3dalloy.share.adapter.v2.%hs.%08x.%08x.%08x.%.*ls.%.*ls", hex.data(),
                        unsigned(kind), unsigned(luid.LowPart), unsigned(luid.HighPart), int(leaf.size()), leaf.data(),
                        int(target.size()), target.empty() ? L"" : target.data());
  return length > 0 && WMTShareHashName(reinterpret_cast<const uint16_t *>(input), uint32_t(length), out);
}

inline bool
ValidAdapter(const uint8_t token[16], Kind kind, const AdapterMetadata &adapter, std::wstring_view leaf,
             std::wstring_view target = {}) {
  if (uint32_t(kind) < uint32_t(Kind::Heap) || uint32_t(kind) > uint32_t(Kind::Fence) ||
      !(adapter.luid.LowPart || adapter.luid.HighPart))
    return false;
  char tag[33];
  return AdapterTag(token, kind, adapter.luid, leaf, target, tag) && !memcmp(tag, adapter.tag, sizeof(tag));
}

inline bool
ValidHeader(const Header &header, std::wstring_view leaf, const uint8_t *expected_token = nullptr) {
  return header.magic == kMagic && header.version == kVersion && header.payload_offset == kPayloadOffset &&
         header.format_list_explicit <= 1 &&
         header.texture_storage <= kTextureStorageSwizzledVolume &&
         (!header.texture_storage || (header.kind==Kind::Buffer &&
          header.resource_desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D)) &&
         (!(header.castable_formats[0]&1) || header.resource_desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER) &&
         (!header.format_list_explicit || header.kind==Kind::Buffer || header.kind==Kind::Texture) &&
         (header.format_list_explicit ||
          (!header.castable_formats[0] && !header.castable_formats[1] &&
           !header.castable_formats[2] && !header.castable_formats[3])) &&
         leaf.rfind(L"d3dalloy.share.", 0) == 0 && leaf.size() < 80 && EndsWithToken(leaf, TokenHex(header.token)) &&
         (!expected_token || !memcmp(expected_token, header.token, 16)) &&
         ValidAdapter(header.token, header.kind, header.adapter, leaf);
}

inline bool
ValidLink(const LinkHeader &link, std::wstring_view leaf) {
  if (link.magic != kLinkMagic || link.version != kVersion ||
      !wmemchr(link.section_name, 0, 80) || wcsncmp(link.section_name, kSharePrefix, std::size(kSharePrefix) - 1))
    return false;
  return EndsWithToken(link.section_name, TokenHex(link.token)) &&
         ValidAdapter(link.token, link.kind, link.adapter, leaf, link.section_name);
}

/* Read only the header, not a potentially multi-GiB payload. No second registry or Metal device lookup. */
inline HRESULT
GetSharedAdapterLuid(HANDLE handle, LUID *luid) {
  if (!luid)
    return DXGI_ERROR_INVALID_CALL;
  *luid = {};
  if (!handle || handle == INVALID_HANDLE_VALUE)
    return DXGI_ERROR_INVALID_CALL;
  const auto leaf = SectionLeafName(handle);
  const bool main = leaf.rfind(L"d3dalloy.share.", 0) == 0;
  if (!main && leaf.rfind(L"d3dalloy.d3d12.", 0) != 0)
    return DXGI_ERROR_INVALID_CALL;
  const SIZE_T bytes = main ? kPayloadOffset : sizeof(LinkHeader);
  const void *view = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, bytes);
  if (!view)
    return DXGI_ERROR_INVALID_CALL;
  AdapterMetadata adapter{};
  Kind kind{};
  bool valid;
  if (main) {
    Header header;
    const auto &source = *static_cast<const Header *>(view);
    header.magic = source.magic;
    header.version = source.version;
    header.kind = source.kind;
    memcpy(header.token, source.token, sizeof(header.token));
    header.payload_offset = source.payload_offset;
    header.adapter = source.adapter;
    header.format_list_explicit = source.format_list_explicit;
    header.initial_layout = source.initial_layout;
    header.texture_storage = source.texture_storage;
    header.resource_desc = source.resource_desc;
    memcpy(header.castable_formats,source.castable_formats,sizeof(header.castable_formats));
    valid = ValidHeader(header, leaf);
    adapter = header.adapter;
    kind = header.kind;
  } else {
    LinkHeader link;
    memcpy(&link, view, sizeof(link));
    valid = ValidLink(link, leaf);
    adapter = link.adapter;
    kind = link.kind;
  }
  UnmapViewOfFile(view);
  if (!valid || kind == Kind::Fence)
    return DXGI_ERROR_INVALID_CALL;
  *luid = adapter.luid;
  return S_OK;
}

} // namespace dxmt::sharing
