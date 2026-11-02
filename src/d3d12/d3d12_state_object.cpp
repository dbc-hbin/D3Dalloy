/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12_state_object.hpp"
#include "Metal4Pipeline.hpp"
#include "d3d12_work_graph.hpp"

#include "DXBCParser/DXBCUtils.h"
#include "airconv_public.h"
#include "airconv_thunks.h"
#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_dxil_subobjects.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "dxil_raytracing.h"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <new>
#include <unordered_map>
#include <vector>

#include "../airconv/dxbc_root_signature.hpp"

namespace d3dalloy {
namespace {

struct LibraryExport {
  /** Public D3D12 export name of the entry point, from the library's RDAT. */
  std::wstring name;
  /** The library's own name for the entry point: `!dx.entryPoints` operand 1. */
  std::wstring entry_point;
  uint32_t stage = kOwnedRTStageInvalid;
};

struct Library {
  SharedShaderBytecode bytecode;
  std::vector<LibraryExport> exports;
};

bool CopyName(const WCHAR *name, std::wstring &out) {
  if (!name || !*name)
    return false;
  out = name;
  return true;
}

bool WideToUtf8(const std::wstring &wide, std::string &out) {
  if (wide.empty())
    return false;
  int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide.size(), nullptr, 0, nullptr, nullptr);
  if (!size)
    return false;
  out.resize(size);
  return WideCharToMultiByte(
             CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wide.size(), out.data(), size, nullptr, nullptr
         ) == size;
}

bool CopyUtf8Name(const char *name, std::wstring &out) {
  const int bytes = static_cast<int>(std::strlen(name));
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, bytes, nullptr, 0);
  if (!length)
    return false;
  out.resize(length);
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, bytes, out.data(), length) == length;
}

std::string Utf8Name(const std::wstring &name) {
  std::string utf8;
  WideToUtf8(name, utf8);
  return utf8;
}

/** The kinds of subobject a subobject-to-exports association attaches to an export. */
enum class AssociableKind : uint8_t { LocalRoot, GlobalRoot, ShaderConfig, PipelineConfig };
constexpr AssociableKind kAssociableKinds[] = {
    AssociableKind::LocalRoot, AssociableKind::GlobalRoot, AssociableKind::ShaderConfig,
    AssociableKind::PipelineConfig,
};

const char *AssociableName(AssociableKind kind) {
  switch (kind) {
  case AssociableKind::LocalRoot:
    return "local root signature";
  case AssociableKind::GlobalRoot:
    return "global root signature";
  case AssociableKind::ShaderConfig:
    return "raytracing shader config";
  case AssociableKind::PipelineConfig:
    return "raytracing pipeline config";
  }
  return "subobject";
}


/**
 * One associable subobject of a description: declared directly in it (scope 0) or in its DXIL
 * library k (scope k + 1, k an index into the state object's library list).
 */
struct Associable {
  AssociableKind kind = AssociableKind::LocalRoot;
  uint32_t scope = 0;
  MTLD3D12RootSignature *root = nullptr;
  D3D12_RAYTRACING_SHADER_CONFIG shader_config{};
  D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeline_config{};
  /**
   * Some association with a nonempty export list, declared in this subobject's own scope,
   * names it. Without one the subobject is a default association of its scope.
   */
  bool explicitly_associated_in_scope = false;
};

/** A subobject-to-exports association, resolved once every subobject has been read. */
struct PendingAssociation {
  /** 0: the description's own scope; k + 1: DXIL library k. */
  uint32_t scope = 0;
  /** The API subobject it points at, or UINT_MAX when it names a DXIL subobject. */
  UINT subobject_index = UINT_MAX;
  std::wstring subobject_name;
  /** The Associable it resolves to; SIZE_MAX for a subobject that applies globally. */
  size_t target = SIZE_MAX;
  /** Empty: a default association of `scope`. */
  std::vector<std::wstring> exports;
};

/** Two root signatures define the same signature: the same object, or byte-identical blobs. */
bool SameRootSignature(MTLD3D12RootSignature *a, MTLD3D12RootSignature *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  const void *blob_a = nullptr, *blob_b = nullptr;
  const UINT size_a = a->GetBlob(&blob_a), size_b = b->GetBlob(&blob_b);
  return size_a == size_b && blob_a && blob_b && std::memcmp(blob_a, blob_b, size_a) == 0;
}

bool SameAssociable(const Associable &a, const Associable &b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case AssociableKind::LocalRoot:
  case AssociableKind::GlobalRoot:
    return SameRootSignature(a.root, b.root);
  case AssociableKind::ShaderConfig:
    return a.shader_config.MaxPayloadSizeInBytes == b.shader_config.MaxPayloadSizeInBytes &&
           a.shader_config.MaxAttributeSizeInBytes == b.shader_config.MaxAttributeSizeInBytes;
  case AssociableKind::PipelineConfig:
    return a.pipeline_config.MaxTraceRecursionDepth == b.pipeline_config.MaxTraceRecursionDepth &&
           a.pipeline_config.Flags == b.pipeline_config.Flags;
  }
  return false;
}

bool ExportHasAssociable(const MTLD3D12StateObject::Export &entry, AssociableKind kind) {
  switch (kind) {
  case AssociableKind::LocalRoot:
    return entry.local_root_signature;
  case AssociableKind::GlobalRoot:
    return entry.global_root_signature;
  case AssociableKind::ShaderConfig:
    return entry.has_shader_config;
  case AssociableKind::PipelineConfig:
    return entry.has_pipeline_config;
  }
  return false;
}

/** The association an export already carries, as an Associable, for comparison. */
Associable AssociableOf(const MTLD3D12StateObject::Export &entry, AssociableKind kind) {
  Associable value;
  value.kind = kind;
  value.root = kind == AssociableKind::LocalRoot ? entry.local_root_signature : entry.global_root_signature;
  value.shader_config = entry.shader_config;
  value.pipeline_config = entry.pipeline_config;
  return value;
}

void ApplyAssociable(MTLD3D12StateObject::Export &entry, const Associable &value) {
  switch (value.kind) {
  case AssociableKind::LocalRoot:
    entry.local_root_signature = value.root;
    break;
  case AssociableKind::GlobalRoot:
    entry.global_root_signature = value.root;
    break;
  case AssociableKind::ShaderConfig:
    entry.shader_config = value.shader_config;
    entry.has_shader_config = true;
    break;
  case AssociableKind::PipelineConfig:
    entry.pipeline_config = value.pipeline_config;
    entry.has_pipeline_config = true;
    break;
  }
}

/** A pipeline config's recursion depth (0..31, 0 forbids TraceRay) and flags are in range. */
bool ValidPipelineConfig(const D3D12_RAYTRACING_PIPELINE_CONFIG1 &config) {
  return config.MaxTraceRecursionDepth <= D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH &&
         !(config.Flags & ~(D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES |
                            D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES | D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS));
}


std::string DescribeCompilerError(sm50_error_t error) {
  if (!error)
    return {};
  return SM50GetErrorMessageString(error);
}

/** True for the stages a raytracing state object accepts as an export. */
bool IsRaytracingStage(uint32_t stage) {
  switch (stage) {
  case kOwnedRTStageRayGeneration:
  case kOwnedRTStageIntersection:
  case kOwnedRTStageAnyHit:
  case kOwnedRTStageClosestHit:
  case kOwnedRTStageMiss:
  case kOwnedRTStageCallable:
    return true;
  default:
    return false;
  }
}

/**
 * Refuse a state object description, with the reason in the log.
 *
 * The application only ever sees the HRESULT, so a silent E_INVALIDARG leaves a
 * rejected raytracing pipeline with no way to tell which D3D12 contract the
 * description broke. Every rejection in this file therefore names itself.
 */
HRESULT RejectStateObject(const char *reason) {
  ERR("RT state object rejected: ", reason);
  return E_INVALIDARG;
}


/**
 * Enumerate the exports of a DXIL library.
 *
 * The compiler reflects the container, because only it can tell which entry
 * points a library defines, what stage each of them is and what name the library
 * publishes for it; without that D3D12_DXIL_LIBRARY_DESC with no explicit export
 * list could not be honoured.
 */
HRESULT EnumerateLibraryExports(const D3D12_SHADER_BYTECODE &bytecode, std::vector<LibraryExport> &out) {
  uint32_t count = 0;
  uint64_t names_size = 0;
  sm50_error_t error = nullptr;
  if (!SM50RTEnumerateLibraryExports(
          bytecode.pShaderBytecode, bytecode.BytecodeLength, nullptr, 0, nullptr, 0, &count, &names_size, &error
      )) {
    const std::string message = DescribeCompilerError(error);
    ERR("RT export enumeration query failed: ", message.empty() ? "no compiler diagnostic" : message);
    SM50FreeError(error);
    return E_FAIL;
  }
  SM50FreeError(error);
  if (!count) {
    /*
     * A DXIL library that reflects no export at all cannot be published: every
     * D3D12 export has to name an entry point this container actually declares, so
     * an empty reflection means the container was not understood rather than that
     * it legitimately has nothing to offer.
     */
    ERR("RT export enumeration found no export in the DXIL library");
    return E_INVALIDARG;
  }

  std::vector<struct sm50_rt_library_export_info> exports(count);
  std::vector<char> names(names_size ? names_size : 1);
  uint32_t filled = 0;
  uint64_t names_filled = 0;
  error = nullptr;
  if (!SM50RTEnumerateLibraryExports(
          bytecode.pShaderBytecode, bytecode.BytecodeLength, exports.data(), count, names.data(), names.size(),
          &filled, &names_filled, &error
      )) {
    const std::string message = DescribeCompilerError(error);
    ERR("RT export enumeration failed: ", message.empty() ? "no compiler diagnostic" : message);
    SM50FreeError(error);
    return E_FAIL;
  }
  SM50FreeError(error);
  if (filled != count || names_filled > names.size()) {
    ERR("RT export enumeration filled ", filled, " of ", count, " exports (names ", names_filled, " of ",
        names.size(), ")");
    return E_FAIL;
  }

  out.clear();
  out.reserve(count);
  /*
   * The compiler reports two names per export, both inside the same buffer: the
   * public D3D12 export name and the library's own entry point name. DXC mangles
   * the latter, so neither can be derived from the other and both have to be
   * carried through the state object.
   */
  auto read_name = [&](uint32_t offset, uint32_t length, const char *what, std::wstring &value) -> HRESULT {
    if (length > names_filled || offset > names_filled - length) {
      ERR("RT export enumeration returned an invalid ", what, " range (offset=", offset,
          ", length=", length, ", names=", names_filled, ")");
      return E_FAIL;
    }
    if (!length) {
      ERR("RT export enumeration returned an empty ", what);
      return E_FAIL;
    }
    const char *utf8 = names.data() + offset;
    const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, length, nullptr, 0);
    if (!wide_length) {
      ERR("RT export enumeration returned a ", what, " that is not UTF-8");
      return E_FAIL;
    }
    value.assign(wide_length, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, length, value.data(), wide_length) != wide_length) {
      ERR("RT export enumeration ", what, " conversion disagreed with its own length");
      return E_FAIL;
    }
    return S_OK;
  };
  for (const auto &source : exports) {
    LibraryExport entry;
    entry.stage = source.stage;
    if (FAILED(read_name(source.name_offset, source.name_length, "export name", entry.name)) ||
        FAILED(read_name(source.entry_point_offset, source.entry_point_length, "entry point name",
                         entry.entry_point)))
      return E_FAIL;
    /*
     * A container built without reflection records no public name, and then the
     * export's exact identity is the name the container itself declares. DXC marks
     * a mangled name with a leading 0x01, so that marker says the public name could
     * not be recovered rather than that the library genuinely exports this name.
     */
    if (entry.name == entry.entry_point && !entry.name.empty() && entry.name[0] == L'\x01') {
      std::string declared;
      WideToUtf8(entry.entry_point, declared);
      WARN("RT library publishes no reflection, so the export ", declared.c_str(),
           " is named by its container entry point name");
    }
    out.push_back(std::move(entry));
  }
  return S_OK;
}

/**
 * Index of an export in an export table, by either name D3D12 allows for it: the
 * name the state object publishes, or the library's own name for the entry point.
 * Returns `exports.size()` when no export answers to that name.
 *
 * Used where a state object names an export of *another* state object (a collection
 * rename), because that table's indexes belong to the collection, not to this one.
 */
