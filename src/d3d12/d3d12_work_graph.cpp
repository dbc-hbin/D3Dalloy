#include "d3d12_work_graph.hpp"
#include "d3d12_work_graph_scheduler.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "d3d12_resource_contract.hpp"
#include "Metal4Pipeline.hpp"
#include "d3d12_dxil_subobjects.hpp"
#include "../airconv/dxbc_root_signature.hpp"
#include "DXBCParser/DXBCUtils.h"
#include "dxil_raytracing.h"
#include "d3dalloy_buffer.hpp"
#include "d3dalloy_command.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace d3dalloy {
WorkGraphDispatch::~WorkGraphDispatch() {
  if (atomic64_tables)
    for (const auto &table : *atomic64_tables) { table.rootsig->ReleasePrivate(); table.heap->ReleasePrivate(); }
}
namespace {
std::mutex registry_mutex;
std::unordered_map<uint64_t, MTLD3D12WorkGraph *> registry;
std::atomic<uint64_t> next_token{1};
constexpr uint64_t identifier_magic = 0x48504152474b5257ull;
HRESULT Reject(const char *reason) { ERR("Work graph rejected: ", reason); return E_INVALIDARG; }
std::wstring Wide(const char *name, uint32_t size) {
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, size, nullptr, 0);
  std::wstring result(length, L'\0');
  if (length) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, size, result.data(), length);
  return result;
}
bool SameRoot(MTLD3D12RootSignature *a, MTLD3D12RootSignature *b) {
  if (a == b) return true;
  if (!a || !b) return false;
  const void *left = nullptr, *right = nullptr;
  const UINT left_blob_size = a->GetBlob(&left), right_blob_size = b->GetBlob(&right);
  const void *left_payload = nullptr, *right_payload = nullptr; UINT left_size = 0, right_size = 0;
  return SUCCEEDED(microsoft::DXBCGetRootSignature(left, left_blob_size, &left_payload, &left_size)) &&
      SUCCEEDED(microsoft::DXBCGetRootSignature(right, right_blob_size, &right_payload, &right_size)) &&
      left_size == right_size && !std::memcmp(left_payload, right_payload, left_size);
}
HRESULT CreateSamplerUnionRoot(MTLD3D12Device *device, MTLD3D12RootSignature *root,
                              const std::vector<D3D12_STATIC_SAMPLER_DESC> &samplers, Com<MTLD3D12RootSignature> &result) {
  const void *blob_data = nullptr, *raw = nullptr; UINT raw_size = 0;
  const UINT blob_size = root->GetBlob(&blob_data);
  if (FAILED(microsoft::DXBCGetRootSignature(blob_data, blob_size, &raw, &raw_size))) return E_INVALIDARG;
  RootSignatureDeserializer deserializer;
  if (FAILED(deserializer.Deserialize(raw, raw_size))) return E_INVALIDARG;
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = deserializer.desc_1_1_;
  versioned.Desc_1_1.NumStaticSamplers = samplers.size(); versioned.Desc_1_1.pStaticSamplers = samplers.data();
  Com<ID3DBlob> blob;
  HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned, &blob, nullptr);
  if (FAILED(hr)) return hr;
  Com<ID3D12RootSignature> union_root;
  hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&union_root));
  if (FAILED(hr)) return hr;
  result = dynamic_cast<MTLD3D12RootSignature *>(union_root.ptr());
  return S_OK;
}
uint32_t FindNode(const std::vector<MTLD3D12WorkGraph::Node> &nodes, const std::wstring &name, uint32_t index) {
  for (uint32_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].name == name && nodes[i].info.NodeArrayIndex == index) return i;
  return SM50_WG_NONE;
}
}

MTLD3D12WorkGraph::MTLD3D12WorkGraph(MTLD3D12Device *device)
    : MTLD3D12DeviceChild(device), token_(next_token.fetch_add(1)) {}
MTLD3D12WorkGraph::~MTLD3D12WorkGraph() {
  std::lock_guard lock(registry_mutex);
  registry.erase(token_);
}
const MTLD3D12WorkGraph::Graph *MTLD3D12WorkGraph::GetGraph(uint32_t index) const {
  return index < graphs_.size() ? &graphs_[index] : nullptr;
}
MTLD3D12RootSignature *MTLD3D12WorkGraph::RootSignature(uint32_t index) const {
  const auto *graph = GetGraph(index);
  return graph ? graph->dispatch_root.ptr() : nullptr;
}
Com<MTLD3D12WorkGraph> MTLD3D12WorkGraph::Resolve(MTLD3D12Device *device, const D3D12_PROGRAM_IDENTIFIER &id,
                                              uint32_t &index) {
  std::lock_guard lock(registry_mutex);
  auto it = registry.find(id.OpaqueData[1]);
  if (id.OpaqueData[0] != identifier_magic || it == registry.end() || it->second->device_ != device ||
      id.OpaqueData[2] >= it->second->graphs_.size() || id.OpaqueData[3]) return {};
  index = uint32_t(id.OpaqueData[2]);
  return Com<MTLD3D12WorkGraph>(it->second);
}
HRESULT MTLD3D12WorkGraph::QueryInterface(REFIID riid, void **object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (riid == __uuidof(ID3D12StateObjectProperties1) || riid == __uuidof(ID3D12StateObjectProperties))
    *object = static_cast<ID3D12StateObjectProperties1 *>(this);
  else if (riid == __uuidof(ID3D12WorkGraphProperties)) *object = static_cast<ID3D12WorkGraphProperties *>(this);
  else if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
           riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12StateObject))
    *object = static_cast<ID3D12StateObject *>(this);
  else return E_NOINTERFACE;
  AddRef();
  return S_OK;
}
void *MTLD3D12WorkGraph::GetShaderIdentifier(LPCWSTR) { return nullptr; }
UINT64 MTLD3D12WorkGraph::GetShaderStackSize(LPCWSTR) { return 0xffffffffu; }
UINT64 MTLD3D12WorkGraph::GetPipelineStackSize() { return 0; }
void MTLD3D12WorkGraph::SetPipelineStackSize(UINT64) {}
D3D12_PROGRAM_IDENTIFIER MTLD3D12WorkGraph::Identifier(UINT index) const {
  return index < graphs_.size() && !graphs_[index].nodes.empty() ? D3D12_PROGRAM_IDENTIFIER{{identifier_magic, token_, index, 0}}
                               : D3D12_PROGRAM_IDENTIFIER{};
}
D3D12_NODE_ID MTLD3D12WorkGraph::NodeID(UINT graph, UINT node) const {
  auto *g = GetGraph(graph);
  return g && node < g->nodes.size() ? D3D12_NODE_ID{g->nodes[node].name.c_str(), g->nodes[node].info.NodeArrayIndex}
                                  : D3D12_NODE_ID{nullptr, 0};
}
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
D3D12_PROGRAM_IDENTIFIER *MTLD3D12WorkGraph::GetProgramIdentifier(D3D12_PROGRAM_IDENTIFIER *result, LPCWSTR name) {
  *result = Identifier(GetWorkGraphIndex(name)); return result;
}
D3D12_NODE_ID *MTLD3D12WorkGraph::GetNodeID(D3D12_NODE_ID *result, UINT graph, UINT node) {
  *result = NodeID(graph, node); return result;
}
D3D12_NODE_ID *MTLD3D12WorkGraph::GetEntrypointID(D3D12_NODE_ID *result, UINT graph, UINT entry) {
  auto *g = GetGraph(graph); *result = NodeID(graph, g && entry < g->entries.size() ? g->entries[entry] : SM50_WG_NONE);
  return result;
}
#else
D3D12_PROGRAM_IDENTIFIER MTLD3D12WorkGraph::GetProgramIdentifier(LPCWSTR name) { return Identifier(GetWorkGraphIndex(name)); }
D3D12_NODE_ID MTLD3D12WorkGraph::GetNodeID(UINT graph, UINT node) { return NodeID(graph, node); }
D3D12_NODE_ID MTLD3D12WorkGraph::GetEntrypointID(UINT graph, UINT entry) {
  auto *g = GetGraph(graph); return NodeID(graph, g && entry < g->entries.size() ? g->entries[entry] : SM50_WG_NONE);
}
#endif
UINT MTLD3D12WorkGraph::GetNumWorkGraphs() { return graphs_.size(); }
LPCWSTR MTLD3D12WorkGraph::GetProgramName(UINT graph) { auto *g = GetGraph(graph); return g ? g->name.c_str() : nullptr; }
UINT MTLD3D12WorkGraph::GetWorkGraphIndex(LPCWSTR name) {
  if (name) for (UINT i = 0; i < graphs_.size(); ++i) if (graphs_[i].name == name) return i;
  return SM50_WG_NONE;
}
UINT MTLD3D12WorkGraph::GetNumNodes(UINT graph) { auto *g = GetGraph(graph); return g ? g->nodes.size() : SM50_WG_NONE; }
UINT MTLD3D12WorkGraph::GetNodeIndex(UINT graph, D3D12_NODE_ID id) {
  auto *g = GetGraph(graph); return g && id.Name ? FindNode(g->nodes, id.Name, id.ArrayIndex) : SM50_WG_NONE;
}
UINT MTLD3D12WorkGraph::GetNodeLocalRootArgumentsTableIndex(UINT graph, UINT node) {
  auto *g = GetGraph(graph); return g && node < g->nodes.size() ? g->nodes[node].info.LocalRootArgumentsTableIndex : SM50_WG_NONE;
}
UINT MTLD3D12WorkGraph::GetNumEntrypoints(UINT graph) { auto *g = GetGraph(graph); return g ? g->entries.size() : SM50_WG_NONE; }
UINT MTLD3D12WorkGraph::GetEntrypointIndex(UINT graph, D3D12_NODE_ID id) {
  auto *g = GetGraph(graph); auto node = GetNodeIndex(graph, id);
  if (g) for (UINT i = 0; i < g->entries.size(); ++i) if (g->entries[i] == node) return i;
  return SM50_WG_NONE;
}
UINT MTLD3D12WorkGraph::GetEntrypointRecordSizeInBytes(UINT graph, UINT entry) {
  auto *g = GetGraph(graph); return g && entry < g->entries.size() ? (g->nodes[g->entries[entry]].info.Input.RecordSize + 3) & ~3u : SM50_WG_NONE;
}
UINT MTLD3D12WorkGraph::GetEntrypointRecordAlignmentInBytes(UINT graph, UINT entry) {
  auto *g = GetGraph(graph);
  if (!g || entry >= g->entries.size()) return SM50_WG_NONE;
  const auto &input = g->nodes[g->entries[entry]].info.Input;
  return input.RecordSize ? std::max(input.RecordAlignment, 4u) : 0;
}
void MTLD3D12WorkGraph::GetWorkGraphMemoryRequirements(UINT graph, D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *memory) {
  auto *g = GetGraph(graph); if (memory) *memory = g ? g->memory : D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS{};
}

