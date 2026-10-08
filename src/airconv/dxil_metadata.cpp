/*
 * DXIL container + LLVM bitcode metadata frontend.
 *
 * Every read is bounds checked: a truncated or hostile container/bitcode produces
 * an llvm::Error, never an out-of-bounds access. Parsing is synchronous and uses a
 * fresh LLVMContext per ModuleInfo, so concurrent compiles share no IR.
 */

#include "dxil_converter.hpp"
#include "dxil_matrix.hpp"

#include <bit>
#include <cstring>

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include "DXBCParser/ShaderBinary.h"
#include "airconv_error.hpp"

namespace dxmt::dxil {

/**
 * The stage of one DXIL entry, from the `ShaderKind` value in its
 * `!dx.entryPoints` properties (tag 8). These are the on-disk DXIL ABI numbers, the
 * same ones SM50_RT_STAGE uses, so a raytracing export's stage never has to be
 * translated: the state object passes the entry's own kind through.
 *
 * Declared in dxil_converter.hpp, so it is defined here at namespace scope rather
 * than in this file's anonymous namespace - a raytracing pipeline lowers its exports
 * through it from another translation unit.
 */
Stage stageFromShaderKind(uint32_t shader_kind) {
  switch (shader_kind) {
  case 0:
    return Stage::Pixel;
  case 1:
    return Stage::Vertex;
  case 2:
    return Stage::Geometry;
  case 3:
    return Stage::Hull;
  case 4:
    return Stage::Domain;
  case 5:
    return Stage::Compute;
  case 6:
    return Stage::Library;
  case 7:
    return Stage::RayGeneration;
  case 8:
    return Stage::Intersection;
  case 9:
    return Stage::AnyHit;
  case 10:
    return Stage::ClosestHit;
  case 11:
    return Stage::Miss;
  case 12:
    return Stage::Callable;
  case 13:
    return Stage::Mesh;
  case 14:
    return Stage::Amplification;
  default:
    return Stage::Invalid;
  }
}

namespace {

constexpr uint32_t kDXBCMagic = 0x43425844;    // 'DXBC'
constexpr uint32_t kBitcodeMagic = 0xdec04342; // 'BC\xc0\xde'

/** ISG1/OSG1/PSG1 elements are at least 32 bytes; PSG1 appends interpolation mode. */
constexpr size_t kSignatureElementMinSize = 32;

struct ChunkView {
  const uint8_t *data = nullptr;
  size_t size = 0;
};

uint32_t readU32(const uint8_t *p) {
  uint32_t value = 0;
  memcpy(&value, p, sizeof(value));
  return value;
}

uint32_t shaderKindFromProgramVersion(uint32_t version) {
  return (version >> 16) & 0xffu;
}

/* ------------------------------------------------------------------ */
/* LLVM metadata helpers                                              */
/* ------------------------------------------------------------------ */

llvm::MDNode *getNode(const llvm::MDNode *node, unsigned index) {
  if (!node || index >= node->getNumOperands())
    return nullptr;
  return llvm::dyn_cast_or_null<llvm::MDNode>(node->getOperand(index));
}

bool getU32(const llvm::MDNode *node, unsigned index, uint32_t &out) {
  if (!node || index >= node->getNumOperands())
    return false;
  auto *constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(node->getOperand(index));
  if (!constant)
    return false;
  auto *integer = llvm::dyn_cast<llvm::ConstantInt>(constant->getValue());
  if (!integer)
    return false;
  out = static_cast<uint32_t>(integer->getZExtValue());
  return true;
}

uint32_t getU32Or(const llvm::MDNode *node, unsigned index, uint32_t fallback) {
  uint32_t value = 0;
  return getU32(node, index, value) ? value : fallback;
}

bool getBool(const llvm::MDNode *node, unsigned index) {
  return getU32Or(node, index, 0) != 0;
}

std::string getString(const llvm::MDNode *node, unsigned index) {
  if (!node || index >= node->getNumOperands())
    return {};
  auto *string = llvm::dyn_cast_or_null<llvm::MDString>(node->getOperand(index));
  return string ? string->getString().str() : std::string();
}

llvm::Value *getValue(const llvm::MDNode *node, unsigned index) {
  if (!node || index >= node->getNumOperands())
    return nullptr;
  auto *constant = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(node->getOperand(index));
  return constant ? constant->getValue() : nullptr;
}

/* ------------------------------------------------------------------ */
/* Container                                                          */
/* ------------------------------------------------------------------ */

llvm::Expected<ChunkView> findChunk(const void *bytecode, size_t size, const char *fourcc) {
  if (!bytecode || size < 32)
    return createError("DXBC container is truncated");
  auto *bytes = static_cast<const uint8_t *>(bytecode);
  if (readU32(bytes) != kDXBCMagic)
    return createError("not a DXBC container");

  uint32_t chunk_count = readU32(bytes + 28);
  if (chunk_count > (size - 32) / 4)
    return createError("DXBC chunk table is out of bounds");

  for (uint32_t i = 0; i < chunk_count; i++) {
    uint32_t offset = readU32(bytes + 32 + i * 4);
    if (offset < 32 || offset > size - 8)
      return createError("DXBC chunk offset is out of bounds");
    uint32_t chunk_size = readU32(bytes + offset + 4);
    if (chunk_size > size - offset - 8)
      return createError("DXBC chunk size is out of bounds");
    if (!memcmp(bytes + offset, fourcc, 4))
      return ChunkView{bytes + offset + 8, chunk_size};
  }
  return createError("DXBC chunk not found");
}

/* ------------------------------------------------------------------ */
/* Signature chunks                                                   */
/* ------------------------------------------------------------------ */

llvm::Error parseSignatureChunk(const ChunkView &chunk, std::vector<SigElement> &out) {
  if (chunk.size == 0)
    return llvm::Error::success();
  if (chunk.size < 8)
    return createError("DXIL signature chunk is truncated");

  uint32_t count = readU32(chunk.data);
  uint32_t elements_offset = readU32(chunk.data + 4);
  if (count == 0)
    return llvm::Error::success();
  if (elements_offset < 8 || elements_offset > chunk.size)
    return createError("DXIL signature element offset is out of bounds");

  /* The name table follows the element array, and names are emitted in element
   * order, so the first name offset marks the end of the element array. This is
   * what fixes the element stride (32 for ISG1/OSG1, larger for PSG1). */
  if (elements_offset + 8 > chunk.size)
    return createError("DXIL signature element is truncated");
  uint32_t first_name_offset = readU32(chunk.data + elements_offset + 4);
  if (first_name_offset < elements_offset || first_name_offset > chunk.size)
    return createError("DXIL signature name offset is out of bounds");
  size_t element_bytes = first_name_offset - elements_offset;
  if (element_bytes % count != 0)
    return createError("DXIL signature element stride is inconsistent");
  size_t stride = element_bytes / count;
  if (stride < kSignatureElementMinSize)
    return createError("DXIL signature element is too small");

  out.reserve(out.size() + count);
  for (uint32_t i = 0; i < count; i++) {
    size_t base = elements_offset + i * stride;
    if (base + stride > chunk.size)
      return createError("DXIL signature element is out of bounds");
    const uint8_t *element = chunk.data + base;

    SigElement sig;
    sig.id = i;
    sig.stream = readU32(element);
    uint32_t name_offset = readU32(element + 4);
    if (name_offset >= chunk.size)
      return createError("DXIL signature name is out of bounds");
    sig.semantic_index = readU32(element + 8);
    sig.semantic_kind = static_cast<uint8_t>(readU32(element + 12));
    sig.comp_type = static_cast<uint8_t>(readU32(element + 16));
    uint32_t register_index = readU32(element + 20);
    sig.mask = element[24];
    sig.read_write_mask = element[25];
    /* element[26..27] is the stream (already read from +0 for the same value) and
     * element[28..31] the min precision, which AIR lowering does not need. */

    size_t name_start = name_offset;
    size_t name_end = name_start;
    while (name_end < chunk.size && chunk.data[name_end] != 0)
      name_end++;
    if (name_end >= chunk.size)
      return createError("DXIL signature name is not terminated");
    sig.name.assign(reinterpret_cast<const char *>(chunk.data + name_start), name_end - name_start);

    sig.rows = 1;
    sig.start_row = register_index;
    sig.start_col = sig.mask ? std::countr_zero(sig.mask) : 0;
    sig.cols = std::popcount(sig.mask & 0xf);
    if (sig.cols == 0)
      sig.cols = 1;
    out.push_back(std::move(sig));
  }
  return llvm::Error::success();
}

/* ------------------------------------------------------------------ */
/* Entry point signature metadata                                     */
/* ------------------------------------------------------------------ */

struct MetadataSignatureElement {
  uint32_t id = 0;
  std::string name;
  uint32_t interp = 0;
  uint32_t rows = 1;
  uint32_t cols = 1;
  uint32_t start_row = 0;
  uint32_t start_col = 0;
};

/**
 * `!dx.entryPoints` signature parameters carry the interpolation mode, which the
 * ISG1/OSG1 chunks do not encode. Field map verified against dxil-spirv and against
 * the chunk contents across the corpus:
 *   4 = semantic index node, 5 = interpolation mode, 6 = rows, 7 = cols,
 *   8 = start row (the register), 9 = start column.
 */
std::vector<MetadataSignatureElement> parseMetadataSignature(llvm::MDNode *list) {
  std::vector<MetadataSignatureElement> elements;
  if (!list)
    return elements;
  elements.reserve(list->getNumOperands());
  for (unsigned i = 0; i < list->getNumOperands(); i++) {
    llvm::MDNode *parameter = getNode(list, i);
    MetadataSignatureElement element;
    if (parameter) {
      element.id = getU32Or(parameter, 0, i);
      element.name = getString(parameter, 1);
      element.interp = getU32Or(parameter, 5, 0);
      element.rows = getU32Or(parameter, 6, 1);
      element.cols = getU32Or(parameter, 7, 1);
      element.start_row = getU32Or(parameter, 8, 0);
      element.start_col = getU32Or(parameter, 9, 0);
    }
    elements.push_back(element);
  }
  return elements;
}

/**
 * The chunk is authoritative for registers and masks; the metadata list only adds
 * the interpolation mode (and, for producers without a chunk, rows/columns).
 */
void applyMetadataSignature(
  const std::vector<MetadataSignatureElement> &metadata, std::vector<SigElement> &signature
) {
  // Container signatures flatten arrays into rows; DXIL instructions address the
  // original metadata element ID plus a row. Match packing, not list ordinals.
  for (auto &row : signature) {
    for (const auto &element : metadata) {
      if (!llvm::StringRef(row.name).equals_insensitive(element.name)) continue;
      if (row.start_row != element.start_row &&
          (row.start_row < element.start_row || row.start_row - element.start_row >= element.rows)) continue;
      if (row.start_col != element.start_col) continue;
      row.id = element.id;
      row.element_row = row.start_row == element.start_row ? 0 : row.start_row - element.start_row;
      row.interp = static_cast<uint8_t>(element.interp);
      break;
    }
  }
}

/** Fallback for containers without signature chunks (libraries). */
std::vector<SigElement> signatureFromMetadata(llvm::MDNode *list) {
  std::vector<SigElement> signature;
  if (!list)
    return signature;
  for (unsigned i = 0; i < list->getNumOperands(); i++) {
    llvm::MDNode *parameter = getNode(list, i);
    if (!parameter)
      continue;
    SigElement element;
    element.id = i;
    element.semantic_index = getU32Or(parameter, 4, 0);
    element.name = getString(parameter, 1);
    element.interp = static_cast<uint8_t>(getU32Or(parameter, 5, 0));
    element.rows = getU32Or(parameter, 6, 1);
    element.cols = getU32Or(parameter, 7, 1);
    element.start_row = getU32Or(parameter, 8, 0);
    element.start_col = getU32Or(parameter, 9, 0);
    element.mask = element.cols >= 4 ? 0xf : ((1u << element.cols) - 1u) << element.start_col;
    /* D3D_NAME is stored at metadata operand 3 (DXIL::SemanticKind there); the
     * signature chunk encoding is authoritative, so this fallback leaves it 0
     * unless the producer used the D3D encoding. */
    signature.push_back(std::move(element));
  }
  return signature;
}

/* ------------------------------------------------------------------ */
/* Resources                                                          */
/* ------------------------------------------------------------------ */

unsigned resourceTagIndex(const llvm::MDNode *node) {
  /* The tag node is the last metadata operand of SRV/UAV declarations. Scanning
   * backwards keeps older producers (fewer flag operands) working. */
  for (unsigned i = node->getNumOperands(); i > 6; i--) {
    llvm::MDNode *candidate = getNode(node, i - 1);
    if (!candidate || candidate->getNumOperands() < 2)
      continue;
    uint32_t tag = 0;
    if (!getU32(candidate, 0, tag))
      continue;
    if (tag == 0 || tag == 1)
      return i - 1;
  }
  return ~0u;
}

llvm::Error parseResourceClass(
  llvm::MDNode *list, ResourceClass klass, std::vector<ResourceBinding> &out
) {
  if (!list)
    return llvm::Error::success();
  for (unsigned i = 0; i < list->getNumOperands(); i++) {
    llvm::MDNode *node = getNode(list, i);
    if (!node)
      continue;
    ResourceBinding binding;
    binding.klass = klass;
    binding.declaration_id = getU32Or(node, 0, 0);
    binding.space = getU32Or(node, 3, 0);
    binding.lower_bound = getU32Or(node, 4, 0);
    binding.range_size = getU32Or(node, 5, 1);
    binding.name = getString(node, 2);
    binding.symbol = getValue(node, 1);

    uint32_t candidate_kind = getU32Or(node, 6, 0);
    switch (klass) {
    case ResourceClass::CBV:
      /* A constant buffer has no element type in metadata; the register access type
       * comes from each load. Operand 6 is the buffer size in bytes. */
      binding.kind = ResourceKind::CBuffer;
      binding.size_in_bytes = candidate_kind;
      break;
    case ResourceClass::Sampler:
      binding.kind = ResourceKind::Sampler;
      break;
    case ResourceClass::UAV:
      binding.kind = static_cast<ResourceKind>(candidate_kind);
      binding.globally_coherent = getBool(node, 7);
      binding.has_counter = getBool(node, 8);
      binding.rasterizer_order = getBool(node, 9);
      break;
    case ResourceClass::SRV:
      binding.kind = static_cast<ResourceKind>(candidate_kind);
      break;
    }

    unsigned tag_index = resourceTagIndex(node);
    if (tag_index != ~0u) {
      llvm::MDNode *tags = getNode(node, tag_index);
      uint32_t tag = getU32Or(tags, 0, ~0u);
      if (tag == 0) {
        /* Sampled format: the second operand is a DXIL::ComponentType. */
        binding.component_type = static_cast<ComponentType>(getU32Or(tags, 1, 0));
      } else if (tag == 1) {
        /* Structured/raw buffer: the second operand is the byte stride. */
        binding.structure_stride = getU32Or(tags, 1, 0);
      }
    }

    out.push_back(std::move(binding));
  }
  return llvm::Error::success();
}

/* ------------------------------------------------------------------ */
/* Usage scan                                                         */
/* ------------------------------------------------------------------ */

/** Opcodes that read, write, sample or compare through a resource handle. */
struct OpUsage {
  bool read = false;
  bool written = false;
  bool sampled = false;
  bool compared = false;
  bool counter = false;
};

OpUsage usageOfOpcode(uint32_t opcode) {
  OpUsage usage;
  switch (opcode) {
  case 58: /* CBufferLoad */
  case 59: /* CBufferLoadLegacy */
    usage.read = true;
    break;
  case 60: /* Sample */
  case 61: /* SampleBias */
  case 62: /* SampleLevel */
  case 63: /* SampleGrad */
    usage.read = true;
    usage.sampled = true;
    break;
  case 64: /* SampleCmp */
  case 65: /* SampleCmpLevelZero */
  case 224: /* SampleCmpLevel */
  case 254: /* SampleCmpGrad */
  case 255: /* SampleCmpBias */
    usage.read = true;
    usage.sampled = true;
    usage.compared = true;
    break;
  case 66: /* TextureLoad */
  case 68: /* BufferLoad */
  case 71: /* CheckAccessFullyMapped */
  case 72: /* GetDimensions */
  case 75: /* Texture2DMSGetSamplePosition */
  case 76: /* RenderTargetGetSamplePosition */
  case 77: /* RenderTargetGetSampleCount */
  case 81: /* CalculateLOD */
  case 139: /* RawBufferLoad */
  case 303: /* RawBufferVectorLoad (DXIL 1.9) */
  case 141: /* RawBufferLoad (status) */
    usage.read = true;
    if (opcode == 81)
      usage.sampled = true;
    break;
  case 73: /* TextureGather */
  case 223: /* TextureGatherRaw */
    usage.read = true;
    usage.sampled = true;
    break;
  case 74: /* TextureGatherCmp */
    usage.read = true;
    usage.sampled = true;
    usage.compared = true;
    break;
  case 67: /* TextureStore */
  case 225: /* TextureStoreSample */
  case 69: /* BufferStore */
  case 140: /* RawBufferStore */
  case 304: /* RawBufferVectorStore (DXIL 1.9) */
  case 142: /* RawBufferStore (status) */
    usage.written = true;
    break;
  case 70: /* BufferUpdateCounter */
    usage.written = true;
    usage.read = true;
    usage.counter = true;
    break;
  case 78: /* AtomicBinOp */
  case 79: /* AtomicCompareExchange */
  case 174: /* WriteSamplerFeedback (the feedback map is arg 1) */
  case 175: /* WriteSamplerFeedbackBias */
  case 176: /* WriteSamplerFeedbackLevel */
  case 177: /* WriteSamplerFeedbackGrad */
    usage.read = true;
    usage.written = true;
    break;
  default:
    break;
  }
  return usage;
}

uint32_t constantU32(const llvm::Value *value) {
  auto *integer = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
  return integer ? static_cast<uint32_t>(integer->getZExtValue()) : 0;
}

struct HandleOrigin {
  ResourceClass klass = ResourceClass::SRV;
  bool has_class = false;
  uint32_t declaration_id = 0;
  bool has_declaration = false;
  uint32_t space = 0;
  uint32_t lower_bound = 0;
  bool has_binding = false;
  const llvm::GlobalVariable *symbol = nullptr;
};

HandleOrigin resolveHandle(const llvm::Value *handle, unsigned depth = 0) {
  HandleOrigin origin;
  if (!handle || depth > 8)
    return origin;
  auto *call = llvm::dyn_cast<llvm::CallInst>(handle);
  if (!call || !call->getCalledFunction())
    return origin;
  llvm::StringRef name = call->getCalledFunction()->getName();
  if (!name.startswith("dx.op."))
    return origin;
  uint32_t opcode = call->arg_size() ? constantU32(call->getArgOperand(0)) : 0;
  switch (opcode) {
  case 160: /* createHandleForLib(opcode, resource object) */
    if (call->arg_size() >= 2) {
      const llvm::Value *symbol = call->getArgOperand(1);
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(symbol))
        symbol = load->getPointerOperand();
      origin.symbol = llvm::dyn_cast<llvm::GlobalVariable>(llvm::getUnderlyingObject(symbol));
    }
    break;
  case 57: /* createHandle(opcode, i8 class, i32 range id, i32 index, i1 nonUniform) */
    if (call->arg_size() >= 3) {
      origin.klass = static_cast<ResourceClass>(constantU32(call->getArgOperand(1)));
      origin.has_class = true;
      origin.declaration_id = constantU32(call->getArgOperand(2));
      origin.has_declaration = true;
    }
    break;
  case 217: /* createHandleFromBinding(opcode, ResBind, index, nonUniform) */
    if (call->arg_size() >= 2) {
      auto *binding = llvm::dyn_cast<llvm::Constant>(call->getArgOperand(1));
      auto *type = binding ? llvm::dyn_cast<llvm::StructType>(binding->getType()) : nullptr;
      if (type && type->getNumElements() >= 4 &&
          (llvm::isa<llvm::ConstantAggregate>(binding) || llvm::isa<llvm::ConstantAggregateZero>(binding))) {
        auto *lower = llvm::dyn_cast_or_null<llvm::ConstantInt>(binding->getAggregateElement(0u));
        auto *space = llvm::dyn_cast_or_null<llvm::ConstantInt>(binding->getAggregateElement(2u));
        auto *klass = llvm::dyn_cast_or_null<llvm::ConstantInt>(binding->getAggregateElement(3u));
        if (lower && space && klass) {
          origin.lower_bound = constantU32(lower);
          origin.space = constantU32(space);
          origin.klass = static_cast<ResourceClass>(constantU32(klass));
          origin.has_class = true;
          origin.has_binding = true;
        }
      }
    }
    break;
  case 216: /* annotateHandle(opcode, Handle, ResourceProperties) */
    if (call->arg_size() >= 3)
      return resolveHandle(call->getArgOperand(1), depth + 1);
    break;
  default:
    break;
  }
  return origin;
}

ResourceBinding *findBinding(std::vector<ResourceBinding> &resources, const HandleOrigin &origin) {
  if (origin.symbol) {
    for (auto &binding : resources)
      if (binding.symbol && llvm::getUnderlyingObject(binding.symbol) == origin.symbol)
        return &binding;
    // Stripped library reflection retains only the resource name. Use the same
    // mangled-name contract as resource lowering, rejecting ambiguous matches.
    ResourceBinding *match = nullptr;
    for (auto &binding : resources) {
      if (binding.name.empty() || !origin.symbol->getName().contains("?" + binding.name + "@@"))
        continue;
      if (match)
        return nullptr;
      match = &binding;
    }
    return match;
  }
  for (auto &binding : resources) {
    if (origin.has_class && binding.klass != origin.klass)
      continue;
    if (origin.has_declaration && binding.declaration_id == origin.declaration_id)
      return &binding;
    if (origin.has_binding && binding.space == origin.space && binding.lower_bound == origin.lower_bound)
      return &binding;
  }
  return nullptr;
}

void accumulateUsage(std::vector<ResourceBinding> &resources, const HandleOrigin &origin, const OpUsage &usage) {
  if (auto *binding = findBinding(resources, origin)) {
    binding->read |= usage.read;
    binding->written |= usage.written;
    binding->sampled |= usage.sampled;
    binding->compared |= usage.compared;
    binding->with_counter |= usage.counter;
  }
}

void scanUsage(llvm::Module &module, std::vector<ResourceBinding> &resources) {
  for (llvm::Function &function : module) {
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call || !call->getCalledFunction())
          continue;
        llvm::StringRef name = call->getCalledFunction()->getName();
        if (!name.startswith("dx.op."))
          continue;
        if (!call->arg_size())
          continue;
        uint32_t opcode = constantU32(call->getArgOperand(0));
        if (opcode == 216 && call->arg_size() >= 3) {
          auto *properties = llvm::dyn_cast<llvm::ConstantAggregate>(call->getArgOperand(2));
          if (properties && properties->getNumOperands()) {
            HandleOrigin origin = resolveHandle(call->getArgOperand(1));
            if (auto *binding = findBinding(resources, origin); binding && binding->klass == ResourceClass::UAV) {
              uint32_t flags = constantU32(properties->getOperand(0));
              binding->rasterizer_order |= (flags & 0x2000u) != 0;
              binding->globally_coherent |= (flags & 0x4000u) != 0;
            }
          }
          continue;
        }
        OpUsage usage = usageOfOpcode(opcode);
        if (!usage.read && !usage.written)
          continue;
        if (opcode == 71) /* CheckAccessFullyMapped takes a status value, not a handle */
          continue;
        if (call->arg_size() < 2)
          continue;
        HandleOrigin origin = resolveHandle(call->getArgOperand(1));
        if (!origin.has_declaration && !origin.has_binding && !origin.symbol)
          continue;
        accumulateUsage(resources, origin, usage);
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                       */
/* ------------------------------------------------------------------ */

Stage stageFromString(llvm::StringRef name) {
  if (name == "ps")
    return Stage::Pixel;
  if (name == "vs")
    return Stage::Vertex;
  if (name == "gs")
    return Stage::Geometry;
  if (name == "hs")
    return Stage::Hull;
  if (name == "ds")
    return Stage::Domain;
  if (name == "cs")
    return Stage::Compute;
  if (name == "lib")
    return Stage::Library;
  if (name == "ms")
    return Stage::Mesh;
  if (name == "as")
    return Stage::Amplification;
  return Stage::Invalid;
}

/**
 * Read one `!dx.entryPoints` properties value, i.e. the second half of a
 * `(tag, value)` pair. `ShaderKind` lives in tag 8 and is what distinguishes the
 * entries of a library container; the `!dx.shaderModel` string only says "lib" for
 * all of them.
 */
bool readEntryProperty(llvm::MDNode *properties, uint32_t tag, uint32_t &value) {
  if (!properties)
    return false;
  for (unsigned i = 0; i + 1 < properties->getNumOperands(); i += 2) {
    uint32_t candidate = 0;
    if (!getU32(properties, i, candidate) || candidate != tag)
      continue;
    return getU32(properties, i + 1, value);
  }
  return false;
}

llvm::MDNode *selectEntryPoint(llvm::Module &module, const char *requested) {
  llvm::NamedMDNode *entry_points = module.getNamedMetadata("dx.entryPoints");
  if (!entry_points || !entry_points->getNumOperands())
    return nullptr;
  llvm::MDNode *fallback = nullptr;
  unsigned candidates = 0;
  for (unsigned i = 0; i < entry_points->getNumOperands(); i++) {
    llvm::MDNode *node = entry_points->getOperand(i);
    if (!node || !getValue(node, 0))
      continue;
    candidates++;
    if (!fallback)
      fallback = node;
    if (requested && requested[0] && getString(node, 1) == requested)
      return node;
  }
  /* Raster shaders are compiled with a DXBC-style entry name that cannot match a
   * DXIL entry point, and they only ever carry one entry. Libraries must name the
   * entry exactly: picking one at random would silently lower the wrong function. */
  if (requested && requested[0] && candidates > 1)
    return nullptr;
  return fallback;
}

bool tryFindChunk(const void *bytecode, size_t size, const char *fourcc, ChunkView &out) {
  auto chunk = findChunk(bytecode, size, fourcc);
  if (!chunk) {
    llvm::consumeError(chunk.takeError());
    return false;
  }
  out = *chunk;
  return true;
}

void readVersion(
  llvm::Module &module, const char *name, uint32_t &major, uint32_t &minor
) {
  auto *named = module.getNamedMetadata(name);
  if (!named || !named->getNumOperands())
    return;
  llvm::MDNode *node = named->getOperand(0);
  major = getU32Or(node, 0, 0);
  minor = getU32Or(node, 1, 0);
}

/**
 * `!dx.entryPoints` properties are tag/value pairs. Tag 4 carries the thread group
 * size of a compute entry; tag 8 (`kDxilShaderKindTag`, declared in
 * dxil_converter.hpp) carries the entry's `DXIL::ShaderKind`.
 */
void readThreadGroupSize(llvm::MDNode *properties, uint32_t (&num_threads)[3]) {
  if (!properties)
    return;
  for (unsigned i = 0; i + 1 < properties->getNumOperands(); i += 2) {
    uint32_t tag = 0;
    if (!getU32(properties, i, tag) || tag != 4)
      continue;
    llvm::MDNode *threads = getNode(properties, i + 1);
    if (!threads || threads->getNumOperands() < 3)
      continue;
    num_threads[0] = getU32Or(threads, 0, 1);
    num_threads[1] = getU32Or(threads, 1, 1);
    num_threads[2] = getU32Or(threads, 2, 1);
  }
}

/**
 * Output registers are what the shader actually writes, not what it declares: the
 * SM 5.x reflection counts output writes the same way.
 */
void scanOutputWrites(llvm::Module &module, ModuleInfo &out) {
  for (llvm::Function &function : module) {
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call || !call->getCalledFunction())
          continue;
        llvm::StringRef name = call->getCalledFunction()->getName();
        if (!name.startswith("dx.op."))
          continue;
        uint32_t opcode = call->arg_size() ? constantU32(call->getArgOperand(0)) : 0;
        if (opcode == 5 && call->arg_size() >= 2) { /* storeOutput(reg, row, col, value) */
          uint32_t reg = constantU32(call->getArgOperand(1));
          if (reg < 32) {
            out.output_register_mask |= (1u << reg);
            out.max_output_register = std::max(out.max_output_register, reg + 1);
          }
        } else if (opcode == 91) { /* coverage */
          out.has_coverage_output = true;
        }
      }
    }
  }
}