size_t FindExportIndexByName(
    const std::vector<MTLD3D12StateObject::Export> &exports, const std::wstring &name) {
  for (size_t i = 0; i < exports.size(); ++i) {
    if (!exports[i].hidden && exports[i].name == name)
      return i;
  }
  /*
   * The library's own name is only a shader export's: a hit group borrows its
   * primary member's entry point once its members are resolved, and D3D12 names a
   * hit group by its own export name, so matching here would let a group answer for
   * the shader it was built from.
   */
  for (size_t i = 0; i < exports.size(); ++i) {
    if (!exports[i].hit_group && !exports[i].hidden && !exports[i].entry_point.empty() &&
        exports[i].entry_point == name)
      return i;
  }
  return exports.size();
}

bool FindSubobjectIndex(const D3D12_STATE_OBJECT_DESC &desc, const D3D12_STATE_SUBOBJECT *needle, UINT &index) {
  if (!needle)
    return false;
  for (UINT i = 0; i < desc.NumSubobjects; ++i) {
    if (&desc.pSubobjects[i] == needle) {
      index = i;
      return true;
    }
  }
  return false;
}

/**
 * The shader table preparation kernel reads the export index from exactly this
 * offset of every record, and the traversal consumes the index array it writes.
 */
static_assert(offsetof(OwnedShaderIdentifier, export_index) == 12);

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