HRESULT MTLD3D12WorkGraph::Initialize(const D3D12_STATE_OBJECT_DESC *desc, const MTLD3D12WorkGraph *parent) {
  if (!desc || (uint32_t(desc->Type) != kStateObjectTypeExecutable && desc->Type != D3D12_STATE_OBJECT_TYPE_COLLECTION) ||
      (desc->NumSubobjects && !desc->pSubobjects))
    return Reject("invalid executable description");
  is_collection_ = desc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION;
  if (parent) {
    if (parent->device_ != device_ || !(parent->flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS))
      return Reject("parent cannot be extended");
    if (!parent->latest_version_ || parent->latest_version_->load() != parent->token_) return Reject("only the latest executable version can be extended");
    libraries_ = parent->libraries_; available_ = parent->available_; graphs_ = parent->graphs_; root_exports_ = parent->root_exports_;
    global_root_ = parent->global_root_; latest_version_ = parent->latest_version_;
  }
  std::vector<const D3D12_WORK_GRAPH_DESC *> descriptions;
  std::vector<const D3D12_STATE_SUBOBJECT *> global_subobjects;
  std::vector<const D3D12_STATE_SUBOBJECT *> local_subobjects;
  std::vector<DxilSubobject> embedded_subobjects;
  std::vector<uint32_t> embedded_scopes;
  std::vector<bool> embedded_visible;
  std::vector<std::pair<size_t, std::wstring>> embedded_exports;
  const size_t first_new = available_.size();
  std::vector<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *> associations;
  std::vector<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *> named_associations;
  bool api_config = false;
  for (UINT i = 0; i < desc->NumSubobjects; ++i) {
    const auto &sub = desc->pSubobjects[i];
    if (!sub.pDesc) return Reject("null subobject description");
    switch (uint32_t(sub.Type)) {
    case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
      if (static_cast<const D3D12_NODE_MASK *>(sub.pDesc)->NodeMask & ~1u) return Reject("invalid single-device node mask");
      break;
    case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
      const auto &collection = *static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(sub.pDesc);
      auto *source = dynamic_cast<MTLD3D12WorkGraph *>(collection.pExistingCollection);
      if (is_collection_ || !source || !source->is_collection_ || source->device_ != device_ || (collection.NumExports && !collection.pExports))
        return Reject("invalid work graph collection");
      uint32_t library_base = libraries_.size();
      libraries_.insert(libraries_.end(), source->libraries_.begin(), source->libraries_.end());
      std::vector<bool> matched(collection.NumExports);
      for (const auto &available : source->available_) {
        Node node = available; node.library += library_base; node.scope = library_base + 1;
        node.collection_flags = source->flags_; node.from_collection = true;
        if (collection.NumExports && node.exported) {
          node.exported = false;
          for (UINT e = 0; e < collection.NumExports; ++e) {
            const auto &exp = collection.pExports[e];
            if (!exp.Name || exp.Flags) return Reject("invalid collection export");
            if (node.shader == (exp.ExportToRename ? exp.ExportToRename : exp.Name) ||
                Wide(node.entry.data(), node.entry.size()) == (exp.ExportToRename ? exp.ExportToRename : exp.Name)) {
              Node alias = node; alias.shader = exp.Name; alias.exported = true;
              available_.push_back(std::move(alias)); matched[e] = true;
            }
          }
        }
        available_.push_back(std::move(node));
      }
      for (const auto &root : source->root_exports_) {
        if (!collection.NumExports) { root_exports_.push_back(root); continue; }
        for (UINT e = 0; e < collection.NumExports; ++e) {
          const auto &exp = collection.pExports[e];
          if (!exp.Name || !exp.Name[0] || exp.Flags) return Reject("invalid collection root export");
          if (root.name == (exp.ExportToRename ? exp.ExportToRename : exp.Name)) {
            root_exports_.push_back({exp.Name, root.root}); matched[e] = true;
          }
        }
      }
      if (std::find(matched.begin(), matched.end(), false) != matched.end()) return Reject("unknown collection export");
      for (const auto &pending : source->pending_associations_) {
        RootAssociation association = pending;
        association.priority = pending.priority >= 0 ? pending.priority : (pending.scope ? 2 : 0) + !pending.explicit_association;
        association.scope = pending.scope ? pending.scope + library_base : library_base + 1;
        association.scope_end = pending.scope ? 0 : libraries_.size() + 1;
        association.exports.clear();
        for (const auto &name : pending.exports) {
          bool renamed = false;
          for (UINT e = 0; e < collection.NumExports; ++e)
            if (name == (collection.pExports[e].ExportToRename ? collection.pExports[e].ExportToRename : collection.pExports[e].Name)) {
              association.exports.emplace_back(collection.pExports[e].Name); renamed = true;
            }
          if (!renamed) association.exports.push_back(name);
        }
        pending_associations_.push_back(std::move(association));
      }
      break;
    }
    case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
      flags_ = static_cast<const D3D12_STATE_OBJECT_CONFIG *>(sub.pDesc)->Flags; api_config = true; break;
    case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
      auto *root = dynamic_cast<MTLD3D12RootSignature *>(static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(sub.pDesc)->pGlobalRootSignature);
      if (!root || root->IsLocal) return Reject("invalid global root signature");
      global_subobjects.push_back(&sub); break;
    }
    case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
      auto *root = dynamic_cast<MTLD3D12RootSignature *>(static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>(sub.pDesc)->pLocalRootSignature);
      if (!root || !root->IsLocal) return Reject("invalid local root signature");
      local_subobjects.push_back(&sub); break;
    }
    case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
      associations.push_back(static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(sub.pDesc)); break;
    case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
      named_associations.push_back(static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(sub.pDesc)); break;
    case kStateSubobjectTypeWorkGraph:
      descriptions.push_back(static_cast<const D3D12_WORK_GRAPH_DESC *>(sub.pDesc)); break;
    case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
      const auto &lib = *static_cast<const D3D12_DXIL_LIBRARY_DESC *>(sub.pDesc);
      if (!lib.DXILLibrary.pShaderBytecode || (lib.NumExports && !lib.pExports)) return Reject("invalid library");
      std::vector<DxilSubobject> embedded;
      HRESULT subobject_result = ReadDxilLibrarySubobjects(lib.DXILLibrary.pShaderBytecode, lib.DXILLibrary.BytecodeLength, embedded);
      if (FAILED(subobject_result)) return subobject_result;
      const size_t embedded_begin = embedded_subobjects.size();
      embedded_visible.insert(embedded_visible.end(), embedded.size(), !lib.NumExports);
      if (!lib.NumExports) for (size_t j = 0; j < embedded.size(); ++j) embedded_exports.emplace_back(embedded_begin + j, embedded[j].name);
      embedded_scopes.insert(embedded_scopes.end(), embedded.size(), libraries_.size() + 1);
      embedded_subobjects.insert(embedded_subobjects.end(), std::make_move_iterator(embedded.begin()), std::make_move_iterator(embedded.end()));
      uint32_t count = 0, output_count = 0; uint64_t names_size = 0; sm50_error_t error = nullptr;
      if (!SM50WGReflectLibrary(lib.DXILLibrary.pShaderBytecode, lib.DXILLibrary.BytecodeLength, nullptr, 0,
                              nullptr, 0, nullptr, 0, &count, &output_count, &names_size, &error)) {
        ERR("Work graph reflection: ", SM50GetErrorMessageString(error)); SM50FreeError(error); return E_INVALIDARG;
      }
      std::vector<SM50_WG_NODE_INFO> nodes(count);
      std::vector<SM50_WG_NODE_IO> outputs(std::max(output_count, 1u));
      std::vector<char> names(std::max<uint64_t>(names_size, 1));
      if (!SM50WGReflectLibrary(lib.DXILLibrary.pShaderBytecode, lib.DXILLibrary.BytecodeLength, nodes.data(), count,
                              outputs.data(), output_count, names.data(), names_size, &count, &output_count, &names_size, &error)) {
        SM50FreeError(error); return E_INVALIDARG;
      }
      const uint32_t library_index = libraries_.size();
      libraries_.push_back(device_->GetShaderArtifactCache().RetainBytecode(
          lib.DXILLibrary.pShaderBytecode, lib.DXILLibrary.BytecodeLength));
      std::vector<bool> matched(lib.NumExports);
      for (UINT e = 0; e < lib.NumExports; ++e) {
        const auto &exp = lib.pExports[e];
        if (!exp.Name || !exp.Name[0] || exp.Flags) return Reject("invalid library export");
        for (size_t j = embedded_begin; j < embedded_subobjects.size(); ++j)
          if (embedded_subobjects[j].name == (exp.ExportToRename ? exp.ExportToRename : exp.Name)) {
            embedded_visible[j] = true; embedded_exports.emplace_back(j, exp.Name); matched[e] = true;
          }
      }
      for (const auto &info : nodes) {
        Node node; node.info = info; node.library = library_index;
        node.entry.assign(names.data() + info.EntryPointOffset, info.EntryPointLength);
        node.shader = Wide(names.data() + info.NameOffset, info.NameLength); node.original_shader = node.shader;
        node.name = Wide(names.data() + info.NodeNameOffset, info.NodeNameLength);
        node.share_input = Wide(names.data() + info.ShareInputNameOffset, info.ShareInputNameLength);
        for (uint32_t o = 0; o < info.OutputCount; ++o) {
          const auto &io = outputs[info.OutputOffset + o];
          node.outputs.push_back({io, Wide(names.data() + io.NodeNameOffset, io.NodeNameLength)});
        }
        if (lib.NumExports) {
          node.exported = false;
          for (UINT e = 0; e < lib.NumExports; ++e) {
            const auto &exp = lib.pExports[e];
            if (!exp.Name || exp.Flags) return Reject("invalid export");
            if (node.shader == (exp.ExportToRename ? exp.ExportToRename : exp.Name) ||
                Wide(node.entry.data(), node.entry.size()) == (exp.ExportToRename ? exp.ExportToRename : exp.Name)) {
              Node alias = node; alias.shader = exp.Name; alias.exported = true;
              available_.push_back(std::move(alias)); matched[e] = true;
            }
          }
        }
        available_.push_back(std::move(node));
      }
      if (std::find(matched.begin(), matched.end(), false) != matched.end()) return Reject("unknown library export");
      break;
    }
    default: return Reject("unsupported executable subobject");
    }
  }
  std::unordered_map<std::wstring, Com<MTLD3D12RootSignature>> embedded_roots;
  for (const auto &root : root_exports_) if (!embedded_roots.emplace(root.name, root.root).second) return Reject("duplicate imported root export");
  std::vector<Com<MTLD3D12RootSignature>> embedded_root_objects(embedded_subobjects.size());
  std::vector<RootAssociation> rules = std::move(pending_associations_);
  for (size_t i = 0; i < embedded_subobjects.size(); ++i) {
    const auto &sub = embedded_subobjects[i];
    if (!embedded_visible[i]) continue;
    if (sub.kind == DxilSubobjectKind::StateObjectConfig && !api_config) flags_ = sub.state_object_flags;
    if (sub.kind != DxilSubobjectKind::GlobalRootSignature && sub.kind != DxilSubobjectKind::LocalRootSignature) continue;
    std::vector<uint8_t> container; HRESULT hr = WrapRootSignaturePayload(sub.root_signature.data(), sub.root_signature.size(), container);
    if (FAILED(hr)) return hr;
    Com<ID3D12RootSignature> root; hr = device_->CreateRootSignature(0, container.data(), container.size(), __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&root));
    if (FAILED(hr)) return hr;
    auto *native = dynamic_cast<MTLD3D12RootSignature *>(root.ptr());
    if (!native) return Reject("invalid embedded root signature");
    embedded_root_objects[i] = native;
    for (const auto &[index, name] : embedded_exports) if (index == i) {
      auto [existing, inserted] = embedded_roots.emplace(name, native);
      if (!inserted) {
        // AddToStateObject may redeclare an inherited non-shader definition,
        // but it cannot replace that definition or introduce a new collision.
        const bool inherited = parent && std::any_of(parent->root_exports_.begin(), parent->root_exports_.end(),
          [&](const RootExport &root) { return root.name == name && SameRoot(root.root.ptr(), native); });
        if (!inherited || !SameRoot(existing->second.ptr(), native)) return Reject("duplicate embedded root signature export");
      } else root_exports_.push_back({name, native});
    }
    bool associated = false;
    for (size_t j = 0; j < embedded_subobjects.size(); ++j) {
      const auto &a = embedded_subobjects[j];
      if (embedded_visible[j] && embedded_scopes[j] == embedded_scopes[i] && a.kind == DxilSubobjectKind::SubobjectToExportsAssociation &&
          a.associated_subobject == sub.name && !a.exports.empty()) associated = true;
    }
    if (!associated) rules.push_back({native, {}, {}, embedded_scopes[i], native->IsLocal, false});
  }
  for (const auto &root : root_exports_)
    for (const auto &node : available_)
      if (node.exported && node.shader == root.name) return Reject("function and root signature exports share a name");
  if (!global_root_) {
    D3D12_ROOT_SIGNATURE_DESC empty{}; Com<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&empty, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&global_root_)))) return E_FAIL;
  }
  auto api_roots = local_subobjects;
  api_roots.insert(api_roots.end(), global_subobjects.begin(), global_subobjects.end());
  for (auto *local : api_roots) {
    bool explicit_association = false;
    for (const auto *association : associations) if (association->pSubobjectToAssociate == local && association->NumExports)
      explicit_association = true;
    if (!explicit_association) {
      auto *root = local->Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE
          ? dynamic_cast<MTLD3D12RootSignature *>(static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>(local->pDesc)->pLocalRootSignature)
          : dynamic_cast<MTLD3D12RootSignature *>(static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(local->pDesc)->pGlobalRootSignature);
      rules.push_back({root, {}, {}, 0, root->IsLocal, false});
    }
  }
  for (size_t i = 0; i < embedded_subobjects.size(); ++i) {
    const auto &a = embedded_subobjects[i];
    if (!embedded_visible[i] || a.kind != DxilSubobjectKind::SubobjectToExportsAssociation) continue;
    RootAssociation rule{{}, a.associated_subobject, a.exports, embedded_scopes[i], false, !a.exports.empty()};
    for (size_t j = 0; j < embedded_subobjects.size(); ++j)
      if (embedded_scopes[j] == embedded_scopes[i] && embedded_subobjects[j].name == a.associated_subobject && embedded_root_objects[j]) {
        rule.root = embedded_root_objects[j]; rule.local = rule.root->IsLocal; break;
      }
    rules.push_back(std::move(rule));
  }
  for (const auto *a : named_associations) {
    if (!a->SubobjectToAssociate || (a->NumExports && !a->pExports)) return Reject("invalid named root association");
    RootAssociation rule; rule.root_name = a->SubobjectToAssociate; rule.explicit_association = a->NumExports != 0;
    for (UINT e = 0; e < a->NumExports; ++e) {
      if (!a->pExports[e]) return Reject("null associated export");
      rule.exports.emplace_back(a->pExports[e]);
    }
    rules.push_back(std::move(rule));
  }
  for (const auto *a : associations) {
    auto target = std::find(api_roots.begin(), api_roots.end(), a->pSubobjectToAssociate);
    if (target == api_roots.end() || (a->NumExports && !a->pExports)) return Reject("invalid root association target");
    RootAssociation rule; rule.explicit_association = a->NumExports != 0;
    rule.local = (*target)->Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    rule.root = rule.local
        ? dynamic_cast<MTLD3D12RootSignature *>(static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>((*target)->pDesc)->pLocalRootSignature)
        : dynamic_cast<MTLD3D12RootSignature *>(static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>((*target)->pDesc)->pGlobalRootSignature);
    for (UINT e = 0; e < a->NumExports; ++e) {
      if (!a->pExports[e]) return Reject("null associated export");
      rule.exports.emplace_back(a->pExports[e]);
    }
    rules.push_back(std::move(rule));
  }
  std::vector<std::array<int, 2>> levels(available_.size(), {4, 4});
  for (auto &rule : rules) {
    if (!rule.root) {
      auto root = embedded_roots.find(rule.root_name);
      if (root != embedded_roots.end()) { rule.root = root->second; rule.local = rule.root->IsLocal; }
      else {
        if (!is_collection_ || !(flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS)) return Reject("unresolved associated root");
        pending_associations_.push_back(std::move(rule)); continue;
      }
    }
    const int priority = rule.priority >= 0 ? rule.priority : (rule.scope ? 2 : 0) + !rule.explicit_association;
    bool unresolved = false;
    for (const auto &name : rule.exports)
      if (std::none_of(available_.begin() + first_new, available_.end(), [&](const Node &n) { return n.exported && n.shader == name; })) unresolved = true;
    if (unresolved && (!rule.scope || rule.priority >= 0)) {
      if (!is_collection_ || !(flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS)) return Reject("unresolved associated export");
      pending_associations_.push_back(rule);
    }
    for (size_t i = first_new; i < available_.size(); ++i) {
      auto &node = available_[i];
      const bool explicit_match = (node.exported && std::find(rule.exports.begin(), rule.exports.end(), node.shader) != rule.exports.end()) ||
          (rule.scope && node.library + 1 == rule.scope && std::find(rule.exports.begin(), rule.exports.end(), node.original_shader) != rule.exports.end());
      if (rule.scope_end && !rule.explicit_association && (node.library + 1 < rule.scope || node.library + 1 >= rule.scope_end)) continue;
      if (rule.explicit_association ? !explicit_match : (rule.scope && !rule.scope_end && node.library + 1 != rule.scope)) continue;
      auto &root = rule.local ? node.local_root : node.global_root;
      if (node.from_collection && root) {
        if (rule.explicit_association && !SameRoot(root.ptr(), rule.root.ptr())) return Reject("conflicting immutable collection association");
        continue;
      }
      auto &level = levels[i][rule.local];
      if (priority > level) continue;
      if (priority == level && root && !SameRoot(root.ptr(), rule.root.ptr())) return Reject("conflicting root associations");
      root = rule.root; level = priority;
    }
  }
  if (parent && !(flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS)) return Reject("addition does not opt in to state object additions");
  if (!parent && !is_collection_) latest_version_ = std::make_shared<std::atomic<uint64_t>>(token_);
  if (is_collection_ && !descriptions.empty()) return Reject("collection contains work graph definition");
  for (size_t i = first_new; i < available_.size(); ++i) {
    auto &node = available_[i];
    if (!node.exported) continue;
    for (size_t j = 0; j < i; ++j) if (available_[j].exported && available_[j].shader == node.shader) {
      auto existing_tail = std::string_view(available_[j].entry), tail = std::string_view(node.entry);
      auto existing_at = existing_tail.find("@@"), at = tail.find("@@");
      if (existing_at != std::string_view::npos) existing_tail.remove_prefix(existing_at);
      if (at != std::string_view::npos) tail.remove_prefix(at);
      if (available_[j].info.Launch || node.info.Launch || existing_at == std::string_view::npos ||
          at == std::string_view::npos || existing_tail == tail) return Reject("duplicate exported function signature");
    }
    if (is_collection_) {
      node.collection_flags = flags_;
      HRESULT hr = Compile(node, node.global_root ? node.global_root.ptr() : global_root_.ptr(), true);
      if (FAILED(hr)) return hr;
    }
  }
  for (auto *description : descriptions) {
    Graph graph;
    if (!description->ProgramName) return Reject("null program name");
    UINT index = GetWorkGraphIndex(description->ProgramName);
    if (index != SM50_WG_NONE && !parent) return Reject("duplicate program name");
    HRESULT hr = BuildGraph(*description, graph, index != SM50_WG_NONE ? &graphs_[index] : nullptr);
    if (FAILED(hr)) return hr;
    if (index != SM50_WG_NONE) graphs_[index] = std::move(graph);
    else graphs_.push_back(std::move(graph));
  }
  if (graphs_.empty() && uint32_t(desc->Type) == kStateObjectTypeExecutable) return Reject("no work graph programs");
  std::lock_guard lock(registry_mutex); registry.emplace(token_, this);
  return S_OK;
}

