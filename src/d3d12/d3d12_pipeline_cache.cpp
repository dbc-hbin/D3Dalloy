#include "d3d12_device.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "d3d12_shader_cache.hpp"
#include "../d3d10/d3d10_blob.hpp"
#include "DXBCParser/BlobContainer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include <string>
#include <vector>

namespace dxmt {

// Bytes are never exposed to the application: GetBlob returns a separate writable copy.
struct PipelineCacheStorage {
  Com<ID3DBlob> bytes;
  std::array<SM50_COMPILED_BITCODE, PipelineCache::StageCount> views{};
};

struct AutomaticPipelineCacheFlight {
  Sha1Digest key;
  uint64_t generation;
  std::condition_variable ready;
  bool done = false;
  bool disk_hit = false;
  HRESULT result = E_ABORT;
  std::shared_ptr<const PipelineCacheStorage> blob;
};

namespace {

constexpr uint64_t kMagic = 0x004f5350544d5844; // "DXMTPSO\0", little endian.
// 2: graphics key covers the stream-output declaration. 3: it covers GS/HS/DS bytecode.
// 4: P3 hidden-resource/signature AIR ABI changed even for unchanged shader/PSO inputs.
// Serialized pipeline libraries validate this revision for every embedded PSO blob.
// 5: atomic phase context relocations and bounded captured application variants.
// 6: immutable linked-stage layouts and native sample-mask function constants (AIR ABI 33).
constexpr uint32_t kVersion = 6;
constexpr size_t kAutomaticBytes = 64u * 1024u * 1024u;
constexpr size_t kAutomaticEntries = 512;

struct AutomaticCache {
  struct Entry {
    std::shared_ptr<const PipelineCacheStorage> blob;
    uint64_t last_use;
  };
  std::mutex mutex;
  // Disk transactions take disk_mutex before mutex, and release mutex before I/O.
  // No metadata owner may wait for disk_mutex.
  std::mutex disk_mutex;
  bool enabled = true;
  uint64_t generation = 0, clock = 0;
  size_t bytes = 0;
  std::unordered_map<Sha1Digest, Entry> entries;
  std::unordered_map<Sha1Digest, std::shared_ptr<AutomaticPipelineCacheFlight>> flights;

  void Retain(const Sha1Digest &key, const std::shared_ptr<const PipelineCacheStorage> &blob) {
    const size_t size = blob->bytes->GetBufferSize();
    if (size > kAutomaticBytes)
      return;
    // ponytail: bounded 512-entry LRU scan; use a list only if eviction CPU becomes material.
    while (!entries.empty() && (bytes > kAutomaticBytes - size || entries.size() >= kAutomaticEntries)) {
      auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return a.second.last_use < b.second.last_use;
      });
      bytes -= oldest->second.blob->bytes->GetBufferSize();
      entries.erase(oldest);
    }
    entries.emplace(key, Entry{blob, ++clock});
    bytes += size;
  }
};

AutomaticCache &Automatic() {
  static AutomaticCache cache; // Module-local, no device or native PSO ownership.
  return cache;
}

struct AutomaticCachePhase {
  const char *phase;
  bool trace = owned_graphics::TraceEnabled();
  std::chrono::steady_clock::time_point start = trace ? std::chrono::steady_clock::now() :
                                                       std::chrono::steady_clock::time_point{};
  explicit AutomaticCachePhase(const char *name) : phase(name) {
    try {
      if (trace)
        owned_graphics::Trace("pipeline_cache_auto_phase", ",\"phase\":\"", phase, "\"");
    } catch (...) {
      // Timing is optional, including at noexcept completion.
    }
  }
  ~AutomaticCachePhase() {
    try {
      if (trace)
        owned_graphics::Trace("pipeline_cache_auto_timing", ",\"phase\":\"", phase, "\",\"ns\":",
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    } catch (...) {
      // Timing cannot fail cache completion/destruction.
    }
  }
};

std::unique_lock<std::mutex> LockMetadata(AutomaticCache &cache) {
  AutomaticCachePhase phase{"metadata_wait"};
  return std::unique_lock(cache.mutex);
}

bool OwnsFlight(const AutomaticCache &cache, const std::shared_ptr<AutomaticPipelineCacheFlight> &flight) {
  auto it = cache.flights.find(flight->key);
  return cache.enabled && cache.generation == flight->generation &&
         it != cache.flights.end() && it->second == flight;
}

struct StageRecord {
  uint64_t library_size;
  uint64_t constant_size;
  uint64_t constant_alignment;
  uint64_t relocation_count;
};

struct CacheHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t stages;
  uint64_t size;
  uint64_t adapter;
  WMTPipelineCacheIdentity identity;
  Sha1Digest key;
  Sha1Digest digest;
  std::array<StageRecord, PipelineCache::StageCount> records;
};
static_assert(sizeof(CacheHeader) == 216);
static_assert(sizeof(SM50_CONSTANT_RELOCATION) == 16);

struct StageOffsets { size_t library, constants, relocations; };

struct NativeIdentity {
  WMTPipelineCacheIdentity value{};
  bool valid = WMTGetPipelineCacheIdentity(&value);
};

const NativeIdentity &Identity() {
  static const NativeIdentity identity;
  return identity;
}

Sha1Digest BlobDigest(const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  constexpr size_t begin = offsetof(CacheHeader, digest);
  constexpr size_t end = begin + sizeof(Sha1Digest);
  Sha1HashState hash;
  hash.update(bytes, begin);
  hash.update(bytes + end, size - end);
  return hash.final();
}

// Offsets are relative to an owned allocation, not to a possibly unaligned
// application pointer. Every region is bounded before its bytes are examined.
bool Layout(const CacheHeader &header, size_t limit,
            std::array<StageOffsets, PipelineCache::StageCount> &offsets, size_t &size) {
  size = sizeof(CacheHeader);
  if (limit < size)
    return false;
  auto region = [&](uint64_t length, size_t &offset) {
    if (length > limit - size)
      return false;
    offset = size;
    size += length;
    const size_t padding = (-size) & 7;
    if (padding > limit - size)
      return false;
    size += padding;
    return true;
  };
  for (uint32_t i = 0; i < PipelineCache::StageCount; ++i) {
    const auto &record = header.records[i];
    if (!(header.stages & (1u << i))) {
      if (record.library_size || record.constant_size || record.constant_alignment || record.relocation_count)
        return false;
      continue;
    }
    if (record.library_size < 4 || !record.constant_alignment ||
        (record.constant_alignment & (record.constant_alignment - 1)) ||
        record.constant_alignment > UINT32_MAX || record.constant_size > UINT32_MAX ||
        record.relocation_count > record.constant_size / sizeof(uint64_t) ||
        (!record.constant_size && record.constant_alignment != 1))
      return false;
    if (!region(record.library_size, offsets[i].library) ||
        !region(record.constant_size, offsets[i].constants) ||
        !region(record.relocation_count * sizeof(SM50_CONSTANT_RELOCATION), offsets[i].relocations))
      return false;
  }
  return true;
}