class MTLD3D12StateObjectImpl final
    : public MTLD3D12DeviceChild<MTLD3D12StateObject, ID3D12StateObjectProperties> {
  std::vector<Library> libraries_;
  std::vector<Export> exports_;
  /** Export index by public D3D12 export name. */
  std::map<std::wstring, size_t> export_indices_;
  std::vector<Com<ID3D12RootSignature>> retained_roots_;
  MTLD3D12RootSignature *global_root_ = nullptr;
  /**
   * The root signature a DispatchRays of this pipeline binds from, when it is not the
   * global root signature itself (see BuildDispatchRootSignature). Retained because the
   * compiled pipeline keeps resolving through it after Initialize returns.
   */
  Com<ID3D12RootSignature> dispatch_root_retained_;
  MTLD3D12RootSignature *dispatch_root_ = nullptr;
  D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config_{1};
  D3D12_RAYTRACING_PIPELINE_FLAGS pipeline_flags_ = D3D12_RAYTRACING_PIPELINE_FLAG_NONE;
  UINT64 pipeline_stack_size_ = 0;
  uint32_t pipeline_token_ = 0;
  uint32_t max_payload_size_ = 0;
  uint32_t max_attribute_size_ = 0;
  bool is_pipeline_ = false;

  std::shared_ptr<const NativeRaytracingArtifact> native_executable_;
  std::shared_ptr<const RaytracingArtifact> compiled_artifact_;
  WMT::Reference<WMT::ComputePipelineState> pipeline_state_;
  uint32_t raygen_threadgroup_threads_ = 1;
  WMT::Reference<WMT::IntersectionFunctionTable> intersection_table_;
  WMT::Reference<WMT::VisibleFunctionTable> visible_function_table_;
  /**
   * What the state object's intersection function table is built from, kept so a
   * dispatch can build its own table with the same functions but its own dispatch
   * arguments block (see BindDispatchIntersectionFunctionTable). Hardware slots
   * select triangle or bounding-box callbacks; GPU SBT records select hit groups.
   */
  uint32_t intersection_callback_slots_ = 0;
  std::vector<std::pair<uint32_t, WMT::Reference<WMT::FunctionHandle>>> intersection_function_handles_;

  /** D3D12_STATE_OBJECT_CONFIG flags of this state object's own description. */
  D3D12_STATE_OBJECT_FLAGS state_flags_ = D3D12_STATE_OBJECT_FLAG_NONE;
  /**
   * Exports [0, first_local_export_) are an AddToStateObject parent's: their associations and
   * hit groups are final, and the addition's names and associations cannot reach them.
   */
  size_t first_local_export_ = 0;
  /**
   * Associations of a collection created with ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS
   * whose subobject or exports the collection could not resolve itself. The state object that
   * imports the collection resolves them against its own contents.
   */
  struct CarriedAssociation {
    bool has_value = false;
    Associable value;
    std::wstring subobject_name;
    std::vector<std::wstring> exports;
  };
  std::vector<CarriedAssociation> carried_associations_;
  /** Collections the description imports, kept for the duration of Describe. */
  std::vector<Com<MTLD3D12StateObjectImpl>> imported_collections_;
  std::vector<Com<MTLD3D12WorkGraph>> imported_function_collections_;
  std::vector<std::pair<std::wstring, Associable>> exported_root_signatures_;
  size_t inherited_root_count_ = 0;
  uint32_t next_collection_scope_ = 1;

  HRESULT AddExport(Export entry) {
    if (entry.name.empty())
      return RejectStateObject("an export has an empty name");
    if (!entry.hidden) {
      for (const auto &root : exported_root_signatures_)
        if (root.first == entry.name)
          return RejectStateObject("a function and a root signature share an export name");
      if (export_indices_.count(entry.name)) {
        for (const auto &other : exports_) {
          if (other.hidden || other.name != entry.name)
            continue;
          std::wstring_view signature = entry.entry_point;
          std::wstring_view other_signature = other.entry_point;
          const size_t separator = signature.find(L"@@");
          const size_t other_separator = other_signature.find(L"@@");
          if (separator != std::wstring_view::npos)
            signature.remove_prefix(separator + 2);
          if (other_separator != std::wstring_view::npos)
            other_signature.remove_prefix(other_separator + 2);
          if (entry.stage != kOwnedRTStageLibrary || other.stage != kOwnedRTStageLibrary ||
              signature == other_signature)
            return RejectStateObject("two exports share a name and signature");
        }
      }
      export_indices_.emplace(entry.name, exports_.size());
    }
    entry.identifier.export_index = static_cast<uint32_t>(exports_.size());
    exports_.push_back(std::move(entry));
    return S_OK;
  }

  HRESULT RetainRoot(ID3D12RootSignature *root, MTLD3D12RootSignature **out) {
    if (!root)
      return RejectStateObject("a root signature subobject holds a null signature");
    Com<ID3D12Device> root_device;
    if (FAILED(root->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&root_device))) ||
        root_device.ptr() != static_cast<ID3D12Device *>(device_))
      return RejectStateObject("a root signature belongs to a different device");
    retained_roots_.emplace_back(root);
    *out = static_cast<MTLD3D12RootSignature *>(root);
    return S_OK;
  }

  HRESULT AddRootExport(std::wstring name, MTLD3D12RootSignature *root, uint32_t scope) {
    if (name.empty() || !root)
      return RejectStateObject("a root signature export has an empty name or definition");
    if (export_indices_.count(name))
      return RejectStateObject("a root signature and a function share an export name");
    for (size_t i = 0; i < exported_root_signatures_.size(); ++i) {
      const auto &other = exported_root_signatures_[i];
      if (other.first == name) {
        // Re-including a parent library may repeat the same immutable root definition.
        if (i < inherited_root_count_ && SameRootSignature(other.second.root, root))
          return S_OK;
        return RejectStateObject("two root signatures share an export name");
      }
    }
    Associable value;
    value.kind = root->IsLocal ? AssociableKind::LocalRoot : AssociableKind::GlobalRoot;
    value.root = root;
    value.scope = scope;
    exported_root_signatures_.emplace_back(std::move(name), value);
    return S_OK;
  }

  const Library *FindLibrary(const Export &entry) const {
    return entry.library_index < libraries_.size() ? &libraries_[entry.library_index] : nullptr;
  }

  /**
   * Build the root signature a DispatchRays of this pipeline binds from.
   *
   * A raytracing shader resolves its resources through the pipeline's global root
   * signature and through its own local one, and a static sampler either of them
   * defines is pipeline state: the runtime binds one static sampler table for the whole
   * dispatch, and every root signature the dispatch can use has to define a static
   * sampler of a given register identically, so that one slot serves all of them and
   * the union fits D3D12's static sampler budget. The table is therefore the union of
   * the global root signature's static samplers and the ones only a local root
   * signature defines.
   *
   * That union cannot come from the application: the compute root signature it sets
   * before DispatchRays *is* the shaders' global root signature, so it cannot carry a
   * sampler that only a local root signature defines - and the same signature also
   * serves raster and compute pipelines, where it has to stay exactly what the
   * application created. The pipeline therefore builds the table's signature itself:
   * the global root signature's parameters verbatim and in order (so the root argument
   * entry is byte-identical to the one the application wrote through SetComputeRoot*,
   * and the compiler's root argument indices do not move), followed by the static
   * samplers the local root signatures add.
   *
   * It is serialized through the same entry point the application uses and created on
   * the same device, so the compiler and the command list read it through the one root
   * signature implementation - there is no second parser and no second index scheme.
   *
   * When no local root signature adds a static sampler the pipeline has nothing to add,
   * and the global root signature itself is the dispatch's. A pipeline with no global
   * root signature at all - which D3D12 allows for raytracing - gets a parameterless
   * dispatch signature: its exports resolve entirely through their local root
   * signatures, and the runtime then has a table to bind for the samplers they define.
   */
  HRESULT BuildDispatchRootSignature() {
    /*
     * The deserializer outlives the serialization below: the description it produces
     * points into its own copy of the blob, so the parameters cannot be read after it
     * goes away.
     */
    RootSignatureDeserializer deserializer;
    std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
    D3D12_ROOT_SIGNATURE_DESC1 desc{};
    if (global_root_) {
      const void *blob = nullptr;
      const UINT blob_size = global_root_->GetBlob(&blob);
      const void *raw = nullptr;
      UINT raw_size = 0;
      if (!blob_size || !blob || FAILED(microsoft::DXBCGetRootSignature(blob, blob_size, &raw, &raw_size)))
        return RejectStateObject("the global root signature has no readable description");
      if (FAILED(deserializer.Deserialize(raw, raw_size)))
        return RejectStateObject("the global root signature does not deserialize");
      desc = deserializer.desc_1_1_.Desc_1_1;
      if (desc.NumStaticSamplers)
        static_samplers.assign(desc.pStaticSamplers, desc.pStaticSamplers + desc.NumStaticSamplers);
    }

    UINT added = 0;
    /*
     * One pass per distinct local root signature: many exports share one, and its
     * definitions are the same every time, so a repeat contributes nothing.
     */
    std::vector<MTLD3D12RootSignature *> visited;
    for (const auto &entry : exports_) {
      if (!entry.local_root_signature)
        continue;
      if (std::find(visited.begin(), visited.end(), entry.local_root_signature) != visited.end())
        continue;
      visited.push_back(entry.local_root_signature);
      UINT appended = 0;
      HRESULT hr = AppendRootStaticSamplers(entry.local_root_signature, static_samplers, &appended);
      if (FAILED(hr))
        return hr;
      added += appended;
    }

    if (global_root_ && !added) {
      dispatch_root_ = global_root_;
      return S_OK;
    }

    /*
     * The dispatch signature holds the same parameters as the global root signature,
     * or none when the pipeline has no global root signature. Its static sampler array
     * is the union, which is what the runtime encodes the dispatch's one static sampler
     * table from.
     */
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1 = desc;
    versioned.Desc_1_1.NumStaticSamplers = static_cast<UINT>(static_samplers.size());
    versioned.Desc_1_1.pStaticSamplers = static_samplers.data();

    Com<ID3DBlob> blob;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned, &blob, nullptr);
    if (FAILED(hr))
      return hr;
    ID3D12RootSignature *root = nullptr;
    hr = CreateRootSignature(
        device_, 0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
        reinterpret_cast<void **>(&root)
    );
    if (FAILED(hr))
      return hr;
    dispatch_root_retained_ = Com<ID3D12RootSignature>::transfer(root);
    dispatch_root_ = static_cast<MTLD3D12RootSignature *>(root);
    return S_OK;
  }

  HRESULT CompilePipeline() {
    SM50_RT_PIPELINE_DESC desc{};
    std::vector<SM50_RT_LIBRARY> libraries;
    std::vector<SM50_RT_EXPORT> compiler_exports;
    std::vector<SM50_WG_EXPORT> link_exports;
    uint64_t function_names_size = 128;
    std::vector<std::string> entry_names;
    std::vector<std::string> entry_point_names;
    libraries.reserve(libraries_.size());
    for (const auto &library : libraries_) {
      libraries.push_back({library.bytecode.data(), library.bytecode.size()});
    }
    entry_names.reserve(exports_.size());
    entry_names.resize(exports_.size());
    entry_point_names.reserve(exports_.size());
    entry_point_names.resize(exports_.size());

    compiler_exports.reserve(exports_.size());
    uint32_t max_payload = 0, max_attribute = 0;
    for (size_t i = 0; i < exports_.size(); ++i) {
      const auto &entry = exports_[i];
      /**
       * A hit group is one compiled Metal function: Initialize already resolved it
       * to the primary member's entry point. The index fields reference the members
       * by export index, and the group entry itself takes the primary stage.
       *
       * Two names travel per export: the public one becomes the Metal function name
       * the runtime looks the export up by, and the library's own (DXC-mangled) name
       * is what the compiler selects the entry point inside the container with.
       */
      std::string name;
      if (!WideToUtf8(entry.name, name)) {
        ERR("RT export has no usable name");
        return E_INVALIDARG;
      }
      entry_names[i] = std::move(name);
      function_names_size += 2 * entry_names[i].size() + 64;
      std::string container_name;
      if (!WideToUtf8(entry.entry_point, container_name)) {
        ERR("RT export ", entry_names[i].c_str(), " has no usable library entry point name");
        return E_INVALIDARG;
      }
      entry_point_names[i] = std::move(container_name);
      if (!entry.hit_group) {
        SM50_WG_EXPORT function{entry_names[i].c_str(), entry_point_names[i].c_str(),
            static_cast<uint32_t>(entry.library_index), entry.scope, entry.collection_flags, entry.hidden ? 1u : 0u};
        if (entry.global_root_signature)
          function.GlobalRootSignatureSize = entry.global_root_signature->GetBlob(&function.GlobalRootSignature);
        if (entry.local_root_signature)
          function.LocalRootSignatureSize = entry.local_root_signature->GetBlob(&function.LocalRootSignature);
        link_exports.push_back(function);
      }

      uint32_t stage = entry.stage;
      uint32_t closest_hit = SM50_RT_INDEX_NONE, any_hit = SM50_RT_INDEX_NONE,
               intersection = SM50_RT_INDEX_NONE;
      if (entry.hit_group) {
        if (entry.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE) {
          stage = SM50_RT_STAGE_INTERSECTION;
          intersection = static_cast<uint32_t>(i);
          any_hit = entry.any_hit;
          closest_hit = entry.closest_hit;
        } else if (entry.closest_hit != kOwnedRTNoExport) {
          stage = SM50_RT_STAGE_CLOSEST_HIT;
          closest_hit = static_cast<uint32_t>(i);
          any_hit = entry.any_hit;
        } else {
          stage = SM50_RT_STAGE_ANY_HIT;
          any_hit = static_cast<uint32_t>(i);
        }
      } else if (entry.stage == kOwnedRTStageClosestHit) {
        closest_hit = static_cast<uint32_t>(i);
      } else if (entry.stage == kOwnedRTStageAnyHit) {
        any_hit = static_cast<uint32_t>(i);
      } else if (entry.stage == kOwnedRTStageIntersection) {
        intersection = static_cast<uint32_t>(i);
      }
      if (entry.has_shader_config) {
        max_payload = std::max(max_payload, entry.shader_config.MaxPayloadSizeInBytes);
        max_attribute = std::max(max_attribute, entry.shader_config.MaxAttributeSizeInBytes);
      }
      SM50_RT_EXPORT compiler_export{};
      compiler_export.Stage = stage;
      compiler_export.LibraryIndex = static_cast<uint32_t>(entry.library_index);
      compiler_export.HitGroupType =
          (entry.stage == kOwnedRTStageIntersection ||
           entry.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE)
              ? SM50_RT_HIT_GROUP_PROCEDURAL
              : SM50_RT_HIT_GROUP_TRIANGLES;
      compiler_export.ClosestHitIndex = closest_hit;
      compiler_export.AnyHitIndex = any_hit;
      compiler_export.IntersectionIndex = intersection;
      compiler_export.EntryPoint = entry_names[i].c_str();
      compiler_export.ContainerEntryPoint = entry_point_names[i].c_str();
      if (entry.local_root_signature) {
        const void *blob = nullptr;
        const UINT size = entry.local_root_signature->GetBlob(&blob);
        compiler_export.LocalRootSignature = blob;
        compiler_export.LocalRootSignatureSize = size;
      }
      compiler_exports.push_back(compiler_export);
    }

    desc.Libraries = libraries.data();
    desc.LibraryCount = static_cast<uint32_t>(libraries.size());
    desc.Exports = compiler_exports.data();
    desc.ExportCount = static_cast<uint32_t>(compiler_exports.size());
    desc.LinkExports = link_exports.data();
    desc.LinkExportCount = static_cast<uint32_t>(link_exports.size());
    std::vector<char> function_names(function_names_size);
    desc.FunctionNames = function_names.data();
    desc.FunctionNamesSize = function_names.size();
    /*
     * The compiler is handed the dispatch's root signature, not the application's
     * global one: it is what a DispatchRays of this pipeline binds, so its static
     * sampler table is the one the compiled shaders read their static samplers from.
     * Its parameters are the global root signature's, verbatim and in order, so the
     * root argument entry the application writes is unchanged.
     */
    MTLD3D12RootSignature *dispatch_root = GetDispatchRootSignature();
    if (dispatch_root) {
      const void *blob = nullptr;
      desc.GlobalRootSignatureSize = dispatch_root->GetBlob(&blob);
      desc.GlobalRootSignature = blob;
    }
    desc.MaxRecursionDepth = pipeline_config_.MaxTraceRecursionDepth;
    desc.MaxPayloadSize = max_payload;
    desc.MaxAttributeSize = max_attribute;
    desc.Flags = static_cast<uint32_t>(pipeline_flags_);
    desc.MetalVersion = kD3D12ShaderMetalVersion;
    /*
     * The traversal reads the payload and attribute sizes from the dispatch
     * constants, so the state object has to publish the same values it handed the
     * compiler; leaving them zero would tell the compiled traversal that a payload
     * or attribute occupies no bytes.
     */
    max_payload_size_ = max_payload;
    max_attribute_size_ = max_attribute;

    SM50_COMPILED_BITCODE metallib{};
    /*
     * One reflection record per export, plus the two shared intersection callbacks
     * and an optional synthesized raygen dispatcher for multi-raygen pipelines.
     */
    size_t continuation_count = 0;
    for (const auto &entry : exports_)
      continuation_count += entry.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE &&
        entry.closest_hit != SM50_RT_INDEX_NONE;
    std::vector<SM50_RT_FUNCTION_INFO> functions(exports_.size() + continuation_count + 3);
    uint32_t function_count = 0;
    sm50_error_t error = nullptr;
    SM50ShaderBitcode bitcode;
    std::shared_ptr<const RaytracingArtifact> artifact;
    if (!device_->GetShaderArtifactCache().CompileRaytracing(
            desc, &bitcode, metallib, functions, function_count, &error, artifact
        )) {
      const std::string message = DescribeCompilerError(error);
      ERR("RT pipeline compilation failed: ", message.empty() ? "no compiler diagnostic" : message);
      SM50FreeError(error);
      return E_FAIL;
    }
    SM50FreeError(error);
    if (!metallib.Data || !metallib.Size || !function_count || function_count > functions.size()) {
      ERR("RT pipeline compilation returned an inconsistent result (functions=", function_count, ")");
      return E_FAIL;
    }
    functions.resize(function_count);
    compiled_artifact_ = artifact;

    HRESULT constants_result = immutable_constants.Initialize(device_, metallib, bitcode);
    if (FAILED(constants_result)) return constants_result;
    WMT::Reference<WMT::Error> metal_error;
    auto library = device_->GetShaderArtifactCache().Library(device_->GetMetal4Compiler(), metallib, bitcode, metal_error);
    if (!library) {
      ERR("RT metallib load failed: ", metal_error ? metal_error.description().getUTF8String() : "no Metal error");
      return E_FAIL;
    }
    bool dispatch_entry_found = false;
    for (size_t i = 0; i < functions.size(); ++i) {
      const auto &function = functions[i];
      uses_atomic64 |= (function.Flags & SM50_RT_FUNCTION_FLAG_DEVICE_ATOMIC64_PHASE) != 0;
      if (!function.Name || !*function.Name) {
        ERR("RT reflection returned an unnamed function");
        return E_FAIL;
      }
      /*
       * A reflection record the runtime does not understand would be installed into
       * the wrong table (or silently dropped), so an unknown kind fails the pipeline
       * instead: the compiler and the runtime share this enumeration.
       */
      if (function.Kind != SM50_RT_FUNCTION_RAYGEN && function.Kind != SM50_RT_FUNCTION_INTERSECTION &&
          function.Kind != SM50_RT_FUNCTION_VISIBLE && function.Kind != SM50_RT_FUNCTION_DISPATCH) {
        ERR("RT reflection returned an unknown function kind (", function.Kind, "): ", function.Name);
        return E_FAIL;
      }
      /*
       * The shared intersection dispatch callback is not an export: it exists only to
       * be installed in the intersection function table, so it must not claim an
       * export's compiled state or a visible function table slot.
       */
      if (function.Kind != SM50_RT_FUNCTION_DISPATCH && function.ExportIndex < exports_.size()) {
        exports_[function.ExportIndex].compiled = true;
      }
      if ((function.Kind == SM50_RT_FUNCTION_RAYGEN && !(function.Flags & SM50_RT_FUNCTION_FLAG_DISPATCH_ENTRY)) ||
          function.Kind == SM50_RT_FUNCTION_INTERSECTION) {
        /*
         * Raygen and intersection dispatchers inline these export bodies; they
         * are not independent Metal entry points or function-table handles.
         */
        continue;
      }
      if (function.Flags & SM50_RT_FUNCTION_FLAG_DISPATCH_ENTRY) {
        if (function.Kind != SM50_RT_FUNCTION_RAYGEN) {
          ERR("RT reflection marked a non-raygen function as the dispatch entry: ", function.Name);
          return E_FAIL;
        }
        if (dispatch_entry_found) {
          ERR("RT pipeline has more than one dispatch entry");
          return E_FAIL;
        }
        /*
         * The dispatch entry becomes the pipeline's compute function; it is part of
         * the pipeline on its own and must not appear in the linked function list.
         */
        dispatch_entry_found = true;
        continue;
      }
    }
    if (!dispatch_entry_found) {
      ERR("RT pipeline reflection has no dispatch entry");
      return E_FAIL;
    }
    for (const auto &entry : exports_) {
      if (entry.stage == kOwnedRTStageLibrary)
        continue;
      if (!entry.compiled) {
        ERR("RT export was not compiled: ", entry.name.c_str());
        return E_FAIL;
      }
    }

    native_executable_ = device_->GetShaderArtifactCache().NativeRaytracing(device_->GetMetal4Compiler(), library,
        functions, std::max<uint32_t>(2, 2 * pipeline_config_.MaxTraceRecursionDepth + 2), metal_error);
    pipeline_state_ = native_executable_ ? native_executable_->compute : WMT::Reference<WMT::ComputePipelineState>{};
    if (!pipeline_state_) {
      ERR("RT pipeline creation failed: ",
          metal_error ? metal_error.description().getUTF8String() : "no Metal error");
      return E_FAIL;
    }
    /* Largest power of two within both the compiled max work group size and what the pipeline allows. */
    const uint64_t pipeline_threads =
        std::min<uint64_t>(SM50_RT_RAYGEN_THREADGROUP_SIZE, pipeline_state_.maxTotalThreadsPerThreadgroup());
    raygen_threadgroup_threads_ = 1;
    while (raygen_threadgroup_threads_ * 2 <= pipeline_threads)
      raygen_threadgroup_threads_ *= 2;

    /* Hardware IFT slots select geometry-kind callbacks only. Each callback
     * resolves the D3D12 hit group from the execution-time GPU shader table. */
    uint32_t intersection_slots = 0;
    for (const auto &function : functions) {
      if (function.TableIndex == SM50_RT_INDEX_NONE)
        continue;
      if (function.Kind != SM50_RT_FUNCTION_DISPATCH)
        continue;
      if (function.TableIndex > 1) {
        ERR("RT reflection returned an invalid hardware callback slot: ", function.TableIndex);
        return E_FAIL;
      }
      intersection_slots = std::max(intersection_slots, function.TableIndex + 1);
    }
    if (!intersection_slots)
      return E_FAIL;
    intersection_callback_slots_ = intersection_slots;
    intersection_table_ = pipeline_state_.newIntersectionFunctionTable(intersection_slots);
    if (!intersection_table_) {
      ERR("RT intersection function table creation failed");
      return E_FAIL;
    }
    for (size_t i = 0; i < functions.size(); ++i) {
      const auto &function = functions[i];
      if (function.Kind != SM50_RT_FUNCTION_DISPATCH || function.TableIndex == SM50_RT_INDEX_NONE)
        continue;
      auto handle = native_executable_->handles[i];
      if (!handle) {
        ERR("RT intersection dispatch callback handle creation failed: ", function.Name);
        return E_FAIL;
      }
      intersection_table_.setFunction(handle, function.TableIndex);
      intersection_function_handles_.emplace_back(function.TableIndex, handle);
    }

    /*
     * Every export the traversal reaches by export index is a Metal function the
     * dispatch has to be able to call indirectly, so it is installed into the
     * visible function table the dispatch binds at
     * SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE: slot 0 stays the null entry and
     * export index N lives in slot N + 1, exactly the layout dxil_raytracing.h
     * documents ("every lowered RT shader"). Miss, closest hit, any hit, callable
     * are reached this way. Procedural intersection bodies are selected by the
     * shared bounding-box callback, not by visible or per-export hardware slots.
     *
     * The raygen is the pipeline's compute function and is never table referenced,
     * so its export index keeps slot 0 and it is skipped.
     */
    uint32_t visible_slots = static_cast<uint32_t>(exports_.size()) + 1;
    for (const auto &function : functions)
      if (function.Kind == SM50_RT_FUNCTION_VISIBLE && function.TableIndex != SM50_RT_INDEX_NONE)
        visible_slots = std::max(visible_slots, function.TableIndex + 1);
    visible_function_table_ = pipeline_state_.newVisibleFunctionTable(visible_slots);
    if (!visible_function_table_) {
      ERR("RT visible function table creation failed");
      return E_FAIL;
    }
    for (size_t i = 0; i < functions.size(); ++i) {
      const auto &function = functions[i];
      if ((function.Flags & SM50_RT_FUNCTION_FLAG_DISPATCH_ENTRY) || function.Kind != SM50_RT_FUNCTION_VISIBLE)
        continue;
      if (function.ExportIndex >= exports_.size()) {
        ERR("RT reflection returned an export index outside the export table: ", function.ExportIndex);
        return E_FAIL;
      }
      auto handle = native_executable_->handles[i];
      if (!handle) {
        ERR("RT visible function handle creation failed: ", function.Name);
        return E_FAIL;
      }
      const uint32_t slot = function.TableIndex == SM50_RT_INDEX_NONE
        ? function.ExportIndex + 1 : function.TableIndex;
      visible_function_table_.setFunction(handle, slot);
    }

    for (auto &entry : exports_) {
      if (entry.stage == kOwnedRTStageLibrary)
        continue;
      OwnedShaderIdentifier identifier{};
      identifier.magic = kOwnedShaderIdentifierMagic;
      identifier.version = kOwnedShaderIdentifierVersion;
      identifier.kind = kOwnedShaderIdentifierPipeline;
      identifier.pipeline_token = pipeline_token_;
      identifier.export_index = entry.identifier.export_index;
      if (entry.hit_group) {
        identifier.flags |= kOwnedShaderIdentifierHitGroup;
        if (entry.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE)
          identifier.flags |= kOwnedShaderIdentifierProcedural;
      }
      entry.identifier = identifier;
    }
    return S_OK;
  }