HRESULT MTLD3D12WorkGraph::BuildGraph(const D3D12_WORK_GRAPH_DESC &desc, Graph &graph, const Graph *parent) {
  if (!desc.ProgramName || (desc.Flags & ~kWorkGraphFlagIncludeAllAvailableNodes) ||
      (desc.NumEntrypoints && !desc.pEntrypoints) || (desc.NumExplicitlyDefinedNodes && !desc.pExplicitlyDefinedNodes))
    return Reject("invalid graph description");
  graph.name = desc.ProgramName;
  std::vector<Node> candidates;
  for (const auto &node : available_) if (node.exported && node.info.Launch) candidates.push_back(node);
  std::vector<uint32_t> explicitly_defined;
  if (parent) {
    graph.nodes = parent->nodes; graph.entries = parent->entries;
    for (const auto &node : parent->nodes) {
      auto index = FindNode(candidates, node.name, node.info.NodeArrayIndex);
      if (index == SM50_WG_NONE) candidates.push_back(node); else candidates[index] = node;
    }
  }
  for (UINT i = 0; i < desc.NumExplicitlyDefinedNodes; ++i) {
    const auto &definition = desc.pExplicitlyDefinedNodes[i];
    if (definition.NodeType != kNodeTypeShader || !definition.Shader.Shader) return Reject("invalid shader node");
    auto source = std::find_if(available_.begin(), available_.end(), [&](const Node &n) { return n.exported && n.info.Launch && n.shader == definition.Shader.Shader; });
    if (source == available_.end()) return Reject("unknown node shader");
    Node node = *source;
    const auto &s = definition.Shader;
    const UINT *local = nullptr; const BOOL *entry = nullptr; const D3D12_NODE_ID *name = nullptr, *share = nullptr;
    const D3D12_NODE_OUTPUT_OVERRIDES *outputs = nullptr; UINT output_count = 0;
    if (s.OverridesType == kNodeOverridesBroadcasting) {
      if (node.info.Launch != SM50_WG_LAUNCH_BROADCASTING || !s.pBroadcastingLaunchOverrides) return Reject("launch override mismatch");
      const auto &o = *s.pBroadcastingLaunchOverrides;
      local = o.pLocalRootArgumentsTableIndex; entry = o.pProgramEntry; name = o.pNewName; share = o.pShareInputOf;
      outputs = o.pOutputOverrides; output_count = o.NumOutputOverrides;
      if (o.pDispatchGrid) { std::copy_n(o.pDispatchGrid, 3, node.info.DispatchGrid); node.info.HasFixedDispatchGrid = 1; }
      if (o.pMaxDispatchGrid) std::copy_n(o.pMaxDispatchGrid, 3, node.info.MaxDispatchGrid);
    } else if (s.OverridesType != kNodeOverridesNone) {
      if (s.OverridesType > kNodeOverridesCommonCompute || !s.pCommonComputeNodeOverrides ||
          (s.OverridesType == kNodeOverridesCoalescing && node.info.Launch != SM50_WG_LAUNCH_COALESCING) ||
          (s.OverridesType == kNodeOverridesThread && node.info.Launch != SM50_WG_LAUNCH_THREAD)) return Reject("launch override mismatch");
      const auto &o = *s.pCommonComputeNodeOverrides;
      local = o.pLocalRootArgumentsTableIndex; entry = o.pProgramEntry; name = o.pNewName; share = o.pShareInputOf;
      outputs = o.pOutputOverrides; output_count = o.NumOutputOverrides;
    }
    if (local) node.info.LocalRootArgumentsTableIndex = *local;
    if (entry) { node.info.IsProgramEntry = *entry; node.entry_forbidden = !*entry; }
    if (name) { if (!name->Name) return Reject("null node name"); node.name = name->Name; node.info.NodeArrayIndex = name->ArrayIndex; }
    if (share) { node.share_input = share->Name ? share->Name : L"";
      node.info.HasShareInputOf = share->Name != nullptr; node.info.ShareInputArrayIndex = share->ArrayIndex; }
    if (output_count && !outputs) return Reject("null output overrides");
    for (UINT o = 0; o < output_count; ++o) {
      const auto &over = outputs[o];
      if (over.OutputIndex >= node.outputs.size() || (over.pMaxRecords && over.pMaxRecordsSharedWithOutputIndex))
        return Reject("invalid output override");
      auto &out = node.outputs[over.OutputIndex];
      if (over.pNewName) { if (!over.pNewName->Name) return Reject("null output name"); out.name = over.pNewName->Name;
        out.info.NodeArrayIndex = over.pNewName->ArrayIndex; }
      if (over.pAllowSparseNodes) out.info.AllowSparseNodes = *over.pAllowSparseNodes;
      if (over.pMaxRecords) { out.info.MaxRecords = *over.pMaxRecords; out.info.MaxRecordsSharedWith = SM50_WG_NONE; }
      if (over.pMaxRecordsSharedWithOutputIndex) { out.info.MaxRecords = 0; out.info.MaxRecordsSharedWith = *over.pMaxRecordsSharedWithOutputIndex; }
    }
    uint32_t existing = FindNode(candidates, node.name, node.info.NodeArrayIndex);
    if (parent && FindNode(parent->nodes, node.name, node.info.NodeArrayIndex) != SM50_WG_NONE)
      return Reject("an addition redefines an existing node");
    uint32_t defined_index = existing == SM50_WG_NONE ? candidates.size() : existing;
    if (std::find(explicitly_defined.begin(), explicitly_defined.end(), defined_index) != explicitly_defined.end())
      return Reject("duplicate explicit node ID");
    explicitly_defined.push_back(defined_index);
    if (existing == SM50_WG_NONE) candidates.push_back(std::move(node)); else candidates[existing] = std::move(node);
  }
  for (auto candidate : explicitly_defined) graph.nodes.push_back(candidates[candidate]);
  std::unordered_set<uint32_t> traversed;
  std::function<HRESULT(uint32_t)> add = [&](uint32_t candidate) -> HRESULT {
    if (candidate == SM50_WG_NONE) return Reject("unresolved node");
    const auto &node = candidates[candidate];
    if (!traversed.insert(candidate).second) return S_OK;
    if (FindNode(graph.nodes, node.name, node.info.NodeArrayIndex) == SM50_WG_NONE) graph.nodes.push_back(node);
    for (const auto &out : node.outputs) {
      uint32_t count = out.info.ArraySize;
      if (count == SM50_WG_NONE) {
        count = 0;
        for (const auto &n : candidates) if (n.name == out.name && n.info.NodeArrayIndex >= out.info.NodeArrayIndex)
          count = std::max(count, n.info.NodeArrayIndex - out.info.NodeArrayIndex + 1);
      }
      for (uint32_t j = 0; j < count; ++j) {
        auto target = FindNode(candidates, out.name, out.info.NodeArrayIndex + j);
        if (target == SM50_WG_NONE && out.info.AllowSparseNodes) continue;
        HRESULT hr = add(target); if (FAILED(hr)) return hr;
      }
    }
    for (uint32_t i = 0; i < candidates.size(); ++i) if (candidates[i].info.HasShareInputOf &&
        candidates[i].share_input == node.name && candidates[i].info.ShareInputArrayIndex == node.info.NodeArrayIndex) {
      HRESULT hr = add(i); if (FAILED(hr)) return hr;
    }
    return S_OK;
  };
  for (auto candidate : explicitly_defined) if (candidates[candidate].info.IsProgramEntry) {
    HRESULT hr = add(candidate); if (FAILED(hr)) return hr;
  }
  for (UINT i = 0; i < desc.NumEntrypoints; ++i) {
    auto id = desc.pEntrypoints[i]; if (!id.Name) return Reject("null entrypoint ID");
    HRESULT hr = add(FindNode(candidates, id.Name, id.ArrayIndex)); if (FAILED(hr)) return hr;
  }
  for (UINT i = 0; i < candidates.size(); ++i)
    if (desc.Flags & kWorkGraphFlagIncludeAllAvailableNodes) {
      HRESULT hr = add(i); if (FAILED(hr)) return hr;
    }
  if (graph.nodes.empty()) return S_OK;
  for (const auto &node : graph.nodes) if (node.global_root) {
    if (graph.global_root && !SameRoot(graph.global_root.ptr(), node.global_root.ptr())) return Reject("work graph nodes have inconsistent global roots");
    graph.global_root = node.global_root;
  }
  if (!graph.global_root) graph.global_root = global_root_;
  graph.dispatch_root = graph.global_root;
  std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
  UINT appended = 0;
  HRESULT sampler_result = AppendRootStaticSamplers(graph.global_root.ptr(), static_samplers, &appended);
  if (FAILED(sampler_result)) return sampler_result;
  const size_t global_sampler_count = static_samplers.size();
  for (const auto &node : graph.nodes) {
    sampler_result = AppendRootStaticSamplers(node.local_root.ptr(), static_samplers, &appended);
    if (FAILED(sampler_result)) return sampler_result;
  }
  if (static_samplers.size() != global_sampler_count) {
    sampler_result = CreateSamplerUnionRoot(device_, graph.global_root.ptr(), static_samplers, graph.dispatch_root);
    if (FAILED(sampler_result)) return sampler_result;
  }
  const uint32_t n = graph.nodes.size();
  graph.queue_capacity.assign(n, 1); graph.queue_stride.assign(n, 4);
  std::vector<uint32_t> incoming(n, 0);
  graph.sharing_nodes.resize(n);
  std::unordered_set<uint32_t> local_indices; uint32_t next_local = 0;
  for (const auto &node : graph.nodes) if (node.local_root && node.info.LocalRootArgumentsTableIndex != SM50_WG_NONE)
    local_indices.insert(node.info.LocalRootArgumentsTableIndex);
  for (uint32_t i = 0; i < n; ++i) {
    auto &node = graph.nodes[i];
    node.element_base.clear(); node.element_count.clear(); node.targets.clear();
    uint32_t next = 0;
    for (uint32_t o = 0; o < node.outputs.size(); ++o) {
      const auto &output = node.outputs[o];
      uint32_t budget = output.info.MaxRecords;
      if (output.info.MaxRecordsSharedWith != SM50_WG_NONE) {
        if (output.info.MaxRecordsSharedWith >= node.outputs.size() ||
            node.outputs[output.info.MaxRecordsSharedWith].info.MaxRecordsSharedWith != SM50_WG_NONE) return Reject("invalid shared output budget");
        budget = node.outputs[output.info.MaxRecordsSharedWith].info.MaxRecords;
      }
      uint32_t count = output.info.ArraySize;
      if (count == SM50_WG_NONE) {
        count = 0; for (const auto &target : graph.nodes) if (target.name == output.name && target.info.NodeArrayIndex >= output.info.NodeArrayIndex)
          count = std::max(count, target.info.NodeArrayIndex - output.info.NodeArrayIndex + 1);
      }
      node.element_base.push_back(next); node.element_count.push_back(count); next += count;
      for (uint32_t j = 0; j < count; ++j) {
        uint32_t target = FindNode(graph.nodes, output.name, output.info.NodeArrayIndex + j);
        node.targets.push_back(target);
        if (target == SM50_WG_NONE) { if (!output.info.AllowSparseNodes) return Reject("unresolved output"); continue; }
        if (target != i) ++incoming[target];
        auto &input = graph.nodes[target].info.Input;
        if (input.RecordSize != output.info.RecordSize) return Reject("output and input record sizes differ");
        if (budget > 0x10000000u || graph.queue_capacity[target] > 0x10000000u - budget) return Reject("output budget exceeds node limits");
        graph.queue_capacity[target] += budget;
        uint64_t stride = 0;
        if (!resource_contract::CheckedAlign(input.RecordSize, 16, stride) || stride > UINT32_MAX)
          return Reject("record stride exceeds node limits");
        graph.queue_stride[target] = std::max(graph.queue_stride[target], uint32_t(stride));
      }
    }
    if (std::find(node.targets.begin(), node.targets.end(), i) == node.targets.end()) node.info.MaxRecursionDepth = 0;
    if (node.local_root && node.info.LocalRootArgumentsTableIndex == SM50_WG_NONE) {
      while (local_indices.contains(next_local)) ++next_local;
      node.info.LocalRootArgumentsTableIndex = next_local; local_indices.insert(next_local++);
    }
    if (!node.local_root) node.info.LocalRootArgumentsTableIndex = SM50_WG_NONE;
  }
  for (uint32_t i = 0; i < n; ++i) {
    bool entry = graph.nodes[i].info.IsProgramEntry || ((desc.Flags & kWorkGraphFlagIncludeAllAvailableNodes) &&
        !incoming[i] && !graph.nodes[i].info.HasShareInputOf);
    for (UINT e = 0; e < desc.NumEntrypoints; ++e) if (graph.nodes[i].name == desc.pEntrypoints[e].Name &&
        graph.nodes[i].info.NodeArrayIndex == desc.pEntrypoints[e].ArrayIndex) entry = true;
    if ((entry && (graph.nodes[i].entry_forbidden || graph.nodes[i].info.HasShareInputOf)) ||
        (!incoming[i] && !entry && !graph.nodes[i].info.HasShareInputOf))
      return Reject("node is neither an entrypoint nor an output target");
    if (entry && !graph.nodes[i].info.HasShareInputOf &&
        std::find(graph.entries.begin(), graph.entries.end(), i) == graph.entries.end()) graph.entries.push_back(i);
  }
  for (uint32_t i = 0; i < n; ++i) if (graph.nodes[i].info.HasShareInputOf) {
    const auto &node = graph.nodes[i];
    uint32_t source = FindNode(graph.nodes, node.share_input, node.info.ShareInputArrayIndex);
    if (parent && i >= parent->nodes.size() && source < parent->nodes.size()) return Reject("addition shares input of an existing node");
    if (source == SM50_WG_NONE || source == i || graph.nodes[source].info.HasShareInputOf || incoming[i] ||
        (incoming[source] && std::find(graph.entries.begin(), graph.entries.end(), source) != graph.entries.end()) ||
        std::find(graph.entries.begin(), graph.entries.end(), i) != graph.entries.end()) return Reject("invalid input sharing topology");
    const auto &input = graph.nodes[source].info.Input;
    if (node.info.MaxRecursionDepth || graph.nodes[source].info.MaxRecursionDepth ||
        (node.info.Input.Flags & SM50_WG_IO_READ_WRITE) || (input.Flags & SM50_WG_IO_READ_WRITE) ||
        graph.sharing_nodes[source].size() >= 255) return Reject("invalid shared input node attributes");
    if (node.info.Input.RecordSize != input.RecordSize || node.info.Input.RecordAlignment != input.RecordAlignment)
      return Reject("shared input layouts differ");
    graph.sharing_nodes[source].push_back(i);
  }
  // Array entries form one graph vertex for depth/cycle rules, even when only
  // different regions of the array are targeted from otherwise unrelated paths.
  std::unordered_map<std::wstring_view, uint32_t> group_by_name;
  std::vector<uint32_t> group_index(n);
  std::vector<std::vector<uint32_t>> members;
  for (uint32_t i = 0; i < n; ++i) {
    auto [where, inserted] = group_by_name.emplace(graph.nodes[i].name, members.size());
    if (inserted) members.emplace_back();
    auto group = where->second; group_index[i] = group;
    if (!members[group].empty() && graph.nodes[members[group][0]].info.Launch != graph.nodes[i].info.Launch)
      return Reject("node array launch modes differ");
    members[group].push_back(i);
  }
  std::vector<uint32_t> recursion(members.size(), 0), depth(members.size(), 0), visiting(members.size(), 0);
  for (uint32_t i = 0; i < n; ++i)
    if (std::find(graph.nodes[i].targets.begin(), graph.nodes[i].targets.end(), i) != graph.nodes[i].targets.end()) {
      if (!graph.nodes[i].info.MaxRecursionDepth) return Reject("self-recursive node has no recursion bound");
      recursion[group_index[i]] = std::max(recursion[group_index[i]], graph.nodes[i].info.MaxRecursionDepth);
    }
  std::function<uint32_t(uint32_t)> measure = [&](uint32_t group) -> uint32_t {
    if (visiting[group]) return 33; if (depth[group]) return depth[group]; visiting[group] = 1;
    uint32_t child = 0;
    for (auto i : members[group]) {
      for (auto target : graph.nodes[i].targets) if (target != SM50_WG_NONE && target != i) {
        if (group_index[target] == group) return 33;
        child = std::max(child, measure(group_index[target]));
      }
      for (auto shared : graph.sharing_nodes[i]) child = std::max(child, measure(group_index[shared]) - 1);
    }
    visiting[group] = 0;
    return depth[group] = child + 1 + recursion[group];
  };
  for (uint32_t group = 0; group < members.size(); ++group) graph.depth = std::max(graph.depth, measure(group));
  if (graph.depth > kWorkGraphsMaxNodeDepth) return Reject("graph depth exceeds 32 or contains a cycle");
  graph.node_depth.assign(n, 0);
  std::function<void(uint32_t, uint32_t)> assign_depth = [&](uint32_t group, uint32_t level) {
    level += recursion[group];
    if (level <= graph.node_depth[members[group][0]]) return;
    for (auto i : members[group]) graph.node_depth[i] = level;
    for (auto i : members[group]) {
      for (auto target : graph.nodes[i].targets) if (target != SM50_WG_NONE && target != i)
        assign_depth(group_index[target], level + 1);
      for (auto shared : graph.sharing_nodes[i]) assign_depth(group_index[shared], level);
    }
  };
  for (auto entry : graph.entries) assign_depth(group_index[entry], 1);
  if (parent) for (uint32_t i = 0; i < parent->nodes.size(); ++i)
    if (graph.node_depth[i] > parent->node_depth[i]) return Reject("addition increases an existing node depth");
  graph.queue_offset.resize(n);
  uint64_t level_alignment = 8;
  for (uint32_t i = 0; i < n; ++i) {
    graph.queue_capacity[i] = std::bit_ceil(graph.queue_capacity[i]);
    const uint32_t record_alignment = graph.nodes[i].info.Input.RecordAlignment;
    const uint64_t alignment = std::max(record_alignment, 8u);
    uint64_t stride = 0, queue_bytes = 0;
    // A small unused queue must not misalign the next queue or recursion level.
    // Device i64 atomics require their payload to fit one aligned canonical cell.
    if ((record_alignment && !std::has_single_bit(record_alignment)) ||
        !resource_contract::CheckedAlign(graph.queue_stride[i], alignment, stride) || stride > UINT32_MAX ||
        !resource_contract::CheckedAlign(graph.level_bytes, alignment, graph.level_bytes) ||
        !resource_contract::CheckedMul(graph.queue_capacity[i], stride, queue_bytes))
      return Reject("invalid record queue layout");
    graph.queue_stride[i] = uint32_t(stride);
    graph.queue_offset[i] = graph.level_bytes;
    if (!resource_contract::CheckedAdd(graph.level_bytes, queue_bytes, graph.level_bytes))
      return Reject("record queue size overflow");
    level_alignment = std::max(level_alignment, alignment);
  }
  uint64_t backing_bytes = 0;
  if (!resource_contract::CheckedAlign(graph.level_bytes, level_alignment, graph.level_bytes) ||
      !resource_contract::CheckedMul(graph.level_bytes, graph.depth, backing_bytes))
    return Reject("record backing memory size overflow");
  graph.memory = {backing_bytes, backing_bytes, 8};
  for (auto &node : graph.nodes) { HRESULT hr = Compile(node, graph.dispatch_root.ptr()); if (FAILED(hr)) return hr; }
  return S_OK;
}