std::shared_ptr<const PipelineCacheStorage> ShareBlob(ID3DBlob *blob) {
  CacheHeader header;
  memcpy(&header, blob->GetBufferPointer(), sizeof(header));
  std::array<StageOffsets, PipelineCache::StageCount> offsets{};
  size_t size = 0;
  if (!Layout(header, blob->GetBufferSize(), offsets, size))
    return {};
  auto storage = std::make_shared<PipelineCacheStorage>();
  storage->bytes = blob;
  auto *bytes = static_cast<uint8_t *>(blob->GetBufferPointer());
  for (uint32_t i = 0; i < PipelineCache::StageCount; ++i) {
    if (!(header.stages & (1u << i)))
      continue;
    const auto &record = header.records[i];
    auto &view = storage->views[i];
    view.Data = bytes + offsets[i].library;
    view.Size = record.library_size;
    view.ConstantData = record.constant_size ? bytes + offsets[i].constants : nullptr;
    view.ConstantDataSize = record.constant_size;
    view.ConstantDataAlignment = record.constant_alignment;
    view.ConstantRelocations = record.relocation_count ? bytes + offsets[i].relocations : nullptr;
    view.ConstantRelocationCount = record.relocation_count;
  }
  return storage;
}

class KeyBuilder {
  Sha1HashState hash_;
  bool valid_ = true;
public:
  template <typename... T> void add(const T &...values) { (hash_.update(values), ...); }
  void bytes(const void *data, size_t size) {
    add(uint64_t(size));
    if (size && !data)
      valid_ = false;
    else if (size)
      hash_.update(data, size);
  }
  void shader(D3D12_SHADER_BYTECODE bytecode) { bytes(bytecode.pShaderBytecode, bytecode.BytecodeLength); }
  void root(ID3D12RootSignature *signature) {
    add(bool(signature));
    if (signature) {
      const void *data = nullptr;
      const auto size = static_cast<MTLD3D12RootSignature *>(signature)->GetBlob(&data);
      bytes(data, size);
    }
  }
  HRESULT finish(Sha1Digest &key) {
    if (!valid_)
      return E_INVALIDARG;
    key = hash_.final();
    return S_OK;
  }
};

// Full canonical bytes are compared, not just a digest. No pointer/padding is a key input.
class StageKeyBuilder {
public:
  std::string key;
  std::vector<std::pair<void *, size_t>> outputs;
  template <typename... T> void add(const T &...values) {
    (key.append(reinterpret_cast<const char *>(&values), sizeof(values)), ...);
  }
  void bytes(const void *data, size_t size) {
    add(uint64_t(size));
    if (size) key.append(static_cast<const char *>(data), size);
  }
  template <typename T> void output(T *data) {
    add(bool(data));
    if (data) outputs.emplace_back(data, sizeof(T));
  }
  void interpolation(const SM50_MSAA_INTERPOLATION_LAYOUT *v) {
    add(bool(v));
    if (!v) return;
    add(v->no_perspective_count, v->flat_count);
    for (uint32_t i = 0; i < v->no_perspective_count; ++i) add(v->no_perspective_user_hashes[i]);
    for (uint32_t i = 0; i < v->flat_count; ++i) add(v->flat_user_hashes[i]);
  }
  void vertex(const SM50_MSAA_VERTEX_LAYOUT *v) {
    add(bool(v));
    if (!v) return;
    add(v->record_dwords, v->position_offset_dwords, v->viewport_index_offset_dwords,
        v->render_target_array_index_offset_dwords, v->clip_distance_offset_dwords,
        v->cull_distance_offset_dwords, v->user_count, v->shading_rate_offset_dwords,
        v->primitive_id_offset_dwords, v->spill_count);
    for (uint32_t i = 0; i < v->user_count; ++i)
      add(v->users[i].hash, v->users[i].offset_dwords, v->users[i].size_bytes, v->users[i].flags);
  }
  bool arguments(const SM50_SHADER_COMPILATION_ARGUMENT_DATA *arg) {
    bool sample_mask_constant = false;
    for (auto *it = arg; it; it = static_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(it->next))
      sample_mask_constant |= it->type == SM50_SHADER_D3D12_SAMPLE_MASK_CONSTANT;
    for (; arg; arg = static_cast<const SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(arg->next)) {
      add(arg->type);
      switch (arg->type) {
      case SM50_SHADER_ROOT_SIGNATURE:
      case SM50_SHADER_ROOT_SIGNATURE2: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_ROOT_SIGNATURE_DATA *>(arg);
        bytes(v.bytecode, v.bytecode_length);
        break;
      }
      case SM50_SHADER_COMMON: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_COMMON_DATA *>(arg);
        add(v.metal_version, v.flags);
        break;
      }
      case SM50_SHADER_IA_INPUT_LAYOUT: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_IA_INPUT_LAYOUT_DATA *>(arg);
        add(v.index_buffer_format, v.slot_mask, v.num_elements);
        for (uint32_t i = 0; i < v.num_elements; ++i) {
          const auto &e = v.elements[i];
          add(e.reg, e.slot, e.aligned_byte_offset, e.format, uint32_t(e.step_function), uint32_t(e.step_rate));
        }
        break;
      }
      case SM50_SHADER_PSO_PIXEL_SHADER: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_PSO_PIXEL_SHADER_DATA *>(arg);
        if (!sample_mask_constant) add(v.sample_mask);
        add(v.dual_source_blending, v.disable_depth_output, v.unorm_output_reg_mask,
            v.pixel_formats, v.native_sample_count);
        break;
      }
      case SM50_SHADER_D3D12_LOGIC_OP: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_LOGIC_OP_DATA *>(arg);
        add(v.op, v.rt_mask, v.component_bits);
        break;
      }
      case SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA *>(arg);
        add(v.record_dwords, v.num_elements);
        for (uint32_t i = 0; i < v.num_elements; ++i) {
          const auto &e = v.elements[i];
          bytes(e.semantic_name, strlen(e.semantic_name));
          add(e.semantic_index, e.start_component, e.component_count, e.dword_offset);
        }
        break;
      }
      case SM50_SHADER_GS_PASS_THROUGH: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_GS_PASS_THROUGH_DATA *>(arg);
        add(v.DataEncoded, v.RasterizationDisabled);
        break;
      }
      case SM50_SHADER_D3D12_MSAA_PIXEL: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_MSAA_PIXEL_DATA *>(arg);
        add(v.variant, v.sample_count, v.depth_attachment, v.single_sample_target, v.line_mode,
            v.center_pattern, v.distance_vertex_count, v.depth_clip_enable, v.depth_bounds_emulated);
        output(v.layout_out); output(v.interpolation_out);
        vertex(v.line_vertex_layout); vertex(v.distance_vertex_layout); vertex(v.coarse_vertex_layout);
        break;
      }
      case SM50_SHADER_D3D12_MSAA_VERTEX: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_MSAA_VERTEX_DATA *>(arg);
        add(v.variant, v.sample_count, v.line_mode, v.depth_clip_enable, v.front_counter_clockwise);
        output(v.layout_out); interpolation(v.interpolation);
        break;
      }
      case SM50_SHADER_D3D12_MSAA_MESH: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_MSAA_MESH_DATA *>(arg);
        add(v.variant, v.line_mode, v.depth_clip_enable, v.front_counter_clockwise);
        // Mesh capture layout has caller-supplied workload/group constraints as well as outputs.
        add(bool(v.mesh_layout_out));
        if (v.mesh_layout_out) add(v.mesh_layout_out->threads_per_group, v.mesh_layout_out->max_workloads);
        output(v.layout_out); output(v.mesh_layout_out); output(v.object_layout_out);
        interpolation(v.interpolation);
        break;
      }
      case SM50_SHADER_D3D12_TESS_CAPTURE: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_TESS_CAPTURE_DATA *>(arg);
        output(v.layout_out); break;
      }
      case SM50_SHADER_GS_TESSELLATION_INPUT: {
        const auto &v = reinterpret_cast<const SM50_SHADER_GS_TESSELLATION_INPUT_DATA *>(arg)->layout;
        add(v.max_workloads, v.max_vertices, v.max_primitives, v.vertex_registers, v.primitive_vertices);
        break;
      }
      case SM50_SHADER_PSO_GEOMETRY_SHADER:
        add(reinterpret_cast<const SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *>(arg)->strip_topology); break;
      case SM50_SHADER_PSO_TESSELLATOR:
        add(reinterpret_cast<const SM50_SHADER_PSO_TESSELLATOR_DATA *>(arg)->max_potential_tess_factor); break;
      case SM50_SHADER_D3D12_GS_STREAMS: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_GS_STREAMS_DATA *>(arg);
        add(v.rasterized_stream);
        for (const auto &s : v.streams) {
          add(s.record_dwords, s.num_elements);
          for (uint32_t i = 0; i < s.num_elements; ++i) {
            const auto &e = s.elements[i];
            bytes(e.semantic_name, strlen(e.semantic_name));
            add(e.semantic_index, e.start_component, e.component_count, e.dword_offset);
          }
        }
        break;
      }
      case SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS: {
        const auto &v = *reinterpret_cast<const SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA *>(arg);
        add(v.view_count, v.viewport_offsets, v.render_target_offsets); break;
      }
      case SM50_SHADER_D3D12_INDEX_PULL:
      case SM50_SHADER_D3D12_SAMPLE_MASK_CONSTANT:
      case SM50_SHADER_D3D12_PIPELINE_STATS:
      case SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL:
      case SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM:
      case SM50_SHADER_D3D12_COARSE_SHADING:
      case SM50_SHADER_D3D12_CONSERVATIVE_RASTER:
      case SM50_SHADER_D3D12_ATOMIC64_PHASE:
        break; // Header-only, but presence and chain order are part of the key.
      default:
        // Output-layout pointers and linked GS/HS/DS inputs need their own immutable contract.
        return false;
      }
    }
    add(SM50_SHADER_ARGUMENT_TYPE_MAX);
    return true;
  }
};