llvm::Error extractMetadata(
  const void *bytecode, size_t size, llvm::Module &module, ModuleInfo &out,
  const char *requested_entry
) {
  if (auto *shader_model = module.getNamedMetadata("dx.shaderModel")) {
    if (shader_model->getNumOperands()) {
      llvm::MDNode *node = shader_model->getOperand(0);
      out.stage = stageFromString(getString(node, 0));
      out.shader_model_major = getU32Or(node, 1, 0);
      out.shader_model_minor = getU32Or(node, 2, 0);
    }
  }
  if (out.stage == Stage::Invalid)
    return createError("DXIL module does not declare a shader model");
  auto matrix_contract = ValidateMatrixContract(module, out.shader_model_major, out.shader_model_minor);
  if (!matrix_contract)
    return matrix_contract.takeError();
  if (out.shader_model_major != 6 ||
      (((out.shader_model_major << 4) | out.shader_model_minor) > SM50_MAX_DXIL_SHADER_MODEL &&
       !(out.shader_model_minor == 10 && *matrix_contract)))
    return createError(llvm::Twine("DXIL shader model ") + llvm::Twine(out.shader_model_major) + "." +
                       llvm::Twine(out.shader_model_minor) + " is not supported (highest supported is " +
                       llvm::Twine(SM50_MAX_DXIL_SHADER_MODEL >> 4) + "." +
                       llvm::Twine(SM50_MAX_DXIL_SHADER_MODEL & 0xf) + ")");
  readVersion(module, "dx.version", out.dxil_major, out.dxil_minor);
  readVersion(module, "dx.valver", out.valver_major, out.valver_minor);

  // Native DXIL vectors are a 1.9 ABI, not a blanket permission for every dx.op
  // overload. Reject illegal resource/signature/payload overloads before staging.
  auto validate_type = [&](auto &self, llvm::Type *type, bool single_allowed) -> llvm::Error {
    if (auto *vector = llvm::dyn_cast<llvm::VectorType>(type)) {
      auto *fixed = llvm::dyn_cast<llvm::FixedVectorType>(vector);
      if (!fixed || fixed->getNumElements() > 1024 ||
          (fixed->getNumElements() == 1 && !single_allowed))
        return createError("DXIL native vector length must be 2 through 1024 (1 only in library interfaces)");
    } else if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type)) {
      return self(self, array->getElementType(), single_allowed);
    } else if (auto *structure = llvm::dyn_cast<llvm::StructType>(type)) {
      for (auto *element : structure->elements())
        if (auto error = self(self, element, single_allowed))
          return error;
    }
    return llvm::Error::success();
  };
  for (auto &function : module) {
    const bool exported = out.stage == Stage::Library && !function.hasLocalLinkage() &&
                          !function.getName().startswith("dx.op.");
    if (auto error = validate_type(validate_type, function.getReturnType(), exported))
      return error;
    for (auto &argument : function.args())
      if (auto error = validate_type(validate_type, argument.getType(), exported))
        return error;
    for (auto &block : function)
      for (auto &instruction : block) {
        if (auto error = validate_type(validate_type, instruction.getType(), exported))
          return error;
        if (auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction))
          if (auto error = validate_type(validate_type, allocation->getAllocatedType(), false))
            return error;
        auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
        if (!call || !call->getCalledFunction() ||
            !call->getCalledFunction()->getName().startswith("dx.op."))
          continue;
        // A library resource object can contain legacy float2/3/4 fields, as
        // can DXR payload/attribute structs. Those are not native dx.op vector
        // overloads. The overload suffix and new vector-only opcodes identify
        // the 1.9 ABI without rejecting older library resource handles.
        auto suffix = call->getCalledFunction()->getName().rsplit('.').second;
        bool vectors = suffix.size() > 1 && suffix.front() == 'v' &&
                       suffix[1] >= '0' && suffix[1] <= '9';
        if (!call->arg_empty()) {
          uint32_t opcode = constantU32(call->getArgOperand(0));
          vectors |= opcode == 303 || opcode == 304 || (opcode >= 309 && opcode <= 311);
        }
        if (!vectors)
          continue;
        auto matrix_operation = IsMatrixOperation(*call);
        if (!matrix_operation)
          return matrix_operation.takeError();
        if (*matrix_operation)
          continue;
        if (call->arg_empty())
          return createError("native vector dx.op call is missing its opcode");
        if (out.shader_model_minor < 9 || out.dxil_major != 1 || out.dxil_minor < 9)
          return createError("native dx.op vectors require Shader Model 6.9 and DXIL 1.9");
        uint32_t opcode = constantU32(call->getArgOperand(0));
        const bool allowed = (opcode >= 6 && opcode <= 40) || (opcode >= 46 && opcode <= 49) ||
            (opcode >= 83 && opcode <= 86) || opcode == 115 || (opcode >= 117 && opcode <= 123) ||
            opcode == 165 || opcode == 166 || opcode == 303 || opcode == 304 ||
            (opcode >= 309 && opcode <= 311);
        if (!allowed)
          return createError(llvm::Twine("dx.op opcode ") + llvm::Twine(opcode) +
                             " does not permit native vectors");
        if (opcode == 115 && call->arg_size() >= 2 && call->getArgOperand(1)->getType()->isVectorTy() &&
            !call->getType()->isVectorTy())
          return createError("vector WaveActiveAllEqual must return a componentwise i1 vector");
      }
  }
  for (auto &global : module.globals())
    if (auto error = validate_type(validate_type, global.getValueType(), false))
      return error;

  llvm::MDNode *entry = selectEntryPoint(module, requested_entry);
  if (!entry && out.stage != Stage::Library)
    return requested_entry && requested_entry[0]
      ? createError(llvm::Twine("DXIL entry point not found: ") + requested_entry)
      : createError("DXIL module has no entry point");
  out.entry = entry ? llvm::dyn_cast_or_null<llvm::Function>(getValue(entry, 0)) :
      (requested_entry && requested_entry[0] ? module.getFunction(requested_entry) : nullptr);
  if (!out.entry && (entry || (requested_entry && requested_entry[0])))
    return createError("DXIL entry point does not reference a function");
  out.entry_name = entry ? getString(entry, 1) : (requested_entry ? requested_entry : "");
  readThreadGroupSize(getNode(entry, 4), out.num_threads);
  /* Smaller waves partition an Apple SIMD32. Never accept a requirement for a
   * cross-SIMD wave: a hidden threadgroup barrier is invalid under divergence. */
  {
    llvm::MDNode *properties = getNode(entry, 4);
    uint32_t wave_size = 32;
    bool declared = false;
    auto legal_size = [](uint32_t size) { return size >= 4 && size <= 128 && (size & (size - 1)) == 0; };
    for (unsigned i = 0; properties && i + 1 < properties->getNumOperands(); i += 2) {
      uint32_t tag = 0;
      if (!getU32(properties, i, tag) || (tag != kDxilWaveSizeTag && tag != kDxilRangedWaveSizeTag))
        continue;
      uint32_t shader_kind = static_cast<uint32_t>(out.stage);
      readEntryProperty(properties, kDxilShaderKindTag, shader_kind);
      /* DXIL kind15 is a work-graph node; other stages cannot declare WaveSize. */
      if (shader_kind != static_cast<uint32_t>(Stage::Compute) && shader_kind != 15)
        return createError("DXIL WaveSize is legal only on compute and node shaders");
      if ((tag == kDxilWaveSizeTag && (out.shader_model_minor < 6 ||
            (out.shader_model_minor >= 8 && !(out.shader_model_minor == 8 && *matrix_contract)))) ||
          (tag == kDxilRangedWaveSizeTag && out.shader_model_minor < 8))
        return createError("DXIL WaveSize metadata tag does not match the shader model");
      if (declared)
        return createError("DXIL has multiple WaveSize declarations");
      declared = true;
      llvm::MDNode *node = getNode(properties, i + 1);
      uint32_t minimum = 0, maximum = 0, preferred = 0;
      if (tag == kDxilWaveSizeTag) {
        if (!node || node->getNumOperands() != 1 || !getU32(node, 0, minimum) || !legal_size(minimum))
          return createError("DXIL has an invalid fixed WaveSize");
        maximum = minimum;
      } else {
        if (!node || node->getNumOperands() != 3 || !getU32(node, 0, minimum) ||
            !getU32(node, 1, maximum) || !getU32(node, 2, preferred) || !legal_size(minimum) ||
            (maximum && (!legal_size(maximum) || maximum < minimum)) ||
            (preferred && (!maximum || !legal_size(preferred) || preferred < minimum || preferred > maximum)))
          return createError("DXIL has an invalid ranged WaveSize tuple");
        if (!maximum)
          maximum = minimum;
      }
      if (minimum > 32)
        return createError("DXIL WaveSize requirement is unsupported: supported widths are 4, 8, 16 and 32; "
                           "64/128 require cross-SIMD execution");
      wave_size = preferred && preferred <= 32 ? preferred : std::min(maximum, 32u);
    }
    /* PSVRuntimeInfo0: 16-byte stage union, followed by expected min/max.
     * DXC may constrain widths even without an entry WaveSize property. */
    auto psv = findChunk(bytecode, size, "PSV0");
    if (psv) {
      if (psv->size < 28 || readU32(psv->data) < 24 || readU32(psv->data) > psv->size - 4)
        return createError("DXIL PSV0 runtime wave constraints are truncated");
      const uint32_t minimum = readU32(psv->data + 20), maximum = readU32(psv->data + 24);
      if (minimum > maximum)
        return createError("DXIL PSV0 expected wave range is invalid");
      if (!declared) {
        for (wave_size = 32; wave_size >= 4 && wave_size > maximum; wave_size >>= 1) {}
      }
      if (wave_size < 4 || wave_size < minimum || wave_size > maximum)
        return createError("DXIL selected wave width does not satisfy PSV0 expected wave range");
    } else {
      llvm::consumeError(psv.takeError()); /* Raw bitcode/library inputs need not have PSV0. */
    }
    if (declared || wave_size != 32)
      module.addModuleFlag(llvm::Module::Error, "dxmt.wave-size", wave_size);
  }
  // ForceEarlyDepthStencil is bit 3 in the low word of ShaderFlags (tag 0).
  uint32_t shader_flags = 0;
  readEntryProperty(getNode(entry, 4), 0, shader_flags);
  out.force_early_depth_stencil = (shader_flags & (1u << 3)) != 0;

  /*
   * A library container's `!dx.shaderModel` is "lib" for every entry, so the
   * shader model string cannot say which entry is a ray generation shader and
   * which is a miss shader. The per-entry answer is the `ShaderKind` in the
   * properties list (tag 8), which is what D3D12's own export reflection uses too.
   */
  {
    uint32_t shader_kind = 0;
    if (readEntryProperty(getNode(entry, 4), kDxilShaderKindTag, shader_kind)) {
      Stage stage = stageFromShaderKind(shader_kind);
      if (stage != Stage::Invalid)
        out.stage = stage;
    }
  }

  llvm::MDNode *resources = getNode(entry, 3);
  /* Stage state: tag 1 GS {input primitive, max vertex count, stream mask, topology,
   * instance count}, tag 2 DS {domain, input control points}, tag 3 HS {patch constant
   * function, input CPs, output CPs, domain, partitioning, output primitive, max factor}. */
  if (llvm::MDNode *properties = getNode(entry, 4)) {
    for (unsigned i = 0; i + 1 < properties->getNumOperands(); i += 2) {
      uint32_t tag = 0;
      if (!getU32(properties, i, tag))
        continue;
      llvm::MDNode *state = getNode(properties, i + 1);
      if (!state)
        continue;
      if (tag == 1 && out.stage == Stage::Geometry) {
        out.geometry.input_primitive = getU32Or(state, 0, 0);
        out.geometry.max_vertex_count = getU32Or(state, 1, 0);
        out.geometry.stream_mask = getU32Or(state, 2, 1);
        out.geometry.output_topology = getU32Or(state, 3, 0);
        out.geometry.instance_count = getU32Or(state, 4, 1);
      } else if (tag == 2 && out.stage == Stage::Domain) {
        out.domain.domain = getU32Or(state, 0, 0);
        out.domain.input_control_points = getU32Or(state, 1, 0);
      } else if (tag == 3 && out.stage == Stage::Hull) {
        out.patch_constant_function = llvm::dyn_cast_or_null<llvm::Function>(getValue(state, 0));
        out.hull.input_control_points = getU32Or(state, 1, 0);
        out.hull.output_control_points = getU32Or(state, 2, 0);
        out.hull.domain = getU32Or(state, 3, 0);
        out.hull.partitioning = getU32Or(state, 4, 0);
        out.hull.output_primitive = getU32Or(state, 5, 0);
        if (auto *factor = llvm::dyn_cast_or_null<llvm::ConstantFP>(getValue(state, 6)))
          out.hull.max_tess_factor = factor->getValueAPF().convertToFloat();
      } else if (tag == 9 && out.stage == Stage::Mesh) {
        /* {numthreads, max vertices, max primitives, topology, payload bytes} (kDxilMSStateTag). */
        if (llvm::MDNode *threads = getNode(state, 0))
          for (unsigned c = 0; c < 3; c++)
            out.num_threads[c] = getU32Or(threads, c, 1);
        out.mesh.max_vertices = getU32Or(state, 1, 0);
        out.mesh.max_primitives = getU32Or(state, 2, 0);
        out.mesh.output_topology = getU32Or(state, 3, 0);
        out.mesh.payload_size = getU32Or(state, 4, 0);
      } else if (tag == 10 && out.stage == Stage::Amplification) {
        /* {numthreads, payload bytes} (kDxilASStateTag). */
        if (llvm::MDNode *threads = getNode(state, 0))
          for (unsigned c = 0; c < 3; c++)
            out.num_threads[c] = getU32Or(threads, c, 1);
        out.mesh.payload_size = getU32Or(state, 1, 0);
      }
    }
  }
  if (!resources) {
    if (auto *named = module.getNamedMetadata("dx.resources"))
      if (named->getNumOperands())
        resources = named->getOperand(0);
  }
  if (resources) {
    for (unsigned klass = 0; klass < 4; klass++) {
      if (auto err = parseResourceClass(
            getNode(resources, klass), static_cast<ResourceClass>(klass), out.resources
          ))
        return err;
    }
  }
  // Library handle structs retain legacy short vectors even before SM6.9.
  // Long vectors, unlike those placeholders, are not legal constant/typed
  // resource element storage; raw and structured buffers are the only forms.
  auto wide_storage = [&](auto &self, llvm::Type *type) -> bool {
    if (auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type))
      return vector->getNumElements() > 4;
    if (auto *array = llvm::dyn_cast<llvm::ArrayType>(type))
      return self(self, array->getElementType());
    if (auto *structure = llvm::dyn_cast<llvm::StructType>(type))
      for (auto *element : structure->elements())
        if (self(self, element))
          return true;
    return false;
  };
  for (const auto &resource : out.resources) {
    if (!resource.symbol || resource.kind == ResourceKind::RawBuffer ||
        resource.kind == ResourceKind::StructuredBuffer)
      continue;
    if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(resource.symbol->stripPointerCasts());
        global && wide_storage(wide_storage, global->getValueType()))
      return createError("DXIL long vectors cannot be stored in constant buffers or typed resources");
  }

  /* Signature chunks are authoritative for registers, masks and component types;
   * the entry point metadata only contributes the interpolation mode, which the
   * chunks do not encode. */
  llvm::MDNode *signature = getNode(entry, 2);
  std::vector<MetadataSignatureElement> metadata_input, metadata_output, metadata_patch;

  ChunkView chunk;
  if (out.stage == Stage::Pixel && tryFindChunk(bytecode, size, "PSG1", chunk)) {
    if (auto err = parseSignatureChunk(chunk, out.input_sig))
      return err;
  } else if (tryFindChunk(bytecode, size, "ISG1", chunk)) {
    if (auto err = parseSignatureChunk(chunk, out.input_sig))
      return err;
  }
  if (tryFindChunk(bytecode, size, "OSG1", chunk)) {
    if (auto err = parseSignatureChunk(chunk, out.output_sig))
      return err;
  }
  /* DXIL containers store the hull/domain patch-constant signature, and the mesh shader's primitive
   * output signature, as PSG1 (DXC's DFCC_PatchConstantSignature); PCSG is the DXBC name. */
  const bool patch_stage = out.stage == Stage::Hull || out.stage == Stage::Domain || out.stage == Stage::Mesh;
  if ((patch_stage && tryFindChunk(bytecode, size, "PSG1", chunk)) ||
      tryFindChunk(bytecode, size, "PCSG", chunk)) {
    if (auto err = parseSignatureChunk(chunk, out.patch_constant_sig))
      return err;
  }

  if (signature) {
    llvm::MDNode *input_list = getNode(signature, 0);
    llvm::MDNode *output_list = getNode(signature, 1);
    llvm::MDNode *patch_list = getNode(signature, 2);
    metadata_input = parseMetadataSignature(input_list);
    metadata_output = parseMetadataSignature(output_list);
    metadata_patch = parseMetadataSignature(patch_list);
    /* Without a signature chunk the metadata list is the only description. */
    if (out.input_sig.empty())
      out.input_sig = signatureFromMetadata(input_list);
    if (out.output_sig.empty())
      out.output_sig = signatureFromMetadata(output_list);
    if (out.patch_constant_sig.empty())
      out.patch_constant_sig = signatureFromMetadata(patch_list);
  }

  applyMetadataSignature(metadata_input, out.input_sig);
  applyMetadataSignature(metadata_output, out.output_sig);
  applyMetadataSignature(metadata_patch, out.patch_constant_sig);


  scanUsage(module, out.resources);
  scanOutputWrites(module, out);
  return llvm::Error::success();
}

} // namespace

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool IsDXILContainer(const void *bytecode, size_t size) {
  auto chunk = findChunk(bytecode, size, "DXIL");
  if (!chunk) {
    llvm::consumeError(chunk.takeError());
    return false;
  }
  return true;
}