HRESULT MTLD3D12WorkGraph::Compile(Node &node, MTLD3D12RootSignature *root, bool validate_only) {
  Com<MTLD3D12RootSignature> validation_root;
  if (validate_only && node.local_root && node.local_root->NumStaticSamplers) {
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers; UINT appended = 0;
    HRESULT hr = AppendRootStaticSamplers(root, samplers, &appended);
    if (FAILED(hr)) return hr;
    const auto count = samplers.size();
    hr = AppendRootStaticSamplers(node.local_root.ptr(), samplers, &appended);
    if (FAILED(hr)) return hr;
    if (samplers.size() != count) {
      hr = CreateSamplerUnionRoot(device_, root, samplers, validation_root);
      if (FAILED(hr)) return hr;
      root = validation_root.ptr();
    }
  }
  const auto &library = libraries_[node.library];
  SM50_WG_NODE_COMPILE_DESC desc{}; desc.Library = library.data(); desc.LibrarySize = library.size();
  desc.EntryPoint = node.entry.c_str(); desc.FunctionName = "d3dalloy_work_graph_node";
  desc.GlobalRootSignatureSize = root->GetBlob(&desc.GlobalRootSignature);
  if (node.local_root) desc.LocalRootSignatureSize = node.local_root->GetBlob(&desc.LocalRootSignature);
  desc.MetalVersion = kD3D12ShaderMetalVersion; desc.Launch = node.info.Launch;
  desc.OutputCount = node.outputs.size(); desc.OutputElementBase = node.element_base.data(); desc.OutputElementCount = node.element_count.data();
  std::vector<SM50_WG_LIBRARY> libraries; libraries.reserve(libraries_.size());
  for (const auto &bytes : libraries_) libraries.push_back({bytes.data(), bytes.size()});
  std::vector<std::string> names; names.reserve(available_.size());
  std::vector<SM50_WG_EXPORT> exports; exports.reserve(available_.size());
  for (const auto &function : available_) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, function.shader.data(), function.shader.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return Reject("invalid UTF-16 export name");
    names.emplace_back(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, function.shader.data(), function.shader.size(), names.back().data(), size, nullptr, nullptr) != size)
      return Reject("invalid UTF-16 export name");
    if (function.shader == node.shader && function.entry == node.entry && function.library == node.library && function.exported)
      desc.EntryExportIndex = exports.size();
    SM50_WG_EXPORT exported{names.back().c_str(), function.entry.c_str(), function.library, function.scope, function.collection_flags, function.exported ? 0u : 1u};
    if (function.global_root) exported.GlobalRootSignatureSize = function.global_root->GetBlob(&exported.GlobalRootSignature);
    if (function.local_root) exported.LocalRootSignatureSize = function.local_root->GetBlob(&exported.LocalRootSignature);
    exports.push_back(exported);
  }
  desc.Libraries = libraries.data(); desc.LibraryCount = libraries.size();
  desc.Exports = exports.data(); desc.ExportCount = exports.size(); desc.EntryLibraryIndex = node.library;
  desc.Reserved = validate_only;
  desc.AllowUnresolved = validate_only && (flags_ & D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS);

  sm50_error_t error = nullptr; sm50_bitcode_t bitcode = nullptr; SM50_COMPILED_BITCODE metallib{};
  SM50_WG_NODE_COMPILED_INFO info{};
  std::shared_ptr<const WorkGraphArtifact> artifact;
  if (!device_->GetShaderArtifactCache().CompileWorkGraph(desc, &bitcode, metallib, info, &error, artifact, validate_only)) {
    ERR("Work graph node ", node.entry, ": ", error ? SM50GetErrorMessageString(error) : "compile failed");
    SM50FreeError(error); return E_INVALIDARG;
  }
  if (validate_only) { if (bitcode) SM50DestroyBitcode(bitcode); return S_OK; }
  node.compiled_artifact = artifact;
  WMT::Reference<WMT::Error> metal_error;
  HRESULT constant_result = node.constants.Initialize(device_, metallib, bitcode);
  if (FAILED(constant_result)) { SM50DestroyBitcode(bitcode); return constant_result; }
  auto metal_library = device_->GetMetal4Compiler().newLibrary(WMT::MakeDispatchData(metallib.Data, metallib.Size), metal_error);
  if (metal_library) {
    auto function = metal_library.newFunction(desc.FunctionName);
    if (function) node.pipeline = device_->GetMetal4Compiler().newComputePipelineState(function, metal_error);
  }
  if (bitcode) SM50DestroyBitcode(bitcode);
  if (!node.pipeline) {
    ERR("Work graph pipeline: ", metal_error ? metal_error.description().getUTF8String() : "no Metal error"); return E_FAIL;
  }
  node.threads = {info.ThreadgroupSize[0], info.ThreadgroupSize[1], info.ThreadgroupSize[2]};
  node.atomic64_phase = info.HasAtomic64 != 0;
  return S_OK;
}