template <typename T> struct ArtifactCache {
  struct Flight {
    std::condition_variable ready;
    bool done = false;
    uint64_t last_use = 0;
    size_t bytes = 0;
    std::shared_ptr<const T> value;
  };
  std::mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<Flight>> entries;
  uint64_t generation = UINT64_MAX, clock = 0, operations = 0;
  const uint64_t id = owned_graphics::NewObjectId();
  size_t bytes = 0, completed = 0;

  void Operation(const char *kind, const std::string &key) {
    uint64_t count;
    { std::lock_guard lock(mutex); count = ++operations; }
    try {
      if (owned_graphics::TraceEnabled())
        owned_graphics::Trace("pipeline_artifact_operation", ",\"kind\":\"", kind, "\",\"cache\":", id,
                              ",\"count\":", count, ",\"key\":\"",
                              Sha1HashState::compute(key.data(), key.size()).string(), "\"");
    } catch (...) {
      // Optional instrumentation cannot change a compiler/native operation result.
    }
  }

  template <typename Create> std::shared_ptr<const T> Get(const std::string &key, const char *kind, Create create) {
    uint64_t current;
    bool enabled;
    {
      auto &automatic = Automatic();
      std::lock_guard lock(automatic.mutex);
      current = automatic.generation;
      enabled = automatic.enabled;
    }
    std::unique_lock lock(mutex);
    if (generation != UINT64_MAX && current < generation) {
      enabled = false; // A control/new request overtook this request before its local lock.
    } else if (generation != current) {
      // Controls retire membership, not references held by PSOs or old-generation waiters.
      entries.clear(); bytes = completed = 0; generation = current;
    }
    std::shared_ptr<Flight> flight;
    if (enabled) {
      if (auto it = entries.find(key); it != entries.end()) {
        flight = it->second;
        flight->last_use = ++clock;
        flight->ready.wait(lock, [&] { return flight->done; });
        auto value = flight->value;
        lock.unlock();
        if (value && owned_graphics::TraceEnabled())
          owned_graphics::Trace("pipeline_artifact_hit", ",\"kind\":\"", kind, "\",\"cache\":", id, ",\"key\":\"",
                                Sha1HashState::compute(key.data(), key.size()).string(), "\"");
        return value;
      }
      flight = std::make_shared<Flight>();
      flight->last_use = ++clock;
      entries.emplace(key, flight);
    }
    lock.unlock();
    std::shared_ptr<const T> value;
    try {
      value = create();
    } catch (...) {
      if (flight) {
        lock.lock();
        if (auto it = entries.find(key); it != entries.end() && it->second == flight) entries.erase(it);
        flight->done = true;
        lock.unlock();
        flight->ready.notify_all();
      }
      throw;
    }
    if (!flight) return value;
    lock.lock();
    flight->value = value;
    flight->done = true;
    const auto it = entries.find(key);
    if (it != entries.end() && it->second == flight) {
      flight->bytes = key.size() + (value ? value->size() : 0);
      constexpr size_t limit = 32u * 1024u * 1024u;
      if (!value || flight->bytes > limit) {
        entries.erase(it);
      } else {
        bytes += flight->bytes; ++completed;
        // ponytail: bounded 256-entry LRU scan; pending flights are never evicted.
        while (bytes > limit || completed > 256) {
          auto oldest = entries.end();
          for (auto entry = entries.begin(); entry != entries.end(); ++entry)
            if (entry->second->done && (oldest == entries.end() ||
                entry->second->last_use < oldest->second->last_use)) oldest = entry;
          bytes -= oldest->second->bytes; --completed; entries.erase(oldest);
        }
      }
    }
    lock.unlock();
    flight->ready.notify_all();
    return value;
  }
};