bool FindDXBCChunk(const void *bytecode, size_t size, const char *fourcc, DXBCChunkView &out) {
  auto chunk = findChunk(bytecode, size, fourcc);
  if (!chunk) {
    llvm::consumeError(chunk.takeError());
    return false;
  }
  out.data = chunk->data;
  out.size = chunk->size;
  return true;
}

/**
 * LLVM 15 parses DXIL bitcode with typed pointers. Diagnostics raised while reading
 * malformed input are collected instead of being printed, so a bad shader cannot
 * spam the host process.
 */
llvm::Expected<std::unique_ptr<llvm::Module>>
parseBitcode(
  llvm::LLVMContext &context, const uint8_t *data, size_t size,
  std::string &diagnostics
) {
  llvm::raw_string_ostream diagnostics_out(diagnostics);
  context.setDiagnosticHandlerCallBack(
    [](const llvm::DiagnosticInfo &info, void *opaque) {
      auto *out = static_cast<llvm::raw_string_ostream *>(opaque);
      if (info.getSeverity() == llvm::DS_Error ||
          info.getSeverity() == llvm::DS_Warning) {
        llvm::DiagnosticPrinterRawOStream printer(*out);
        info.print(printer);
        *out << '\n';
      }
    },
    &diagnostics_out
  );
  auto buffer = llvm::MemoryBuffer::getMemBuffer(
    llvm::StringRef(reinterpret_cast<const char *>(data), size), "dxil", false
  );
  auto module = llvm::parseBitcodeFile(buffer->getMemBufferRef(), context);
  /* The handler captures a stack object; drop it before returning. */
  context.setDiagnosticHandlerCallBack(nullptr);
  return module;
}