bool IsWorkGraphCollection(const D3D12_STATE_OBJECT_DESC *desc) {
  if (!desc || desc->Type != D3D12_STATE_OBJECT_TYPE_COLLECTION || (desc->NumSubobjects && !desc->pSubobjects)) return false;
  /* Generic function-only collections share this immutable namespace with both WG and DXR consumers. */
  for (UINT i = 0; i < desc->NumSubobjects; ++i) {
    const auto &sub = desc->pSubobjects[i];
    if (sub.Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !sub.pDesc) continue;
    const auto &library = *static_cast<const D3D12_DXIL_LIBRARY_DESC *>(sub.pDesc);
    uint32_t count = 0; uint64_t names = 0; sm50_error_t error = nullptr;
    SM50RTEnumerateLibraryExports(library.DXILLibrary.pShaderBytecode, library.DXILLibrary.BytecodeLength,
        nullptr, 0, nullptr, 0, &count, &names, &error);
    if (error) SM50FreeError(error);
    if (!count) continue;
    std::vector<sm50_rt_library_export_info> reflected(count); std::vector<char> strings(names);
    if (!SM50RTEnumerateLibraryExports(library.DXILLibrary.pShaderBytecode, library.DXILLibrary.BytecodeLength,
        reflected.data(), count, strings.data(), names, &count, &names, &error)) {
      if (error) SM50FreeError(error);
      return false;
    }
    if (std::any_of(reflected.begin(), reflected.end(), [](const auto &entry) { return entry.stage >= 7 && entry.stage <= 12; })) return false;
  }
  for (UINT i = 0; i < desc->NumSubobjects; ++i) {
    const auto &sub = desc->pSubobjects[i]; if (!sub.pDesc) continue;
    if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION && dynamic_cast<MTLD3D12WorkGraph *>(
        static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(sub.pDesc)->pExistingCollection)) return true;
    if (sub.Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) continue;
    const auto &library = *static_cast<const D3D12_DXIL_LIBRARY_DESC *>(sub.pDesc);
    uint32_t nodes = 0, outputs = 0; uint64_t names = 0; sm50_error_t error = nullptr;
    bool valid = SM50WGReflectLibrary(library.DXILLibrary.pShaderBytecode, library.DXILLibrary.BytecodeLength,
        nullptr, 0, nullptr, 0, nullptr, 0, &nodes, &outputs, &names, &error);
    if (error) SM50FreeError(error);
    if (valid && nodes) return true;
  }
  return false;
}

HRESULT CreateWorkGraphStateObject(MTLD3D12Device *device, const D3D12_STATE_OBJECT_DESC *desc,
                                  ID3D12StateObject *parent, REFIID riid, void **result) {
  if (!result) return E_POINTER; *result = nullptr;
  auto *source = parent ? dynamic_cast<MTLD3D12WorkGraph *>(parent) : nullptr;
  if (parent && !source) return Reject("parent is not a work graph executable");
  auto object = Com(new MTLD3D12WorkGraph(device));
  HRESULT hr = object->Initialize(desc, source);
  if (FAILED(hr)) return hr;
  hr = object->QueryInterface(riid, result);
  if (FAILED(hr)) return hr;
  if (source) {
    uint64_t expected = source->token_;
    if (!source->latest_version_->compare_exchange_strong(expected, object->token_)) {
      object->Release(); *result = nullptr;
      return Reject("executable was extended concurrently");
    }
  }
  return S_OK;
}

namespace {
static_assert(sizeof(WorkGraphSchedulerQueue) == sizeof(SM50_WG_QUEUE));
static_assert(sizeof(WorkGraphSchedulerBatch) == sizeof(SM50_WG_BATCH_RECORD));
static_assert(sizeof(WorkGraphSchedulerContext) == offsetof(SM50_WG_NODE_CONTEXT, Outputs));
static_assert(offsetof(WorkGraphSchedulerContext, GroupStart) == offsetof(SM50_WG_NODE_CONTEXT, GroupStart));
struct GraphExecution {
  MTLD3D12Device *device;
  WMT::MTL4CommandQueue queue;
  const WorkGraphDispatch &dispatch;
  const MTLD3D12WorkGraph::Graph &graph;
  const WorkGraphTexturePublisher &textures;
  std::shared_ptr<Atomic64PhaseCache> &atomic_cache;
  WMT::Reference<WMT::Buffer> control, readback, seed_resource;
  uint64_t seed_address = 0, seed_length = 0;
  WMT::Reference<WMT::MTL4CommandAllocator> allocator;
  WMTBufferInfo control_info{}, readback_info{};
  uint64_t control_stride = 0, context_offset = 0, batch_offset = 0, sharing_offset = 0;
  GPUExecutionGate::Ticket execution_ticket;
  std::optional<GPUExecutionGate::EncodingAdmission> encoding_admission;
  WMT::Reference<WMT::Fence> atomic_fence;
  std::shared_ptr<Atomic64Phase> atomic_phase;
  struct NodeBindings {
    bool host = false;
    std::vector<Atomic64RootBinding> roots;
    std::vector<StandardSwizzleTableUse> tables;
  };
  std::vector<NodeBindings> node_bindings;
  WMT::Reference<WMT::Buffer> frontier;
  WMTBufferInfo frontier_info{};
  WMT::ComputePipelineState frontier_pipeline;
  Rc<BufferAllocation> backing_owner;
  uint64_t params_offset = 0, seeds_offset = 0, indirect_offset = 0;
  bool use_frontier = false;