struct NativeLibraryArtifact {
  WMT::Reference<WMT4::Library> library;
  size_t size() const { return 0; } // Serialized library bytes are counted in the exact key.
};
struct NativeFunctionArtifact {
  WMT::Reference<WMT4::Function> function;
  size_t size() const { return 0; }
};
struct ShaderReflectionArtifact {
  MTL_SHADER_REFLECTION reflection{};
  std::vector<MTL_SM50_SHADER_ARGUMENT> bindings;
  size_t size() const { return sizeof(reflection) + bindings.size() * sizeof(MTL_SM50_SHADER_ARGUMENT); }
};
struct RasterMarkerArtifact {
  bool needed;
  size_t size() const { return sizeof(needed); }
};

} // namespace

struct DeviceShaderArtifactCache::Impl {
  ArtifactCache<ShaderStageArtifact> stages;
  ArtifactCache<NativeLibraryArtifact> libraries;
  ArtifactCache<NativeFunctionArtifact> functions;
  ArtifactCache<ShaderReflectionArtifact> reflections;
  ArtifactCache<RasterMarkerArtifact> markers;
};

DeviceShaderArtifactCache::DeviceShaderArtifactCache() : impl_(std::make_unique<Impl>()) {}
DeviceShaderArtifactCache::~DeviceShaderArtifactCache() = default;

HRESULT DeviceShaderArtifactCache::Reflect(
    D3D12_SHADER_BYTECODE source, sm50_shader_t *shader, MTL_SHADER_REFLECTION &reflection,
    std::vector<MTL_SM50_SHADER_ARGUMENT> *bindings) {
  if (*shader) SM50Destroy(*shader);
  *shader = nullptr;
  StageKeyBuilder key;
  key.add(uint32_t(AIRCONV_VERSION)); key.bytes(source.pShaderBytecode, source.BytecodeLength);
  SM50Error error;
  HRESULT result = E_FAIL;
  auto value = impl_->reflections.Get(key.key, "shader_initialize", [&]() -> std::shared_ptr<const ShaderReflectionArtifact> {
    microsoft::CDXBCParser parser;
    if (FAILED(result = parser.ReadDXBC(source.pShaderBytecode, source.BytecodeLength))) return {};
    result = E_FAIL;
    impl_->reflections.Operation("shader_initialize", key.key);
    auto artifact = std::make_shared<ShaderReflectionArtifact>();
    if (SM50Initialize(source.pShaderBytecode, source.BytecodeLength, shader, &artifact->reflection, &error))
      return {};
    artifact->bindings.resize(artifact->reflection.NumConstantBuffers + artifact->reflection.NumArguments);
    if (!artifact->bindings.empty())
      SM50GetArgumentsInfo(*shader, artifact->bindings.data(),
                          artifact->bindings.data() + artifact->reflection.NumConstantBuffers);
    return artifact;
  });
  if (!value) {
    // A failed flight does not carry its producer's HRESULT to waiting callers.
    // Revalidate only failures so malformed bytecode preserves the parser error.
    microsoft::CDXBCParser parser;
    HRESULT parsed = parser.ReadDXBC(source.pShaderBytecode, source.BytecodeLength);
    return FAILED(parsed) ? parsed : result;
  }
  reflection = value->reflection;
  if (bindings) bindings->insert(bindings->end(), value->bindings.begin(), value->bindings.end());
  return S_OK;
}

bool DeviceShaderArtifactCache::RasterOrderedMarker(
    D3D12_SHADER_BYTECODE source, D3D12_SHADER_BYTECODE root, sm50_shader_t shader) {
  StageKeyBuilder key;
  key.add(uint32_t(AIRCONV_VERSION)); key.bytes(source.pShaderBytecode, source.BytecodeLength);
  key.bytes(root.pShaderBytecode, root.BytecodeLength);
  auto value = impl_->markers.Get(key.key, "raster_ordered_marker", [&]() -> std::shared_ptr<const RasterMarkerArtifact> {
    SM50Shader local; SM50Error error;
    if (!shader) {
      if (InitializeCompiler(source, &local, &error)) return {};
      shader = local;
    }
    return std::make_shared<RasterMarkerArtifact>(RasterMarkerArtifact{
        SM50NeedsRasterOrderedBufferMarker(shader, root.pShaderBytecode, root.BytecodeLength) != 0});
  });
  return value && value->needed;
}

int DeviceShaderArtifactCache::InitializeCompiler(
    D3D12_SHADER_BYTECODE source, sm50_shader_t *shader, sm50_error_t *error) {
  StageKeyBuilder key; key.add(uint32_t(AIRCONV_VERSION));
  key.bytes(source.pShaderBytecode, source.BytecodeLength);
  impl_->reflections.Operation("shader_initialize", key.key);
  // HS reflection normalizes its payload; cached reflection cannot normalize fresh compiler IR.
  MTL_SHADER_REFLECTION reflection{};
  return SM50Initialize(source.pShaderBytecode, source.BytecodeLength, shader, &reflection, error);
}

int PipelineCache::Compile(Stage stage, DeviceShaderArtifactCache &artifacts, D3D12_SHADER_BYTECODE source,
                           sm50_shader_t shader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
                           const char *name, sm50_error_t *error) {
  if (loaded()) return 0;
  auto lower = [&](sm50_bitcode_t *out, sm50_error_t *err) {
    SM50Shader local;
    if (!shader) {
      if (int result = artifacts.InitializeCompiler(source, &local, err)) return result;
      shader = local;
    }
    return SM50Compile(shader, args, name, out, err);
  };
  return artifacts.Compile("ordinary", {source}, args, name, output(stage), error, stage_artifacts_[stage],
                           std::ref(lower));
}

int DeviceShaderArtifactCache::Compile(
    const char *family, std::initializer_list<D3D12_SHADER_BYTECODE> sources,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name, sm50_bitcode_t *output,
    sm50_error_t *error, std::shared_ptr<const ShaderStageArtifact> &artifact,
    const std::function<int(sm50_bitcode_t *, sm50_error_t *)> &lower) {
  int result = 0;
  StageKeyBuilder key;
  auto compile = [&] {
    impl_->stages.Operation("air_compile", key.key);
    return lower(output, error);
  };
  try {
    key.add(kVersion, uint32_t(AIRCONV_VERSION), uint32_t(sources.size()));
    key.add(Identity().value);
    key.bytes(family, strlen(family));
    for (const auto &source : sources) key.bytes(source.pShaderBytecode, source.BytecodeLength);
    key.bytes(name, strlen(name));
    if (!key.arguments(args)) return compile();
    artifact = impl_->stages.Get(key.key, "air_compile", [&]() -> std::shared_ptr<const ShaderStageArtifact> {
      if ((result = compile())) return {};
      SM50_COMPILED_BITCODE code{};
      SM50GetCompiledBitcode(*output, &code);
      auto value = std::make_shared<ShaderStageArtifact>();
      value->library.resize(code.Size);
      value->constants.resize((code.ConstantDataSize + 7) / 8);
      value->relocations.resize(code.ConstantRelocationCount);
      if (!SM50ReadCompiledBitcode(*output, SM50_COMPILED_BITCODE_PART_LIBRARY, code.Data,
                                  value->library.data(), code.Size) ||
          !SM50ReadCompiledBitcode(*output, SM50_COMPILED_BITCODE_PART_CONSTANTS, code.ConstantData,
                                  value->constants.data(), code.ConstantDataSize) ||
          !SM50ReadCompiledBitcode(*output, SM50_COMPILED_BITCODE_PART_RELOCATIONS, code.ConstantRelocations,
                                  value->relocations.data(), code.ConstantRelocationCount * sizeof(SM50_CONSTANT_RELOCATION)))
        return {}; // The producer can still consume its compiler-owned output.
      value->view = code;
      value->view.Data = value->library.data();
      value->view.ConstantData = code.ConstantDataSize ? value->constants.data() : nullptr;
      value->view.ConstantRelocations = code.ConstantRelocationCount ? value->relocations.data() : nullptr;
      for (const auto &[data, size] : key.outputs) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        value->metadata.insert(value->metadata.end(), bytes, bytes + size);
      }
      return value;
    });
  } catch (const std::bad_alloc &) {
    // Optional retention must not fail a successful compiler invocation.
  }
  if (artifact) {
    size_t offset = 0;
    for (const auto &[data, size] : key.outputs) {
      memcpy(data, artifact->metadata.data() + offset, size); offset += size;
    }
    if (sm50_bitcode_t handle = *output) {
      SM50DestroyBitcode(handle); *output = nullptr;
    }
    return 0;
  }
  // Failed flights are not cached. Waiters compile locally to preserve their own error handle.
  return result || sm50_bitcode_t(*output) ? result : compile();
}