public:
  MTLD3D12StateObjectImpl(MTLD3D12Device *device, uint32_t pipeline_token) :
      MTLD3D12DeviceChild<MTLD3D12StateObject, ID3D12StateObjectProperties>(device),
      pipeline_token_(pipeline_token) {}

  /**
   * Build this state object from `desc`.
   *
   * `parent` is an AddToStateObject parent: its exports are copied first, with their
   * identifiers and associations unchanged, and `desc` must stand on its own (D3D12 forbids
   * it from naming anything the parent defines).
   */
  HRESULT Initialize(const D3D12_STATE_OBJECT_DESC *desc, const MTLD3D12StateObjectImpl *parent = nullptr) {
    if (!desc || (desc->NumSubobjects && !desc->pSubobjects) ||
        (desc->Type != D3D12_STATE_OBJECT_TYPE_COLLECTION &&
         desc->Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE))
      return RejectStateObject("the description or its state object type is not a raytracing pipeline");
    is_pipeline_ = desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    if (parent && !is_pipeline_)
      return RejectStateObject("AddToStateObject only adds raytracing pipeline descriptions");

    try {
      if (parent) {
        if (HRESULT hr = InheritParent(*parent); FAILED(hr))
          return hr;
      }
      HRESULT hr = Describe(*desc, parent);
      /* Imported collections are only consulted while resolving; their contents were copied. */
      imported_collections_.clear();
      imported_function_collections_.clear();
      for (auto &entry : exports_)
        entry.collection = nullptr;
      return hr;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

private:
  /**
   * Take over an AddToStateObject parent: every export (hit groups resolved, associations final,
   * identifiers kept so the application's shader tables stay valid), its libraries, retained
   * root signatures and pipeline-wide settings.
   */
  HRESULT InheritParent(const MTLD3D12StateObjectImpl &parent) {
    if (!parent.is_pipeline_ || !(parent.state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS))
      return RejectStateObject(
          "AddToStateObject grows only raytracing pipelines created with ALLOW_STATE_OBJECT_ADDITIONS"
      );
    libraries_ = parent.libraries_;
    next_collection_scope_ = parent.next_collection_scope_;
    retained_roots_ = parent.retained_roots_;
    exported_root_signatures_ = parent.exported_root_signatures_;
    inherited_root_count_ = exported_root_signatures_.size();
    global_root_ = parent.global_root_;
    pipeline_config_ = parent.pipeline_config_;
    pipeline_flags_ = parent.pipeline_flags_;
    pipeline_stack_size_ = parent.pipeline_stack_size_;
    /*
     * Identifiers carry the pipeline token, and the traversal rejects a record whose token is
     * not the dispatching pipeline's: the child keeps its parent's token, which is what keeps
     * every identifier the application already wrote into a shader table valid.
     */
    pipeline_token_ = parent.pipeline_token_;
    for (const auto &source : parent.exports_) {
      Export entry = source;
      entry.compiled = false;
      if (HRESULT hr = AddExport(std::move(entry)); FAILED(hr))
        return hr;
    }
    first_local_export_ = exports_.size();
    return S_OK;
  }

  /** Read one root signature subobject of a DXIL library into a retained signature. */
  HRESULT CreateLibraryRootSignature(const DxilSubobject &source, MTLD3D12RootSignature **out) {
    std::vector<uint8_t> container;
    if (HRESULT hr = WrapRootSignaturePayload(source.root_signature.data(), source.root_signature.size(), container);
        FAILED(hr))
      return hr;
    ID3D12RootSignature *root = nullptr;
    HRESULT hr = CreateRootSignature(
        device_, 0, container.data(), container.size(), __uuidof(ID3D12RootSignature),
        reinterpret_cast<void **>(&root)
    );
    if (FAILED(hr)) {
      ERR("RT state object rejected: DXIL root signature subobject ", Utf8Name(source.name).c_str(),
          " does not create a root signature");
      return E_INVALIDARG;
    }
    retained_roots_.push_back(Com<ID3D12RootSignature>::transfer(root));
    *out = static_cast<MTLD3D12RootSignature *>(root);
    return S_OK;
  }

  HRESULT Describe(const D3D12_STATE_OBJECT_DESC &desc, const MTLD3D12StateObjectImpl *parent) {
    std::vector<Associable> associables;
    /* API subobject index -> its Associable. */
    std::unordered_map<UINT, size_t> api_associables;
    /* DXIL subobject name -> its Associable (names are unique across the description's libraries). */
    std::map<std::wstring, size_t> dxil_associables;
    std::vector<PendingAssociation> associations;
    struct PendingHitGroup {
      D3D12_HIT_GROUP_DESC desc{};
      std::wstring name, any_hit, closest_hit, intersection;
    };
    std::vector<PendingHitGroup> hit_groups;
    std::vector<std::wstring> dxil_subobject_names;
    bool have_state_config = false;
    bool have_node_mask = false;
    UINT node_mask = 0;

    auto claim_dxil_name = [&](const std::wstring &name) -> HRESULT {
      if (std::find(dxil_subobject_names.begin(), dxil_subobject_names.end(), name) != dxil_subobject_names.end()) {
        ERR("RT state object rejected: two DXIL subobjects are named ", Utf8Name(name).c_str());
        return E_INVALIDARG;
      }
      dxil_subobject_names.push_back(name);
      return S_OK;
    };
    auto set_state_config = [&](D3D12_STATE_OBJECT_FLAGS flags) -> HRESULT {
      const UINT known = D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS |
          D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS |
          D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
      if (flags & ~known)
        return RejectStateObject("a state object config holds an unknown flag");
      /* D3D12 requires every state object config of one description to match. */
      if (have_state_config && flags != state_flags_)
        return RejectStateObject("two state object configs of one description differ");
      have_state_config = true;
      state_flags_ = flags;
      return S_OK;
    };

    /* ---- Pass 1: read every subobject ---- */
    for (UINT i = 0; i < desc.NumSubobjects; ++i) {
      const auto &subobject = desc.pSubobjects[i];
      if (!subobject.pDesc) {
        ERR("RT state object rejected: subobject ", i, " of type ", uint32_t(subobject.Type), " has no description");
        return E_INVALIDARG;
      }
      switch (subobject.Type) {
      case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
        const auto &config = *static_cast<const D3D12_STATE_OBJECT_CONFIG *>(subobject.pDesc);
        if (HRESULT hr = set_state_config(config.Flags); FAILED(hr))
          return hr;
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK: {
        const UINT mask = static_cast<const D3D12_NODE_MASK *>(subobject.pDesc)->NodeMask;
        if (mask > 1 || (have_node_mask && mask != node_mask))
          return RejectStateObject("an out-of-range or mismatching node mask");
        have_node_mask = true;
        node_mask = mask;
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
      case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
        Associable value;
        value.kind = subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE ? AssociableKind::GlobalRoot
                                                                                         : AssociableKind::LocalRoot;
        if (HRESULT hr = RetainRoot(*static_cast<ID3D12RootSignature *const *>(subobject.pDesc), &value.root);
            FAILED(hr))
          return hr;
        api_associables.emplace(i, associables.size());
        associables.push_back(value);
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
        Associable value;
        value.kind = AssociableKind::ShaderConfig;
        value.shader_config = *static_cast<const D3D12_RAYTRACING_SHADER_CONFIG *>(subobject.pDesc);
        if (value.shader_config.MaxAttributeSizeInBytes > D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES)
          return RejectStateObject("a shader config attribute size above D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES");
        api_associables.emplace(i, associables.size());
        associables.push_back(value);
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
      case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: {
        Associable value;
        value.kind = AssociableKind::PipelineConfig;
        if (subobject.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG)
          value.pipeline_config = {
              static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG *>(subobject.pDesc)->MaxTraceRecursionDepth,
              D3D12_RAYTRACING_PIPELINE_FLAG_NONE
          };
        else
          value.pipeline_config = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1 *>(subobject.pDesc);
        if (!ValidPipelineConfig(value.pipeline_config))
          return RejectStateObject(
              "a raytracing pipeline config recursion depth outside 0..D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH "
              "or a flag outside SKIP_TRIANGLES | SKIP_PROCEDURAL_PRIMITIVES | ALLOW_OPACITY_MICROMAPS"
          );
        api_associables.emplace(i, associables.size());
        associables.push_back(value);
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
        const auto &library_desc = *static_cast<const D3D12_DXIL_LIBRARY_DESC *>(subobject.pDesc);
        /*
         * The library's own subobjects (HLSL GlobalRootSignature, SubobjectToExportsAssociation,
         * TriangleHitGroup, ...) are part of the state object exactly as if the description
         * declared them, but in the library's scope for association purposes.
         */
        std::vector<DxilSubobject> subobjects;
        HRESULT hr = library_desc.DXILLibrary.pShaderBytecode
            ? ReadDxilLibrarySubobjects(
                  library_desc.DXILLibrary.pShaderBytecode, library_desc.DXILLibrary.BytecodeLength, subobjects
              )
            : S_OK;
        if (hr == E_NOTIMPL)
          return RejectStateObject("a DXIL library declares a subobject kind this runtime does not read");
        if (FAILED(hr))
          return hr == E_OUTOFMEMORY ? hr : RejectStateObject("a DXIL library's RDAT subobject table is malformed");
        /*
         * An export list selects shaders and the library's subobjects alike: with one, only the
         * subobjects it names are part of the state object (and may be renamed).
         */
        std::vector<std::pair<std::wstring, std::wstring>> subobject_names;
        for (const auto &source : subobjects)
          subobject_names.emplace_back(source.name, source.name);
        hr = AddLibrary(library_desc, subobject_names);
        if (FAILED(hr))
          return hr;
        const uint32_t scope = static_cast<uint32_t>(libraries_.size());
        for (size_t k = 0; k < subobjects.size(); ++k)
          subobjects[k].name = subobject_names[k].second;
        subobjects.erase(
            std::remove_if(subobjects.begin(), subobjects.end(), [](const DxilSubobject &v) { return v.name.empty(); }),
            subobjects.end()
        );
        for (const auto &source : subobjects) {
          if (HRESULT claim = claim_dxil_name(source.name); FAILED(claim))
            return claim;
          Associable value;
          value.scope = scope;
          switch (source.kind) {
          case DxilSubobjectKind::StateObjectConfig:
            if (HRESULT set = set_state_config(source.state_object_flags); FAILED(set))
              return set;
            continue;
          case DxilSubobjectKind::GlobalRootSignature:
          case DxilSubobjectKind::LocalRootSignature:
            value.kind = source.kind == DxilSubobjectKind::GlobalRootSignature ? AssociableKind::GlobalRoot
                                                                                : AssociableKind::LocalRoot;
            if (HRESULT created = CreateLibraryRootSignature(source, &value.root); FAILED(created))
              return created;
            if (HRESULT published = AddRootExport(source.name, value.root, scope); FAILED(published))
              return published;
            break;
          case DxilSubobjectKind::RaytracingShaderConfig:
            value.kind = AssociableKind::ShaderConfig;
            value.shader_config = source.shader_config;
            if (value.shader_config.MaxAttributeSizeInBytes > D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES)
              return RejectStateObject("a DXIL shader config attribute size above the D3D12 maximum");
            break;
          case DxilSubobjectKind::RaytracingPipelineConfig:
          case DxilSubobjectKind::RaytracingPipelineConfig1:
            value.kind = AssociableKind::PipelineConfig;
            value.pipeline_config = source.pipeline_config;
            if (!ValidPipelineConfig(value.pipeline_config))
              return RejectStateObject("a DXIL raytracing pipeline config is out of range");
            break;
          case DxilSubobjectKind::SubobjectToExportsAssociation: {
            PendingAssociation association;
            association.scope = scope;
            association.subobject_name = source.associated_subobject;
            association.exports = source.exports;
            associations.push_back(std::move(association));
            continue;
          }
          case DxilSubobjectKind::HitGroup: {
            PendingHitGroup group;
            group.name = source.name;
            group.desc.Type = source.hit_group_type;
            group.any_hit = source.any_hit;
            group.closest_hit = source.closest_hit;
            group.intersection = source.intersection;
            hit_groups.push_back(std::move(group));
            continue;
          }
          }
          dxil_associables.emplace(source.name, associables.size());
          associables.push_back(value);
        }
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
        const size_t root_base = exported_root_signatures_.size();
        HRESULT hr = AddCollection(*static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(subobject.pDesc));
        if (FAILED(hr))
          return hr;
        for (size_t root = root_base; root < exported_root_signatures_.size(); ++root) {
          const auto &[name, definition] = exported_root_signatures_[root];
          if (HRESULT claim = claim_dxil_name(name); FAILED(claim))
            return claim;
          auto value = definition;
          value.scope = 0;
          value.explicitly_associated_in_scope = true; // Its collection already resolved its defaults.
          dxil_associables.emplace(name, associables.size());
          associables.push_back(value);
        }
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
        const auto &hit = *static_cast<const D3D12_HIT_GROUP_DESC *>(subobject.pDesc);
        PendingHitGroup group;
        group.desc = hit;
        if (!CopyName(hit.HitGroupExport, group.name))
          return RejectStateObject("a hit group has an empty export name");
        if (hit.AnyHitShaderImport)
          group.any_hit = hit.AnyHitShaderImport;
        if (hit.ClosestHitShaderImport)
          group.closest_hit = hit.ClosestHitShaderImport;
        if (hit.IntersectionShaderImport)
          group.intersection = hit.IntersectionShaderImport;
        hit_groups.push_back(std::move(group));
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
        const auto &source = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(subobject.pDesc);
        if (source.NumExports && !source.pExports)
          return RejectStateObject("an association has an export count but no export list");
        PendingAssociation association;
        if (!FindSubobjectIndex(desc, source.pSubobjectToAssociate, association.subobject_index))
          return RejectStateObject("an association does not point at a subobject of this description");
        for (UINT j = 0; j < source.NumExports; ++j) {
          std::wstring name;
          if (!CopyName(source.pExports[j], name))
            return RejectStateObject("an association names an empty export");
          association.exports.push_back(std::move(name));
        }
        associations.push_back(std::move(association));
        break;
      }
      case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
        const auto &source = *static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(subobject.pDesc);
        if (source.NumExports && !source.pExports)
          return RejectStateObject("a DXIL association has an export count but no export list");
        PendingAssociation association;
        if (!CopyName(source.SubobjectToAssociate, association.subobject_name))
          return RejectStateObject("a DXIL association names no subobject");
        for (UINT j = 0; j < source.NumExports; ++j) {
          std::wstring name;
          if (!CopyName(source.pExports[j], name))
            return RejectStateObject("a DXIL association names an empty export");
          association.exports.push_back(std::move(name));
        }
        associations.push_back(std::move(association));
        break;
      }
      default:
        ERR("RT state object rejected: subobject type ", uint32_t(subobject.Type), " is not a raytracing subobject");
        return E_INVALIDARG;
      }
    }

    /*
     * D3D12: an executable state object and the part AddToStateObject adds must both opt in with
     * ALLOW_STATE_OBJECT_ADDITIONS; a collection imported into a pipeline that sets the flag
     * must set it too, and one imported into a pipeline that does not must not.
     */
    if (parent && !(state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS))
      return RejectStateObject("an AddToStateObject addition does not set ALLOW_STATE_OBJECT_ADDITIONS");
    for (const auto &collection : imported_collections_) {
      const bool collection_allows = collection->state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
      const bool pipeline_allows = state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
      if (pipeline_allows && !collection_allows)
        return RejectStateObject(
            "a pipeline with ALLOW_STATE_OBJECT_ADDITIONS imports a collection that does not set it"
        );
    }

    for (const auto &collection : imported_function_collections_)
      if ((state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS) &&
          !(collection->Flags() & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS))
        return RejectStateObject("a pipeline with ALLOW_STATE_OBJECT_ADDITIONS imports a collection that does not set it");

    /* ---- Pass 2: hit groups (D3D12 API ones and DXIL-declared ones) ---- */
    for (auto &group : hit_groups) {
      Export entry;
      entry.name = group.name;
      if (group.desc.Type != D3D12_HIT_GROUP_TYPE_TRIANGLES && group.desc.Type != D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE)
        return RejectStateObject("a hit group type is neither TRIANGLES nor PROCEDURAL_PRIMITIVE");
      entry.hit_group = true;
      entry.hit_group_type = group.desc.Type;
      entry.any_hit_name = group.any_hit;
      entry.closest_hit_name = group.closest_hit;
      entry.intersection_name = group.intersection;
      if (entry.hit_group_type == D3D12_HIT_GROUP_TYPE_TRIANGLES && !entry.intersection_name.empty())
        return RejectStateObject("a triangle hit group names an intersection shader");
      if (entry.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE && entry.intersection_name.empty())
        return RejectStateObject("a procedural hit group names no intersection shader");
      if (entry.closest_hit_name.empty() && entry.any_hit_name.empty() && entry.intersection_name.empty())
        return RejectStateObject("a hit group names no closest-hit, any-hit or intersection shader");
      if (HRESULT hr = AddExport(std::move(entry)); FAILED(hr))
        return hr;
    }
    if (HRESULT hr = ResolveHitGroups(); FAILED(hr))
      return hr;

    /* ---- Pass 3: associations ---- */
    for (size_t root = 0; root < inherited_root_count_; ++root) {
      const auto &[name, definition] = exported_root_signatures_[root];
      if (dxil_associables.count(name))
        continue; // An identical root was redeclared by this addition.
      if (HRESULT claim = claim_dxil_name(name); FAILED(claim))
        return claim;
      auto value = definition;
      value.scope = 0;
      value.explicitly_associated_in_scope = true;
      dxil_associables.emplace(name, associables.size());
      associables.push_back(value);
    }
    for (auto &association : associations) {
      if (association.subobject_index != UINT_MAX) {
        auto found = api_associables.find(association.subobject_index);
        if (found != api_associables.end()) {
          association.target = found->second;
          continue;
        }
        /* Associating a state object config or node mask is legal; both apply to the whole object. */
        const auto type = desc.pSubobjects[association.subobject_index].Type;
        if (type != D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG && type != D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK)
          return RejectStateObject("an association points at a subobject that cannot be associated with exports");
        continue;
      }
      auto found = dxil_associables.find(association.subobject_name);
      if (found != dxil_associables.end()) {
        association.target = found->second;
        continue;
      }
      /*
       * A DXIL subobject that is not an associable one (a hit group, a state object config, an
       * association) applies to the state object as a whole, so associating it changes nothing.
       */
      if (std::find(dxil_subobject_names.begin(), dxil_subobject_names.end(), association.subobject_name) !=
          dxil_subobject_names.end())
        continue;
      /*
       * Not defined in this description. A collection that allows local dependencies on external
       * definitions may name a subobject its container will define; anything else is an
       * unresolved reference, which D3D12 rejects.
       */
      if (!is_pipeline_ && (state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS)) {
        CarriedAssociation carried;
        carried.subobject_name = association.subobject_name;
        carried.exports = association.exports;
        carried_associations_.push_back(std::move(carried));
        association.target = SIZE_MAX;
        association.subobject_name.clear();
        association.exports.clear();
        association.subobject_index = UINT_MAX - 1;
        continue;
      }
      ERR("RT state object rejected: an association names subobject ", Utf8Name(association.subobject_name).c_str(),
          ", which this description does not define");
      return E_INVALIDARG;
    }
    for (const auto &association : associations) {
      if (association.target == SIZE_MAX || association.exports.empty())
        continue;
      auto &target = associables[association.target];
      if (association.scope == target.scope)
        target.explicitly_associated_in_scope = true;
    }

    /*
     * The association a given export ends up with, per kind, in D3D12's precedence:
     *  1. explicit associations declared directly in the description (scope 0);
     *  2. default associations declared directly in the description, which override anything
     *     a DXIL library declares for the exports of a directly included library
     *     ("Exception: overriding DXIL library associations");
     *  3. explicit associations declared in a DXIL library;
     *  4. default associations of the export's own library scope.
     * Conflicting definitions at one precedence level reject the description. An imported
     * collection's exports keep the associations the collection resolved; the description's
     * own associations can only fill in what the collection left open.
     */
    const size_t first_new = first_local_export_;
    std::vector<std::array<int, 4>> level(exports_.size());
    for (auto &levels : level)
      levels.fill(-1);
    std::vector<std::array<size_t, 4>> chosen(exports_.size());
    auto offer = [&](size_t index, const Associable &value, size_t associable, int precedence) -> HRESULT {
      Export &entry = exports_[index];
      const auto kind = static_cast<size_t>(value.kind);
      if (entry.from_collection) {
        /* A collection's resolved association is final; agreeing with it is fine. */
        if (ExportHasAssociable(entry, value.kind)) {
          if (precedence == 0 && !SameAssociable(AssociableOf(entry, value.kind), value)) {
            ERR("RT state object rejected: an association conflicts with the ", AssociableName(value.kind),
                " collection export ", Utf8Name(entry.name).c_str(), " already has");
            return E_INVALIDARG;
          }
          return S_OK;
        }
      }
      int &current = level[index][kind];
      if (current != -1 && current < precedence)
        return S_OK;
      if (current == precedence && !SameAssociable(associables[chosen[index][kind]], value)) {
        ERR("RT state object rejected: two different ", AssociableName(value.kind), "s are associated with export ",
            Utf8Name(entry.name).c_str());
        return E_INVALIDARG;
      }
      current = precedence;
      chosen[index][kind] = associable;
      return S_OK;
    };
    auto candidate = [&](size_t index, uint32_t scope) {
      const Export &entry = exports_[index];
      if (index < first_new)
        return false;
      /* A default association reaches the exports of its scope and of the scopes it contains. */
      if (scope == 0)
        return true;
      return !entry.from_collection && entry.library_index + 1 == scope;
    };
    for (const auto &association : associations) {
      if (association.target == SIZE_MAX)
        continue;
      const Associable &value = associables[association.target];
      if (!association.exports.empty()) {
        const int precedence = association.scope == 0 ? 0 : 2;
        for (const auto &name : association.exports) {
          const size_t index = FindExportIndexByName(exports_, name);
          if (index == exports_.size() || index < first_new) {
            /*
             * A library's own association may name a library export the description's export
             * list left out: that export is not part of this state object, so neither is the
             * association to it.
             */
            if (association.scope != 0 && index == exports_.size())
              continue;
            if (!is_pipeline_ &&
                (state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS)) {
              CarriedAssociation carried;
              carried.has_value = true;
              carried.value = value;
              carried.exports = {name};
              carried_associations_.push_back(std::move(carried));
              continue;
            }
            ERR("RT state object rejected: an association names ", Utf8Name(name).c_str(),
                ", which is not an export of this state object");
            return E_INVALIDARG;
          }
          if (HRESULT hr = offer(index, value, association.target, precedence); FAILED(hr))
            return hr;
          if (exports_[index].stage == kOwnedRTStageLibrary)
            for (size_t overload = first_new; overload < exports_.size(); ++overload) {
              const auto &entry = exports_[overload];
              if (overload != index && !entry.hidden && entry.stage == kOwnedRTStageLibrary &&
                  (entry.name == name || entry.entry_point == name))
                if (HRESULT hr = offer(overload, value, association.target, precedence); FAILED(hr))
                  return hr;
            }
        }
      } else {
        const int precedence = association.scope == 0 ? 1 : 3;
        for (size_t index = 0; index < exports_.size(); ++index) {
          if (!candidate(index, association.scope))
            continue;
          if (HRESULT hr = offer(index, value, association.target, precedence); FAILED(hr))
            return hr;
        }
      }
    }
    /* A subobject that no association of its own scope names is a default association of that scope. */
    for (size_t a = 0; a < associables.size(); ++a) {
      const Associable &value = associables[a];
      if (value.explicitly_associated_in_scope)
        continue;
      const int precedence = value.scope == 0 ? 1 : 3;
      for (size_t index = 0; index < exports_.size(); ++index) {
        if (!candidate(index, value.scope))
          continue;
        if (HRESULT hr = offer(index, value, a, precedence); FAILED(hr))
          return hr;
      }
    }
    /* Associations carried by imported collections resolve against this description's subobjects. */
    for (const auto &collection : imported_collections_) {
      for (const auto &carried : collection->carried_associations_) {
        const Associable *value = carried.has_value ? &carried.value : nullptr;
        if (!value) {
          auto found = dxil_associables.find(carried.subobject_name);
          if (found == dxil_associables.end()) {
            ERR("RT state object rejected: a collection depends on subobject ", Utf8Name(carried.subobject_name).c_str(),
                ", which the containing state object does not define");
            return E_INVALIDARG;
          }
          value = &associables[found->second];
        }
        for (size_t index = first_new; index < exports_.size(); ++index) {
          Export &entry = exports_[index];
          if (!entry.from_collection || entry.collection != collection.ptr())
            continue;
          const bool named = carried.exports.empty() ||
              std::find(carried.exports.begin(), carried.exports.end(), entry.collection_name) != carried.exports.end();
          if (!named)
            continue;
          if (ExportHasAssociable(entry, value->kind)) {
            if (!SameAssociable(AssociableOf(entry, value->kind), *value))
              return RejectStateObject("a collection's external association conflicts with one it resolved itself");
            continue;
          }
          ApplyAssociable(entry, *value);
        }
      }
    }
    /*
     * An association made to a hit group applies to its members (D3D12 "Subobject associations
     * for hit groups"), so it wins over anything weaker the members got by default.
     */
    for (size_t index = first_new; index < exports_.size(); ++index) {
      const Export &group = exports_[index];
      if (!group.hit_group || group.from_collection)
        continue;
      for (uint32_t member : {group.closest_hit, group.any_hit, group.intersection}) {
        if (member == kOwnedRTNoExport || member < first_new)
          continue;
        for (AssociableKind kind : kAssociableKinds) {
          const auto k = static_cast<size_t>(kind);
          if (level[index][k] != -1 && (level[member][k] == -1 || level[member][k] > level[index][k])) {
            level[member][k] = level[index][k];
            chosen[member][k] = chosen[index][k];
          }
        }
      }
    }
    for (size_t index = first_new; index < exports_.size(); ++index) {
      for (AssociableKind kind : kAssociableKinds) {
        const auto k = static_cast<size_t>(kind);
        if (level[index][k] != -1)
          ApplyAssociable(exports_[index], associables[chosen[index][k]]);
      }
    }

    /*
     * A hit group and its members share every association: what the group has its members get,
     * and the members of a group without one have to agree with each other.
     */
    for (size_t index = first_new; index < exports_.size(); ++index) {
      Export &group = exports_[index];
      if (!group.hit_group)
        continue;
      const uint32_t members[] = {group.closest_hit, group.any_hit, group.intersection};
      for (AssociableKind kind : kAssociableKinds) {
        Associable group_value = AssociableOf(group, kind);
        bool have = ExportHasAssociable(group, kind);
        for (uint32_t member : members) {
          if (member == kOwnedRTNoExport || !ExportHasAssociable(exports_[member], kind))
            continue;
          const Associable member_value = AssociableOf(exports_[member], kind);
          if (have && !SameAssociable(group_value, member_value)) {
            ERR("RT state object rejected: hit group ", Utf8Name(group.name).c_str(), " and a member hold different ",
                AssociableName(kind), "s");
            return E_INVALIDARG;
          }
          if (!have) {
            group_value = member_value;
            have = true;
          }
        }
        if (!have)
          continue;
        ApplyAssociable(group, group_value);
        for (uint32_t member : members) {
          if (member != kOwnedRTNoExport && !ExportHasAssociable(exports_[member], kind))
            ApplyAssociable(exports_[member], group_value);
        }
      }
    }

    if (!is_pipeline_)
      return ValidateCollection();
    return FinishPipeline(parent);
  }

  /**
   * A collection without ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS has to be complete on
   * its own: every export needs its shader and pipeline configs, exactly what a pipeline would
   * require of it. With the flag, anything left open is resolved by the containing pipeline.
   */
  HRESULT ValidateCollection() {
    const bool allow_unresolved = state_flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS;
    std::vector<SM50_WG_LIBRARY> libraries;
    for (const auto &library : libraries_)
      libraries.push_back({library.bytecode.data(), library.bytecode.size()});
    std::vector<std::string> names(exports_.size()), symbols(exports_.size());
    std::vector<Com<ID3DBlob>> validation_roots(exports_.size());
    std::vector<SM50_WG_EXPORT> functions;
    for (size_t i = 0; i < exports_.size(); ++i) {
      const auto &entry = exports_[i];
      if (entry.hit_group)
        continue;
      if (!WideToUtf8(entry.name, names[i]) || !WideToUtf8(entry.entry_point, symbols[i]))
        return RejectStateObject("a collection function has an invalid name");
      SM50_WG_EXPORT function{names[i].c_str(), symbols[i].c_str(), static_cast<uint32_t>(entry.library_index),
          1, state_flags_, entry.hidden ? 1u : 0u};
      if (entry.global_root_signature)
        function.GlobalRootSignatureSize = entry.global_root_signature->GetBlob(&function.GlobalRootSignature);
      if (entry.local_root_signature)
        function.LocalRootSignatureSize = entry.local_root_signature->GetBlob(&function.LocalRootSignature);
      // Collections can associate a different global root with each export. Validate
      // that export against its own global parameters and global/local sampler union.
      std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
      UINT appended = 0;
      HRESULT hr = AppendRootStaticSamplers(entry.global_root_signature, static_samplers, &appended);
      if (FAILED(hr))
        return hr;
      hr = AppendRootStaticSamplers(entry.local_root_signature, static_samplers, &appended);
      if (FAILED(hr))
        return hr;
      if (appended) {
        RootSignatureDeserializer deserializer;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if (entry.global_root_signature) {
          const void *raw = nullptr;
          UINT raw_size = 0;
          if (FAILED(microsoft::DXBCGetRootSignature(function.GlobalRootSignature, function.GlobalRootSignatureSize,
                      &raw, &raw_size)) ||
              FAILED(deserializer.Deserialize(raw, raw_size)))
            return E_INVALIDARG;
          root_desc.Desc_1_1 = deserializer.desc_1_1_.Desc_1_1;
        }
        root_desc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(static_samplers.size());
        root_desc.Desc_1_1.pStaticSamplers = static_samplers.data();
        hr = D3D12SerializeVersionedRootSignature(&root_desc, &validation_roots[i], nullptr);
        if (FAILED(hr))
          return hr;
        function.GlobalRootSignature = validation_roots[i]->GetBufferPointer();
        function.GlobalRootSignatureSize = validation_roots[i]->GetBufferSize();
      }
      functions.push_back(function);
    }
    for (const auto &function : functions) {
      SM50_WG_NODE_COMPILE_DESC desc{};
      desc.Library = libraries[function.LibraryIndex].Bytecode;
      desc.LibrarySize = libraries[function.LibraryIndex].Size;
      desc.EntryPoint = function.EntryPoint;
      desc.FunctionName = function.Name;
      desc.GlobalRootSignature = function.GlobalRootSignature;
      desc.GlobalRootSignatureSize = function.GlobalRootSignatureSize;
      desc.LocalRootSignature = function.LocalRootSignature;
      desc.LocalRootSignatureSize = function.LocalRootSignatureSize;
      desc.Libraries = libraries.data();
      desc.LibraryCount = libraries.size();
      desc.Exports = functions.data();
      desc.ExportCount = functions.size();
      desc.EntryLibraryIndex = function.LibraryIndex;
      desc.EntryExportIndex = &function - functions.data();
      desc.AllowUnresolved = allow_unresolved;
      desc.Reserved = 1; // Validate dependencies and required resource bindings without emitting AIR.

      SM50ShaderBitcode bitcode;
      SM50_COMPILED_BITCODE metallib{};
      SM50_WG_NODE_COMPILED_INFO info{};
      sm50_error_t error = nullptr;
      const bool valid = SM50WGCompileNode(&desc, &bitcode, &metallib, &info, &error);
      if (!valid)
        ERR("RT collection dependency validation failed: ", DescribeCompilerError(error));
      SM50FreeError(error);
      if (!valid)
        return E_INVALIDARG;
    }
    if (allow_unresolved)
      return S_OK;
    for (const auto &entry : exports_) {
      if (entry.stage == kOwnedRTStageLibrary)
        continue;
      if (!entry.has_shader_config || !entry.has_pipeline_config) {
        ERR("RT state object rejected: collection export ", Utf8Name(entry.name).c_str(),
            " has no raytracing shader or pipeline config, and the collection does not set "
            "ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS");
        return E_INVALIDARG;
      }
    }
    return S_OK;
  }

  HRESULT FinishPipeline(const MTLD3D12StateObjectImpl *parent) {
    /*
     * Shader and pipeline configs: required on every export and matching across all of them
     * (D3D12 "Subobject association requirements": full state object scope).
     */
    const Export *first_config = nullptr;
    for (const auto &entry : exports_) {
      if (entry.stage == kOwnedRTStageLibrary)
        continue;
      if (!entry.has_shader_config || !entry.has_pipeline_config) {
        ERR("RT state object rejected: export ", Utf8Name(entry.name).c_str(),
            " has no raytracing shader config or no raytracing pipeline config");
        return E_INVALIDARG;
      }
      if (!first_config) {
        first_config = &entry;
        continue;
      }
      if (entry.pipeline_config.MaxTraceRecursionDepth != first_config->pipeline_config.MaxTraceRecursionDepth ||
          entry.pipeline_config.Flags != first_config->pipeline_config.Flags)
        return RejectStateObject("the exports of a raytracing pipeline hold different pipeline configs");
      if (entry.shader_config.MaxPayloadSizeInBytes != first_config->shader_config.MaxPayloadSizeInBytes ||
          entry.shader_config.MaxAttributeSizeInBytes != first_config->shader_config.MaxAttributeSizeInBytes)
        return RejectStateObject("the exports of a raytracing pipeline hold different shader configs");
    }
    if (!first_config)
      return RejectStateObject("a raytracing pipeline has no exports");
    pipeline_config_.MaxTraceRecursionDepth = first_config->pipeline_config.MaxTraceRecursionDepth;
    pipeline_flags_ = first_config->pipeline_config.Flags;

    /*
     * One global root signature per pipeline: the compute root signature DispatchRays binds
     * is the global root signature of every shader the dispatch can reach, and an export with
     * none uses only its local root signature. Different global root signatures on different
     * exports would need one compiled pipeline per signature.
     */
    MTLD3D12RootSignature *global = nullptr;
    for (const auto &entry : exports_) {
      if (!entry.global_root_signature)
        continue;
      if (global && !SameRootSignature(global, entry.global_root_signature))
        return RejectStateObject(
            "the exports of one raytracing pipeline are associated with different global root signatures"
        );
      global = global ? global : entry.global_root_signature;
    }
    if (parent && global_root_ && global && !SameRootSignature(global_root_, global))
      return RejectStateObject("an AddToStateObject addition uses a different global root signature");
    global_root_ = global ? global : global_root_;

    size_t raygen_count = 0;
    for (const auto &entry : exports_) {
      if (!entry.hit_group && entry.stage == kOwnedRTStageRayGeneration)
        raygen_count++;
    }
    if (raygen_count < 1) {
      ERR("RT pipeline needs at least one ray generation shader, found 0");
      return E_INVALIDARG;
    }

    HRESULT hr = BuildDispatchRootSignature();
    if (FAILED(hr))
      return hr;
    return CompilePipeline();
  }

  /**
   * Add one DXIL library subobject's exports. `subobjects` holds (declared name, published
   * name) for each DXIL subobject of the library; with an export list, a subobject the list
   * does not name gets an empty published name, and a renamed one its new name.
   */
  HRESULT AddLibrary(
      const D3D12_DXIL_LIBRARY_DESC &library_desc, std::vector<std::pair<std::wstring, std::wstring>> &subobjects
  ) {
    if (!library_desc.DXILLibrary.pShaderBytecode || !library_desc.DXILLibrary.BytecodeLength ||
        (library_desc.NumExports && !library_desc.pExports))
      return RejectStateObject("a DXIL library subobject has no bytecode, or an export list with no entries");
    Library library;
    library.bytecode = device_->GetShaderArtifactCache().RetainBytecode(
        library_desc.DXILLibrary.pShaderBytecode, library_desc.DXILLibrary.BytecodeLength);
    std::vector<LibraryExport> reflected;
    HRESULT hr = EnumerateLibraryExports(library_desc.DXILLibrary, reflected);
    if (FAILED(hr))
      return hr;
    if (!library_desc.NumExports) {
      /* Ordinary functions are legal exports but are never shader-table entries. */
      for (const auto &reflected_export : reflected) {
        if (!IsRaytracingStage(reflected_export.stage) && reflected_export.stage != kOwnedRTStageLibrary) {
          ERR("RT state object rejected: library export ", Utf8Name(reflected_export.name).c_str(), " has stage ",
              reflected_export.stage, ", which is not a raytracing stage");
          return E_INVALIDARG;
        }
      }
      library.exports = std::move(reflected);
    } else {
      std::vector<bool> listed(subobjects.size(), false);
      std::vector<std::wstring> published(subobjects.size());
      for (UINT j = 0; j < library_desc.NumExports; ++j) {
        const auto &source = library_desc.pExports[j];
        LibraryExport target;
        if (!CopyName(source.Name, target.name))
          return RejectStateObject("a DXIL library export has an empty name");
        /*
         * D3D12 names the library's own export in ExportToRename when that is present and in
         * Name otherwise, and either spelling is allowed there: the container's entry point
         * name is the mangled one, while the name the library publishes is the unmangled one.
         */
        const WCHAR *internal = source.ExportToRename ? source.ExportToRename : source.Name;
        bool is_subobject = false;
        for (size_t k = 0; k < subobjects.size(); ++k) {
          if (subobjects[k].first != internal)
            continue;
          listed[k] = true;
          published[k] = target.name;
          is_subobject = true;
        }
        if (is_subobject)
          continue;
        const LibraryExport *match = nullptr;
        unsigned matches = 0;
        for (const auto &candidate : reflected) {
          if (candidate.name == internal || candidate.entry_point == internal) {
            match = &candidate;
            matches++;
          }
        }
        if (!matches) {
          ERR("RT state object rejected: the library does not declare entry point ", Utf8Name(internal).c_str());
          return E_INVALIDARG;
        }
        if (matches > 1) {
          for (const auto &candidate : reflected) {
            if (candidate.name != internal && candidate.entry_point != internal)
              continue;
            if (candidate.stage != kOwnedRTStageLibrary)
              return RejectStateObject("a shader export name is ambiguous");
            auto overload = target;
            overload.entry_point = candidate.entry_point;
            overload.stage = candidate.stage;
            library.exports.push_back(std::move(overload));
          }
          continue;
        }
        if (!IsRaytracingStage(match->stage) && match->stage != kOwnedRTStageLibrary) {
          ERR("RT state object rejected: library export ", Utf8Name(internal).c_str(), " has stage ", match->stage,
              ", which is not a raytracing stage");
          return E_INVALIDARG;
        }
        target.entry_point = match->entry_point;
        target.stage = match->stage;
        library.exports.push_back(std::move(target));
      }
      for (size_t k = 0; k < subobjects.size(); ++k)
        subobjects[k].second = listed[k] ? published[k] : std::wstring();
    }
    for (const auto &target : library.exports) {
      Export entry;
      entry.name = target.name;
      entry.entry_point = target.entry_point;
      entry.stage = target.stage;
      entry.library_index = libraries_.size();
      hr = AddExport(std::move(entry));
      if (FAILED(hr))
        return hr;
    }
    libraries_.push_back(std::move(library));
    return S_OK;
  }

  /**
   * Import an existing collection. Its exports keep the associations the collection resolved
   * and its hit groups their members; a hit group whose member the export list leaves out
   * still compiles that member, as an export the application cannot name.
   */
  HRESULT AddCollection(const D3D12_EXISTING_COLLECTION_DESC &collection) {
    if (!collection.pExistingCollection || (collection.NumExports && !collection.pExports))
      return RejectStateObject("an existing collection subobject has no collection, or an export list with no entries");
    Com<ID3D12Device> collection_device;
    if (FAILED(collection.pExistingCollection->GetDevice(
            __uuidof(ID3D12Device), reinterpret_cast<void **>(&collection_device)
        )) ||
        collection_device.ptr() != static_cast<ID3D12Device *>(device_))
      return RejectStateObject("an existing collection belongs to a different device");
    auto *existing = dynamic_cast<MTLD3D12StateObjectImpl *>(collection.pExistingCollection);
    if (auto *functions = dynamic_cast<MTLD3D12WorkGraph *>(collection.pExistingCollection))
      return AddFunctionCollection(collection, *functions);
    if (!existing || existing->is_pipeline_)
      return RejectStateObject("an existing collection subobject does not hold a collection this backend created");
    if (!is_pipeline_)
      return RejectStateObject("a collection cannot be built from another collection");
    imported_collections_.emplace_back(existing);

    /* The collection's libraries come along; its exports' library indexes are rebased. */
    const size_t library_base = libraries_.size();
    const uint32_t scope = next_collection_scope_++;
    libraries_.insert(libraries_.end(), existing->libraries_.begin(), existing->libraries_.end());
    retained_roots_.insert(retained_roots_.end(), existing->retained_roots_.begin(), existing->retained_roots_.end());

    const auto &source_exports = existing->exports_;
    std::vector<size_t> mapped(source_exports.size(), SIZE_MAX);
    auto import = [&](size_t source_index, std::wstring public_name, bool hidden) -> HRESULT {
      if (mapped[source_index] != SIZE_MAX) {
        if (!hidden)
          return RejectStateObject("a collection export is imported twice");
        return S_OK;
      }
      Export entry = source_exports[source_index];
      entry.collection_name = entry.name;
      entry.name = std::move(public_name);
      entry.hidden = hidden;
      entry.from_collection = true;
      entry.collection = existing;
      entry.scope = scope;
      entry.collection_flags = existing->state_flags_;
      entry.library_index += library_base;
      entry.identifier = {};
      entry.compiled = false;
      mapped[source_index] = exports_.size();
      return AddExport(std::move(entry));
    };
    if (!collection.NumExports) {
      for (const auto &[name, value] : existing->exported_root_signatures_)
        if (HRESULT hr = AddRootExport(name, value.root, scope); FAILED(hr))
          return hr;
      for (size_t i = 0; i < source_exports.size(); ++i) {
        if (HRESULT hr = import(i, source_exports[i].name, source_exports[i].hidden); FAILED(hr))
          return hr;
      }
    } else {
      for (UINT j = 0; j < collection.NumExports; ++j) {
        const auto &rename = collection.pExports[j];
        std::wstring public_name;
        if (!CopyName(rename.Name, public_name) || rename.Flags != D3D12_EXPORT_FLAG_NONE)
          return RejectStateObject("a collection export has an invalid name or flags");
        const WCHAR *source_name = rename.ExportToRename ? rename.ExportToRename : rename.Name;
        bool root_found = false;
        for (const auto &[name, value] : existing->exported_root_signatures_) {
          if (name != source_name)
            continue;
          if (HRESULT hr = AddRootExport(public_name, value.root, scope); FAILED(hr))
            return hr;
          root_found = true;
          break;
        }
        if (root_found)
          continue;
        const size_t index = FindExportIndexByName(source_exports, source_name);
        if (index == source_exports.size())
          return RejectStateObject("the collection does not hold the export being imported");
        if (source_exports[index].stage == kOwnedRTStageLibrary) {
          for (size_t overload = 0; overload < source_exports.size(); ++overload) {
            const auto &entry = source_exports[overload];
            if (!entry.hidden && entry.stage == kOwnedRTStageLibrary &&
                (entry.name == source_name || entry.entry_point == source_name))
              if (HRESULT hr = import(overload, public_name, false); FAILED(hr))
                return hr;
          }
        } else if (HRESULT hr = import(index, std::move(public_name), false); FAILED(hr)) {
          return hr;
        }
      }
    }
    // Unpublished ordinary functions remain dependencies inside the collection namespace.
    for (size_t i = 0; i < source_exports.size(); ++i)
      if (mapped[i] == SIZE_MAX && source_exports[i].stage == kOwnedRTStageLibrary)
        if (HRESULT hr = import(i, source_exports[i].name, true); FAILED(hr))
          return hr;
    /* Members of an imported hit group come along even when the export list left them out. */
    for (size_t i = 0; i < source_exports.size(); ++i) {
      if (mapped[i] == SIZE_MAX || !source_exports[i].hit_group)
        continue;
      for (uint32_t member : {source_exports[i].closest_hit, source_exports[i].any_hit, source_exports[i].intersection}) {
        if (member == kOwnedRTNoExport || mapped[member] != SIZE_MAX)
          continue;
        std::wstring hidden_name = source_exports[i].name + L"::" + source_exports[member].name;
        if (HRESULT hr = import(member, std::move(hidden_name), true); FAILED(hr))
          return hr;
      }
    }
    for (size_t i = 0; i < source_exports.size(); ++i) {
      if (mapped[i] == SIZE_MAX || !source_exports[i].hit_group)
        continue;
      Export &group = exports_[mapped[i]];
      for (uint32_t *member : {&group.closest_hit, &group.any_hit, &group.intersection}) {
        if (*member != kOwnedRTNoExport)
          *member = static_cast<uint32_t>(mapped[*member]);
      }
    }
    return S_OK;
  }

  HRESULT AddFunctionCollection(const D3D12_EXISTING_COLLECTION_DESC &collection, MTLD3D12WorkGraph &source) {
    if (!source.IsCollection() || !is_pipeline_)
      return RejectStateObject("an existing collection subobject does not hold an importable collection");
    imported_function_collections_.emplace_back(&source);
    const size_t library_base = libraries_.size();
    for (const auto &bytes : source.Libraries()) {
      Library library;
      library.bytecode = bytes;
      libraries_.push_back(std::move(library));
    }
    const uint32_t scope = next_collection_scope_++;
    std::vector<bool> imported(source.Exports().size(), false);
    auto import = [&](const MTLD3D12WorkGraph::Node &function, std::wstring name, bool hidden = false) -> HRESULT {
      if (function.info.Launch != 0)
        return RejectStateObject("a raytracing pipeline cannot import a work-graph node");
      Export entry;
      entry.name = std::move(name);
      if (!CopyUtf8Name(function.entry.c_str(), entry.entry_point))
        return RejectStateObject("a collection function has no usable container symbol");
      entry.stage = kOwnedRTStageLibrary;
      entry.library_index = library_base + function.library;
      entry.scope = scope;
      entry.collection_flags = source.Flags();
      entry.from_collection = true;
      entry.collection = &source;
      entry.collection_name = function.shader;
      entry.hidden = hidden;
      imported[&function - source.Exports().data()] = true;
      entry.local_root_signature = function.local_root.ptr();
      entry.global_root_signature = function.global_root.ptr();
      if (function.local_root)
        retained_roots_.emplace_back(function.local_root.ptr());
      if (function.global_root)
        retained_roots_.emplace_back(function.global_root.ptr());
      return AddExport(std::move(entry));
    };
    if (!collection.NumExports) {
      for (const auto &root : source.Roots()) {
        retained_roots_.emplace_back(root.root.ptr());
        if (HRESULT hr = AddRootExport(root.name, root.root.ptr(), scope); FAILED(hr))
          return hr;
      }
      for (const auto &function : source.Exports())
        if (function.exported)
          if (HRESULT hr = import(function, function.shader); FAILED(hr))
            return hr;
    } else {
      for (UINT i = 0; i < collection.NumExports; ++i) {
        const auto &rename = collection.pExports[i];
        std::wstring name;
        if (!CopyName(rename.Name, name) || rename.Flags != D3D12_EXPORT_FLAG_NONE)
          return RejectStateObject("a collection export has an invalid name or flags");
        const WCHAR *original = rename.ExportToRename ? rename.ExportToRename : rename.Name;
        bool found = false;
        for (const auto &root : source.Roots()) {
          if (root.name != original)
            continue;
          retained_roots_.emplace_back(root.root.ptr());
          if (HRESULT hr = AddRootExport(name, root.root.ptr(), scope); FAILED(hr))
            return hr;
          found = true;
          break;
        }
        if (found)
          continue;
        for (const auto &function : source.Exports()) {
          std::wstring symbol;
          if (!function.exported || !CopyUtf8Name(function.entry.c_str(), symbol))
            continue;
          if (function.shader != original && symbol != original)
            continue;
          found = true;
          if (HRESULT hr = import(function, name); FAILED(hr))
            return hr;
        }
        if (!found)
          return RejectStateObject("the collection does not hold the export being imported");
      }
    }
    for (size_t i = 0; i < source.Exports().size(); ++i) {
      const auto &function = source.Exports()[i];
      if (!imported[i] && function.info.Launch == 0)
        if (HRESULT hr = import(function, function.shader, true); FAILED(hr))
          return hr;
    }
    return S_OK;
  }

  /** Resolve the members of every hit group this description declares. */
  HRESULT ResolveHitGroups() {
    for (size_t group_index = first_local_export_; group_index < exports_.size(); ++group_index) {
      auto &group = exports_[group_index];
      if (!group.hit_group || group.from_collection)
        continue;
      struct {
        std::wstring *name;
        uint32_t *index;
        uint32_t stage;
      } members[] = {
          {&group.closest_hit_name, &group.closest_hit, kOwnedRTStageClosestHit},
          {&group.any_hit_name, &group.any_hit, kOwnedRTStageAnyHit},
          {&group.intersection_name, &group.intersection, kOwnedRTStageIntersection},
      };
      for (auto &member : members) {
        *member.index = kOwnedRTNoExport;
        if (member.name->empty())
          continue;
        /* Exported or library (mangled) name; never an AddToStateObject parent's export. */
        size_t index = FindExportIndexByName(exports_, *member.name);
        if (index < first_local_export_)
          index = exports_.size();
        if (index == exports_.size()) {
          ERR("RT state object rejected: a hit group member ", Utf8Name(*member.name).c_str(),
              " is not an export of this state object");
          return E_INVALIDARG;
        }
        Export &component = exports_[index];
        if (component.hit_group || component.stage != member.stage) {
          ERR("RT state object rejected: hit group member ", Utf8Name(*member.name).c_str(), " has stage ",
              component.stage, " where stage ", member.stage, " is required");
          return E_INVALIDARG;
        }
        *member.index = static_cast<uint32_t>(index);
      }
      /* The group compiles as one function, out of its primary member's library and entry point. */
      const uint32_t primary = group.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE
          ? group.intersection
          : (group.closest_hit != kOwnedRTNoExport ? group.closest_hit : group.any_hit);
      if (primary == kOwnedRTNoExport) {
        ERR("RT state object rejected: hit group ", Utf8Name(group.name).c_str(), " resolved no shader to compile");
        return E_INVALIDARG;
      }
      group.entry_point = exports_[primary].entry_point;
      group.library_index = exports_[primary].library_index;
    }
    return S_OK;
  }

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(ID3D12StateObjectProperties)) {
      *object = static_cast<ID3D12StateObjectProperties *>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12StateObject)) {
      *object = static_cast<MTLD3D12StateObject *>(this);
      AddRef();
      return S_OK;
    }
    if (logQueryInterfaceError(__uuidof(ID3D12StateObject), riid))
      WARN("D3D12StateObject: unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  void *STDMETHODCALLTYPE GetShaderIdentifier(const WCHAR *name) override {
    const Export *entry = FindExport(name);
    /**
     * D3D12 returns null for an export that is not fully resolved in the state
     * object. A collection never resolves its exports, so its identifiers are null
     * rather than a zero filled record the application could put in a shader table.
     */
    if (!entry || !entry->compiled)
      return nullptr;
    return const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&entry->identifier));
  }

  /**
   * D3D12 reports per-shader stack sizes for raygen, miss and callable exports and, through
   * "hitGroup::closesthit|anyhit|intersection", for each member of a hit group; a hit group
   * itself, a hit-group member named directly, an unknown name or a shader that is not fully
   * resolved report 0xffffffff (a 32-bit value, so a sum of them stays detectable).
   *
   * The compiled shaders use no application-visible stack: TraceRay/CallShader state lives in
   * per-call-site invocation blocks of the caller's Metal frame, and recursion is bounded by the
   * pipeline's Metal max_call_stack_depth, not by SetPipelineStackSize. So every valid query is
   * exactly 0, and the default pipeline stack size computed from them is 0 as well.
   */
  UINT64 STDMETHODCALLTYPE GetShaderStackSize(const WCHAR *name) override {
    constexpr UINT64 kInvalid = 0xffffffffu;
    if (!name)
      return kInvalid;
    const std::wstring query = name;
    const size_t separator = query.find(L"::");
    const Export *entry = nullptr;
    if (separator == std::wstring::npos) {
      entry = FindExport(name);
      if (!entry || entry->hit_group)
        return kInvalid;
      switch (entry->stage) {
      case kOwnedRTStageRayGeneration:
      case kOwnedRTStageMiss:
      case kOwnedRTStageCallable:
        break;
      default:
        return kInvalid;
      }
    } else {
      const Export *group = FindExport(query.substr(0, separator).c_str());
      if (!group || !group->hit_group)
        return kInvalid;
      const std::wstring type = query.substr(separator + 2);
      const uint32_t member = type == L"closesthit" ? group->closest_hit
          : type == L"anyhit"                       ? group->any_hit
          : type == L"intersection"                 ? group->intersection
                                                    : kOwnedRTNoExport;
      entry = FindExport(member);
      if (!entry)
        return kInvalid;
    }
    /* A shader is fully resolved once it has its shader and pipeline configs. */
    if (!entry->has_shader_config || !entry->has_pipeline_config)
      return kInvalid;
    return 0;
  }

  UINT64 STDMETHODCALLTYPE GetPipelineStackSize() override { return is_pipeline_ ? pipeline_stack_size_ : 0; }

  /**
   * D3D12 drops the call on anything but a raytracing pipeline and for sizes >= 0xffffffff
   * (the likely sum of invalid GetShaderStackSize results), keeping the previous size.
   */
  void STDMETHODCALLTYPE SetPipelineStackSize(UINT64 size) override {
    if (!is_pipeline_ || size >= 0xffffffffu)
      return;
    pipeline_stack_size_ = size;
  }

  /** The export the application names `name`; hidden hit-group members have no public name. */
  const Export *FindExport(const WCHAR *name) const override {
    if (!name)
      return nullptr;
    auto found = export_indices_.find(name);
    return found == export_indices_.end() ? nullptr : &exports_[found->second];
  }

  const Export *FindExport(uint32_t index) const override {
    return index < exports_.size() ? &exports_[index] : nullptr;
  }

  const std::vector<Export> &GetExports() const override { return exports_; }
  MTLD3D12RootSignature *GetGlobalRootSignature() const override { return global_root_; }
  MTLD3D12RootSignature *GetDispatchRootSignature() const override { return dispatch_root_; }
  const D3D12_RAYTRACING_PIPELINE_CONFIG &GetPipelineConfig() const override { return pipeline_config_; }
  D3D12_RAYTRACING_PIPELINE_FLAGS GetPipelineFlags() const override { return pipeline_flags_; }
  uint32_t GetPipelineToken() const override { return pipeline_token_; }
  uint32_t GetMaxPayloadSize() const override { return max_payload_size_; }
  uint32_t GetMaxAttributeSize() const override { return max_attribute_size_; }
  WMT::Reference<WMT::ComputePipelineState> GetPipelineState() const override { return pipeline_state_; }
  uint32_t GetRayGenerationThreadgroupThreads() const override { return raygen_threadgroup_threads_; }
  WMT::Reference<WMT::IntersectionFunctionTable> GetIntersectionFunctionTable() const override {
    return intersection_table_;
  }
  /**
   * A table for one dispatch, with the same functions as the state object's own but
   * with the dispatch's argument block in its buffer argument table.
   *
   * The state object's table is shared and immutable, so a dispatch that needs the
   * callback to see per-dispatch constants cannot bind them into it. This uses an
   * equivalent table instead: the shared callbacks keep their fixed geometry slots,
   * and the dispatch arguments block is bound at SM50_RT_BINDING_INDEX_DISPATCH
   * together with the visible function table a hit shader reached from here calls
   * through. A reused table already holds this pipeline's callbacks.
   */
  bool BindDispatchIntersectionFunctionTable(
      WMT::Reference<WMT::IntersectionFunctionTable> &table, WMT::Buffer arguments, uint64_t arguments_offset,
      WMT::Reference<WMT::VisibleFunctionTable> visible_function_table
  ) override {
    if (intersection_function_handles_.empty() || !intersection_callback_slots_)
      return false;
    if (!table) {
      table = pipeline_state_.newIntersectionFunctionTable(intersection_callback_slots_);
      if (!table) {
        ERR("RT dispatch intersection function table creation failed");
        return false;
      }
      for (const auto &[slot, handle] : intersection_function_handles_)
        table.setFunction(handle, slot);
    }
    table.setBuffer(arguments, arguments_offset, SM50_RT_BINDING_INDEX_DISPATCH);
    if (visible_function_table)
      table.setVisibleFunctionTable(visible_function_table, SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE);
    return true;
  }
  WMT::Reference<WMT::VisibleFunctionTable> GetVisibleFunctionTable() const override {
    return visible_function_table_;
  }
};