llvm::Expected<std::unique_ptr<ModuleInfo>>
ModuleInfo::Parse(
  const void *bytecode, size_t size, const char *entry_name, llvm::LLVMContext *shared_context
) {
  auto program = FindDXILProgram(bytecode, size);
  if (!program)
    return program.takeError();

  auto info = std::make_unique<ModuleInfo>();
  llvm::LLVMContext *context = shared_context;
  if (!context) {
    info->context = std::make_unique<llvm::LLVMContext>();
    info->context->setOpaquePointers(false);
    context = info->context.get();
  }

  std::string diagnostics;
  auto module = parseBitcode(*context, program->bitcode, program->bitcode_size, diagnostics);
  if (!module) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    stream << "failed to parse DXIL bitcode: ";
    llvm::handleAllErrors(module.takeError(), [&](const llvm::ErrorInfoBase &base) {
      base.log(stream);
    });
    if (!diagnostics.empty())
      stream << " (" << diagnostics << ")";
    return createError(stream.str());
  }
  info->module = std::move(*module);

  if (auto err = extractMetadata(bytecode, size, *info->module, *info, entry_name))
    return std::move(err);
  return info;
}

llvm::Expected<ShaderMetadata>
ParseMetadata(const void *bytecode, size_t size, const char *entry_name) {
  auto info = ModuleInfo::Parse(bytecode, size, entry_name);
  if (!info)
    return info.takeError();
  ShaderMetadata metadata;
  metadata.stage = (*info)->stage;
  metadata.entry_name = (*info)->entry_name;
  metadata.shader_model_major = (*info)->shader_model_major;
  metadata.shader_model_minor = (*info)->shader_model_minor;
  for (unsigned i = 0; i < 3; i++)
    metadata.num_threads[i] = (*info)->num_threads[i];
  metadata.output_register_mask = (*info)->output_register_mask;
  metadata.max_output_register = (*info)->max_output_register;
  metadata.has_coverage_output = (*info)->has_coverage_output;
  metadata.force_early_depth_stencil = (*info)->force_early_depth_stencil;
  metadata.has_atomic64 = HasDeviceAtomic64(*(*info)->module);
  metadata.input_sig = std::move((*info)->input_sig);
  metadata.output_sig = std::move((*info)->output_sig);
  metadata.patch_constant_sig = std::move((*info)->patch_constant_sig);
  metadata.geometry = (*info)->geometry;
  metadata.hull = (*info)->hull;
  metadata.domain = (*info)->domain;
  metadata.mesh = (*info)->mesh;
  metadata.resources = std::move((*info)->resources);
  return metadata;
}