int DeviceShaderArtifactCache::CompileShader(
    D3D12_SHADER_BYTECODE source, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    const char *name, sm50_bitcode_t *output, sm50_error_t *error,
    std::shared_ptr<const ShaderStageArtifact> &artifact) {
  auto lower = [&](sm50_bitcode_t *out, sm50_error_t *err) {
    SM50Shader shader;
    if (int result = InitializeCompiler(source, &shader, err))
      return result;
    return SM50Compile(shader, args, name, out, err);
  };
  return Compile("ordinary", {source}, args, name, output, error, artifact, std::ref(lower));
}

WMT::Reference<WMT4::Library> DeviceShaderArtifactCache::Library(
    WMT4::Compiler compiler, const SM50_COMPILED_BITCODE &code, sm50_bitcode_t owner, WMT::Error &error) {
  std::string bytes(code.Size, char(0));
  if (!SM50ReadCompiledBitcode(owner, SM50_COMPILED_BITCODE_PART_LIBRARY, code.Data, bytes.data(), code.Size)) return {};
  // Device ownership fixes native device/driver identity; compiler handle is an explicit key input.
  StageKeyBuilder key;
  key.add(compiler.handle); key.bytes(bytes.data(), bytes.size());
  auto value = impl_->libraries.Get(key.key, "new_library", [&]() -> std::shared_ptr<const NativeLibraryArtifact> {
    impl_->libraries.Operation("new_library", key.key);
    auto library = compiler.newLibrary(WMT::MakeDispatchData(bytes.data(), bytes.size()), error);
    if (!library) return {};
    auto artifact = std::make_shared<NativeLibraryArtifact>();
    artifact->library = std::move(library);
    return artifact;
  });
  return value ? value->library : WMT::Reference<WMT4::Library>{};
}

WMT::Reference<WMT4::Function> DeviceShaderArtifactCache::SampleMaskFunction(
    WMT4::Library library, const char *name, D3D12_SHADER_BYTECODE root, uint32_t mask, WMT::Error &error) {
  StageKeyBuilder key;
  key.add(library.handle, uint16_t(0), WMTDataTypeUInt, mask);
  key.bytes(name, strlen(name)); key.bytes(root.pShaderBytecode, root.BytecodeLength);
  auto value = impl_->functions.Get(key.key, "specialize_function", [&]() -> std::shared_ptr<const NativeFunctionArtifact> {
    impl_->functions.Operation("specialize_function", key.key);
    WMTFunctionConstant constant{};
    constant.index = 0; constant.type = WMTDataTypeUInt;
    constant.data.set(&mask);
    auto function = library.newFunctionWithConstants(name, &constant, 1, error);
    if (!function) return {};
    auto artifact = std::make_shared<NativeFunctionArtifact>();
    artifact->function = std::move(function); return artifact;
  });
  return value ? value->function : WMT::Reference<WMT4::Function>{};
}

HRESULT PipelineCacheKey(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc, Sha1Digest &key) {
  KeyBuilder hash;
  hash.add(uint32_t(3), kD3D12ShaderMetalVersion, desc.NodeMask, desc.Flags);
  hash.root(desc.pRootSignature);
  hash.shader(desc.CS);
  return hash.finish(key);
}

