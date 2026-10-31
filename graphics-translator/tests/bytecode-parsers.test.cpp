// Production DXBC signature/program, pipeline-library and RDAT subobject parsers on
// malformed inputs. Host-portable: python3 graphics-translator/tests/run-bytecode-parsers-cpu.py
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/DXBCUtils.h"
#include "DXBCParser/DxbcSignature.h"
#include "DXBCParser/PeImage.h"
#include "DXBCParser/PipelineLibrary.h"
#include "DXBCParser/PsoStream.h"
#include "DXBCParser/RtasSerialized.h"
#include "DXBCParser/ShaderBinary.h"
#include "../../src/d3d12/d3d12_dxil_subobjects.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
using namespace microsoft;
using namespace microsoft::D3D10ShaderBinary;

static unsigned checks = 0, failures = 0;
static void check(bool okay, const char *name) {
  ++checks;
  failures += !okay;
  if (!okay)
    std::printf("PARSERS_FAIL %s\n", name);
}

static void put(Bytes &bytes, size_t offset, uint32_t value) {
  if (bytes.size() < offset + 4)
    bytes.resize(offset + 4);
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

static uint32_t get(const Bytes &bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

static Bytes words(std::initializer_list<uint32_t> values) {
  Bytes bytes(values.size() * 4);
  std::memcpy(bytes.data(), values.begin(), bytes.size());
  return bytes;
}

static Bytes container(std::initializer_list<std::pair<const char *, Bytes>> chunks) {
  Bytes bytes(32 + chunks.size() * 4);
  std::memcpy(bytes.data(), "DXBC", 4);
  put(bytes, 20, 1);
  put(bytes, 28, chunks.size());
  size_t index = 32;
  for (const auto &[tag, payload] : chunks) {
    const size_t offset = bytes.size();
    put(bytes, index, offset);
    index += 4;
    bytes.resize(offset + 8 + payload.size());
    std::memcpy(bytes.data() + offset, tag, 4);
    put(bytes, offset + 4, payload.size());
    if (!payload.empty())
      std::memcpy(bytes.data() + offset + 8, payload.data(), payload.size());
  }
  put(bytes, 24, bytes.size());
  return bytes;
}

struct Param {
  uint32_t stream;
  const char *name;
  uint32_t index, system_value, component_type, reg, mask, never_writes, precision;
};

// stride 24 (ISGN/OSGN/PCSG), 28 (OSG5) or 32 (ISG1/OSG1/PSG1).
static Bytes signature(unsigned stride, std::initializer_list<Param> params) {
  const size_t table = 8 + params.size() * stride;
  Bytes bytes(table);
  put(bytes, 0, params.size());
  put(bytes, 4, 8);
  size_t offset = 8;
  for (const auto &p : params) {
    const size_t base = stride == 24 ? 0 : 4;
    if (base)
      put(bytes, offset, p.stream);
    put(bytes, offset + base, bytes.size());
    bytes.insert(bytes.end(), p.name, p.name + std::strlen(p.name) + 1);
    put(bytes, offset + base + 4, p.index);
    put(bytes, offset + base + 8, p.system_value);
    put(bytes, offset + base + 12, p.component_type);
    put(bytes, offset + base + 16, p.reg);
    bytes[offset + base + 20] = p.mask;
    bytes[offset + base + 21] = p.never_writes;
    if (stride == 32)
      put(bytes, offset + 28, p.precision);
    offset += stride;
  }
  return bytes;
}

static void signatures() {
  const auto osg1 = signature(32, {{0, "SV_Position", 0, 1, 3, 0, 0xf, 0, 0},
                                   {0, "TEXCOORD", 1, 0, 3, 1, 0x3, 0x1, 1},
                                   {1, "Stream", 0, 0, 1, 0, 0x1, 0, 2},
                                   {3, "Last", 2, 0, 2, 5, 0x7, 0, 0}});
  CSignatureParser5 parser;
  const auto valid = container({{"OSG1", osg1}});
  check(DXBCGetOutputSignature(valid.data(), &parser) == S_OK, "osg1-valid");
  check(parser.NumStreams() == 4 && parser.GetTotalParameters() == 4, "osg1-stream-groups");
  const D3D11_SIGNATURE_PARAMETER *p0, *p1, *p2, *p3;
  check(parser.Signature(0)->GetParameters(&p0) == 2 && parser.Signature(1)->GetParameters(&p1) == 1 &&
            parser.Signature(2)->GetParameters(&p2) == 0 && parser.Signature(3)->GetParameters(&p3) == 1,
        "osg1-stream-counts");
  check(!std::strcmp(p0[0].SemanticName, "SV_Position") && p0[0].SystemValue == D3D10_SB_NAME_POSITION &&
            p0[1].Register == 1 && p0[1].Mask == 0x3 && p0[1].NeverWrites_Mask == 1 &&
            p0[1].MinPrecision == D3D_MIN_PRECISION_FLOAT_16 && p1[0].Stream == 1 && p3[0].Stream == 3 &&
            !std::strcmp(p3[0].SemanticName, "Last") && p3[0].SemanticIndex == 2,
        "osg1-fields-and-min-precision");
  // Copies are owned; char sums include the terminator, as before.
  check((const uint8_t *)p0[0].SemanticName < valid.data() ||
            (const uint8_t *)p0[0].SemanticName >= valid.data() + valid.size(),
        "osg1-names-copied");
  unsigned sum = 0;
  for (const char *c = "sv_position"; *c; ++c)
    sum += *c;
  check(const_cast<CSignatureParser *>(parser.Signature(0))->GetSemanticNameCharSum(0) == sum, "char-sum");

  CSignatureParser5 referenced;
  check(DXBCGetOutputSignature(valid.data(), &referenced, true) == S_OK &&
            referenced.Signature(0)->GetParameters(&p0) == 2 &&
            (const uint8_t *)p0[0].SemanticName > valid.data() &&
            (const uint8_t *)p0[0].SemanticName < valid.data() + valid.size(),
        "force-string-reference");

  // fxc never emits these; before the fix Stream=4 records were copied without
  // being measured, overflowing the string area.
  for (uint32_t stream : {4u, 7u, UINT32_MAX}) {
    auto bad = osg1;
    put(bad, 8 + 3 * 32, stream);
    const auto bytes = container({{"OSG1", bad}});
    CSignatureParser5 rejected;
    check(DXBCGetOutputSignature(bytes.data(), &rejected) == E_FAIL && rejected.NumStreams() == 0 &&
              rejected.GetTotalParameters() == 0,
          "stream-out-of-range-rejected");
  }
  auto unordered = osg1;
  put(unordered, 8 + 2 * 32, 2);
  put(unordered, 8 + 3 * 32, 1);
  const auto unordered_bytes = container({{"OSG1", unordered}});
  check(DXBCGetOutputSignature(unordered_bytes.data(), &parser) == E_FAIL, "decreasing-stream-rejected");

  auto registers = osg1;
  put(registers, 8 + 0 * 32 + 20, 9);
  const auto register_bytes = container({{"OSG1", registers}});
  check(DXBCGetOutputSignature(register_bytes.data(), &parser) == E_FAIL, "decreasing-register-rejected");
  // Register order restarts per stream.
  auto restart = osg1;
  put(restart, 8 + 2 * 32 + 20, 0);
  put(restart, 8 + 1 * 32 + 20, 9);
  const auto restart_bytes = container({{"OSG1", restart}});
  check(DXBCGetOutputSignature(restart_bytes.data(), &parser) == S_OK, "register-order-per-stream");

  // OSG5 (stride 28) keeps the default precision and stream grouping.
  const auto osg5 = signature(28, {{0, "A", 0, 0, 3, 0, 1, 0, 0}, {2, "B", 0, 0, 3, 0, 1, 0, 0}});
  const auto osg5_bytes = container({{"OSG5", osg5}});
  check(DXBCGetOutputSignature(osg5_bytes.data(), &parser) == S_OK && parser.NumStreams() == 3 &&
            parser.Signature(2)->GetParameters(&p2) == 1 && p2[0].MinPrecision == D3D_MIN_PRECISION_DEFAULT,
        "osg5-valid");

  // Single-stream ISG1 ignores record streams; ISGN/PCSG have none.
  const auto isg1 = container({{"ISG1", signature(32, {{0, "A", 0, 0, 3, 0, 1, 0, 1}, {3, "B", 0, 0, 3, 2, 1, 0, 0}})}});
  CSignatureParser input;
  check(DXBCGetInputSignature(isg1.data(), &input) == S_OK && input.GetParameters(&p0) == 2 && p0[1].Stream == 0 &&
            p0[0].MinPrecision == D3D_MIN_PRECISION_FLOAT_16,
        "isg1-ungrouped");
  const auto isgn = container({{"ISGN", signature(24, {{0, "POSITION", 0, 0, 3, 0, 0xf, 0xf, 0}})}});
  check(DXBCGetInputSignature(isgn.data(), &input) == S_OK && input.GetParameters(&p0) == 1 &&
            p0[0].AlwaysReads_Mask == 0xf && p0[0].MinPrecision == D3D_MIN_PRECISION_DEFAULT,
        "isgn-valid");
  const auto empty = container({{"PCSG", words({0, 0})}});
  check(DXBCGetPatchConstantSignature(empty.data(), &input) == S_OK && input.GetNumParameters() == 0,
        "empty-signature");

  const auto legacy = signature(24, {{0, "A", 0, 0, 3, 0, 1, 0, 0}, {0, "B", 0, 0, 3, 1, 1, 0, 0}});
  auto reject = [&](const Bytes &payload, const char *name) {
    const auto bytes = container({{"ISGN", payload}});
    CSignatureParser rejected;
    check(DXBCGetInputSignature(bytes.data(), &rejected) == E_FAIL && rejected.GetNumParameters() == 0, name);
  };
  reject(words({1}), "truncated-header");
  for (uint32_t count : {3u, 0x0aaaaaabu, UINT32_MAX}) {
    auto bad = legacy;
    put(bad, 0, count);
    reject(bad, "record-count-past-blob");
  }
  for (uint32_t offset : {uint32_t(legacy.size() - 47), uint32_t(legacy.size()), UINT32_MAX - 8}) {
    auto bad = legacy;
    put(bad, 4, offset);
    reject(bad, "record-offset-past-blob");
  }
  for (uint32_t name : {uint32_t(legacy.size()), UINT32_MAX}) {
    auto bad = legacy;
    put(bad, 8 + 24, name);
    reject(bad, "name-offset-past-blob");
  }
  auto unterminated = legacy;
  unterminated.back() = 'x';
  reject(unterminated, "unterminated-name");

  // The DXIL frontend parses ISG1/OSG1/PSG1 payloads grouped, as DXC sorts them:
  // a multi-stream GS output restarts its registers per stream.
  const auto gs = signature(32, {{0, "SV_Position", 0, 1, 3, 0, 0xf, 0, 0}, {0, "COLOR", 0, 0, 3, 1, 0xf, 0, 0},
                                 {1, "SV_Position", 0, 1, 3, 0, 0xf, 0, 0}, {1, "COLOR", 0, 0, 3, 1, 0xf, 0, 0}});
  DxmtDxbcSignatureInfo info{}, untouched{};
  check(dxmt_dxbc_signature_parse(gs.data(), gs.size(), DXMT_DXBC_SIGNATURE_11_1, 1, &info) == 0 &&
            info.parameter_count == 4 && info.stream_count == 2 && info.stream_parameters[1] == 2,
        "dxil-gs-streams-grouped");
  check(dxmt_dxbc_signature_parse(gs.data(), gs.size(), DXMT_DXBC_SIGNATURE_11_1, 0, &untouched) ==
            DXMT_DXBC_SIGNATURE_REGISTER_ORDER && untouched.parameter_count == 0,
        "dxil-gs-streams-ungrouped-rejected");
  DxmtDxbcSignatureParameter record{};
  check(dxmt_dxbc_signature_parameter(gs.data(), gs.size(), &info, 3, &record) == 0 && record.stream == 1 &&
            record.register_index == 1 && record.name_size == 5 &&
            !std::memcmp(gs.data() + record.name_offset, "COLOR", 6),
        "dxil-gs-record");
  check(dxmt_dxbc_signature_parameter(gs.data(), gs.size(), &info, 4, &record) == DXMT_DXBC_SIGNATURE_INVALID_ARGUMENT,
        "dxil-record-index-past-count");
  for (size_t size = 8; size < 8 + 4 * 32; size += 31)
    check(dxmt_dxbc_signature_parse(gs.data(), size, DXMT_DXBC_SIGNATURE_11_1, 1, &untouched) ==
              DXMT_DXBC_SIGNATURE_RECORDS && untouched.parameter_count == 0,
          "dxil-truncated-record-table");
  check(dxmt_dxbc_signature_parse(gs.data(), gs.size() - 1, DXMT_DXBC_SIGNATURE_11_1, 1, &untouched) ==
            DXMT_DXBC_SIGNATURE_NAME,
        "dxil-truncated-last-name");
}

static Bytes program(uint32_t version, std::initializer_list<uint32_t> body) {
  Bytes bytes = words({version, uint32_t(2 + body.size())});
  for (uint32_t token : body)
    put(bytes, bytes.size(), token);
  return bytes;
}

static bool parse_all(const Bytes &code, size_t size, unsigned *count = nullptr) {
  CShaderCodeParser parser;
  if (FAILED(parser.SetShader(reinterpret_cast<const CShaderToken *>(code.data()), size)))
    return false;
  unsigned parsed = 0;
  while (!parser.EndOfShader()) {
    CInstruction instruction;
    if (!parser.ParseInstruction(&instruction))
      return false;
    ++parsed;
  }
  if (count)
    *count = parsed;
  return true;
}

static void programs() {
  const uint32_t vs50 = 0x00010050, vs51 = 0x00010051;
  // mov r0.xyzw, l(1.0, 2.0, 3.0, 4.0); ret
  const uint32_t mov = 0x08000036, dst = 0x00100000 | (1 << 20) | 0xf0 | 2, imm = 0x00004002;
  const auto valid = program(vs50, {mov, dst, 0, imm, 1, 2, 3, 4, 0x0100003e});
  unsigned count = 0;
  check(parse_all(valid, valid.size(), &count) && count == 2, "program-valid");
  // Every truncation of the chunk or the declared program length fails cleanly.
  for (size_t size = 0; size < valid.size(); size += 4)
    check(!parse_all(valid, size), "program-truncated-chunk");
  for (uint32_t length : {0u, 1u, 12u, UINT32_MAX}) {
    auto bad = valid;
    put(bad, 4, length);
    check(!parse_all(bad, bad.size()), "program-length-past-chunk");
  }
  auto shorter = valid;
  put(shorter, 4, 5);
  check(!parse_all(shorter, shorter.size()), "instruction-past-program");
  auto zero = valid;
  put(zero, 8, mov & ~0x7f000000u);
  check(!parse_all(zero, zero.size()), "zero-instruction-length");
  auto short_length = valid;
  put(short_length, 8, (mov & ~0x7f000000u) | (4 << 24));
  check(!parse_all(short_length, short_length.size()), "operands-past-instruction-length");
  auto opcode = valid;
  put(opcode, 8, (mov & ~0x7ffu) | 0x7ff);
  check(!parse_all(opcode, opcode.size()), "opcode-out-of-range");
  // Relative 1D temp index: the index is another operand.
  const uint32_t relative = 0x00900000, dst_relative = (dst & ~(3u << 22)) | (2u << 22);
  const auto relative_ok = program(vs50, {0x05000036, dst_relative, 0, 0x00000001, 0x0100003e});
  check(parse_all(relative_ok, relative_ok.size()), "relative-index-valid");
  const auto relative_short = program(vs50, {0x03000036, dst_relative});
  check(!parse_all(relative_short, relative_short.size()), "relative-index-truncated");
  const auto nested = program(vs50, {0x08000036, dst_relative, relative, relative, relative, 0, 1, 0});
  check(!parse_all(nested, nested.size()), "relative-index-nesting-limit");
  const auto extended = program(vs50, {0x03000036, dst | 0x80000000u, 0});
  check(!parse_all(extended, extended.size()), "extended-operand-past-program");
  // dcl_temps 4 without its count token.
  const auto temps = program(vs50, {0x01000068});
  check(!parse_all(temps, temps.size()), "declaration-token-missing");
  const auto temps_ok = program(vs50, {0x02000068, 4});
  check(parse_all(temps_ok, temps_ok.size()), "declaration-valid");
  // dcl_constantbuffer cb0[1] in SM5.1 reads size and space tokens.
  // SM5.1 cbuffer operands are 3D (id, lower, upper), followed by size and space.
  const auto cb = program(vs51, {0x07000059, 0x00308046, 0, 0, 1, 4, 0});
  check(parse_all(cb, cb.size()), "sm51-constant-buffer-valid");
  const auto cb_short = program(vs51, {0x06000059, 0x00308046, 0, 0, 1, 4});
  check(!parse_all(cb_short, cb_short.size()), "sm51-constant-buffer-space-missing");
  // Custom data: comment length covers itself; lengths escaping the program fail.
  const auto comment = program(vs50, {0x00000035, 3, 0x00434241});
  check(parse_all(comment, comment.size()), "customdata-valid");
  for (uint32_t length : {4u, 0x40000000u, UINT32_MAX}) {
    const auto bad = program(vs50, {0x00000035, length, 0});
    check(!parse_all(bad, bad.size()), "customdata-length-past-program");
  }
  const auto empty_comment = program(vs50, {0x00000035, 2});
  check(parse_all(empty_comment, empty_comment.size()), "customdata-empty-comment");
  // Shader messages are kept as opaque payload bytes: an inconsistent string length
  // (which wrapped the old decoder's word count) only bounds the block, and the
  // instruction after it still parses.
  const uint32_t message = 0x00000035 | (D3D11_SB_CUSTOMDATA_SHADER_MESSAGE << 11);
  for (uint32_t chars : {0u, 3u, UINT32_MAX - 2, UINT32_MAX}) {
    const auto code = program(vs50, {message, 8, 0, 0, chars, 1, 0, 0x00434241, 0x0100003e});
    CShaderCodeParser parser;
    CInstruction custom, ret;
    check(SUCCEEDED(parser.SetShader(reinterpret_cast<const CShaderToken *>(code.data()), code.size())) &&
              parser.ParseInstruction(&custom) && custom.OpCode() == D3D10_SB_OPCODE_CUSTOMDATA &&
              custom.m_CustomData.Type == D3D11_SB_CUSTOMDATA_SHADER_MESSAGE &&
              custom.m_CustomData.DataSizeInBytes == 24 &&
              static_cast<const uint32_t *>(custom.m_CustomData.pData)[2] == chars &&
              parser.ParseInstruction(&ret) && ret.OpCode() == D3D10_SB_OPCODE_RET && parser.EndOfShader(),
          "customdata-shader-message-opaque");
  }
  const auto short_message = program(vs50, {message, 3, 0});
  check(parse_all(short_message, short_message.size()), "customdata-shader-message-short");
  const auto message_past = program(vs50, {message, 9, 0, 0, 0, 0, 0});
  check(!parse_all(message_past, message_past.size()), "customdata-shader-message-past-program");
  const auto table = program(vs50, {0x80000091, 6, 0, 2, 7});
  check(!parse_all(table, table.size()), "function-table-past-program");
  const auto table_ok = program(vs50, {0x80000091, 6, 0, 2, 7, 8});
  check(parse_all(table_ok, table_ok.size()), "function-table-valid");
  const auto table_mismatch = program(vs50, {0x80000091, 7, 0, 2, 7, 8, 0});
  check(!parse_all(table_mismatch, table_mismatch.size()), "function-table-length-mismatch");
}

static void library() {
  const DxmtPipelineLibraryExtent start{DXMT_PIPELINE_LIBRARY_HEADER_SIZE, 0};
  DxmtPipelineLibraryExtent extent{}, untouched{77, 77};
  check(dxmt_pipeline_library_extend(&start, 6, 9, &extent) == DXMT_PIPELINE_LIBRARY_SUCCESS &&
            extent.size == 104 + 40 + 8 + 16 && extent.count == 1,
        "library-extend");
  check(dxmt_pipeline_library_extend(&start, 3, 9, &untouched) == DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT &&
            dxmt_pipeline_library_extend(&start, 2, 0, &untouched) == DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT &&
            dxmt_pipeline_library_extend(&start, uint64_t(UINT32_MAX) + 1, 1, &untouched) ==
                DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT &&
            dxmt_pipeline_library_extend(&start, 2, UINT64_MAX - 3, &untouched) == DXMT_PIPELINE_LIBRARY_OVERFLOW &&
            untouched.size == 77 && untouched.count == 77,
        "library-extend-rejects");
  const DxmtPipelineLibraryExtent full{104, UINT32_MAX};
  check(dxmt_pipeline_library_extend(&full, 2, 1, &untouched) == DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT,
        "library-count-limit");

  // Build a two-entry library with the production writer API.
  const std::u16string names[] = {u"abc", u"longer-name"};
  const Bytes blobs[] = {Bytes(9, 0xa1), Bytes(16, 0xb2)};
  DxmtPipelineLibraryExtent total = start;
  for (int i = 0; i < 2; ++i)
    dxmt_pipeline_library_extend(&total, names[i].size() * 2, blobs[i].size(), &total);
  Bytes bytes(total.size, 0xcc);
  DxmtPipelineLibraryHeader header{};
  header.adapter = 0x1122334455667788ull;
  header.size = total.size;
  header.count = 2;
  std::memset(header.identity, 0x5a, sizeof(header.identity));
  check(dxmt_pipeline_library_begin(&header, bytes.data(), bytes.size() - 1) != DXMT_PIPELINE_LIBRARY_SUCCESS,
        "library-begin-size");
  check(dxmt_pipeline_library_begin(&header, bytes.data(), bytes.size()) == DXMT_PIPELINE_LIBRARY_SUCCESS,
        "library-begin");
  uint64_t offset = DXMT_PIPELINE_LIBRARY_HEADER_SIZE;
  for (int i = 0; i < 2; ++i) {
    DxmtPipelineLibraryRecord record{names[i].size() * 2, blobs[i].size(), uint32_t(i), 0, {}, 0};
    std::memset(record.key, 0x10 + i, sizeof(record.key));
    DxmtPipelineLibraryEntry placed;
    check(dxmt_pipeline_library_put_entry(bytes.data(), bytes.size(), offset, &record, &placed) ==
              DXMT_PIPELINE_LIBRARY_SUCCESS,
          "library-put-entry");
    std::memcpy(bytes.data() + placed.name_offset, names[i].data(), placed.name_bytes);
    std::memcpy(bytes.data() + placed.blob_offset, blobs[i].data(), placed.blob_size);
    offset = placed.next_offset;
  }
  const uint8_t digest[20] = {1, 2, 3};
  check(offset == bytes.size() && dxmt_pipeline_library_set_digest(bytes.data(), bytes.size(), digest) == 0,
        "library-digest");
  // Byte layout of the previous C++ writer: header fields, records, zero padding.
  check(get(bytes, 0) == 0x544d5844 && get(bytes, 4) == 0x00424c50 && get(bytes, 8) == 1 && get(bytes, 12) == 2 &&
            get(bytes, 16) == bytes.size() && get(bytes, 24) == 0x55667788 && get(bytes, 28) == 0x11223344 &&
            bytes[32] == 0x5a && bytes[79] == 0x5a && bytes[80] == 1 && bytes[82] == 3 && get(bytes, 100) == 0 &&
            get(bytes, 104) == 6 && get(bytes, 108) == 0 && get(bytes, 112) == 9 && bytes[120] == 0x10 &&
            get(bytes, 140) == 0 && bytes[150] == 0 && bytes[151] == 0 && bytes[152] == 0xa1 && bytes[161] == 0 &&
            get(bytes, 168) == 22 && get(bytes, 172) == 1,
        "library-golden-layout");

  DxmtPipelineLibraryHeader read;
  check(dxmt_pipeline_library_header(bytes.data(), bytes.size(), &read) == 0 && read.count == 2 &&
            read.adapter == header.adapter && read.digest[2] == 3 && read.identity[47] == 0x5a,
        "library-header-read");
  DxmtPipelineLibraryEntry first, second;
  check(dxmt_pipeline_library_entry(bytes.data(), bytes.size(), 104, &first) == 0 && first.name_bytes == 6 &&
            first.blob_offset == 152 && first.blob_size == 9 &&
            dxmt_pipeline_library_entry(bytes.data(), bytes.size(), first.next_offset, &second) == 0 &&
            second.kind == 1 && second.next_offset == bytes.size(),
        "library-entry-read");
  // Magic before version before size; truncated or overrunning records fail.
  auto version = bytes;
  put(version, 8, 2);
  put(version, 16, 1);
  check(dxmt_pipeline_library_header(version.data(), version.size(), &read) == DXMT_PIPELINE_LIBRARY_VERSION_MISMATCH,
        "library-version-before-size");
  put(version, 0, 0);
  check(dxmt_pipeline_library_header(version.data(), version.size(), &read) == DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT,
        "library-magic-before-version");
  check(dxmt_pipeline_library_header(bytes.data(), bytes.size() - 8, &read) == DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT,
        "library-size-mismatch");
  for (const auto &[at, value] : {std::pair<size_t, uint32_t>{104, 7}, {104, UINT32_MAX - 1}, {108, 2}, {112, 0},
                                  {112, UINT32_MAX}, {116, 1}, {140, 1}}) {
    auto bad = bytes;
    put(bad, at, value);
    check(dxmt_pipeline_library_entry(bad.data(), bad.size(), 104, &first) == DXMT_PIPELINE_LIBRARY_INVALID_ARGUMENT,
          "library-entry-rejects");
  }
  check(dxmt_pipeline_library_entry(bytes.data(), bytes.size(), bytes.size() - 8, &first) != 0 &&
            dxmt_pipeline_library_entry(bytes.data(), bytes.size(), 100, &first) != 0 &&
            dxmt_pipeline_library_entry(bytes.data(), bytes.size(), UINT64_MAX, &first) != 0,
        "library-entry-offset-rejects");
}

static Bytes read_file(const char *path) {
  std::ifstream file(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(file), {});
}

static size_t rdat_offset(const Bytes &dxil) {
  for (uint32_t i = 0; i < get(dxil, 28); ++i) {
    const uint32_t offset = get(dxil, 32 + 4 * i);
    if (!std::memcmp(dxil.data() + offset, "RDAT", 4))
      return offset + 8;
  }
  return 0;
}

static size_t part_offset(const Bytes &dxil, uint32_t type) {
  const size_t rdat = rdat_offset(dxil);
  for (uint32_t i = 0; i < get(dxil, rdat + 4); ++i) {
    const size_t part = rdat + get(dxil, rdat + 8 + 4 * i);
    if (get(dxil, part) == type)
      return part + 8;
  }
  return 0;
}

static void subobjects(const char *path) {
  using dxmt::DxilSubobjectKind;
  const auto dxil = read_file(path);
  std::vector<dxmt::DxilSubobject> out;
  check(dxmt::ReadDxilLibrarySubobjects(dxil.data(), dxil.size(), out) == S_OK && out.size() == 11, "rdat-fixture");
  if (out.size() == 11) {
    check(out[0].kind == DxilSubobjectKind::GlobalRootSignature && out[0].name == L"SoGlobalRS" &&
              out[0].root_signature.size() == 92 && out[1].root_signature.size() == 48,
          "rdat-root-signatures");
    check(out[3].kind == DxilSubobjectKind::SubobjectToExportsAssociation && out[3].associated_subobject == L"SoLocalRS" &&
              out[3].exports == std::vector<std::wstring>{L"SoHitGroup", L"SoProcGroup", L"SoMiss"},
          "rdat-association");
    check(out[6].shader_config.MaxPayloadSizeInBytes == 8 && out[7].pipeline_config.MaxTraceRecursionDepth == 1 &&
              out[10].state_object_flags == D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS,
          "rdat-configs");
    check(out[8].any_hit == L"SoAnyHit" && out[8].closest_hit == L"SoClosestHit" && out[8].intersection.empty() &&
              out[9].hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE && out[9].any_hit.empty() &&
              out[9].intersection == L"SoIntersection",
          "rdat-hit-groups-and-empty-names");
  }
  const size_t table = part_offset(dxil, 6), strings = part_offset(dxil, 1);
  if (!table || !strings) {
    check(false, "rdat-fixture-layout");
    return;
  }
  const uint32_t stride = get(dxil, table + 4);
  auto record = [&](unsigned i) { return table + 8 + i * stride; };
  // As before, records preceding the failing one remain in the output.
  auto expect = [&](const Bytes &bytes, HRESULT hr, const char *name) {
    std::vector<dxmt::DxilSubobject> result;
    check(dxmt::ReadDxilLibrarySubobjects(bytes.data(), bytes.size(), result) == hr, name);
  };
  auto bad = dxil;
  put(bad, record(4), 77);
  expect(bad, E_NOTIMPL, "rdat-unknown-kind");
  // An unknown kind with an invalid name is still a malformed table.
  put(bad, record(4) + 4, UINT32_MAX);
  expect(bad, E_INVALIDARG, "rdat-unknown-kind-bad-name");
  bad = dxil;
  // Point a name at the empty string: the strings part begins with an empty entry in DXC output.
  put(bad, record(0) + 4, uint32_t(std::find(dxil.begin() + strings, dxil.end(), 0) - dxil.begin() - strings));
  expect(bad, E_INVALIDARG, "rdat-empty-subobject-name");
  bad = dxil;
  put(bad, record(0) + 12, 0);
  expect(bad, E_INVALIDARG, "rdat-empty-root-signature");
  bad = dxil;
  put(bad, record(0) + 12, UINT32_MAX);
  expect(bad, E_INVALIDARG, "rdat-root-signature-past-raw");
  bad = dxil;
  put(bad, record(3) + 12, UINT32_MAX);
  expect(bad, E_INVALIDARG, "rdat-export-array-past-indices");
  bad = dxil;
  put(bad, record(8) + 8, 2);
  expect(bad, E_INVALIDARG, "rdat-hit-group-type");
  bad = dxil;
  put(bad, table + 4, 23);
  expect(bad, E_INVALIDARG, "rdat-short-stride");
  bad = dxil;
  put(bad, table, 0x10000000);
  expect(bad, E_INVALIDARG, "rdat-record-count-past-table");
  const Bytes empty = container({{"TEST", words({1})}});
  std::vector<dxmt::DxilSubobject> none(1);
  check(dxmt::ReadDxilLibrarySubobjects(empty.data(), empty.size(), none) == S_OK && none.empty(), "rdat-absent");
  check(dxmt::ReadDxilLibrarySubobjects(nullptr, 0, none) == E_INVALIDARG, "rdat-null");
}

static void put64(Bytes &bytes, size_t offset, uint64_t value) {
  if (bytes.size() < offset + 8)
    bytes.resize(offset + 8);
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

// Serialized structure: 56-byte header, pointers, 64-byte dxmt header, shadow.
struct Serialized {
  Bytes bytes;
  uint64_t header;
  DxmtRtasSerializedLayout layout{};
  uint32_t prefix() { return dxmt_rtas_serialized_prefix(bytes.data(), 56, bytes.size(), &layout); }
  uint32_t parse(DxmtRtasSerializedInfo &info, uint64_t destination = 1 << 20) {
    if (uint32_t status = prefix())
      return status;
    if (layout.blocks)
      if (uint32_t status = dxmt_rtas_serialized_block(&layout, bytes.data() + 56, 16, &layout))
        return status;
    if (layout.header_offset + 64 > bytes.size())
      return 99;
    uint64_t geometry = 0;
    if (uint32_t status =
            dxmt_rtas_serialized_geometry_bytes(&layout, bytes.data() + layout.header_offset, 64, &geometry))
      return status;
    return dxmt_rtas_serialized_parse(bytes.data(), 56, &layout, bytes.data() + layout.header_offset, 64,
                                      bytes.data() + layout.header_offset + 64, geometry, destination, &info);
  }
};

static Serialized serialized(uint32_t type, uint32_t count, uint64_t pointers, uint64_t shadow, uint64_t prefix = 0) {
  Serialized s;
  s.header = 56 + prefix;
  s.bytes.assign(s.header + 64 + shadow, 0);
  put64(s.bytes, 32, s.bytes.size());
  put64(s.bytes, 40, 64 + shadow);
  put64(s.bytes, 48, pointers);
  put(s.bytes, s.header + 16, 0x53415844u);
  put(s.bytes, s.header + 20, type);
  put(s.bytes, s.header + 28, count);
  put64(s.bytes, s.header + 32, shadow);
  put64(s.bytes, s.header + 56, 64 + shadow);
  return s;
}

static void rtas() {
  DxmtRtasSerializedInfo info{};
  auto top = serialized(0, 2, 2, 128, 16);
  check(top.parse(info) == 0 && info.header_offset == 72 && info.instances.offset == 64 &&
            info.instances.size == 128 && info.desc_count == 2,
        "rtas-top-level");
  auto bad = top;
  put64(bad.bytes, 48, 1);
  check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-top-pointer-count");
  bad = top;
  put64(bad.bytes, top.header + 32, 64);
  check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-top-instances-past-shadow");
  check(top.parse(info, 64 + 127) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-destination-too-small");
  bad = top;
  put64(bad.bytes, 48, UINT64_MAX >> 33);
  check(bad.prefix() == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-pointer-overflow");
  // Every size below the 120-byte minimum or past the readable source fails.
  bool truncated = true;
  for (uint64_t size = 0; size <= top.bytes.size() + 1; ++size) {
    auto cut = top;
    put64(cut.bytes, 32, size);
    truncated &= (cut.prefix() == 0) == (size >= 120 + 16 && size <= top.bytes.size());
  }
  check(truncated, "rtas-serialized-size-bounds");

  // Bottom level: triangles with indices and a transform, then AABBs.
  auto bottom = serialized(1, 2, 0, 2 * 96 + 256);
  const size_t g0 = bottom.header + 64, g1 = g0 + 96;
  put(bottom.bytes, g0 + 8, 6);      // DXGI_FORMAT_R32G32B32_FLOAT
  put(bottom.bytes, g0 + 12, 42);    // DXGI_FORMAT_R32_UINT
  put(bottom.bytes, g0 + 16, 3);     // vertices
  put(bottom.bytes, g0 + 20, 3);     // indices
  put(bottom.bytes, g0 + 24, 1);     // primitives
  put(bottom.bytes, g0 + 28, 12u | 0x80000000u);
  put64(bottom.bytes, g0 + 32, 256); // data
  put64(bottom.bytes, g0 + 40, 292); // indices
  put64(bottom.bytes, g0 + 48, 304); // transform
  put(bottom.bytes, g1, 1);
  put(bottom.bytes, g1 + 24, 1);
  put(bottom.bytes, g1 + 28, 24);
  put64(bottom.bytes, g1 + 32, 352);
  DxmtRtasSerializedGeometry spans{};
  const uint8_t *records = bottom.bytes.data() + g0;
  check(bottom.parse(info) == 0 && info.type == 1 && info.shadow_size == 448 &&
            dxmt_rtas_serialized_geometry(&info, records, 192, 0, &spans) == 0 && spans.present == 7 &&
            spans.data.offset == 256 && spans.data.size == 36 && spans.indices.size == 12 &&
            spans.transform.offset == 304 && spans.transform.size == 48 &&
            dxmt_rtas_serialized_geometry(&info, records, 192, 1, &spans) == 0 && spans.present == 1 &&
            spans.data.size == 24 && dxmt_rtas_serialized_geometry(&info, records, 192, 2, &spans) ==
                                         DXMT_RTAS_SERIALIZED_INVALID_ARGUMENT,
        "rtas-bottom-level");
  for (const auto &[at, value] : {std::pair<size_t, uint64_t>{g1 + 32, 64 + 448 - 23}, {g0 + 32, 63},
                                  {g0 + 40, 64 + 448 - 11}, {g0 + 48, 64 + 448 - 47}, {g1, 3}}) {
    bad = bottom;
    put64(bad.bytes, at, value);
    check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-geometry-span-rejects");
  }
  bad = bottom;
  put(bad.bytes, g0 + 20, UINT32_MAX);
  put(bad.bytes, g0 + 12, 0); // no index buffer: index count is ignored
  check(bad.parse(info) == 0, "rtas-index-count-without-indices");
  bad = bottom;
  put(bad.bytes, bottom.header + 28, 5);
  check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-geometry-count-past-shadow");
  bad = bottom;
  put(bad.bytes, bottom.header + 16, 0);
  check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-magic");
  bad = bottom;
  put64(bad.bytes, 40, 1);
  check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-deserialized-size");

  // Micromap array: packed 2-bit states of 4^level microtriangles.
  auto omm = serialized(2, 1, 0, 96 + 64);
  put(omm.bytes, omm.header + 64, 2);
  put(omm.bytes, omm.header + 64 + 8, 2);
  put(omm.bytes, omm.header + 64 + 12, 3);
  put(omm.bytes, omm.header + 64 + 16, 16);
  put64(omm.bytes, omm.header + 64 + 32, 160);
  check(omm.parse(info) == 0 && info.type == 2, "rtas-omm-array");
  for (const auto &[at, value] : {std::pair<size_t, uint32_t>{12, 13}, {12, UINT32_MAX}, {16, 15}, {8, 3}, {0, 1}}) {
    bad = omm;
    put(bad.bytes, omm.header + 64 + at, value);
    check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-omm-rejects");
  }

  // OMM bottom level: header, block, one array pointer, then the dxmt header.
  auto blocks = serialized(1, 1, 0xffffffff00000001ull, 96 + 64, 24);
  put64(blocks.bytes, 64, 1);
  put(blocks.bytes, blocks.header + 52, 2);
  const size_t record = blocks.header + 64;
  put(blocks.bytes, record, 2);
  put(blocks.bytes, record + 8, 6);
  put(blocks.bytes, record + 28, 12);
  put64(blocks.bytes, record + 32, 160);
  put64(blocks.bytes, record + 64, 0x1000);
  check(blocks.parse(info) == 0 && info.header_offset == 80, "rtas-omm-blocks");
  for (const auto &[at, value] : {std::pair<size_t, uint64_t>{56, 1}, {64, 2}, {64, UINT64_MAX / 8},
                                  {48, 0xffffffff00000002ull}, {blocks.header + 52, 3}, {record + 64, 0}}) {
    bad = blocks;
    put64(bad.bytes, at, value);
    if (at == blocks.header + 52)
      put(bad.bytes, at + 4, 0);
    check(bad.parse(info) == DXMT_RTAS_SERIALIZED_INVALID_DATA, "rtas-blocks-rejects");
  }
  bool untouched = true;
  DxmtRtasSerializedLayout layout{7, 7, 7, 7};
  untouched &= dxmt_rtas_serialized_prefix(top.bytes.data(), 55, 1024, &layout) ==
                   DXMT_RTAS_SERIALIZED_INVALID_ARGUMENT && layout.serialized_size == 7;
  untouched &= dxmt_rtas_serialized_block(&top.layout, top.bytes.data(), 16, &layout) ==
                   DXMT_RTAS_SERIALIZED_INVALID_ARGUMENT && layout.header_offset == 7;
  check(untouched, "rtas-outputs-untouched");
}

// Little subobjects in the D3D12 stream layout of this ABI.
template <class T> static void subobject(Bytes &stream, uint32_t type, const T &data) {
  struct alignas(void *) Subobject {
    uint32_t type;
    T data;
  } value{type, data};
  const size_t offset = stream.size();
  stream.resize(offset + (sizeof(value) + sizeof(void *) - 1) / sizeof(void *) * sizeof(void *));
  std::memcpy(stream.data() + offset, &value, sizeof(value));
}

static void pso_streams() {
  DxmtPsoStreamLayout layouts[64] = {};
  layouts[0] = {sizeof(void *), alignof(void *)};
  layouts[1] = layouts[6] = {sizeof(D3D12_SHADER_BYTECODE), alignof(D3D12_SHADER_BYTECODE)};
  layouts[9] = {4, 4};
  layouts[10] = {sizeof(D3D12_RASTERIZER_DESC), 4};
  layouts[27] = {44, 4};
  layouts[15] = {sizeof(D3D12_RT_FORMAT_ARRAY), 4};
  Bytes stream;
  subobject(stream, 0, (void *)0x1234);
  subobject(stream, 9, 0xffu);
  subobject(stream, 1, D3D12_SHADER_BYTECODE{(void *)8, 16});
  subobject(stream, 15, D3D12_RT_FORMAT_ARRAY{});
  uint64_t defined = 0, offset = 0;
  unsigned seen = 0;
  bool walked = true;
  while (offset < stream.size()) {
    DxmtPsoStreamSubobject next;
    if (dxmt_pso_stream_next(stream.data(), stream.size(), layouts, offset, defined, &next)) {
      walked = false;
      break;
    }
    walked &= next.payload_offset == offset + (next.type == 0 || next.type == 1 ? sizeof(void *) : 4);
    defined = next.defined;
    offset = next.next_offset;
    ++seen;
  }
  check(walked && seen == 4 && defined == (1u << 0 | 1u << 9 | 1u << 1 | 1u << 15), "pso-stream-walk");
  // The final subobject may end before its pointer-aligned stride.
  auto status = [&](const Bytes &bytes, uint64_t at = 0, uint64_t mask = 0) {
    DxmtPsoStreamSubobject next;
    return dxmt_pso_stream_next(bytes.data(), bytes.size(), layouts, at, mask, &next);
  };
  Bytes mask;
  subobject(mask, 9, 0u);
  mask.resize(8);
  check(status(mask) == 0, "pso-stream-short-final-padding");
  bool truncated = true;
  for (size_t size = 1; size < sizeof(void *) + sizeof(D3D12_SHADER_BYTECODE); ++size) {
    Bytes cut(stream.begin() + 2 * sizeof(void *) + 8, stream.begin() + 2 * sizeof(void *) + 8 + size);
    truncated &= status(cut) == DXMT_PSO_STREAM_TRUNCATED;
  }
  check(truncated, "pso-stream-truncated-subobject");
  Bytes unknown;
  subobject(unknown, 23, 0u);
  check(status(unknown) == DXMT_PSO_STREAM_UNKNOWN_TYPE, "pso-stream-unhandled-type");
  put(unknown, 0, 64);
  check(status(unknown) == DXMT_PSO_STREAM_UNKNOWN_TYPE, "pso-stream-type-range");
  // Range precedes duplicates, which precede handled type and bounds.
  check(status(unknown, 0, ~0ull) == DXMT_PSO_STREAM_UNKNOWN_TYPE, "pso-stream-range-before-duplicate");
  Bytes truncated_duplicate{9, 0, 0, 0};
  check(status(truncated_duplicate, 0, 1u << 9) == DXMT_PSO_STREAM_DUPLICATE, "pso-stream-duplicate-before-bounds");
  Bytes rasterizer;
  subobject(rasterizer, 27, D3D12_RASTERIZER_DESC{});
  check(status(rasterizer, 0, 1u << 10) == DXMT_PSO_STREAM_DUPLICATE && status(rasterizer, 0, 1u << 27) ==
            DXMT_PSO_STREAM_DUPLICATE && status(rasterizer, 0, 1u << 11) == 0,
        "pso-stream-revision-groups");
  check(status(Bytes{1, 0, 0}) == DXMT_PSO_STREAM_TRUNCATED &&
            status(stream, stream.size()) == DXMT_PSO_STREAM_INVALID_ARGUMENT,
        "pso-stream-type-word");
}

// A PE32+ AMD64 image with one section holding the export directory and tables.
static Bytes pe_image(const std::initializer_list<std::pair<const char *, uint32_t>> &exports) {
  Bytes image(0x400, 0);
  put(image, 0, 0x5a4d);
  put(image, 60, 0x80);
  put(image, 0x80, 0x4550);
  put(image, 0x84, 0x8664 | 1u << 16); // machine, one section
  put(image, 0x94, 240);              // SizeOfOptionalHeader
  put(image, 0x98, 0x20b);
  put(image, 0x98 + 108, 16);          // NumberOfRvaAndSizes
  put(image, 0x98 + 112, 0x1000);      // export directory
  put(image, 0x98 + 116, 40);
  const size_t section = 0x98 + 240;
  put(image, section + 12, 0x1000);
  put(image, section + 16, 0x200);
  put(image, section + 20, 0x200);
  const uint32_t count = uint32_t(exports.size());
  put(image, 0x200 + 20, count);
  put(image, 0x200 + 24, count);
  put(image, 0x200 + 28, 0x1040);
  put(image, 0x200 + 32, 0x1080);
  put(image, 0x200 + 36, 0x10c0);
  uint32_t name = 0x1100, index = 0;
  for (const auto &[text, rva] : exports) {
    put(image, 0x240 + index * 4, rva);
    put(image, 0x280 + index * 4, name);
    image[0x2c0 + index * 2] = uint8_t(index);
    std::memcpy(image.data() + name - 0xe00, text, std::strlen(text) + 1);
    name += uint32_t(std::strlen(text) + 1);
    ++index;
  }
  return image;
}

static void pe_images() {
  const Bytes image = pe_image({{"Data", 0x1180}, {"Forward", 0x1010}, {"Late", 0x11fe}});
  DxmtPeImage pe{};
  uint64_t offset = 0;
  const auto lookup = [&](const Bytes &bytes, const char *name, uint64_t count) {
    return dxmt_pe_export(bytes.data(), bytes.size(), &pe, name, std::strlen(name), count, &offset);
  };
  check(dxmt_pe_image(image.data(), image.size(), 0x8664, &pe) == 0 && pe.section_count == 1 &&
            pe.name_count == 3 && lookup(image, "Data", 4) == 0 && offset == 0x380,
        "pe-valid-image");
  check(lookup(image, "Forward", 1) == DXMT_PE_NOT_FOUND && lookup(image, "Late", 2) == 0 &&
            lookup(image, "Late", 3) == DXMT_PE_NOT_FOUND && lookup(image, "Dat", 1) == DXMT_PE_NOT_FOUND &&
            lookup(image, "Missing", 1) == DXMT_PE_NOT_FOUND,
        "pe-export-lookup");
  check(dxmt_pe_image(image.data(), image.size(), 0x14c, &pe) == DXMT_PE_UNSUPPORTED &&
            dxmt_pe_image(image.data(), image.size(), 0xaa64, &pe) == DXMT_PE_UNSUPPORTED,
        "pe-machine");
  bool truncated = true;
  for (size_t size = 0; size < 0x2c6; ++size)
    truncated &= dxmt_pe_image(image.data(), size, 0x8664, &pe) == DXMT_PE_UNSUPPORTED;
  check(truncated, "pe-truncated-image");
  for (const auto &[at, value] : {std::pair<size_t, uint32_t>{60, 0x80000000u}, {0x84, 0x8664 | 97u << 16},
                                  {0x84, 0x8664}, {0x94, 224}, {0x98 + 108, 0}, {0x98 + 112, 0x1200},
                                  {0x98 + 240 + 16, 0x10}, {0x200 + 24, 65537}, {0x200 + 20, 0},
                                  {0x200 + 32, 0x11ff}, {0x40, 0x656e6957}}) {
    auto bad = image;
    put(bad, at, value);
    if (at == 0x40)
      std::memcpy(bad.data() + 0x40, "Wine builtin DLL", 17);
    check(dxmt_pe_image(bad.data(), bad.size(), 0x8664, &pe) == DXMT_PE_UNSUPPORTED, "pe-header-rejects");
  }
  dxmt_pe_image(image.data(), image.size(), 0x8664, &pe);
  auto ordinal = image;
  ordinal[0x2c0] = 3;
  check(lookup(ordinal, "Data", 4) == DXMT_PE_NOT_FOUND, "pe-ordinal-range");
  check(dxmt_pe_export(image.data(), image.size(), &pe, "Da\0a", 4, 1, &offset) == DXMT_PE_INVALID_ARGUMENT,
        "pe-name-argument");
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s raytracing-stateobj.dxil\n", argv[0]);
    return 2;
  }
  signatures();
  programs();
  library();
  subobjects(argv[1]);
  rtas();
  pso_streams();
  pe_images();
  std::printf("PARSERS_%s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