  WMT::Reference<WMT::MTL4CommandBuffer> Begin(bool exclusive = false) {
    execution_ticket = exclusive ? device->gpu_execution_gate.AcquireExclusive() : device->gpu_execution_gate.AcquireShared();
    if (!execution_ticket) return {};
    encoding_admission.emplace(device->gpu_execution_gate);
    auto buffer = device->GetMTLDevice().newMTL4CommandBuffer();
    if (buffer) {
      buffer.begin(allocator);
      buffer.useResidencySet(device->GetGlobalResidencySet());
    }
    return buffer;
  }
  bool Complete(WMT::MTL4CommandBuffer buffer) {
    buffer.end();
    auto submission = device->CommitMetal4CommandBuffers(queue, &buffer, 1);
    if (!submission) { encoding_admission.reset(); execution_ticket.Reset(); return false; }
    submission.waitUntilCompleted();
    const bool success = !submission.error();
    allocator.reset();
    if (success && atomic_phase) atomic_phase->MarkRetired();
    atomic_phase.reset();
    encoding_admission.reset();
    execution_ticket.Reset();
    return success;
  }
  Rc<BufferAllocation> ResolveRange(uint64_t address, uint64_t size, uint64_t &offset) {
    auto allocation = device->LookupBufferByVA(address, &offset);
    if (!allocation || offset > allocation->byteLength() || size > allocation->byteLength() - offset)
      return nullptr;
    return allocation;
  }
  HRESULT Read(uint64_t address, void *destination, uint64_t size, const char *kind) {
    if (!size) return S_OK;
    uint64_t offset = 0;
    auto allocation = ResolveRange(address, size, offset);
    if (!allocation) return E_INVALIDARG;
    if (size > readback_info.length) {
      readback_info = {}; readback_info.length = (size + 4095) & ~uint64_t(4095);
      readback_info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE; readback = device->GetMTLDevice().newBuffer(readback_info);
      if (!readback) return E_OUTOFMEMORY;
    }
    auto buffer = Begin();
    if (!buffer) return E_OUTOFMEMORY;
    auto encoder = buffer.blitCommandEncoder();
    wmtcmd_blit_copy_from_buffer_to_buffer copy{}; copy.type = WMTBlitCommandCopyFromBufferToBuffer;
    copy.src = allocation->buffer(); copy.src_offset = offset; copy.dst = readback; copy.dst_offset = 0; copy.copy_length = size;
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy)); encoder.endEncoding();
    if (!Complete(buffer)) return E_FAIL;
    std::memcpy(destination, readback_info.memory.get_accessible_or_null(), size);
    owned_graphics::Trace("work_graph_readback", ",\"kind\":\"", kind, "\",\"bytes\":", size);
    return S_OK;
  }
  void Bind(WMT::MTL4ComputeCommandEncoder encoder, WMT::Buffer buffer, uint64_t offset, uint8_t index) {
    wmtcmd_compute_setbuffer command{}; command.type = WMTComputeCommandSetBuffer;
    command.buffer = buffer; command.offset = offset; command.index = index;
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&command));
  }
  void Use(WMT::MTL4ComputeCommandEncoder encoder, WMT::Resource resource) {
    wmtcmd_compute_useresource command{}; command.type = WMTComputeCommandUseResource;
    command.resource = resource; command.usage = WMTResourceUsage(WMTResourceUsageRead | WMTResourceUsageWrite);
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&command));
  }
  HRESULT RunBatch(uint32_t index, uint64_t input_address, uint32_t level, uint32_t batch_groups, bool host,
                   WMT::MTL4CommandBuffer pending = {}) {
    const auto &node = graph.nodes[index];
    const uint64_t base = level * control_stride;
    const uint64_t active_context = host ? context_offset : base + context_offset;
    auto *words = static_cast<uint8_t *>(control_info.memory.get_accessible_or_null());
    const auto *context = reinterpret_cast<const SM50_WG_NODE_CONTEXT *>(words + active_context);
    const auto *queues = reinterpret_cast<const SM50_WG_QUEUE *>(words + base);
    const auto &local_roots = node_bindings[index].roots;
    const auto &local_tables = node_bindings[index].tables;
    WMT::Reference<WMT::MTL4CommandBuffer> owned_buffer;
    if (!pending) owned_buffer = Begin(node.atomic64_phase);
    WMT::MTL4CommandBuffer buffer = pending ? pending : WMT::MTL4CommandBuffer(owned_buffer);
    if (!buffer) return E_OUTOFMEMORY;
    const auto global_tables = dispatch.atomic64_tables
        ? std::span<const StandardSwizzleTableUse>(*dispatch.atomic64_tables)
        : std::span<const StandardSwizzleTableUse>{};
    HRESULT texture_result = pending ? S_FALSE : textures.begin(buffer, atomic_fence, global_tables, local_tables);
    if (FAILED(texture_result)) return texture_result;
    const bool texture_phase = texture_result == S_OK;
    if (node.atomic64_phase) {
      Atomic64PhaseBindings bindings;
      HRESULT hr = CollectAtomic64PhaseBindings(device, dispatch.atomic64_tables
          ? std::span<const StandardSwizzleTableUse>(*dispatch.atomic64_tables) : std::span<const StandardSwizzleTableUse>{},
          dispatch.atomic64_roots, bindings);
      if (SUCCEEDED(hr)) hr = CollectAtomic64PhaseBindings(device, local_tables, local_roots, bindings);
      if (SUCCEEDED(hr)) {
        // Record data alone participates in the immutable phase. Queue tails,
        // batch headers and the shared-input counter remain native 32-bit state.
        std::vector<Atomic64RootBinding> payloads;
        if (node.info.Input.RecordSize) {
          if (seed_resource && input_address >= seed_address && input_address - seed_address <= seed_length &&
                node.info.Input.RecordSize <= seed_length - (input_address - seed_address)) {
            Atomic64BufferBinding seed{};
            seed.nativeBuffer = seed_resource;
            seed.nativeAddress = seed_address;
            seed.nativeLength = seed_length;
            seed.byteOffset = input_address - seed_address;
            seed.byteLength = node.info.Input.RecordSize;
            seed.writable = true;
            bindings.buffers.push_back(std::move(seed));
          } else payloads.push_back({input_address, node.info.Input.RecordSize, true});
        }
        for (uint32_t target = 0; target < graph.nodes.size(); ++target) {
          if (!graph.nodes[target].info.Input.RecordSize ||
              std::find(node.targets.begin(), node.targets.end(), target) == node.targets.end()) continue;
          payloads.push_back({queues[target].Records, uint64_t(graph.queue_capacity[target]) * queues[target].Stride, true});
        }
        hr = CollectAtomic64PhaseBindings(device, {}, payloads, bindings);
      }
      if (SUCCEEDED(hr)) hr = PrepareAtomic64Phase(device, uint64_t(node.threads.width) * node.threads.height * node.threads.depth * batch_groups,
                                                 bindings, atomic_phase, atomic_cache);
      if (SUCCEEDED(hr)) hr = atomic_phase->EncodeImport(buffer, atomic_fence);
      if (SUCCEEDED(hr)) hr = atomic_phase->PublishContext(buffer, atomic_fence);
      if (FAILED(hr)) return hr;
    }
    auto encoder = buffer.computeCommandEncoder(false);
    if (node.atomic64_phase || texture_phase || (pending && level)) encoder.waitForFence(atomic_fence);
    Use(encoder, control);
    if (seed_resource) Use(encoder, seed_resource);
    wmtcmd_compute_setpso pipeline{}; pipeline.type = WMTComputeCommandSetPSO;
    pipeline.pso = node.pipeline; pipeline.threadgroup_size = node.threads;
    Bind(encoder, control, active_context, SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT);
    Bind(encoder, dispatch.arguments, dispatch.roots, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
    Bind(encoder, dispatch.arguments, dispatch.samplers, SM50_BINDING_INDEX_STATIC_SAMPLERS);
    if (node.constants.buffer) Bind(encoder, node.constants.buffer, node.constants.offset, SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
    wmtcmd_compute_dispatch launch{}; launch.type = WMTComputeCommandDispatch; launch.size = {batch_groups, 1, 1};
    pipeline.next.set(&launch);
    encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pipeline));
    if (node.atomic64_phase || texture_phase || pending) encoder.updateFence(atomic_fence);
    encoder.endEncoding();
    if (node.atomic64_phase) {
      HRESULT hr = atomic_phase->EncodeExport(buffer, atomic_fence);
      if (FAILED(hr)) return hr;
    }
    if (texture_phase) {
      HRESULT hr = textures.end(buffer, atomic_fence);
      if (FAILED(hr)) return hr;
    }
    if (node.atomic64_phase) {
      HRESULT hr = atomic_phase->ClearContext(buffer, atomic_fence);
      if (FAILED(hr)) return hr;
    }
    if (!pending && !Complete(buffer)) return E_FAIL;
    if (host) {
      owned_graphics::Trace("work_graph_host_batch", ",\"node\":", index,
          ",\"groups\":", batch_groups, ",\"atomic64\":", unsigned(node.atomic64_phase),
          ",\"texture\":", unsigned(texture_phase));
    } else {
      owned_graphics::Trace("work_graph_node_batch", ",\"node\":", index,
          ",\"first_group\":", context->GroupStart, ",\"groups\":", batch_groups, ",\"terminal\":1");
    }
    return S_OK;
  }
  HRESULT InitializeFrontier() {
    const uint32_t count = graph.nodes.size();
    node_bindings.resize(count);
    std::vector<WorkGraphSchedulerNode> nodes(count);
    std::vector<WorkGraphSchedulerQueueTemplate> queues(count);
    std::vector<uint32_t> targets, shares;
    const auto global_tables = dispatch.atomic64_tables
        ? std::span<const StandardSwizzleTableUse>(*dispatch.atomic64_tables)
        : std::span<const StandardSwizzleTableUse>{};
    for (uint32_t index = 0; index < count; ++index) {
      const auto &source = graph.nodes[index];
      auto &node = nodes[index];
      const auto &input = source.info.Input;
      const bool terminal = std::all_of(source.targets.begin(), source.targets.end(),
          [](uint32_t target) { return target == SM50_WG_NONE; });
      use_frontier |= !terminal || !graph.sharing_nodes[index].empty();
      if (source.local_root && source.local_root->ParameterQwords) {
        uint64_t offset = 0;
        if (!resource_contract::CheckedMul(source.info.LocalRootArgumentsTableIndex,
                dispatch.program.NodeLocalRootArgumentsTable.StrideInBytes, offset) ||
            !resource_contract::CheckedAdd(dispatch.program.NodeLocalRootArgumentsTable.StartAddress, offset, node.LocalRoot))
          return E_INVALIDARG;
        const auto &root = *source.local_root.ptr();
        const uint64_t bytes = uint64_t(root.ParameterQwords) * sizeof(uint64_t);
        bool needs_snapshot = dispatch.atomic64_heap && root.HasTextureDescriptorTables;
        if (source.atomic64_phase) {
          for (uint32_t p = 0; !needs_snapshot && p < root.ParameterSlots; ++p) {
            const auto type = root.ParameterTypes[p];
            if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
              const auto ranges = root.GetTableRanges(p);
              needs_snapshot = dispatch.atomic64_heap && !ranges.empty() &&
                               ranges.front().type != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            } else {
              needs_snapshot = type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            }
          }
        }
        if (needs_snapshot) {
          // The local-root table is constant during DispatchGraph. Resolve it
          // once per Execute, after the producing queue work has retired.
          std::vector<uint64_t> arguments(source.local_root->ParameterQwords);
          HRESULT hr = Read(node.LocalRoot, arguments.data(), bytes, "local_root_arguments");
          if (FAILED(hr)) return hr;
          auto &bindings = node_bindings[index];
          for (uint32_t parameter = 0; parameter < source.local_root->ParameterSlots; ++parameter) {
            const auto type = source.local_root->ParameterTypes[parameter];
            const auto address = arguments[source.local_root->SlotQwordOffsets[parameter]];
            if (!address) continue;
            if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && dispatch.atomic64_heap)
              bindings.tables.push_back({source.local_root.ptr(), dispatch.atomic64_heap.ptr(), address, parameter});
            else if (source.atomic64_phase && (type == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                     type == D3D12_ROOT_PARAMETER_TYPE_SRV || type == D3D12_ROOT_PARAMETER_TYPE_UAV))
              bindings.roots.push_back({address, UINT64_MAX, type == D3D12_ROOT_PARAMETER_TYPE_UAV});
          }
        } else if ((source.atomic64_phase || dispatch.atomic64_tables || dispatch.atomic64_heap) &&
                   !ResolveRange(node.LocalRoot, bytes, offset)) {
          // The shader reads this local table directly. Validate its extent without a host blit and wait.
          return E_INVALIDARG;
        }
      }
      node_bindings[index].host = source.atomic64_phase || textures.needs_publication(global_tables, node_bindings[index].tables);
      node.Flags = (source.info.HasFixedDispatchGrid ? kWorkGraphSchedulerFixedGrid : 0u) |
          (node_bindings[index].host ? kWorkGraphSchedulerHostRequired : 0u) |
          (terminal ? kWorkGraphSchedulerTerminal : 0u);
      node.Launch = source.info.Launch;
      std::copy_n(source.info.DispatchGrid, 3, node.Grid);
      std::copy_n(source.info.MaxDispatchGrid, 3, node.MaxGrid);
      node.DynamicOffset = input.DispatchGridOffset;
      node.DynamicBytes = input.DispatchGridComponentType == 3 ? 2u : 4u;
      node.DynamicComponents = input.DispatchGridComponents;
      node.RecordSize = input.RecordSize;
      node.Recursion = source.info.MaxRecursionDepth;
      if (source.targets.size() > UINT32_MAX - targets.size() ||
          graph.sharing_nodes[index].size() > UINT32_MAX - shares.size()) return E_OUTOFMEMORY;
      node.TargetOffset = targets.size(); node.TargetCount = source.targets.size();
      node.ShareOffset = shares.size(); node.ShareCount = graph.sharing_nodes[index].size();
      node.GroupLimit = terminal ? (source.atomic64_phase ? 32u : 65535u) : 1u;
      targets.insert(targets.end(), source.targets.begin(), source.targets.end());
      // Outputs retain their public indices. Drain each reachable queue once, in
      // the same ascending node order as the dense traversal.
      node.DrainOffset = targets.size();
      for (uint32_t target : source.targets) {
        if (target == SM50_WG_NONE) continue;
        if (target >= count) return E_INVALIDARG;
        if (targets.size() == UINT32_MAX) return E_OUTOFMEMORY;
        targets.push_back(target);
      }
      auto begin = targets.begin() + node.DrainOffset;
      std::sort(begin, targets.end());
      targets.erase(std::unique(begin, targets.end()), targets.end());
      node.DrainCount = targets.size() - node.DrainOffset;
      shares.insert(shares.end(), graph.sharing_nodes[index].begin(), graph.sharing_nodes[index].end());
      queues[index] = {graph.queue_offset[index], graph.queue_capacity[index] - 1, graph.queue_stride[index]};
    }
    if (!use_frontier) return S_OK;
    if (!count || !graph.depth || graph.depth > kWorkGraphSchedulerMaxDepth ||
        batch_offset > UINT32_MAX || sharing_offset > UINT32_MAX) return E_INVALIDARG;
    uint64_t backing_offset = 0;
    backing_owner = device->LookupBufferByVA(dispatch.program.BackingMemory.StartAddress, &backing_offset);
    if (!backing_owner || backing_offset > backing_owner->byteLength() ||
        graph.memory.MinSizeInBytes > backing_owner->byteLength() - backing_offset) return E_INVALIDARG;
    WMT::Reference<WMT::Error> error;
    frontier_pipeline = device->GetLib().getWorkGraphSchedulerPipeline(device->GetMetal4Compiler(), error);
    if (error || !frontier_pipeline) {
      if (error) ERR("Work graph scheduler pipeline failed: ", error.description().getUTF8String());
      else ERR("Work graph scheduler pipeline is unavailable");
      return E_FAIL;
    }

    uint64_t length = sizeof(WorkGraphSchedulerState);
    const auto reserve = [&](uint64_t bytes, uint64_t &offset) {
      return resource_contract::CheckedAlign(length, 64, offset) && resource_contract::CheckedAdd(offset, bytes, length);
    };
    uint64_t nodes_offset = 0, queues_offset = 0, targets_offset = 0, shares_offset = 0, frames_offset = 0;
    if (!reserve(sizeof(WorkGraphSchedulerParams), params_offset) ||
        !reserve(uint64_t(count) * sizeof(WorkGraphSchedulerNode), nodes_offset) ||
        !reserve(uint64_t(count) * sizeof(WorkGraphSchedulerQueueTemplate), queues_offset) ||
        !reserve(uint64_t(targets.size()) * sizeof(uint32_t), targets_offset) ||
        !reserve(uint64_t(shares.size()) * sizeof(uint32_t), shares_offset) ||
        !reserve(sizeof(WorkGraphSchedulerSeed), seeds_offset) ||
        !reserve(uint64_t(graph.depth) * sizeof(WorkGraphSchedulerFrame), frames_offset) ||
        !reserve(uint64_t(count) * sizeof(WorkGraphSchedulerIndirect), indirect_offset) ||
        !resource_contract::CheckedAlign(length, 64, length) || length > device->GetMTLDevice().maxBufferLength())
      return E_OUTOFMEMORY;
    frontier_info.length = length;
    frontier_info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
    frontier = device->GetMTLDevice().newBuffer(frontier_info);
    if (!frontier) return E_OUTOFMEMORY;
    auto *bytes = static_cast<uint8_t *>(frontier_info.memory.get_accessible_or_null());
    if (!bytes) return E_OUTOFMEMORY;
    std::memset(bytes, 0, length);
    auto &params = *reinterpret_cast<WorkGraphSchedulerParams *>(bytes + params_offset);
    const uint64_t address = frontier_info.gpu_address;
    params.Nodes = address + nodes_offset; params.Queues = address + queues_offset;
    params.Targets = address + targets_offset; params.Shares = address + shares_offset;
    params.Seeds = address + seeds_offset; params.Frames = address + frames_offset;
    params.Levels = control_info.gpu_address; params.Context = control_info.gpu_address + context_offset;
    params.Indirect = address + indirect_offset; params.Backing = dispatch.program.BackingMemory.StartAddress;
    params.LevelBytes = graph.level_bytes; params.LevelStride = control_stride;
    params.NodeCount = count; params.Depth = graph.depth;
    params.BatchOffset = batch_offset; params.SharingOffset = sharing_offset;
    std::memcpy(bytes + nodes_offset, nodes.data(), nodes.size() * sizeof(nodes[0]));
    std::memcpy(bytes + queues_offset, queues.data(), queues.size() * sizeof(queues[0]));
    if (!targets.empty()) std::memcpy(bytes + targets_offset, targets.data(), targets.size() * sizeof(uint32_t));
    if (!shares.empty()) std::memcpy(bytes + shares_offset, shares.data(), shares.size() * sizeof(uint32_t));
    return S_OK;
  }

  HRESULT RunFrontier(uint32_t index, uint32_t count, uint64_t address, uint64_t stride, const uint8_t *cpu) {
    if (!count) return S_OK;
    const uint64_t record_size = graph.nodes[index].info.Input.RecordSize;
    // Empty input records ignore the application-supplied address and stride.
    // Count still schedules that many invocations, even for sentinel addresses.
    if (!record_size) address = stride = 0;
    uint64_t extent = 0;
    if (!resource_contract::CheckedMul(count - 1, stride, extent) ||
        !resource_contract::CheckedAdd(extent, record_size, extent) || address > UINT64_MAX - extent)
      return E_INVALIDARG;
    Rc<BufferAllocation> input_owner;
    if (cpu) {
      WMTBufferInfo info{};
      if (!resource_contract::CheckedAlign(std::max<uint64_t>(extent, 1), 4096, info.length) ||
          info.length > device->GetMTLDevice().maxBufferLength()) return E_OUTOFMEMORY;
      info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
      seed_resource = device->GetMTLDevice().newBuffer(info);
      if (!seed_resource) return E_OUTOFMEMORY;
      if (extent) std::memcpy(info.memory.get_accessible_or_null(), cpu, extent);
      seed_address = address = info.gpu_address; seed_length = info.length;
    } else {
      seed_resource = {}; seed_address = seed_length = 0;
      if (record_size) {
        // Keep the complete GPU input range pinned through all continuations;
        // generated child records are bounded by the graph's backing layout.
        uint64_t offset = 0; input_owner = device->LookupBufferByVA(address, &offset);
        if (!input_owner || offset > input_owner->byteLength() || extent > input_owner->byteLength() - offset)
          return E_INVALIDARG;
      }
    }
    auto *bytes = static_cast<uint8_t *>(frontier_info.memory.get_accessible_or_null());
    auto &state = *reinterpret_cast<WorkGraphSchedulerState *>(bytes);
    state = {}; state.SelectedNode = state.PendingPacket = SM50_WG_NONE;
    *reinterpret_cast<WorkGraphSchedulerSeed *>(bytes + seeds_offset) = {address, stride, count, index};
    // All dynamic output indices select from these immutable target tables.
    // A zero-grid producer can still forward its input to a sharing peer.
    std::vector<uint32_t> reachable{index};
    std::vector<bool> seen(graph.nodes.size());
    seen[index] = true;
    auto visit = [&](uint32_t target) {
      if (target != SM50_WG_NONE && !seen[target]) {
        seen[target] = true;
        reachable.push_back(target);
      }
    };
    for (size_t i = 0; i < reachable.size(); ++i) {
      const auto node = reachable[i];
      for (auto target : graph.nodes[node].targets) visit(target);
      for (auto peer : graph.sharing_nodes[node]) visit(peer);
    }
    std::erase_if(reachable, [&](uint32_t node) { return node_bindings[node].host; });
    const bool group_indirect = reachable.size() > 1;
    uint64_t quantum = 0;
    while (state.Status == kWorkGraphSchedulerRunning) {
      const uint64_t nodes_before = state.NodeSteps, nonterminal_before = state.NonterminalSteps, host_before = state.HostSteps;
      // Only retired CPU-coherent control is changed here. Suspended frames,
      // records, queue tails and sharing counters stay entirely GPU-owned.
      state.QuantumGroups = 0;
      auto buffer = Begin();
      if (!buffer) return E_OUTOFMEMORY;
      auto encoder = buffer.computeCommandEncoder(group_indirect);
      Use(encoder, frontier); Use(encoder, control); Use(encoder, backing_owner->buffer());
      if (seed_resource) Use(encoder, seed_resource);
      if (input_owner) Use(encoder, input_owner->buffer());
      for (uint32_t slot = 0; slot < kWorkGraphSchedulerQuantum; ++slot) {
        if (slot && group_indirect)
          encoder.barrierAfterEncoderStages(WMTMetal4StageDispatch, WMTMetal4StageDispatch, WMTMetal4VisibilityDevice);
        Bind(encoder, frontier, 0, 0); Bind(encoder, frontier, params_offset, 1);
        wmtcmd_compute_setpso prepare{}; prepare.type = WMTComputeCommandSetPSO;
        prepare.pso = frontier_pipeline; prepare.threadgroup_size = {1, 1, 1};
        wmtcmd_compute_dispatch step{}; step.type = WMTComputeCommandDispatch; step.size = {1, 1, 1};
        prepare.next.set(&step);
        encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&prepare));
        // Prepare clears the previous live packet and enables at most one.
        // Other indirect calls read zero grids and cannot touch node data.
        if (group_indirect)
          encoder.barrierAfterEncoderStages(WMTMetal4StageDispatch, WMTMetal4StageDispatch, WMTMetal4VisibilityDevice);
        if (!reachable.empty()) {
          Bind(encoder, control, context_offset, SM50_BINDING_INDEX_WORK_GRAPH_CONTEXT);
          Bind(encoder, dispatch.arguments, dispatch.roots, SM50_BINDING_INDEX_ROOT_ARGUMENTS);
          Bind(encoder, dispatch.arguments, dispatch.samplers, SM50_BINDING_INDEX_STATIC_SAMPLERS);
        }
        for (uint32_t node_index : reachable) {
          const auto &node = graph.nodes[node_index];
          if (node.constants.buffer) Bind(encoder, node.constants.buffer, node.constants.offset, SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS);
          wmtcmd_compute_setpso pipeline{}; pipeline.type = WMTComputeCommandSetPSO;
          pipeline.pso = node.pipeline; pipeline.threadgroup_size = node.threads;
          wmtcmd_compute_dispatch_indirect launch{}; launch.type = WMTComputeCommandDispatchIndirect;
          launch.indirect_args_buffer = frontier;
          launch.indirect_args_offset = indirect_offset + uint64_t(node_index) * sizeof(WorkGraphSchedulerIndirect);
          pipeline.next.set(&launch);
          encoder.encodeCommands(reinterpret_cast<const wmtcmd_compute_nop *>(&pipeline));
        }
      }
      // The last batch retires before the CPU consumes scheduler or host state.
      encoder.endEncoding();
      if (!Complete(buffer)) return E_FAIL;
      owned_graphics::Trace("work_graph_frontier_quantum", ",\"quantum\":", ++quantum,
          ",\"node_steps\":", state.NodeSteps - nodes_before,
          ",\"nonterminal_steps\":", state.NonterminalSteps - nonterminal_before,
          ",\"host_steps\":", state.HostSteps - host_before,
          ",\"total_node_steps\":", state.NodeSteps, ",\"total_nonterminal_steps\":", state.NonterminalSteps,
          ",\"peak_depth\":", state.PeakDepth, ",\"host_yield\":", state.HostYield,
          ",\"done\":", unsigned(state.Status == kWorkGraphSchedulerDone), ",\"error\":", state.Error,
          ",\"gpu_slots\":", kWorkGraphSchedulerQuantum, ",\"indirect_nodes\":", reachable.size(),
          ",\"graph_nodes\":", graph.nodes.size(), ",\"admitted_groups\":", state.QuantumGroups);
      if (state.Status == kWorkGraphSchedulerError) {
        ERR("Work graph GPU frontier failed: ", state.Error, " node=", state.ErrorNode, " level=", state.ErrorLevel);
        return E_INVALIDARG;
      }
      if (state.HostYield) {
        if (state.SelectedNode >= graph.nodes.size() || state.SelectedLevel >= graph.depth ||
            !node_bindings[state.SelectedNode].host) return E_FAIL;
        const auto *context = reinterpret_cast<const SM50_WG_NODE_CONTEXT *>(
            static_cast<const uint8_t *>(control_info.memory.get_accessible_or_null()) + context_offset);
        HRESULT hr = RunBatch(state.SelectedNode, state.InputAddress, state.SelectedLevel, context->BatchGroups, true);
        if (FAILED(hr)) return hr;
        state.HostYield = 0;
      }
    }
    return state.Status == kWorkGraphSchedulerDone ? S_OK : E_FAIL;
  }

  HRESULT RunTerminal(uint32_t index, uint64_t input_address, const uint8_t *input, uint32_t slot, WMT::MTL4CommandBuffer pending) {
    const auto &node = graph.nodes[index];
    if (node.info.Input.RecordSize && !input && !pending) {
      // Removing a CPU readback must not remove its bounds validation. The GPU
      // still consumes the complete record, even when the scheduler reads none.
      uint64_t offset = 0; auto allocation = device->LookupBufferByVA(input_address, &offset);
      if (!allocation || offset > allocation->byteLength() ||
          node.info.Input.RecordSize > allocation->byteLength() - offset) return E_INVALIDARG;
    }
    uint32_t grid[3] = {1, 1, 1}; uint64_t groups = 1;
    if (node.info.Launch == SM50_WG_LAUNCH_BROADCASTING) {
      std::copy_n(node.info.DispatchGrid, 3, grid);
      if (!node.info.HasFixedDispatchGrid && node.info.Input.DispatchGridComponents) {
        // Only dynamic broadcasting metadata is consumed by the CPU scheduler.
        // Thread/coalescing/fixed-grid nodes read their records directly on GPU.
        std::array<uint8_t, 12> grid_record{};
        const uint32_t component_bytes = node.info.Input.DispatchGridComponentType == 3 ? 2 : 4;
        const auto *metadata = input ? input + node.info.Input.DispatchGridOffset : grid_record.data();
        if (!input) {
          const HRESULT hr = Read(input_address + node.info.Input.DispatchGridOffset, grid_record.data(),
              node.info.Input.DispatchGridComponents * component_bytes, "terminal_grid");
          if (FAILED(hr)) return hr;
        }
        for (uint32_t axis = 0; axis < node.info.Input.DispatchGridComponents; ++axis) {
          grid[axis] = 0; std::memcpy(&grid[axis], metadata + axis * component_bytes, component_bytes);
        }
        for (uint32_t axis = node.info.Input.DispatchGridComponents; axis < 3; ++axis) grid[axis] = 1;
      }
      groups = uint64_t(grid[0]) * grid[1] * grid[2];
    }
    if (!groups) return S_OK;
    // A leaf cannot overflow output queues: there are no descendants to drain
    // between its groups. Dispatch an independent frontier in bounded batches,
    // retaining the original group coordinates and input-completion counter.
    const uint32_t group_limit = node.atomic64_phase ? 32u : 65535u;
    const uint64_t base = slot * control_stride;
    auto *words = static_cast<uint8_t *>(control_info.memory.get_accessible_or_null()) + base;
    auto *queues = reinterpret_cast<SM50_WG_QUEUE *>(words);
    auto *context = reinterpret_cast<SM50_WG_NODE_CONTEXT *>(words + context_offset);
    auto *batch = reinterpret_cast<SM50_WG_BATCH_RECORD *>(words + batch_offset);
    auto *sharing = reinterpret_cast<uint32_t *>(words + sharing_offset);
    *sharing = uint32_t(groups); batch->GroupEnd = uint32_t(groups); std::copy_n(grid, 3, batch->Grid);
    for (uint64_t group = 0; group < groups;) {
      const uint32_t batch_groups = static_cast<uint32_t>(std::min<uint64_t>(groups - group, group_limit));
      // Leaf roots have no output or shared queue. The input queue is the only reachable entry.
      queues[0] = {input_address, control_info.gpu_address + base + sharing_offset, 0, node.info.Input.RecordSize, 0, 0};
      *context = {}; context->QueueTable = control_info.gpu_address + base;
      context->BatchTable = control_info.gpu_address + base + batch_offset;
      context->InputQueue = 0; context->BatchRecords = 1; context->BatchGroups = batch_groups;
      context->GroupStart = group; context->RemainingRecursionLevels = node.info.MaxRecursionDepth;
      context->OutputElementCount = node.targets.size();
      if (node.local_root && node.local_root->ParameterQwords) context->LocalRootArguments = dispatch.program.NodeLocalRootArgumentsTable.StartAddress +
          uint64_t(node.info.LocalRootArgumentsTableIndex) * dispatch.program.NodeLocalRootArgumentsTable.StrideInBytes;
      for (uint32_t o = 0; o < node.targets.size(); ++o) context->Outputs[o] =
          SM50_WG_NONE;
      HRESULT hr = RunBatch(index, input_address, slot, batch_groups, false, pending);
      if (FAILED(hr)) return hr;
      group += batch_groups;
    }
    return S_OK;
  }
  HRESULT RunInput(uint32_t entry, uint32_t count, uint64_t address, uint64_t stride, const uint8_t *cpu) {
    if (entry >= graph.entries.size()) return E_INVALIDARG;
    const uint32_t index = graph.entries[entry]; const auto &node = graph.nodes[index];
    if (use_frontier) return RunFrontier(index, count, address, stride, cpu);
    if (!count) return S_OK;
    const uint64_t record_size = node.info.Input.RecordSize;
    const uint64_t seed_stride = (record_size + 15) & ~uint64_t(15);
    uint32_t capacity = 1;
    const bool fixed = node.info.Launch != SM50_WG_LAUNCH_BROADCASTING || node.info.HasFixedDispatchGrid;
    const uint64_t groups = node.info.Launch == SM50_WG_LAUNCH_BROADCASTING
        ? uint64_t(node.info.DispatchGrid[0]) * node.info.DispatchGrid[1] * node.info.DispatchGrid[2] : 1;
    Rc<BufferAllocation> input_owner;
    // Each seed owns its mutable context and input-completion word until retirement. Host publication,
    // changing grids and multi-batch records retain their original per-batch retirement.
    if (count > 1 && fixed && groups && groups <= 65535 && !node_bindings[index].host &&
        (!dispatch.atomic64_tables || dispatch.atomic64_tables->empty()) && node_bindings[index].tables.empty()) {
      capacity = uint32_t(std::min<uint64_t>({count, 64, (256u * 1024u) / std::max(control_stride, seed_stride)}));
      capacity = std::max(1u, capacity);
      if (capacity > 1 && record_size && !cpu) {
        uint64_t extent, offset;
        if (resource_contract::CheckedMul(count - 1, stride, extent) &&
            resource_contract::CheckedAdd(extent, record_size, extent))
          input_owner = ResolveRange(address, extent, offset);
        if (!input_owner) capacity = 1;
      }
      if (capacity > 1 && control_info.length < control_stride * capacity) {
        WMTBufferInfo info{};
        info.length = control_stride * capacity; info.options = control_info.options;
        auto storage = device->GetMTLDevice().newBuffer(info);
        if (storage) { control = std::move(storage); control_info = info; }
        else capacity = 1;
      }
    }
    WMTBufferInfo seed_info{};
    seed_resource = {}; seed_address = seed_length = 0;
    if (cpu && record_size) {
      seed_info.length = std::max<uint64_t>(4096, (seed_stride * capacity + 4095) & ~uint64_t(4095));
      seed_info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
      seed_resource = device->GetMTLDevice().newBuffer(seed_info);
      if (!seed_resource) return E_OUTOFMEMORY;
      seed_address = seed_info.gpu_address; seed_length = seed_info.length;
    }
    for (uint32_t first = 0; first < count;) {
      const uint32_t chunk = std::min(capacity, count - first);
      WMT::Reference<WMT::MTL4CommandBuffer> pending;
      if (chunk > 1) {
        pending = Begin();
        if (!pending) return E_OUTOFMEMORY;
        if (input_owner) pending.useAllocation(input_owner->buffer());
      }
      for (uint32_t i = 0; i < chunk; ++i) {
        const uint64_t source_offset = uint64_t(first + i) * stride;
        const auto *record = cpu && record_size ? cpu + source_offset : nullptr;
        uint64_t input = record_size ? address + source_offset : 0;
        if (record) {
          const auto offset = uint64_t(i) * seed_stride;
          std::memcpy(static_cast<uint8_t *>(seed_info.memory.get_accessible_or_null()) + offset, record, record_size);
          input = seed_address + offset;
        }
        HRESULT hr = RunTerminal(index, input, record, i, pending);
        if (FAILED(hr)) return hr;
      }
      if (pending) {
        if (!Complete(pending)) return E_FAIL;
        owned_graphics::Trace("work_graph_terminal_chunk", ",\"node\":", index, ",\"records\":", chunk,
            ",\"gpu_input\":", unsigned(!cpu && record_size), ",\"completions\":1");
      }
      first += chunk;
    }
    return S_OK;
  }
};
}