HRESULT PipelineCacheKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc, Sha1Digest &key,
                         const GraphicsPipelineExtras *extras) {
  if (desc.NumRenderTargets > 8 || (desc.InputLayout.NumElements && !desc.InputLayout.pInputElementDescs))
    return E_INVALIDARG;
  KeyBuilder hash;
  // 6: Metal 4 compiles UINT logic operations into every owned framebuffer-fetch
  // 7: device atomic64 phase selection changes every participating application stage.
  hash.add(uint32_t(7), uint32_t(AIRCONV_VERSION), kD3D12ShaderMetalVersion, desc.NodeMask, desc.Flags);
  hash.root(desc.pRootSignature);
  hash.shader(desc.VS);
  hash.shader(desc.PS);
  hash.shader(desc.DS);
  hash.shader(desc.HS);
  hash.shader(desc.GS);
  hash.add(desc.SampleMask, desc.IBStripCutValue, desc.PrimitiveTopologyType,
           desc.NumRenderTargets, desc.DSVFormat, desc.SampleDesc.Count, desc.SampleDesc.Quality);
  for (UINT i = 0; i < desc.NumRenderTargets; ++i)
    hash.add(desc.RTVFormats[i]);
  const auto &blend = desc.BlendState;
  hash.add(bool(blend.AlphaToCoverageEnable), bool(blend.IndependentBlendEnable));
  const UINT blend_count = blend.IndependentBlendEnable ? desc.NumRenderTargets : std::min(desc.NumRenderTargets, 1u);
  for (UINT i = 0; i < blend_count; ++i) {
    const auto &rt = blend.RenderTarget[i];
    hash.add(bool(rt.BlendEnable), bool(rt.LogicOpEnable), rt.SrcBlend, rt.DestBlend, rt.BlendOp,
             rt.SrcBlendAlpha, rt.DestBlendAlpha, rt.BlendOpAlpha, rt.LogicOp, rt.RenderTargetWriteMask);
  }
  const auto &raster = desc.RasterizerState;
  hash.add(raster.FillMode, raster.CullMode, bool(raster.FrontCounterClockwise), raster.DepthBias,
           raster.DepthBiasClamp, raster.SlopeScaledDepthBias, bool(raster.DepthClipEnable),
           bool(raster.MultisampleEnable), bool(raster.AntialiasedLineEnable), raster.ForcedSampleCount,
           raster.ConservativeRaster);
  const auto &depth = desc.DepthStencilState;
  hash.add(bool(depth.DepthEnable), depth.DepthWriteMask, depth.DepthFunc, bool(depth.StencilEnable),
           depth.StencilReadMask, depth.StencilWriteMask);
  for (const auto *face : {&depth.FrontFace, &depth.BackFace})
    hash.add(face->StencilFailOp, face->StencilDepthFailOp, face->StencilPassOp, face->StencilFunc);
  // Stream-only state (fractional depth bias, back-face stencil masks); absent for classic descs.
  if (extras && !extras->empty()) {
    if (extras->depth_bounds) hash.add(uint32_t(0x44424e44));
    hash.add(uint32_t(0x45585452), bool(extras->depth_bias), extras->depth_bias.value_or(0.0f),
             bool(extras->back_stencil_read_write_mask));
    if (extras->back_stencil_read_write_mask)
      hash.add(extras->back_stencil_read_write_mask->first, extras->back_stencil_read_write_mask->second);
    if (extras->view_instancing) {
      hash.add(uint32_t(0x56494e53), extras->view_instancing->count, extras->view_instancing->masking);
      for (UINT i = 0; i < extras->view_instancing->count; ++i)
        hash.add(extras->view_instancing->locations[i].ViewportArrayIndex,
                 extras->view_instancing->locations[i].RenderTargetArrayIndex);
    }
    if (extras->ms.pShaderBytecode) {
      hash.add(uint32_t(0x4d455348));
      hash.shader(extras->as);
      hash.shader(extras->ms);
    }
  }
  hash.add(desc.InputLayout.NumElements);
  for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
    const auto &element = desc.InputLayout.pInputElementDescs[i];
    if (!element.SemanticName)
      return E_INVALIDARG;
    hash.bytes(element.SemanticName, std::strlen(element.SemanticName));
    hash.add(element.SemanticIndex, element.Format, element.InputSlot, element.AlignedByteOffset,
             element.InputSlotClass, element.InstanceDataStepRate);
  }
  // Stream output selects the VS capture epilogue, and RasterizedStream selects the
  // rasterization-disabled (void) VS variant without a fragment stage.
  const auto &so = desc.StreamOutput;
  if ((so.NumEntries && !so.pSODeclaration) || (so.NumStrides && !so.pBufferStrides))
    return E_INVALIDARG;
  hash.add(so.NumEntries, so.NumStrides, so.RasterizedStream);
  for (UINT i = 0; i < so.NumEntries; ++i) {
    const auto &entry = so.pSODeclaration[i];
    hash.add(entry.Stream, bool(entry.SemanticName));
    if (entry.SemanticName)
      hash.bytes(entry.SemanticName, std::strlen(entry.SemanticName));
    hash.add(entry.SemanticIndex, entry.StartComponent, entry.ComponentCount, entry.OutputSlot);
  }
  for (UINT i = 0; i < so.NumStrides; ++i)
    hash.add(so.pBufferStrides[i]);
  return hash.finish(key);
}

bool PipelineCacheIdentity(WMTPipelineCacheIdentity &identity) {
  const auto &native = Identity();
  identity = native.value;
  return native.valid;
}

HRESULT PipelineCacheBlobKey(const void *data, size_t size, Sha1Digest &key) {
  if (!data || size < sizeof(CacheHeader))
    return E_INVALIDARG;
  CacheHeader header;
  memcpy(&header, data, sizeof(header));
  if (header.magic != kMagic)
    return E_INVALIDARG;
  if (header.version != kVersion)
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
  if (header.size != size)
    return E_INVALIDARG;
  key = header.key;
  return S_OK;
}

PipelineCache::~PipelineCache() {
  Complete(E_ABORT);
}

void PipelineCache::Complete(HRESULT result) noexcept {
  auto flight = std::move(flight_);
  if (!flight)
    return;
  auto &cache = Automatic();
  {
    auto lock = LockMetadata(cache);
    if (flight->done)
      return;
  }
  // Optional serialization is outside the map lock; it cannot fail PSO creation.
  if (SUCCEEDED(result) && !blob_) {
    try {
      Com<ID3DBlob> serialized;
      // Oversized results may be shared by live flight/PSO owners, but are never
      // retained as completed cache entries or written to disk.
      if (SUCCEEDED(GetBlob(&serialized))) {
        blob_ = ShareBlob(serialized.ptr());
        if (blob_) {
          // Native functions and immutable GPU constants already consumed these bytes.
          // DispatchData_alloc_init copies input via dispatch_data_create(..., NULL, NULL).
          for (auto &compiled : compiled_) {
            if (sm50_bitcode_t handle = compiled) {
              SM50DestroyBitcode(handle);
              *&compiled = nullptr;
            }
          }
          stage_artifacts_.fill({}); // The PSO blob now owns its complete CPU serialization.
        }
      }
    } catch (...) {
      // Cache allocation is optional; joined requests must still terminate.
    }
  }
  auto lock = LockMetadata(cache);
  if (flight->done)
    return;
  // Controls invalidate cache membership, not valid in-progress PSO creations.
  // Old-generation waiters still consume their producer's actual result/bytes.
  if (SUCCEEDED(result))
    flight->blob = blob_;
  const bool current = OwnsFlight(cache, flight);
  if (current) {
    if (SUCCEEDED(result) && blob_) {
      try {
        cache.Retain(flight->key, blob_);
      } catch (...) {
        // Retention/disk failure does not change the native PSO result.
      }
    }
    cache.flights.erase(flight->key);
  }
  flight->result = result;
  flight->done = true;
  lock.unlock();
  flight->ready.notify_all();
  try {
    if (owned_graphics::TraceEnabled())
      owned_graphics::Trace(FAILED(result) ? "pipeline_cache_auto_failure" :
                            !current ? "pipeline_cache_auto_retired_complete" :
                            flight->disk_hit ? "pipeline_cache_auto_restore_complete" : "pipeline_cache_auto_publish",
                            ",\"key\":\"", flight->key.string(), "\",\"hr\":", uint32_t(result),
                            ",\"bytes\":", uint64_t(flight->blob ? flight->blob->bytes->GetBufferSize() : 0));
  } catch (...) {
    // Tracing must not escape completion/destruction.
  }
  if (current && SUCCEEDED(result) && blob_ && !flight->disk_hit &&
      blob_->bytes->GetBufferSize() <= kAutomaticBytes) {
    try {
      AutomaticCachePhase phase{"disk_write"};
      std::lock_guard disk_lock(cache.disk_mutex);
      auto metadata = LockMetadata(cache);
      const auto it = cache.entries.find(flight->key);
      const bool persist = cache.enabled && cache.generation == flight->generation &&
                           it != cache.entries.end() && it->second.blob == blob_;
      metadata.unlock();
      if (persist)
        WriteAutomaticPipelineCache(flight->key, blob_->bytes.ptr());
    } catch (...) {
      // Optional synchronous persistence cannot fail the published flight.
    }
  }
}

