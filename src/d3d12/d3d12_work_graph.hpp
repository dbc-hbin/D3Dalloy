#pragma once

#include "d3d12_device_child.hpp"
#include "d3d12_pipeline_cache.hpp"
#include "dxil_workgraph.h"
#include <memory>
#include <functional>
#include <span>
#include <atomic>
#include <string>
#include <vector>

namespace dxmt {

class MTLD3D12WorkGraph final : public MTLD3D12DeviceChild<ID3D12StateObject, ID3D12StateObjectProperties1,
                                                       ID3D12WorkGraphProperties> {
public:
  struct RootExport { std::wstring name; Com<MTLD3D12RootSignature> root; };
  struct Output {
    SM50_WG_NODE_IO info{};
    std::wstring name;
  };
  struct Node {
    SM50_WG_NODE_INFO info{};
    bool entry_forbidden = false;
    std::wstring name, shader, original_shader, share_input;
    std::string entry;
    uint32_t library = 0;
    uint32_t scope = 0, collection_flags = 0;
    bool exported = true, from_collection = false;
    std::vector<Output> outputs;
    std::vector<uint32_t> element_base, element_count, targets;
    Com<MTLD3D12RootSignature> local_root;
    Com<MTLD3D12RootSignature> global_root;
    WMT::Reference<WMT::ComputePipelineState> pipeline;
    std::shared_ptr<const WorkGraphArtifact> compiled_artifact;
    ShaderImmutableConstants constants;
    WMTSize threads{};
    bool atomic64_phase = false;
  };
  struct Graph {
    std::wstring name;
    Com<MTLD3D12RootSignature> global_root;
    Com<MTLD3D12RootSignature> dispatch_root;
    std::vector<Node> nodes;
    std::vector<uint32_t> entries;
    std::vector<uint32_t> node_depth;
    std::vector<std::vector<uint32_t>> sharing_nodes;
    std::vector<uint32_t> queue_capacity, queue_stride;
    std::vector<uint64_t> queue_offset;
    uint64_t level_bytes = 0;
    uint32_t depth = 1;
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memory{};
  };

  explicit MTLD3D12WorkGraph(MTLD3D12Device *device);
  ~MTLD3D12WorkGraph();
  HRESULT Initialize(const D3D12_STATE_OBJECT_DESC *desc, const MTLD3D12WorkGraph *parent = nullptr);
  static Com<MTLD3D12WorkGraph> Resolve(MTLD3D12Device *device, const D3D12_PROGRAM_IDENTIFIER &id, uint32_t &index);
  MTLD3D12RootSignature *RootSignature(uint32_t graph) const;
  const std::vector<std::vector<uint8_t>> &Libraries() const { return libraries_; }
  const std::vector<Node> &Exports() const { return available_; }
  const std::vector<RootExport> &Roots() const { return root_exports_; }
  UINT Flags() const { return flags_; }
  bool IsCollection() const { return is_collection_; }
  const Graph *GetGraph(uint32_t index) const;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  void *STDMETHODCALLTYPE GetShaderIdentifier(LPCWSTR) override;
  UINT64 STDMETHODCALLTYPE GetShaderStackSize(LPCWSTR) override;
  UINT64 STDMETHODCALLTYPE GetPipelineStackSize() override;
  void STDMETHODCALLTYPE SetPipelineStackSize(UINT64) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_PROGRAM_IDENTIFIER *STDMETHODCALLTYPE GetProgramIdentifier(D3D12_PROGRAM_IDENTIFIER *result, LPCWSTR name) override;
  D3D12_NODE_ID *STDMETHODCALLTYPE GetNodeID(D3D12_NODE_ID *result, UINT graph, UINT node) override;
  D3D12_NODE_ID *STDMETHODCALLTYPE GetEntrypointID(D3D12_NODE_ID *result, UINT graph, UINT entry) override;
#else
  D3D12_PROGRAM_IDENTIFIER STDMETHODCALLTYPE GetProgramIdentifier(LPCWSTR name) override;
  D3D12_NODE_ID STDMETHODCALLTYPE GetNodeID(UINT graph, UINT node) override;
  D3D12_NODE_ID STDMETHODCALLTYPE GetEntrypointID(UINT graph, UINT entry) override;
#endif
  UINT STDMETHODCALLTYPE GetNumWorkGraphs() override;
  LPCWSTR STDMETHODCALLTYPE GetProgramName(UINT graph) override;
  UINT STDMETHODCALLTYPE GetWorkGraphIndex(LPCWSTR name) override;
  UINT STDMETHODCALLTYPE GetNumNodes(UINT graph) override;
  UINT STDMETHODCALLTYPE GetNodeIndex(UINT graph, D3D12_NODE_ID id) override;
  UINT STDMETHODCALLTYPE GetNodeLocalRootArgumentsTableIndex(UINT graph, UINT node) override;
  UINT STDMETHODCALLTYPE GetNumEntrypoints(UINT graph) override;
  UINT STDMETHODCALLTYPE GetEntrypointIndex(UINT graph, D3D12_NODE_ID id) override;
  UINT STDMETHODCALLTYPE GetEntrypointRecordSizeInBytes(UINT graph, UINT entry) override;
  void STDMETHODCALLTYPE GetWorkGraphMemoryRequirements(UINT graph, D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS *memory) override;
  UINT STDMETHODCALLTYPE GetEntrypointRecordAlignmentInBytes(UINT graph, UINT entry) override;

private:
  friend HRESULT CreateWorkGraphStateObject(MTLD3D12Device *, const D3D12_STATE_OBJECT_DESC *, ID3D12StateObject *, REFIID, void **);
  std::shared_ptr<std::atomic<uint64_t>> latest_version_;
  std::vector<std::vector<uint8_t>> libraries_;
  std::vector<Node> available_;
  std::vector<RootExport> root_exports_;
  std::vector<Graph> graphs_;
  Com<MTLD3D12RootSignature> global_root_;
  struct RootAssociation {
    Com<MTLD3D12RootSignature> root;
    std::wstring root_name;
    std::vector<std::wstring> exports;
    uint32_t scope = 0; /* zero: API, library index + 1: DXIL scope */
    bool local = false, explicit_association = false;
    uint32_t scope_end = 0;
    int priority = -1;
  };
  std::vector<RootAssociation> pending_associations_;
  bool is_collection_ = false;
  uint64_t token_ = 0;
  UINT flags_ = 0;
  HRESULT BuildGraph(const D3D12_WORK_GRAPH_DESC &desc, Graph &graph, const Graph *parent);
  HRESULT Compile(Node &node, MTLD3D12RootSignature *root, bool validate_only = false);
  D3D12_PROGRAM_IDENTIFIER Identifier(UINT graph) const;
  D3D12_NODE_ID NodeID(UINT graph, UINT node) const;
};

struct WorkGraphDispatch {
  ~WorkGraphDispatch();
  struct Input { uint32_t entry, count; uint64_t address, stride; std::vector<uint8_t> records; };
  Com<MTLD3D12WorkGraph> object;
  uint32_t graph = 0, mode = 0;
  D3D12_SET_WORK_GRAPH_DESC program{};
  uint64_t gpu_input = 0;
  uint32_t max_gpu_records = UINT32_MAX, max_gpu_node_inputs = UINT32_MAX;
  std::vector<Input> inputs;
  WMT::Reference<WMT::Buffer> arguments;
  uint64_t roots = 0, samplers = 0;
  std::unique_ptr<std::vector<StandardSwizzleTableUse>> atomic64_tables;
  std::vector<Atomic64RootBinding> atomic64_roots;
  Com<MTLD3D12DescriptorHeap, false> atomic64_heap;
};

HRESULT CreateWorkGraphStateObject(MTLD3D12Device *device, const D3D12_STATE_OBJECT_DESC *desc,
                                 ID3D12StateObject *parent, REFIID riid, void **object);
bool IsWorkGraphCollection(const D3D12_STATE_OBJECT_DESC *desc);
// Queue-owned texture transfers and scratch survive every native graph buffer.
// Callbacks run under the executor admission, without a nested queue commit.
struct WorkGraphTexturePublisher {
  std::function<HRESULT(WMT::MTL4CommandBuffer, WMT::Fence,
      std::span<const StandardSwizzleTableUse>, std::span<const StandardSwizzleTableUse>)> begin;
  std::function<HRESULT(WMT::MTL4CommandBuffer, WMT::Fence)> end;
};
HRESULT ExecuteWorkGraph(MTLD3D12Device *device, WMT::MTL4CommandQueue queue, const WorkGraphDispatch &dispatch,
                        const WorkGraphTexturePublisher &textures);

} // namespace dxmt