/** Per device token source so identifiers are unique across state objects. */
uint32_t NextPipelineToken() {
  static std::atomic<uint32_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

HRESULT
CreateStateObject(MTLD3D12Device *device, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object) {
  if (desc && (uint32_t(desc->Type) == kStateObjectTypeExecutable || IsWorkGraphCollection(desc)))
    return CreateWorkGraphStateObject(device, desc, nullptr, riid, state_object);
  InitReturnPtr(state_object);
  if (!state_object)
    return E_POINTER;
  auto object = Com(new (std::nothrow) MTLD3D12StateObjectImpl(device, NextPipelineToken()));
  if (!object)
    return E_OUTOFMEMORY;
  HRESULT hr = object->Initialize(desc);
  if (FAILED(hr))
    return hr;
  return object->QueryInterface(riid, state_object);
}

HRESULT AddToStateObject(
    MTLD3D12Device *device, const D3D12_STATE_OBJECT_DESC *addition, ID3D12StateObject *parent, REFIID riid,
    void **state_object
) {
  if (addition && uint32_t(addition->Type) == kStateObjectTypeExecutable)
    return CreateWorkGraphStateObject(device, addition, parent, riid, state_object);
  InitReturnPtr(state_object);
  if (!state_object)
    return E_POINTER;
  if (!parent)
    return RejectStateObject("AddToStateObject has no state object to grow from");
  Com<ID3D12Device> parent_device;
  if (FAILED(parent->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&parent_device))) ||
      parent_device.ptr() != static_cast<ID3D12Device *>(device))
    return RejectStateObject("AddToStateObject grows a state object of a different device");
  auto *source = dynamic_cast<MTLD3D12StateObjectImpl *>(parent);
  if (!source)
    return RejectStateObject("AddToStateObject grows a state object this backend did not create");
  /* The child takes the parent's token in Initialize, so no fresh one is drawn here. */
  auto object = Com(new (std::nothrow) MTLD3D12StateObjectImpl(device, 0));
  if (!object)
    return E_OUTOFMEMORY;
  HRESULT hr = object->Initialize(addition, source);
  if (FAILED(hr))
    return hr;
  return object->QueryInterface(riid, state_object);
}

} // namespace d3dalloy