void PipelineCache::RetryAutomatic(PipelineCache &fresh) {
  auto &cache = Automatic();
  auto lock = LockMetadata(cache);
  fresh.retry_ = true;
  if (flight_) {
    fresh.flight_ = std::move(flight_);
    fresh.flight_->disk_hit = false;
    fresh.bypass_automatic_ = true; // Keep old-generation waiters through cold retry too.
  }
  if (cache.generation != automatic_generation_ || !cache.enabled) {
    fresh.bypass_automatic_ = true;
    return;
  }
  // A racing retry may already have installed a different, good result. Never
  // delete its disk entry or flight on behalf of this old restored owner.
  const auto owner = fresh.flight_;
  lock.unlock();
  AutomaticCachePhase phase{"disk_remove"};
  std::lock_guard disk_lock(cache.disk_mutex);
  lock = LockMetadata(cache);
  auto it = cache.entries.find(automatic_key_);
  const auto producer = cache.flights.find(automatic_key_);
  if (cache.enabled && cache.generation == automatic_generation_ &&
      (producer == cache.flights.end() || producer->second == owner) &&
      (it == cache.entries.end() || it->second.blob == blob_)) {
    if (it != cache.entries.end()) {
      cache.bytes -= it->second.blob->bytes->GetBufferSize();
      cache.entries.erase(it);
    }
    lock.unlock();
    RemoveAutomaticPipelineCache(automatic_key_);
  } else {
    lock.unlock();
  }
  if (owned_graphics::TraceEnabled())
    owned_graphics::Trace("pipeline_cache_auto_retry", ",\"key\":\"", automatic_key_.string(), "\"");
}

HRESULT AutomaticPipelineCacheControl(D3D12_SHADER_CACHE_CONTROL_FLAGS control) {
  const auto flags = static_cast<UINT>(control);
  constexpr UINT disable = D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE;
  constexpr UINT enable = D3D12_SHADER_CACHE_CONTROL_FLAG_ENABLE;
  constexpr UINT clear = D3D12_SHADER_CACHE_CONTROL_FLAG_CLEAR;
  if (!flags || (flags & ~(disable | enable | clear)) || ((flags & disable) && (flags & enable)))
    return E_INVALIDARG;
  auto &cache = Automatic();
  AutomaticCachePhase phase{"disk_control"};
  std::lock_guard disk_lock(cache.disk_mutex);
  auto lock = LockMetadata(cache);
  if (flags & (disable | clear)) {
    ++cache.generation;
    // Owners/waiters retain old flights until actual completion. New requests
    // cannot join them, and generation prevents their memory/disk publication.
    cache.flights.clear();
  }
  if (flags & clear) {
    cache.entries.clear();
    cache.bytes = 0;
  }
  if (flags & disable)
    cache.enabled = false;
  if (flags & enable)
    cache.enabled = true;
  const auto generation = cache.generation;
  lock.unlock();
  const HRESULT hr = (flags & clear) ? ClearAutomaticPipelineCacheDisk() : S_OK;
  owned_graphics::Trace("pipeline_cache_auto_control", ",\"flags\":", flags,
                        ",\"generation\":", generation, ",\"hr\":", uint32_t(hr));
  return hr;
}

HRESULT PipelineCache::Initialize(const D3D12_CACHED_PIPELINE_STATE &cached, const Sha1Digest &key,
                                  uint32_t stages, uint64_t adapter) {
  key_ = key;
  stages_ = stages;
  adapter_ = adapter;
  if (cached.CachedBlobSizeInBytes)
    return LoadBlob(cached); // Explicit CachedPSO is strict and always takes precedence.
  if (bypass_automatic_) {
    owned_graphics::Trace("pipeline_cache_cold", ",\"stages\":", stages);
    return S_OK;
  }
  const auto &identity = Identity();
  if (stages && identity.valid) {
    Sha1HashState hash;
    hash.update(key).update(stages).update(adapter).update(identity.value).update(kVersion);
    const auto automatic_key = hash.final();
    auto &cache = Automatic();
    try {
      auto lock = LockMetadata(cache);
      automatic_key_ = automatic_key;
      automatic_generation_ = cache.generation;
      if (cache.enabled) {
        if (auto it = cache.entries.find(automatic_key); it != cache.entries.end()) {
          it->second.last_use = ++cache.clock;
          blob_ = it->second.blob;
          automatic_loaded_ = true;
          if (owned_graphics::TraceEnabled())
            owned_graphics::Trace("pipeline_cache_auto_hit", ",\"key\":\"", automatic_key.string(),
                                  "\",\"source\":\"memory\",\"stages\":", stages);
          return S_OK;
        }
        if (auto it = cache.flights.find(automatic_key); it != cache.flights.end()) {
          auto flight = it->second;
          if (owned_graphics::TraceEnabled())
            owned_graphics::Trace("pipeline_cache_auto_wait", ",\"key\":\"", automatic_key.string(),
                                  "\",\"stages\":", stages);
          flight->ready.wait(lock, [&] { return flight->done; });
          if (FAILED(flight->result))
            return flight->result;
          blob_ = flight->blob;
          automatic_loaded_ = bool(blob_);
          return S_OK;
        }
        auto flight = std::make_shared<AutomaticPipelineCacheFlight>();
        flight->key = automatic_key;
        flight->generation = cache.generation;
        cache.flights.emplace(automatic_key, flight);
        flight_ = std::move(flight);
        lock.unlock();
        Com<ID3DBlob> disk;
        HRESULT disk_hr = DXGI_ERROR_NOT_FOUND;
        {
          AutomaticCachePhase phase{"disk_read"};
          std::lock_guard disk_lock(cache.disk_mutex);
          lock = LockMetadata(cache);
          const bool read = OwnsFlight(cache, flight_);
          lock.unlock();
          if (read)
            disk_hr = ReadAutomaticPipelineCache(automatic_key, &disk);
        }
        HRESULT invalid = disk_hr;
        if (SUCCEEDED(disk_hr) && disk) {
          D3D12_CACHED_PIPELINE_STATE input{disk->GetBufferPointer(), disk->GetBufferSize()};
          const HRESULT hr = invalid = LoadBlob(input, disk.ptr());
          if (SUCCEEDED(hr)) {
            automatic_loaded_ = true;
            lock = LockMetadata(cache);
            flight_->disk_hit = true;
            lock.unlock();
            if (owned_graphics::TraceEnabled())
              owned_graphics::Trace("pipeline_cache_auto_hit", ",\"key\":\"", automatic_key.string(),
                                    "\",\"source\":\"disk\",\"stages\":", stages);
            return S_OK;
          }
        }
        if (disk_hr == E_FAIL || (SUCCEEDED(disk_hr) && disk)) {
          AutomaticCachePhase phase{"disk_remove"};
          std::lock_guard disk_lock(cache.disk_mutex);
          lock = LockMetadata(cache);
          const bool remove = OwnsFlight(cache, flight_) && !cache.entries.count(automatic_key);
          lock.unlock();
          if (remove)
            RemoveAutomaticPipelineCache(automatic_key);
          owned_graphics::Trace("pipeline_cache_auto_invalid", ",\"hr\":", uint32_t(invalid));
        }
        if (owned_graphics::TraceEnabled())
          owned_graphics::Trace("pipeline_cache_auto_begin", ",\"key\":\"", automatic_key.string(),
                                "\",\"stages\":", stages);
      } else if (owned_graphics::TraceEnabled()) {
        owned_graphics::Trace("pipeline_cache_auto_bypass", ",\"key\":\"", automatic_key.string(),
                              "\",\"reason\":\"disabled\"");
      }
    } catch (const std::bad_alloc &) {
      // Cache allocation is optional; the owner still completes any registered flight.
    }
  }
  owned_graphics::Trace("pipeline_cache_cold", ",\"stages\":", stages);
  return S_OK;
}