HRESULT ExecuteWorkGraph(MTLD3D12Device *device, WMT::MTL4CommandQueue queue, const WorkGraphDispatch &dispatch,
                        const WorkGraphTexturePublisher &textures, std::shared_ptr<Atomic64PhaseCache> &atomic_cache) {
  const auto *graph = dispatch.object->GetGraph(dispatch.graph); if (!graph) return E_INVALIDARG;
  GraphExecution execution{device, queue, dispatch, *graph, textures, atomic_cache};
  execution.allocator = device->GetMTLDevice().newMTL4CommandAllocator();
  if (!execution.allocator) return E_OUTOFMEMORY;
  execution.atomic_fence = device->GetMTLDevice().newFence();
  if (!execution.atomic_fence) return E_OUTOFMEMORY;
  uint64_t elements = 0; for (const auto &node : graph->nodes) elements = std::max<uint64_t>(elements, node.targets.size());
  execution.context_offset = (sizeof(SM50_WG_QUEUE) * (graph->nodes.size() + 1) + 63) & ~uint64_t(63);
  execution.batch_offset = (execution.context_offset + offsetof(SM50_WG_NODE_CONTEXT, Outputs) + elements * 4 + 63) & ~uint64_t(63);
  execution.sharing_offset = execution.batch_offset + sizeof(SM50_WG_BATCH_RECORD);
  execution.control_stride = (execution.sharing_offset + 4 + 63) & ~uint64_t(63);
  execution.control_info.length = execution.control_stride * graph->depth;
  execution.control_info.options = WMTResourceStorageModeShared | WMT_RESOURCE_CPU_ADDRESSABLE;
  execution.control = device->GetMTLDevice().newBuffer(execution.control_info);
  if (!execution.control) return E_OUTOFMEMORY;
  HRESULT initialized = execution.InitializeFrontier();
  if (FAILED(initialized)) return initialized;
  if (dispatch.mode == kDispatchModeNodeCPUInput || dispatch.mode == kDispatchModeMultiNodeCPUInput) {
    for (const auto &input : dispatch.inputs) {
      HRESULT hr = execution.RunInput(input.entry, input.count, 0, input.stride, input.records.data());
      if (FAILED(hr)) return hr;
    }
  } else if (dispatch.mode == kDispatchModeNodeGPUInput) {
    D3D12_NODE_GPU_INPUT input{}; HRESULT hr = execution.Read(dispatch.gpu_input, &input, sizeof(input), "gpu_input_header");
    if (FAILED(hr)) return hr;
    if (input.NumRecords > dispatch.max_gpu_records || dispatch.max_gpu_node_inputs < 1) return E_INVALIDARG;
    return execution.RunInput(input.EntrypointIndex, input.NumRecords, input.Records.StartAddress, input.Records.StrideInBytes, nullptr);
  } else {
    D3D12_MULTI_NODE_GPU_INPUT multi{}; HRESULT hr = execution.Read(dispatch.gpu_input, &multi, sizeof(multi), "gpu_multi_input_header");
    if (FAILED(hr)) return hr;
    if (multi.NumNodeInputs > dispatch.max_gpu_node_inputs) return E_INVALIDARG;
    uint64_t total_records = 0;
    for (uint32_t i = 0; i < multi.NumNodeInputs; ++i) {
      D3D12_NODE_GPU_INPUT input{}; hr = execution.Read(multi.NodeInputs.StartAddress + uint64_t(i) * multi.NodeInputs.StrideInBytes, &input, sizeof(input), "gpu_input_header");
      if (FAILED(hr)) return hr;
      total_records += input.NumRecords;
      if (total_records > dispatch.max_gpu_records) return E_INVALIDARG;
      hr = execution.RunInput(input.EntrypointIndex, input.NumRecords, input.Records.StartAddress, input.Records.StrideInBytes, nullptr);
      if (FAILED(hr)) return hr;
    }
  }
  return S_OK;
}

} // namespace d3dalloy