const ResourceBinding *
ModuleInfo::find_resource(ResourceClass klass, uint32_t declaration_id) const {
  for (const ResourceBinding &binding : resources) {
    if (binding.klass == klass && binding.declaration_id == declaration_id)
      return &binding;
  }
  return nullptr;
}

const ResourceBinding *ModuleInfo::find_resource_at(
  ResourceClass klass, uint32_t space, uint32_t lower_bound
) const {
  for (const ResourceBinding &binding : resources) {
    if (binding.klass == klass && binding.space == space &&
        binding.lower_bound == lower_bound)
      return &binding;
  }
  return nullptr;
}

llvm::Expected<DXILProgram> FindDXILProgram(const void *bytecode, size_t size) {
  auto chunk = findChunk(bytecode, size, "DXIL");
  if (!chunk)
    return chunk.takeError();

  const uint8_t *data = chunk->data;
  size_t chunk_size = chunk->size;

  DXILProgram program;
  if (chunk_size < 4)
    return createError("DXIL chunk is truncated");
  program.program_version = readU32(data);
  program.shader_kind = shaderKindFromProgramVersion(program.program_version);

  if (readU32(data) == kBitcodeMagic) {
    program.bitcode = data;
    program.bitcode_size = chunk_size;
    return program;
  }

  /* Wrapped bitcode: 'DXIL' magic, a version, then the bitcode offset and size,
   * both relative to the magic. */
  if (chunk_size < 24 || memcmp(data + 8, "DXIL", 4))
    return createError("unsupported DXIL chunk encoding");
  uint32_t bitcode_offset = readU32(data + 16);
  uint32_t bitcode_size = readU32(data + 20);
  if (bitcode_offset < 16 || bitcode_offset > chunk_size - 8)
    return createError("DXIL bitcode offset is out of bounds");
  if (bitcode_size > chunk_size - 8 - bitcode_offset)
    return createError("DXIL bitcode size is out of bounds");
  program.bitcode = data + 8 + bitcode_offset;
  program.bitcode_size = bitcode_size;
  if (program.bitcode_size < 4 || readU32(program.bitcode) != kBitcodeMagic)
    return createError("DXIL chunk does not contain bitcode");
  return program;
}

microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE to_program_type(Stage stage) {
  using microsoft::D3D10_SB_TOKENIZED_PROGRAM_TYPE;
  switch (stage) {
  case Stage::Pixel:
    return microsoft::D3D10_SB_PIXEL_SHADER;
  case Stage::Vertex:
    return microsoft::D3D10_SB_VERTEX_SHADER;
  case Stage::Geometry:
    return microsoft::D3D10_SB_GEOMETRY_SHADER;
  case Stage::Hull:
    return microsoft::D3D11_SB_HULL_SHADER;
  case Stage::Domain:
    return microsoft::D3D11_SB_DOMAIN_SHADER;
  case Stage::Compute:
    return microsoft::D3D11_SB_COMPUTE_SHADER;
  case Stage::Mesh:
    return microsoft::D3D12_SB_MESH_SHADER;
  case Stage::Amplification:
    return microsoft::D3D12_SB_AMPLIFICATION_SHADER;
  case Stage::Library:
  case Stage::RayGeneration:
  case Stage::Intersection:
  case Stage::AnyHit:
  case Stage::ClosestHit:
  case Stage::Miss:
  case Stage::Callable:
    /* The state-object stages have no D3D10 token value; the DXIL `ShaderKind`
     * number is what the container's version token carries for them, and it is what
     * the root-signature visibility check compares against. */
    return (D3D10_SB_TOKENIZED_PROGRAM_TYPE)(uint32_t)stage;
  default:
    break;
  }
  return (D3D10_SB_TOKENIZED_PROGRAM_TYPE)0xffffffffu;
}

shader::common::ScalerDataType to_scaler_type(ComponentType type) {
  switch (type) {
  case ComponentType::I16:
  case ComponentType::I32:
  case ComponentType::I64:
    return shader::common::ScalerDataType::Int;
  case ComponentType::I1:
  case ComponentType::U16:
  case ComponentType::U32:
  case ComponentType::U64:
    return shader::common::ScalerDataType::Uint;
  case ComponentType::F64:
  case ComponentType::SNormF64:
  case ComponentType::UNormF64:
    return shader::common::ScalerDataType::Double;
  default:
    return shader::common::ScalerDataType::Float;
  }
}

} // namespace dxmt::dxil