HRESULT PipelineCache::LoadBlob(const D3D12_CACHED_PIPELINE_STATE &cached, ID3DBlob *owned) {
  if (!cached.pCachedBlob || cached.CachedBlobSizeInBytes < sizeof(CacheHeader))
    return E_INVALIDARG;
  CacheHeader header;
  memcpy(&header, cached.pCachedBlob, sizeof(header));
  if (header.magic != kMagic)
    return E_INVALIDARG;
  if (header.version != kVersion)
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
  if (header.size != cached.CachedBlobSizeInBytes || header.stages != stages_ || header.key != key_)
    return E_INVALIDARG;
  std::array<StageOffsets, StageCount> offsets{};
  size_t size = 0;
  if (!Layout(header, cached.CachedBlobSizeInBytes, offsets, size) || size != header.size ||
      BlobDigest(cached.pCachedBlob, size) != header.digest)
    return E_INVALIDARG;
  if (header.adapter != adapter_)
    return D3D12_ERROR_ADAPTER_NOT_FOUND;
  const auto &identity = Identity();
  if (!identity.valid)
    return E_FAIL;
  if (memcmp(&identity.value, &header.identity, sizeof(header.identity)))
    return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
  const auto *input = static_cast<const uint8_t *>(cached.pCachedBlob);
  for (uint32_t i = 0; i < StageCount; ++i) {
    if (!(stages_ & (1u << i)))
      continue;
    const auto &record = header.records[i];
    if (memcmp(input + offsets[i].library, "MTLB", 4))
      return E_INVALIDARG;
    for (uint64_t j = 0; j < record.relocation_count; ++j) {
      SM50_CONSTANT_RELOCATION relocation;
      memcpy(&relocation, input + offsets[i].relocations + j * sizeof(relocation), sizeof(relocation));
      if (relocation.Offset > record.constant_size - sizeof(uint64_t) ||
          (relocation.TargetOffset > record.constant_size &&
           relocation.TargetOffset != SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE &&
           relocation.TargetOffset != SM50_CONSTANT_RELOCATION_TARGET_ATOMIC64_CONTEXT))
        return E_INVALIDARG;
    }
  }
  try {
    Com<ID3DBlob> copy;
    if (!owned) {
      if (HRESULT hr = CreateBlobFromMalloc(size, &copy); FAILED(hr))
        return hr;
      memcpy(copy->GetBufferPointer(), input, size);
      owned = copy.ptr();
    }
    blob_ = ShareBlob(owned);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  owned_graphics::Trace("pipeline_cache_restore", ",\"stages\":", stages_, ",\"bytes\":", uint64_t(size));
  return S_OK;
}

sm50_bitcode_t PipelineCache::bitcode(Stage stage) {
  return loaded() || stage_artifacts_[stage] ? sm50_bitcode_t{} : sm50_bitcode_t(compiled_[stage]);
}

SM50_COMPILED_BITCODE PipelineCache::view(Stage stage) {
  if (loaded())
    return blob_->views[stage];
  if (stage_artifacts_[stage])
    return stage_artifacts_[stage]->view;
  SM50_COMPILED_BITCODE result{};
  if (sm50_bitcode_t bitcode = compiled_[stage])
    SM50GetCompiledBitcode(bitcode, &result);
  return result;
}

HRESULT PipelineCache::GetBlob(ID3DBlob **blob) {
  if (!blob)
    return E_POINTER;
  *blob = nullptr;
  try {
    if (loaded()) {
      const auto size = blob_->bytes->GetBufferSize();
      const HRESULT hr = CreateBlobFromMalloc(size, blob);
      if (FAILED(hr))
        return hr;
      memcpy((*blob)->GetBufferPointer(), blob_->bytes->GetBufferPointer(), size);
      return S_OK;
    }
    const auto &identity = Identity();
    if (!identity.valid)
      return E_FAIL;
    CacheHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.stages = stages_;
    header.adapter = adapter_;
    header.identity = identity.value;
    header.key = key_;
    std::array<SM50_COMPILED_BITCODE, StageCount> views{};
    for (uint32_t i = 0; i < StageCount; ++i) {
      if (!(stages_ & (1u << i)))
        continue;
      auto &compiled = views[i] = view(Stage(i));
      header.records[i] = {compiled.Size, compiled.ConstantDataSize, compiled.ConstantDataAlignment,
                           compiled.ConstantRelocationCount};
    }
    std::array<StageOffsets, StageCount> offsets{};
    size_t size = 0;
    if (!Layout(header, std::numeric_limits<size_t>::max(), offsets, size))
      return E_FAIL;
    header.size = size;
    const HRESULT hr = CreateBlobFromMalloc(size, blob);
    if (FAILED(hr))
      return hr;
    auto *bytes = static_cast<uint8_t *>((*blob)->GetBufferPointer());
    memset(bytes, 0, size);
    memcpy(bytes, &header, sizeof(header));
    for (uint32_t i = 0; i < StageCount; ++i) {
      if (!(stages_ & (1u << i)))
        continue;
      const auto &compiled = views[i];
      const sm50_bitcode_t source = bitcode(Stage(i));
      if (!SM50ReadCompiledBitcode(source, SM50_COMPILED_BITCODE_PART_LIBRARY, compiled.Data,
                                   bytes + offsets[i].library, compiled.Size) ||
          !SM50ReadCompiledBitcode(source, SM50_COMPILED_BITCODE_PART_CONSTANTS, compiled.ConstantData,
                                   bytes + offsets[i].constants, compiled.ConstantDataSize) ||
          !SM50ReadCompiledBitcode(source, SM50_COMPILED_BITCODE_PART_RELOCATIONS, compiled.ConstantRelocations,
                                   bytes + offsets[i].relocations,
                                   compiled.ConstantRelocationCount * sizeof(SM50_CONSTANT_RELOCATION))) {
        (*blob)->Release();
        *blob = nullptr;
        return E_FAIL;
      }
    }
    const auto digest = BlobDigest(bytes, size);
    memcpy(bytes + offsetof(CacheHeader, digest), &digest, sizeof(digest));
    return S_OK;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

} // namespace dxmt
