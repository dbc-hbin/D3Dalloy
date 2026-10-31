// Exercise the production legacy and DXIL container APIs, not a test-only decoder.
#include "DXBCParser/BlobContainer.h"
#include "dxil_converter.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

using Bytes = std::vector<uint8_t>;
struct Chunk {
  const char *tag;
  Bytes payload;
};

static void put(Bytes &bytes, size_t offset, uint32_t value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

static Bytes words(std::initializer_list<uint32_t> values) {
  Bytes bytes(values.size() * sizeof(uint32_t));
  std::memcpy(bytes.data(), values.begin(), bytes.size());
  return bytes;
}

static Bytes container(std::initializer_list<Chunk> chunks, size_t gap = 0) {
  Bytes bytes(32 + chunks.size() * 4);
  std::memcpy(bytes.data(), "DXBC", 4);
  put(bytes, 20, 1);
  put(bytes, 28, chunks.size());
  size_t index = 32;
  for (const auto &chunk : chunks) {
    bytes.resize(bytes.size() + gap);
    const size_t offset = bytes.size();
    put(bytes, index, offset);
    index += 4;
    bytes.resize(offset + 8 + chunk.payload.size());
    std::memcpy(bytes.data() + offset, chunk.tag, 4);
    put(bytes, offset + 4, chunk.payload.size());
    if (!chunk.payload.empty())
      std::memcpy(bytes.data() + offset + 8, chunk.payload.data(), chunk.payload.size());
  }
  put(bytes, 24, bytes.size());
  return bytes;
}

static Bytes runtime_data(std::initializer_list<std::pair<uint32_t, Bytes>> parts) {
  Bytes bytes(8 + parts.size() * 4);
  put(bytes, 0, 0x10);
  put(bytes, 4, parts.size());
  size_t index = 8;
  for (const auto &[type, payload] : parts) {
    const size_t offset = bytes.size();
    put(bytes, index, offset);
    index += 4;
    bytes.resize(offset + 8 + payload.size());
    put(bytes, offset, type);
    put(bytes, offset + 4, payload.size());
    if (!payload.empty())
      std::memcpy(bytes.data() + offset + 8, payload.data(), payload.size());
  }
  return bytes;
}

int main() {
  unsigned checks = 0, failures = 0;
  auto check = [&](bool okay, const char *name) {
    ++checks;
    failures += !okay;
    if (!okay)
      std::printf("CONTAINER_FAIL %s\n", name);
  };
  const Bytes first = words({0x12345678}), second = words({0x87654321});
  const auto valid = container({{"TEST", first}, {"DXIL", second}, {"DXIL", first}});
  microsoft::CDXBCParser legacy;
  const HRESULT loaded = legacy.ReadDXBC(valid.data(), valid.size());
  check(loaded == S_OK, "legacy-valid");
  if (SUCCEEDED(loaded)) {
    check(legacy.GetBlobCount() == 3 && legacy.FindNextMatchingBlob(microsoft::DXBC_DXIL) == 1 &&
          legacy.FindNextMatchingBlob(microsoft::DXBC_DXIL, 2) == 2 &&
          legacy.FindNextMatchingBlob(microsoft::DXBC_DXIL, 3) == DXBC_BLOB_NOT_FOUND, "legacy-duplicate-search");
    check(legacy.GetBlobSize(1) == second.size() && legacy.GetBlobFourCC(1) == microsoft::DXBC_DXIL &&
          !std::memcmp(legacy.GetBlob(1), second.data(), second.size()), "legacy-exact-payload");
    check(!legacy.GetBlob(3) && legacy.GetBlobSize(UINT32_MAX) == 0 && legacy.GetBlobFourCC(3) == 0,
          "legacy-invalid-index");
    const auto *version = legacy.GetVersion();
    const auto *hash = legacy.GetHash();
    std::array<unsigned char, 16> empty_hash{};
    check(version && version->Major == 1 && version->Minor == 0 && hash &&
          !std::memcmp(hash->Digest, empty_hash.data(), empty_hash.size()), "legacy-header-fields");
  }
  auto find = [&](const void *data, size_t size, const Bytes &expected, const char *name) {
    dxmt::dxil::DXBCChunkView view;
    const bool found = dxmt::dxil::FindDXBCChunk(data, size, "DXIL", view);
    check(found && view.size == expected.size() &&
          (expected.empty() || !std::memcmp(view.data, expected.data(), expected.size())), name);
  };
  auto reject = [&](const void *data, size_t size, const char *name) {
    microsoft::CDXBCParser parser;
    check(FAILED(parser.ReadDXBC(data, size)), name);
    dxmt::dxil::DXBCChunkView view;
    check(!dxmt::dxil::FindDXBCChunk(data, size, "DXIL", view), name);
  };
  find(valid.data(), valid.size(), second, "first-matching-chunk");
  const auto missing = container({{"TEST", first}});
  dxmt::dxil::DXBCChunkView missing_view;
  check(!dxmt::dxil::FindDXBCChunk(missing.data(), missing.size(), "DXIL", missing_view), "missing-chunk");
  const auto empty = container({});
  microsoft::CDXBCParser empty_parser;
  check(empty_parser.ReadDXBC(empty.data(), empty.size()) == S_OK && empty_parser.GetBlobCount() == 0,
        "empty-legacy-container");
  const auto empty_chunk = container({{"DXIL", {}}});
  find(empty_chunk.data(), empty_chunk.size(), {}, "empty-chunk");
  Bytes unaligned(valid.size() + 1);
  std::memcpy(unaligned.data() + 1, valid.data(), valid.size());
  find(unaligned.data() + 1, valid.size(), second, "unaligned-dxil-container");
  microsoft::CDXBCParser unaligned_parser;
  check(unaligned_parser.ReadDXBC(unaligned.data() + 1, valid.size()) == S_OK &&
        unaligned_parser.GetBlobCount() == 3 && unaligned_parser.GetBlobSize(1) == second.size() &&
        unaligned_parser.GetVersion()->Major == 1 &&
        !std::memcmp(unaligned_parser.GetBlob(1), second.data(), second.size()), "unaligned-legacy");
  auto padded = valid;
  padded.push_back(0xa5);
  find(padded.data(), padded.size(), second, "caller-padding");
  microsoft::CDXBCParser exact_parser;
  check(FAILED(exact_parser.ReadDXBC(padded.data(), padded.size())), "legacy-exact-length");
  auto indexed = container({{"DXIL", first}, {"DXIL", second}}, 4);
  // Header/index ends at40; physical chunks start at44 and60. Reverse the index order.
  put(indexed, 32, 60);
  put(indexed, 36, 44);
  find(indexed.data(), indexed.size(), second, "indexed-gaps-and-reordering");
  microsoft::CDXBCParser contiguous;
  check(FAILED(contiguous.ReadDXBC(indexed.data(), indexed.size())), "legacy-rejects-indexed-gaps");
  put(indexed, 36, 60);
  find(indexed.data(), indexed.size(), second, "duplicate-index-alias");

  reject(nullptr, 0, "null-input");
  for (size_t size = 0; size < valid.size(); ++size)
    reject(valid.data(), size, "truncated-caller-span");
  for (const auto &[offset, value] : std::array<std::pair<size_t, uint32_t>, 8>{{
           {0, 0}, {20, 0}, {20, 2}, {24, 31}, {24, uint32_t(valid.size() + 1)},
           {28, UINT32_MAX}, {32, 32}, {32, UINT32_MAX}}}) {
    auto bad = valid;
    put(bad, offset, value);
    reject(bad.data(), bad.size(), "invalid-header-or-index");
  }
  auto late_bad = container({{"DXIL", first}, {"TEST", second}});
  // Second header is at52; a five-byte payload would end one byte after the container.
  put(late_bad, 56, second.size() + 1);
  reject(late_bad.data(), late_bad.size(), "malformed-chunk-after-match");
  put(late_bad, 56, UINT32_MAX);
  reject(late_bad.data(), late_bad.size(), "overflowing-chunk-after-match");

  const Bytes bitcode = words({0xdec04342, 0x01234567});
  const Bytes wrapped = words({0x00050066, 8, 0x4c495844, 0x00000100, 16, 8, 0xdec04342, 0x01234567});
  auto program = [&](const Bytes &payload, bool accepted, uint32_t version, uint32_t kind, const char *name) {
    const auto bytes = container({{"DXIL", payload}});
    auto parsed = dxmt::dxil::FindDXILProgram(bytes.data(), bytes.size());
    if (!parsed) {
      llvm::consumeError(parsed.takeError());
      check(!accepted, name);
      return;
    }
    check(accepted && parsed->program_version == version && parsed->shader_kind == kind &&
          parsed->bitcode_size == bitcode.size() &&
          !std::memcmp(parsed->bitcode, bitcode.data(), bitcode.size()), name);
  };
  program(bitcode, true, 0xdec04342, 0xc0, "raw-bitcode");
  program(wrapped, true, 0x00050066, 5, "wrapped-bitcode");
  for (size_t size : {size_t(0), size_t(3), size_t(4), size_t(23), size_t(24)})
    program(Bytes(wrapped.begin(), wrapped.begin() + size), false, 0, 0, "truncated-program");
  for (uint32_t offset : {0u, 15u, 24u, UINT32_MAX}) {
    auto bad = wrapped;
    put(bad, 16, offset);
    program(bad, false, 0, 0, "invalid-bitcode-offset");
  }
  for (uint32_t size : {0u, 1u, 3u, 9u, UINT32_MAX}) {
    auto bad = wrapped;
    put(bad, 20, size);
    program(bad, false, 0, 0, "invalid-bitcode-length");
  }
  auto bad_magic = wrapped;
  put(bad_magic, 24, 0);
  program(bad_magic, false, 0, 0, "invalid-bitcode-magic");

  using Names = std::map<std::string, std::string>;
  auto names = [&](const Bytes &payload, bool accepted, const Names &expected, const char *name, Names actual = Names()) {
    const auto bytes = container({{"RDAT", payload}});
    const bool read = dxmt::dxil::ReadLibraryFunctionNames(bytes.data(), bytes.size(), actual);
    check(read == accepted && actual == expected, name);
  };
  Bytes strings;
  auto add_string = [&](const std::string &value) {
    const uint32_t offset = strings.size();
    strings.insert(strings.end(), value.begin(), value.end());
    strings.push_back(0);
    return offset;
  };
  const std::string mangled = "\x01?ClosestHit@@YAXXZ", public_name = "ClosestHit";
  const std::string raw_mangled = "\x01?\x80\xff@@", raw_public = "\xfe\x81";
  const auto mangled_offset = add_string(mangled), public_offset = add_string(public_name);
  const auto raw_mangled_offset = add_string(raw_mangled), raw_public_offset = add_string(raw_public);
  const auto functions = words({2, 8, mangled_offset, public_offset, raw_mangled_offset, raw_public_offset});
  const auto rdat = runtime_data({{1, strings}, {4, functions}});
  const Names expected_names{{mangled, public_name}, {raw_mangled, raw_public}};
  names(rdat, true, expected_names, "rdat-exact-names-and-raw-bytes");
  auto other_version = rdat;
  put(other_version, 0, UINT32_MAX);
  names(other_version, true, expected_names, "rdat-version-is-not-interpreted");
  auto strided = words({2, 12, mangled_offset, public_offset, UINT32_MAX,
                       raw_mangled_offset, raw_public_offset, 0xa5a5a5a5});
  strided.push_back(0xa5);
  names(runtime_data({{1, strings}, {4, strided}}), true, expected_names, "rdat-record-stride-and-trailing-data");
  names(runtime_data({{77, {0xa5}}, {4, functions}, {1, strings}, {99, {}}}),
        true, expected_names, "rdat-unknown-and-reordered-parts");
  auto old_strings = strings;
  old_strings[public_offset] = 'X';
  // Earlier tables need valid extents, but only the last of each type is interpreted.
  names(runtime_data({{1, old_strings}, {4, {}}, {1, strings}, {4, functions}}),
        true, expected_names, "rdat-last-duplicate-parts-win");
  const auto duplicates = words({3, 8, mangled_offset, public_offset, mangled_offset, raw_public_offset,
                                raw_mangled_offset, raw_public_offset});
  names(runtime_data({{1, strings}, {4, duplicates}}), true, expected_names, "rdat-first-duplicate-name-wins");
  const auto library = container({{"RDAT", rdat}});
  Bytes unaligned_library(library.size() + 1);
  std::memcpy(unaligned_library.data() + 1, library.data(), library.size());
  Names unaligned_names;
  check(dxmt::dxil::ReadLibraryFunctionNames(unaligned_library.data() + 1, library.size(), unaligned_names) &&
        unaligned_names == expected_names, "rdat-unaligned-input");
  Names null_names;
  check(!dxmt::dxil::ReadLibraryFunctionNames(nullptr, 0, null_names) && null_names.empty(), "rdat-null-input");
  for (const auto &payload : {runtime_data({{1, strings}}), runtime_data({{4, functions}}),
                              runtime_data({{77, {}}})})
    names(payload, false, {}, "rdat-missing-required-part");
  names(runtime_data({{1, {}}, {4, functions}}), false, {}, "rdat-empty-string-table");
  for (size_t size : {size_t(0), size_t(3), size_t(4), size_t(7)})
    names(Bytes(rdat.begin(), rdat.begin() + size), false, {}, "rdat-truncated-header");
  for (const auto &payload : {words({0x10, 0}), words({0x10, 2, 16}), words({0x10, UINT32_MAX})})
    names(payload, false, {}, "rdat-invalid-or-truncated-part-table");
  for (uint32_t offset : {uint32_t(rdat.size() - 7), uint32_t(rdat.size()), UINT32_MAX}) {
    auto bad = rdat;
    put(bad, 8, offset);
    names(bad, false, {}, "rdat-truncated-or-out-of-range-part-header");
  }
  for (uint32_t size : {uint32_t(functions.size() + 1), UINT32_MAX}) {
    auto bad = rdat;
    put(bad, bad.size() - functions.size() - 4, size);
    names(bad, false, {}, "rdat-truncated-or-overflowing-part-payload");
  }
  for (size_t size : {size_t(0), size_t(4), size_t(7), size_t(8), size_t(15), functions.size() - 1})
    names(runtime_data({{1, strings}, {4, Bytes(functions.begin(), functions.begin() + size)}}),
          false, {}, "rdat-truncated-function-header-or-record-span");
  for (const auto &[count, stride] : std::array<std::pair<uint32_t, uint32_t>, 7>{{
           {0, 8}, {1, 0}, {1, 7}, {UINT32_MAX, 8}, {0x20000000, 8},
           {2, 0x80000000}, {UINT32_MAX, UINT32_MAX}}}) {
    auto bad = functions;
    put(bad, 0, count);
    put(bad, 4, stride);
    names(runtime_data({{1, strings}, {4, bad}}), false, {}, "rdat-invalid-or-overflowing-record-layout");
  }

  Bytes invalid_strings = strings;
  const uint32_t empty_offset = invalid_strings.size();
  invalid_strings.push_back(0);
  const uint32_t unterminated_offset = invalid_strings.size();
  invalid_strings.push_back('x');
  const uint32_t end_offset = invalid_strings.size();
  const auto invalid_records = words({8, 8,
    end_offset, public_offset, mangled_offset, end_offset,
    UINT32_MAX, public_offset, mangled_offset, UINT32_MAX,
    empty_offset, public_offset, mangled_offset, empty_offset,
    unterminated_offset, public_offset, mangled_offset, unterminated_offset});
  const auto no_usable_names = runtime_data({{1, invalid_strings}, {4, invalid_records}});
  names(no_usable_names, false, {}, "rdat-all-invalid-strings-skipped");
  auto mixed_records = invalid_records;
  put(mixed_records, 0, 10);
  mixed_records.insert(mixed_records.end(), functions.begin() + 8, functions.end());
  names(runtime_data({{1, invalid_strings}, {4, mixed_records}}),
        true, expected_names, "rdat-valid-records-after-invalid-strings");

  const Names retained{{mangled, "Existing"}, {"unrelated", "Kept"}};
  Names merged = retained;
  merged.emplace(raw_mangled, raw_public);
  names(rdat, true, merged, "rdat-preserves-existing-keys-and-adds-names", retained);
  // A valid nonempty record table returns !out.empty(), even if every record is skipped.
  names(no_usable_names, true, retained, "rdat-skipped-records-retain-success", retained);
  names(runtime_data({{1, {}}, {4, functions}}),
        true, retained, "rdat-empty-strings-retain-success", retained);
  names(runtime_data({{1, strings}, {4, words({0, 8})}}),
        false, retained, "rdat-empty-record-table-still-fails", retained);
  auto malformed_later = runtime_data({{1, strings}, {4, functions}, {99, {}}});
  put(malformed_later, malformed_later.size() - 4, 1);
  names(malformed_later, false, retained, "rdat-malformed-later-part-does-not-change-output", retained);
  Names missing_names = retained;
  check(!dxmt::dxil::ReadLibraryFunctionNames(missing.data(), missing.size(), missing_names) &&
        missing_names == retained, "rdat-missing-chunk-does-not-change-output");

  std::printf("CONTAINER_%s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
