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

#include "DXBCParser/DXBCUtils.h"
#include "airconv_public.h"
#include "airconv_thunks.h"
#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device_child.hpp"
#include "d3d12_pipeline.hpp"
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

namespace dxmt {
namespace {

struct LibraryExport {
  /** Public D3D12 export name of the entry point, from the library's RDAT. */
  std::wstring name;
  /** The library's own name for the entry point: `!dx.entryPoints` operand 1. */
  std::wstring entry_point;
  uint32_t stage = kOwnedRTStageInvalid;
};

struct Library {
  std::vector<uint8_t> bytecode;
  std::vector<LibraryExport> exports;
};

struct Association {
  UINT subobject_index = 0;
  std::vector<std::wstring> exports;
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
 * Append the static samplers of one root signature to the dispatch's static sampler
 * table, keeping the table's own order.
 *
 * Static samplers are pipeline state, not root arguments: the runtime binds one table
 * for the whole dispatch, and every root signature a dispatch can use has to define a
 * static sampler of a given register identically - that is what lets one slot serve all
 * of them (and what makes the union fit D3D12's static sampler budget). A register the
 * table already holds with a different definition is therefore rejected rather than one
 * of the two winning by precedence.
 *
 * The definitions are copied out of the blob they were read from, so the table does not
 * depend on the deserializer that produced them.
 *
 * `appended` is the number of definitions the signature added that the table did not
 * already hold, which is what tells the caller whether a signature defines anything of
 * its own.
 */
HRESULT AppendStaticSamplers(
    MTLD3D12RootSignature *root, std::vector<D3D12_STATIC_SAMPLER_DESC> &table, UINT *appended
) {
  *appended = 0;
  if (!root)
    return S_OK;
  const void *blob = nullptr;
  const UINT blob_size = root->GetBlob(&blob);
  const void *raw = nullptr;
  UINT raw_size = 0;
  if (!blob_size || !blob || FAILED(microsoft::DXBCGetRootSignature(blob, &raw, &raw_size)))
    return RejectStateObject("a root signature has no readable static sampler table");
  RootSignatureDeserializer deserializer;
  if (FAILED(deserializer.Deserialize(raw, raw_size)))
    return RejectStateObject("a root signature does not deserialize");
  const auto &desc = deserializer.desc_1_1_.Desc_1_1;
  for (UINT i = 0; i < desc.NumStaticSamplers; i++) {
    const auto &state = desc.pStaticSamplers[i];
    bool known = false;
    for (const auto &existing : table) {
      if (existing.RegisterSpace != state.RegisterSpace || existing.ShaderRegister != state.ShaderRegister)
        continue;
      if (std::memcmp(&existing, &state, sizeof(state)) != 0)
        return RejectStateObject(
            "a local root signature defines a static sampler of a register the pipeline "
            "already defines differently"
        );
      known = true;
      break;
    }
    if (known)
      continue;
    table.push_back(state);
    (*appended)++;
  }
  return S_OK;
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
    if (exports[i].name == name)
      return i;
  }
  /*
   * The library's own name is only a shader export's: a hit group borrows its
   * primary member's entry point once its members are resolved, and D3D12 names a
   * hit group by its own export name, so matching here would let a group answer for
   * the shader it was built from.
   */
  for (size_t i = 0; i < exports.size(); ++i) {
    if (!exports[i].hit_group && !exports[i].entry_point.empty() && exports[i].entry_point == name)
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

  std::vector<WMT::Reference<WMT::Library>> metal_libraries_;
  std::vector<WMT::Reference<WMT::Function>> metal_functions_;
  WMT::Reference<WMT::ComputePipelineState> pipeline_state_;
  WMT::Reference<WMT::IntersectionFunctionTable> intersection_table_;
  WMT::Reference<WMT::VisibleFunctionTable> visible_function_table_;
  WMT::Reference<WMT::ComputePipelineState> shader_table_prepare_pipeline_;
  /**
   * What the state object's intersection function table is built from, kept so a
   * dispatch can build its own table with the same functions but its own dispatch
   * arguments block (see CreateDispatchIntersectionFunctionTable). Hardware slots
   * select triangle or bounding-box callbacks; GPU SBT records select hit groups.
   */
  uint32_t intersection_callback_slots_ = 0;
  std::vector<std::pair<uint32_t, WMT::Reference<WMT::FunctionHandle>>> intersection_function_handles_;

  std::vector<raytracing::LocalRootLayout> local_root_layouts_;
  std::vector<raytracing::LocalRootCopy> local_root_copies_;
  uint32_t local_root_entry_stride_ = 0;

  HRESULT AddExport(Export entry) {
    if (entry.name.empty())
      return RejectStateObject("an export has an empty name");
    if (export_indices_.count(entry.name))
      return RejectStateObject("two exports share a name");
    export_indices_.emplace(entry.name, exports_.size());
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
      if (!blob_size || !blob || FAILED(microsoft::DXBCGetRootSignature(blob, &raw, &raw_size)))
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
      HRESULT hr = AppendStaticSamplers(entry.local_root_signature, static_samplers, &appended);
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

  /**
   * Collect the local root argument translation of every export.
   *
   * The application writes the local root arguments of a shader record using the
   * D3D12 layout (root constants 4 byte packed, descriptors and tables 8 byte
   * aligned).  The compiled traversal reads the existing qword root argument
   * layout of the root signature, so each record is materialized once per
   * dispatch by the shader table preparation pass.
   */
  HRESULT BuildLocalRootLayouts() {
    local_root_layouts_.assign(exports_.size(), {});
    std::unordered_map<const MTLD3D12RootSignature *, uint32_t> source_sizes;
    for (size_t i = 0; i < exports_.size(); ++i) {
      auto &entry = exports_[i];
      entry.local_root_data_size = 0;
      auto *root = entry.local_root_signature;
      if (!root) {
        continue;
      }
      auto known = source_sizes.find(root);
      if (known != source_sizes.end()) {
        entry.local_root_data_size = known->second;
      } else {
        entry.local_root_data_size = root->LocalRootDataSize;
        source_sizes.emplace(root, root->LocalRootDataSize);
      }
      local_root_layouts_[i].copy_offset = static_cast<uint32_t>(local_root_copies_.size());
      local_root_layouts_[i].copy_count = root->LocalRootCopyCount;
      local_root_layouts_[i].source_size = entry.local_root_data_size;
      local_root_copies_.insert(
          local_root_copies_.end(), root->LocalRootCopies, root->LocalRootCopies + root->LocalRootCopyCount
      );
      const uint32_t entry_bytes = std::max(root->UploadQwords * 8u, AlignUp(entry.local_root_data_size, 8));
      local_root_entry_stride_ = std::max(local_root_entry_stride_, entry_bytes);
    }
    local_root_entry_stride_ = AlignUp(std::max(local_root_entry_stride_, 8u), 8);
    return S_OK;
  }

  HRESULT CompilePipeline() {
    SM50_RT_PIPELINE_DESC desc{};
    std::vector<SM50_RT_LIBRARY> libraries;
    std::vector<SM50_RT_EXPORT> compiler_exports;
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
      std::string container_name;
      if (!WideToUtf8(entry.entry_point, container_name)) {
        ERR("RT export ", entry_names[i].c_str(), " has no usable library entry point name");
        return E_INVALIDARG;
      }
      entry_point_names[i] = std::move(container_name);

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
    desc.MetalVersion = SM50_SHADER_METAL_310;
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
    if (!SM50CompileRaytracingPipeline(
            &desc, &bitcode, &metallib, functions.data(), static_cast<uint32_t>(functions.size()), &function_count,
            &error
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

    WMT::Reference<WMT::Error> metal_error;
    auto library = device_->GetMTLDevice().newLibrary(
        WMT::MakeDispatchData(metallib.Data, metallib.Size), metal_error
    );
    if (!library) {
      ERR("RT metallib load failed: ", metal_error ? metal_error.description().getUTF8String() : "no Metal error");
      return E_FAIL;
    }
    metal_libraries_.push_back(library);

    std::vector<obj_handle_t> linked;
    linked.reserve(functions.size());
    metal_functions_.assign(functions.size(), {});
    obj_handle_t dispatch_entry_function = NULL_OBJECT_HANDLE;
    for (size_t i = 0; i < functions.size(); ++i) {
      const auto &function = functions[i];
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
        exports_[function.ExportIndex].metal_function = function.Name;
      }
      if ((function.Kind == SM50_RT_FUNCTION_RAYGEN && !function.IsDispatchEntry) ||
          function.Kind == SM50_RT_FUNCTION_INTERSECTION) {
        /*
         * Raygen and intersection dispatchers inline these export bodies; they
         * are not independent Metal entry points or function-table handles.
         */
        continue;
      }
      auto metal_function = library.newFunction(function.Name);
      if (!metal_function) {
        ERR("RT metallib function not found: ", function.Name);
        return E_FAIL;
      }
      if (function.IsDispatchEntry) {
        if (function.Kind != SM50_RT_FUNCTION_RAYGEN) {
          ERR("RT reflection marked a non-raygen function as the dispatch entry: ", function.Name);
          return E_FAIL;
        }
        if (dispatch_entry_function != NULL_OBJECT_HANDLE) {
          ERR("RT pipeline has more than one dispatch entry");
          return E_FAIL;
        }
        /*
         * The dispatch entry becomes the pipeline's compute function; it is part of
         * the pipeline on its own and must not appear in the linked function list.
         */
        dispatch_entry_function = metal_function;
        metal_functions_[i] = std::move(metal_function);
        continue;
      }
      linked.push_back(metal_function);
      metal_functions_[i] = std::move(metal_function);
    }
    if (dispatch_entry_function == NULL_OBJECT_HANDLE) {
      ERR("RT pipeline reflection has no dispatch entry");
      return E_FAIL;
    }
    for (const auto &entry : exports_) {
      if (!entry.compiled) {
        ERR("RT export was not compiled: ", entry.name.c_str());
        return E_FAIL;
      }
    }

    WMTComputePipelineInfo info;
    WMT::InitializeComputePipelineInfo(info);
    info.compute_function = dispatch_entry_function;
    info.support_indirect_command_buffers = false;
    info.max_call_stack_depth = std::max<uint32_t>(2, 2 * pipeline_config_.MaxTraceRecursionDepth + 2);
    pipeline_state_ = device_->GetMTLDevice().newComputePipelineStateWithLinkedFunctions(
        info, linked.data(), static_cast<uint32_t>(linked.size()), metal_error
    );
    if (!pipeline_state_) {
      ERR("RT pipeline creation failed: ",
          metal_error ? metal_error.description().getUTF8String() : "no Metal error");
      return E_FAIL;
    }

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
      auto handle = pipeline_state_.functionHandle(metal_functions_[i]);
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
    visible_function_table_ = pipeline_state_.newVisibleFunctionTable(static_cast<uint32_t>(exports_.size()) + 1);
    if (!visible_function_table_) {
      ERR("RT visible function table creation failed");
      return E_FAIL;
    }
    for (size_t i = 0; i < functions.size(); ++i) {
      const auto &function = functions[i];
      if (function.IsDispatchEntry || function.Kind != SM50_RT_FUNCTION_VISIBLE)
        continue;
      if (function.ExportIndex >= exports_.size()) {
        ERR("RT reflection returned an export index outside the export table: ", function.ExportIndex);
        return E_FAIL;
      }
      auto handle = pipeline_state_.functionHandle(metal_functions_[i]);
      if (!handle) {
        ERR("RT visible function handle creation failed: ", function.Name);
        return E_FAIL;
      }
      visible_function_table_.setFunction(handle, uint64_t(function.ExportIndex) + 1);
    }

    for (auto &entry : exports_) {
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

  HRESULT Initialize(const D3D12_STATE_OBJECT_DESC *desc) {
    if (!desc || (desc->NumSubobjects && !desc->pSubobjects) ||
        (desc->Type != D3D12_STATE_OBJECT_TYPE_COLLECTION &&
         desc->Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE))
      return RejectStateObject("the description or its state object type is not a raytracing pipeline");
    is_pipeline_ = desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;

    std::vector<Association> associations;
    std::unordered_map<UINT, MTLD3D12RootSignature *> local_roots;
    std::unordered_map<UINT, D3D12_RAYTRACING_SHADER_CONFIG> shader_configs;
    bool have_global_root = false;
    bool have_pipeline_config = false;
    bool have_state_config = false;
    bool have_node_mask = false;

    try {
      for (UINT i = 0; i < desc->NumSubobjects; ++i) {
        const auto &subobject = desc->pSubobjects[i];
        if (!subobject.pDesc) {
          ERR("RT state object rejected: subobject ", i, " of type ", uint32_t(subobject.Type), " has no description");
          return E_INVALIDARG;
        }
        switch (subobject.Type) {
        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
          if (have_state_config)
            return RejectStateObject("two state object configs");
          /**
           * ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS and
           * ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS describe exports that
           * resolve against libraries outside this state object, which the
           * whole-pipeline compiler cannot see; refusing is honest, whereas
           * accepting would silently drop a shader.
           */
          const auto &config = *static_cast<const D3D12_STATE_OBJECT_CONFIG *>(subobject.pDesc);
          if (config.Flags != D3D12_STATE_OBJECT_FLAG_NONE &&
              config.Flags != D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS) {
            ERR("RT state object rejected: state object config flags 0x", uint32_t(config.Flags),
                " need external library resolution");
            return E_NOTIMPL;
          }
          have_state_config = true;
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK: {
          if (have_node_mask || *static_cast<const UINT *>(subobject.pDesc) > 1)
            return RejectStateObject("a duplicate or out-of-range node mask");
          have_node_mask = true;
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
          if (have_global_root)
            return RejectStateObject("two global root signatures");
          auto root = *static_cast<ID3D12RootSignature *const *>(subobject.pDesc);
          HRESULT hr = RetainRoot(root, &global_root_);
          if (FAILED(hr))
            return hr;
          have_global_root = true;
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
          MTLD3D12RootSignature *root = nullptr;
          HRESULT hr = RetainRoot(*static_cast<ID3D12RootSignature *const *>(subobject.pDesc), &root);
          if (FAILED(hr))
            return hr;
          local_roots.emplace(i, root);
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
          const auto config = *static_cast<const D3D12_RAYTRACING_SHADER_CONFIG *>(subobject.pDesc);
          if (config.MaxAttributeSizeInBytes > D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES)
            return RejectStateObject("a shader config attribute size above D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES");
          shader_configs.emplace(i, config);
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
          if (have_pipeline_config)
            return RejectStateObject("two raytracing pipeline configs");
          pipeline_config_ = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG *>(subobject.pDesc);
          if (!pipeline_config_.MaxTraceRecursionDepth ||
              pipeline_config_.MaxTraceRecursionDepth > D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH)
            return RejectStateObject("a raytracing pipeline config recursion depth outside 1..D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH");
          have_pipeline_config = true;
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: {
          if (have_pipeline_config)
            return RejectStateObject("two raytracing pipeline configs");
          const auto &config = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1 *>(subobject.pDesc);
          pipeline_config_.MaxTraceRecursionDepth = config.MaxTraceRecursionDepth;
          if (!pipeline_config_.MaxTraceRecursionDepth ||
              pipeline_config_.MaxTraceRecursionDepth > D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH ||
              (config.Flags & ~(D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES |
                                D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_PROCEDURAL_PRIMITIVES)))
            return RejectStateObject("a raytracing pipeline config1 depth or flag outside its range");
          pipeline_flags_ = config.Flags;
          have_pipeline_config = true;
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
          const auto &library_desc = *static_cast<const D3D12_DXIL_LIBRARY_DESC *>(subobject.pDesc);
          if (!library_desc.DXILLibrary.pShaderBytecode || !library_desc.DXILLibrary.BytecodeLength ||
              (library_desc.NumExports && !library_desc.pExports))
            return RejectStateObject("a DXIL library subobject has no bytecode, or an export list with no entries");
          Library library;
          const auto *bytes = static_cast<const uint8_t *>(library_desc.DXILLibrary.pShaderBytecode);
          library.bytecode.assign(bytes, bytes + library_desc.DXILLibrary.BytecodeLength);
          std::vector<LibraryExport> reflected;
          HRESULT hr = EnumerateLibraryExports(library_desc.DXILLibrary, reflected);
          if (FAILED(hr))
            return hr;
          if (!library_desc.NumExports) {
            /* Publishing every export requires every export to be a raytracing shader. */
            for (const auto &reflected_export : reflected) {
              if (!IsRaytracingStage(reflected_export.stage)) {
                std::string name;
                WideToUtf8(reflected_export.name, name);
                ERR("RT state object rejected: library export ", name.c_str(), " has stage ",
                    reflected_export.stage, ", which is not a raytracing stage");
                return E_INVALIDARG;
              }
            }
            library.exports = std::move(reflected);
          } else {
            for (UINT j = 0; j < library_desc.NumExports; ++j) {
              const auto &source = library_desc.pExports[j];
              LibraryExport target;
              if (!CopyName(source.Name, target.name))
                return RejectStateObject("a DXIL library export has an empty name");
              /*
               * D3D12 names the library's own export in ExportToRename when that is
               * present and in Name otherwise, and either spelling is allowed there:
               * the container's entry point name is the mangled one, while the name
               * the library publishes is the unmangled one. Both are matched, so
               * neither spelling is silently unsupported.
               */
              const WCHAR *internal = source.ExportToRename ? source.ExportToRename : source.Name;
              const LibraryExport *match = nullptr;
              unsigned matches = 0;
              for (const auto &candidate : reflected) {
                if (candidate.name == internal || candidate.entry_point == internal) {
                  match = &candidate;
                  matches++;
                }
              }
              if (!matches) {
                std::string name;
                WideToUtf8(internal, name);
                ERR("RT state object rejected: the library does not declare entry point ", name.c_str());
                return E_INVALIDARG;
              }
              if (matches > 1) {
                /*
                 * Overloads share a public name, so an unmangled request can name
                 * more than one entry point. Picking one would silently compile the
                 * wrong shader; D3D12 answers this by requiring the mangled name.
                 */
                std::string name;
                WideToUtf8(internal, name);
                ERR("RT state object rejected: the name ", name.c_str(), " matches ", matches,
                    " entry points of the library, so the mangled name is required");
                return E_INVALIDARG;
              }
              if (!IsRaytracingStage(match->stage)) {
                std::string name;
                WideToUtf8(internal, name);
                ERR("RT state object rejected: library export ", name.c_str(), " has stage ", match->stage,
                    ", which is not a raytracing stage");
                return E_INVALIDARG;
              }
              target.entry_point = match->entry_point;
              target.stage = match->stage;
              library.exports.push_back(std::move(target));
            }
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
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
          const auto &collection = *static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(subobject.pDesc);
          if (!collection.pExistingCollection || (collection.NumExports && !collection.pExports))
            return RejectStateObject("an existing collection subobject has no collection, or an export list with no entries");
          Com<ID3D12Device> collection_device;
          if (FAILED(collection.pExistingCollection->GetDevice(
                  __uuidof(ID3D12Device), reinterpret_cast<void **>(&collection_device)
              )) ||
              collection_device.ptr() != static_cast<ID3D12Device *>(device_))
            return RejectStateObject("an existing collection belongs to a different device");
          auto *existing = dynamic_cast<MTLD3D12StateObjectImpl *>(collection.pExistingCollection);
          if (!existing)
            return RejectStateObject("an existing collection is not a state object this backend created");
          if (!collection.NumExports) {
            for (const auto &source : existing->GetExports()) {
              Export entry = source;
              entry.identifier = {};
              entry.compiled = false;
              entry.metal_function.clear();
              HRESULT hr = AddExport(std::move(entry));
              if (FAILED(hr))
                return hr;
            }
          } else {
            for (UINT j = 0; j < collection.NumExports; ++j) {
              const auto &rename = collection.pExports[j];
              std::wstring public_name;
              if (!CopyName(rename.Name, public_name))
                return RejectStateObject("a collection export has an empty name");
              const WCHAR *source_name = rename.ExportToRename ? rename.ExportToRename : rename.Name;
              const auto &collection_exports = existing->GetExports();
              const size_t source_index = FindExportIndexByName(collection_exports, source_name);
              if (source_index == collection_exports.size())
                return RejectStateObject("the collection does not hold the export being renamed");
              Export entry = collection_exports[source_index];
              entry.name = std::move(public_name);
              entry.identifier = {};
              entry.compiled = false;
              entry.metal_function.clear();
              HRESULT hr = AddExport(std::move(entry));
              if (FAILED(hr))
                return hr;
            }
          }
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
          const auto &hit = *static_cast<const D3D12_HIT_GROUP_DESC *>(subobject.pDesc);
          Export entry;
          if (!CopyName(hit.HitGroupExport, entry.name))
            return RejectStateObject("a hit group has an empty export name");
          if (hit.Type != D3D12_HIT_GROUP_TYPE_TRIANGLES &&
              hit.Type != D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE)
            return RejectStateObject("a hit group type is neither TRIANGLES nor PROCEDURAL_PRIMITIVE");
          entry.hit_group = true;
          entry.hit_group_type = hit.Type;
          if (hit.ClosestHitShaderImport)
            entry.closest_hit_name = hit.ClosestHitShaderImport;
          if (hit.AnyHitShaderImport)
            entry.any_hit_name = hit.AnyHitShaderImport;
          if (hit.IntersectionShaderImport)
            entry.intersection_name = hit.IntersectionShaderImport;
          if (hit.Type == D3D12_HIT_GROUP_TYPE_TRIANGLES && !entry.intersection_name.empty())
            return RejectStateObject("a triangle hit group names an intersection shader");
          if (hit.Type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE && entry.intersection_name.empty())
            return RejectStateObject("a procedural hit group names no intersection shader");
          /** A hit group needs at least one shader to run. */
          if (entry.closest_hit_name.empty() && entry.any_hit_name.empty() && entry.intersection_name.empty())
            return RejectStateObject("a hit group names no closest-hit, any-hit or intersection shader");
          HRESULT hr = AddExport(std::move(entry));
          if (FAILED(hr))
            return hr;
          break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
          const auto &source = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(subobject.pDesc);
          if (source.NumExports && !source.pExports)
            return RejectStateObject("an association has an export count but no export list");
          Association association;
          if (!FindSubobjectIndex(*desc, source.pSubobjectToAssociate, association.subobject_index))
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
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
          /**
           * This associates a subobject named inside the DXIL (for example
           * [shader("closesthit")] attributes carrying subobject names).  Resolving
           * it needs DXIL subobject reflection; treating the name as a runtime
           * subobject pointer would associate the wrong subobject.
           */
          return E_NOTIMPL;
        default:
          return E_NOTIMPL;
        }
      }
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }

    /** Resolve hit group imports against the export table. */
    for (auto &group : exports_) {
      if (!group.hit_group)
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
        if (member.name->empty())
          continue;
        /*
         * A hit group names its shaders the same way the library does, so either the
         * exported name or the library's own (mangled) entry point name is accepted.
         */
        const size_t index = FindExportIndexByName(exports_, *member.name);
        if (index == exports_.size()) {
          std::string name;
          WideToUtf8(*member.name, name);
          ERR("RT state object rejected: a hit group member ", name.c_str(), " is not an export of this state object");
          return E_INVALIDARG;
        }
        Export &component = exports_[index];
        if (component.hit_group || component.stage != member.stage) {
          std::string name;
          WideToUtf8(*member.name, name);
          ERR("RT state object rejected: hit group member ", name.c_str(), " has stage ", component.stage,
              " where stage ", member.stage, " is required");
          return E_INVALIDARG;
        }
        *member.index = static_cast<uint32_t>(index);
      }
      /**
       * The group compiles as one function: the shader that runs when nothing
       * rejects the hit. Its library and entry point are the primary member's.
       */
      const uint32_t primary = group.hit_group_type == D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE
          ? group.intersection
          : (group.closest_hit != kOwnedRTNoExport ? group.closest_hit : group.any_hit);
      if (primary == kOwnedRTNoExport) {
        std::string name;
        WideToUtf8(group.name, name);
        ERR("RT state object rejected: hit group ", name.c_str(), " resolved no shader to compile");
        return E_INVALIDARG;
      }
      group.entry_point = exports_[primary].entry_point;
      group.library_index = exports_[primary].library_index;
    }

    /** Apply subobject to exports associations. */
    for (const auto &association : associations) {
      auto local_root = local_roots.find(association.subobject_index);
      auto shader_config = shader_configs.find(association.subobject_index);
      if (local_root == local_roots.end() && shader_config == shader_configs.end())
        return RejectStateObject("an association points at a subobject that is neither a local root signature nor a shader config");
      auto apply = [&](Export &entry) -> HRESULT {
        if (local_root != local_roots.end()) {
          if (entry.local_root_signature && entry.local_root_signature != local_root->second)
            return RejectStateObject("two local root signatures are associated with the same export");
          entry.local_root_signature = local_root->second;
          return S_OK;
        }
        if (entry.has_shader_config)
          return RejectStateObject("two shader configs are associated with the same export");
        entry.shader_config = shader_config->second;
        entry.has_shader_config = true;
        return S_OK;
      };
      if (association.exports.empty()) {
        for (auto &entry : exports_) {
          HRESULT hr = apply(entry);
          if (FAILED(hr))
            return hr;
        }
      } else {
        for (const auto &name : association.exports) {
          const size_t index = FindExportIndexByName(exports_, name);
          if (index == exports_.size()) {
            std::string utf8;
            WideToUtf8(name, utf8);
            ERR("RT state object rejected: an association names ", utf8.c_str(), ", which is not an export of this state object");
            return E_INVALIDARG;
          }
          HRESULT hr = apply(exports_[index]);
          if (FAILED(hr))
            return hr;
        }
      }
    }

    /**
     * A shader configuration that no association names applies to every export.
     * More than one such default is ambiguous and is rejected rather than picked
     * by declaration order.
     */
    const D3D12_RAYTRACING_SHADER_CONFIG *default_shader_config = nullptr;
    for (const auto &[index, config] : shader_configs) {
      bool associated = false;
      for (const auto &association : associations)
        associated |= association.subobject_index == index;
      if (!associated) {
        if (default_shader_config)
          return RejectStateObject("two shader configs are associated with nothing, so neither is the default");
        default_shader_config = &config;
      }
    }
    if (default_shader_config) {
      for (auto &entry : exports_) {
        if (!entry.has_shader_config) {
          entry.shader_config = *default_shader_config;
          entry.has_shader_config = true;
        }
      }
    }

    /** A hit group shares the local root signature and configuration of its members. */
    for (auto &group : exports_) {
      if (!group.hit_group)
        continue;
      const uint32_t members[] = {group.closest_hit, group.any_hit, group.intersection};
      for (uint32_t index : members) {
        if (index == kOwnedRTNoExport)
          continue;
        Export &component = exports_[index];
        if (group.local_root_signature) {
          if (component.local_root_signature && component.local_root_signature != group.local_root_signature)
            return RejectStateObject("a hit group and one of its members hold different local root signatures");
          component.local_root_signature = group.local_root_signature;
        }
        if (group.has_shader_config) {
          if (component.has_shader_config &&
              (component.shader_config.MaxAttributeSizeInBytes != group.shader_config.MaxAttributeSizeInBytes ||
               component.shader_config.MaxPayloadSizeInBytes != group.shader_config.MaxPayloadSizeInBytes))
            return RejectStateObject("a hit group and one of its members hold different shader configs");
          component.shader_config = group.shader_config;
          component.has_shader_config = true;
        }
      }
    }

    if (!is_pipeline_)
      return S_OK;

    if (!have_pipeline_config)
      return RejectStateObject("a raytracing pipeline has no raytracing pipeline config");
    for (const auto &entry : exports_) {
      if (!entry.has_shader_config) {
        std::string name;
        WideToUtf8(entry.name, name);
        ERR("RT state object rejected: export ", name.c_str(), " has no raytracing shader config");
        return E_INVALIDARG;
      }
    }
    /**
     * DispatchRays selects a ray generation shader through the GPU SBT record.
     * The pipeline must contain at least one ray generation shader.
     */
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
    hr = BuildLocalRootLayouts();
    if (FAILED(hr))
      return hr;
    return CompilePipeline();
  }

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
   * No application visible stack: Metal owns the traversal state, so every export
   * that exists reports a zero sized shader stack and SetPipelineStackSize is
   * free to keep whatever the application asked for.
   */
  UINT64 STDMETHODCALLTYPE GetShaderStackSize(const WCHAR *name) override {
    return FindExport(name) ? 0 : UINT64_MAX;
  }

  UINT64 STDMETHODCALLTYPE GetPipelineStackSize() override { return pipeline_stack_size_; }

  void STDMETHODCALLTYPE SetPipelineStackSize(UINT64 size) override { pipeline_stack_size_ = size; }

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
  WMT::Reference<WMT::IntersectionFunctionTable> GetIntersectionFunctionTable() const override {
    return intersection_table_;
  }
  /**
   * A table for one dispatch, built from the same functions as the state object's
   * own but with the dispatch's argument block in its buffer argument table.
   *
   * The state object's table is shared and immutable, so a dispatch that needs the
   * callback to see per-dispatch constants cannot bind them into it. This builds
   * the equivalent table instead: the shared callbacks keep their fixed geometry
   * slots, and the dispatch arguments block is bound at
   * SM50_RT_BINDING_INDEX_DISPATCH together with the visible function table a hit
   * shader reached from here calls through.
   */
  WMT::Reference<WMT::IntersectionFunctionTable> CreateDispatchIntersectionFunctionTable(
      WMT::Buffer arguments, uint64_t arguments_offset,
      WMT::Reference<WMT::VisibleFunctionTable> visible_function_table
  ) override {
    if (intersection_function_handles_.empty() || !intersection_callback_slots_)
      return {};
    auto table = pipeline_state_.newIntersectionFunctionTable(intersection_callback_slots_);
    if (!table) {
      ERR("RT dispatch intersection function table creation failed");
      return {};
    }
    for (const auto &[slot, handle] : intersection_function_handles_)
      table.setFunction(handle, slot);
    table.setBuffer(arguments, arguments_offset, SM50_RT_BINDING_INDEX_DISPATCH);
    if (visible_function_table)
      table.setVisibleFunctionTable(visible_function_table, SM50_RT_BINDING_INDEX_VISIBLE_FUNCTION_TABLE);
    return table;
  }
  WMT::Reference<WMT::VisibleFunctionTable> GetVisibleFunctionTable() const override {
    return visible_function_table_;
  }

  WMT::Reference<WMT::ComputePipelineState> GetShaderTablePreparePipeline() override {
    if (shader_table_prepare_pipeline_)
      return shader_table_prepare_pipeline_;
    auto device = device_->GetMTLDevice();
    WMT::Reference<WMT::Error> error;
    auto library = device.newLibraryWithSource(
        std::string_view(raytracing::kShaderTablePrepareMSL, sizeof(raytracing::kShaderTablePrepareMSL) - 1), error
    );
    if (!library) {
      ERR("RT shader table preparation library failed: ",
          error ? error.description().getUTF8String() : "no Metal error");
      return {};
    }
    auto function = library.newFunction(raytracing::kShaderTablePrepareFunction);
    if (!function) {
      ERR("RT shader table preparation function missing");
      return {};
    }
    shader_table_prepare_pipeline_ = device.newComputePipelineState(function, error);
    if (!shader_table_prepare_pipeline_) {
      ERR("RT shader table preparation pipeline failed: ",
          error ? error.description().getUTF8String() : "no Metal error");
      return {};
    }
    return shader_table_prepare_pipeline_;
  }

  const std::vector<raytracing::LocalRootLayout> &GetLocalRootLayouts() const override {
    return local_root_layouts_;
  }
  const std::vector<raytracing::LocalRootCopy> &GetLocalRootCopies() const override {
    return local_root_copies_;
  }
  uint32_t GetLocalRootEntryStride() const override { return local_root_entry_stride_; }
};

/** Per device token source so identifiers are unique across state objects. */
uint32_t NextPipelineToken() {
  static std::atomic<uint32_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

HRESULT
CreateStateObject(MTLD3D12Device *device, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid, void **state_object) {
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

} // namespace dxmt
