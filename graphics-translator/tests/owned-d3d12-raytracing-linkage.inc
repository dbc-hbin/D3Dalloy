// Real DXC ordinary-function-only collection consumed by a separate DXR library.
namespace linkage {

int run(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &stateDxil,
        contract::Runner &geometry, const std::vector<uint8_t> &consumer,
        const std::vector<uint8_t> &helper, const std::vector<uint8_t> &final) {
  stateobj::Runner runner(device5, ctx, stateDxil, geometry);
  const char *name = "generic-function-collection";
  int bad = 0;
  const uint32_t floatExpected[] = {0x40a80000u, 0x40e80000u, 0x41140000u, 0x41340000u};
  D3D12_EXPORT_DESC bridge{L"BridgeValue", L"BaseValue", D3D12_EXPORT_FLAG_NONE};
  D3D12_DXIL_LIBRARY_DESC helperLibrary{{helper.data(), helper.size()}, 1, &bridge};
  D3D12_DXIL_LIBRARY_DESC finalLibrary{{final.data(), final.size()}, 0, nullptr};
  D3D12_DXIL_LIBRARY_DESC consumerLibrary{{consumer.data(), consumer.size()}, 0, nullptr};
  D3D12_STATE_OBJECT_CONFIG deferred{D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS |
                                     D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS};
  ID3D12RootSignature *root = runner.globalRoot();
  ComPtr<ID3D12StateObject> collection;
  HRESULT hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helperLibrary},
                             {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred},
                             {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root}}, collection,
                            D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "deferred_collection", uint32_t(hr), uint32_t(S_OK));
  if (FAILED(hr))
    return stateobj::reportCase(name, bad);
  bad += stateobj::mismatch(name, "ordinary_identifier", stateobj::Runner::identifier(collection.Get(), L"BridgeValue") != nullptr, 0);
  ComPtr<ID3D12StateObject> rejected;
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helperLibrary}}, rejected,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "unresolved_without_flag", uint32_t(hr), uint32_t(E_INVALIDARG));

  D3D12_EXPORT_DESC published{L"CollectionValue", L"BridgeValue", D3D12_EXPORT_FLAG_NONE};
  D3D12_EXISTING_COLLECTION_DESC existing{collection.Get(), 1, &published};
  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{0, 0};
  D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{0};
  auto pipeline = [&](const D3D12_EXISTING_COLLECTION_DESC &imports, const D3D12_DXIL_LIBRARY_DESC &provider,
                      ComPtr<ID3D12StateObject> &object) {
    return runner.create({{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &imports},
                          {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &consumerLibrary},
                          {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &provider},
                          {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root},
                          {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
                          {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}}, object);
  };
  ComPtr<ID3D12StateObject> object;
  hr = pipeline(existing, finalLibrary, object);
  bad += stateobj::mismatch(name, "resolved_pipeline", uint32_t(hr), uint32_t(S_OK));
  if (SUCCEEDED(hr)) {
    runner.raygen(object.Get(), L"LinkRayGen");
    const auto words = runner.trace(name, object.Get(), 0, std::vector<stateobj::LaneRay>(4));
    for (UINT lane = 0; lane < 4; ++lane) {
      bad += stateobj::mismatch(name, "linked_gpu_value", words[lane], 0x2320u + lane * 3u, lane);
      bad += stateobj::mismatch(name, "helper_root_resource", words[21 + lane], 0xabc005u + lane, lane);
      bad += stateobj::mismatch(name, "linked_float_overload", words[4 + lane], floatExpected[lane], lane);
    }
    std::printf("OWNED_D3D12_RAYTRACING_LINKAGE words=%08x,%08x,%08x,%08x\n",
                words[0], words[1], words[2], words[3]);
    bad += stateobj::mismatch(name, "ordinary_pipeline_identifier", stateobj::Runner::identifier(object.Get(), L"CollectionValue") != nullptr, 0);
  }

  // A provider omitted by the explicit export list must not satisfy a different library's call.
  D3D12_EXPORT_DESC unused{L"UnusedValue", nullptr, D3D12_EXPORT_FLAG_NONE};
  auto hiddenProvider = finalLibrary;
  hiddenProvider.NumExports = 1;
  hiddenProvider.pExports = &unused;
  hr = pipeline(existing, hiddenProvider, rejected);
  bad += stateobj::mismatch(name, "hidden_dependency_rejected", SUCCEEDED(hr), 0);
  D3D12_EXPORT_DESC wrongRename{L"OtherValue", L"BridgeValue", D3D12_EXPORT_FLAG_NONE};
  auto renamedAway = existing;
  renamedAway.pExports = &wrongRename;
  hr = pipeline(renamedAway, finalLibrary, rejected);
  bad += stateobj::mismatch(name, "original_name_not_visible", SUCCEEDED(hr), 0);
  D3D12_EXPORT_DESC conflicting{L"CollectionValue", L"FinalValue", D3D12_EXPORT_FLAG_NONE};
  auto conflictProvider = finalLibrary;
  conflictProvider.NumExports = 1;
  conflictProvider.pExports = &conflicting;
  hr = pipeline(existing, conflictProvider, rejected);
  bad += stateobj::mismatch(name, "conflicting_export", uint32_t(hr), uint32_t(E_INVALIDARG));

  D3D12_ROOT_SIGNATURE_DESC1 emptyRootDesc{};
  auto differentRoot = createRootSignature(runner.device(), emptyRootDesc, "linkage: different root");
  ID3D12RootSignature *different = differentRoot.Get();
  const WCHAR *functionName[] = {L"CollectionValue"};
  std::vector<D3D12_STATE_SUBOBJECT> conflictRoots = {
      {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &existing},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &consumerLibrary},
      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &finalLibrary},
      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &different},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig},
      {D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, nullptr}};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION rootConflict{&conflictRoots[3], 1, functionName};
  conflictRoots[6].pDesc = &rootConflict;
  D3D12_STATE_OBJECT_DESC conflictRootDesc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                                          UINT(conflictRoots.size()), conflictRoots.data()};
  rejected.Reset();
  hr = device5->CreateStateObject(&conflictRootDesc, IID_PPV_ARGS(&rejected));
  bad += stateobj::mismatch(name, "collection_root_conflict", uint32_t(hr), uint32_t(E_INVALIDARG));

  D3D12_ROOT_PARAMETER1 localParameter{};
  localParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  localParameter.Constants = {7, 0, 1};
  localParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC1 localDesc{1, &localParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE};
  auto helperLocalRoot = createRootSignature(runner.device(), localDesc, "linkage: helper local root");
  ID3D12RootSignature *local = helperLocalRoot.Get();
  ComPtr<ID3D12StateObject> localCollection;
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helperLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred},
                      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root},
                      {D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &local}}, localCollection,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "helper_local_root_collection", uint32_t(hr), uint32_t(S_OK));
  if (SUCCEEDED(hr)) {
    auto localImport = existing;
    localImport.pExistingCollection = localCollection.Get();
    hr = pipeline(localImport, finalLibrary, rejected);
    bad += stateobj::mismatch(name, "incompatible_reachable_local_root", SUCCEEDED(hr), 0);
  }

  // Fully resolved collection, but without permission for outside functions to call it.
  D3D12_STATE_OBJECT_CONFIG closed{D3D12_STATE_OBJECT_FLAG_NONE};
  ComPtr<ID3D12StateObject> closedCollection;
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helperLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &finalLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &closed},
                      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root}}, closedCollection,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "closed_collection", uint32_t(hr), uint32_t(S_OK));
  if (SUCCEEDED(hr)) {
    auto closedImport = existing;
    closedImport.pExistingCollection = closedCollection.Get();
    hr = pipeline(closedImport, finalLibrary, rejected);
    bad += stateobj::mismatch(name, "external_dependency_without_flag", SUCCEEDED(hr), 0);
  }
  D3D12_STATE_OBJECT_CONFIG open{D3D12_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS};
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helperLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &finalLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &open}}, rejected,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "closed_collection_requires_resource_root", uint32_t(hr), uint32_t(E_INVALIDARG));
  ComPtr<ID3D12StateObject> resolvedCollection;
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &helperLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &finalLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &open},
                      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root}}, resolvedCollection,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "resolved_collection", uint32_t(hr), uint32_t(S_OK));
  if (SUCCEEDED(hr)) {
    auto resolvedImport = existing;
    resolvedImport.pExistingCollection = resolvedCollection.Get();
    hr = pipeline(resolvedImport, hiddenProvider, object);
    bad += stateobj::mismatch(name, "internal_hidden_dependency", uint32_t(hr), uint32_t(S_OK));
    if (SUCCEEDED(hr)) {
      runner.raygen(object.Get(), L"LinkRayGen");
      const auto words = runner.trace(name, object.Get(), 0, std::vector<stateobj::LaneRay>(4));
      for (UINT lane = 0; lane < 4; ++lane) {
        bad += stateobj::mismatch(name, "internal_hidden_gpu_value", words[lane], 0x2320u + lane * 3u, lane);
        bad += stateobj::mismatch(name, "internal_hidden_root_resource", words[21 + lane], 0xabc005u + lane, lane);
        bad += stateobj::mismatch(name, "internal_hidden_float_overload", words[4 + lane], floatExpected[lane], lane);
      }
    }
  }
  D3D12_EXPORT_DESC directName{L"CollectionValue", L"BaseValue", D3D12_EXPORT_FLAG_NONE};
  auto mixedHelper = helperLibrary;
  mixedHelper.pExports = &directName;
  ComPtr<ID3D12StateObject> mixedCollection;
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &consumerLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &mixedHelper},
                      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred},
                      {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root}}, mixedCollection,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "mixed_rt_collection", uint32_t(hr), uint32_t(S_OK));
  if (SUCCEEDED(hr)) {
    D3D12_EXPORT_DESC selectedRaygen{L"CollectedRayGen", L"LinkRayGen", D3D12_EXPORT_FLAG_NONE};
    D3D12_EXISTING_COLLECTION_DESC mixedImport{mixedCollection.Get(), 1, &selectedRaygen};
    hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &mixedImport},
                        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &finalLibrary},
                        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &root},
                        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
                        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}}, object);
    bad += stateobj::mismatch(name, "mixed_rt_hidden_closure", uint32_t(hr), uint32_t(S_OK));
    if (SUCCEEDED(hr)) {
      runner.raygen(object.Get(), L"CollectedRayGen");
      const auto words = runner.trace(name, object.Get(), 0, std::vector<stateobj::LaneRay>(4));
      for (UINT lane = 0; lane < 4; ++lane) {
        bad += stateobj::mismatch(name, "mixed_rt_uint_overload", words[lane], 0x2320u + lane * 3u, lane);
        bad += stateobj::mismatch(name, "mixed_rt_float_overload", words[4 + lane], floatExpected[lane], lane);
        bad += stateobj::mismatch(name, "mixed_rt_helper_resource", words[21 + lane], 0xabc005u + lane, lane);
      }
    }
  }
  // Imported named roots keep aliases and filters independently of their functions.
  D3D12_EXPORT_DESC rootExports[] = {bridge, {L"BridgeRoot", L"LinkGlobalRoot", D3D12_EXPORT_FLAG_NONE}};
  auto rootedLibrary = helperLibrary;
  rootedLibrary.NumExports = 2;
  rootedLibrary.pExports = rootExports;
  ComPtr<ID3D12StateObject> rootedCollection;
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &rootedLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred}}, rootedCollection,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "embedded_root_collection", uint32_t(hr), uint32_t(S_OK));
  const WCHAR *raygenNames[] = {L"LinkRayGen"};
  D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION namedRoot{L"ImportedRoot", 1, raygenNames};
  if (SUCCEEDED(hr)) {
    D3D12_EXPORT_DESC selected[] = {published, {L"ImportedRoot", L"BridgeRoot", D3D12_EXPORT_FLAG_NONE}};
    D3D12_EXISTING_COLLECTION_DESC rootedImport{rootedCollection.Get(), 2, selected};
    auto rootPipeline = [&](const D3D12_EXISTING_COLLECTION_DESC &imports,
                            const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION &association,
                            ComPtr<ID3D12StateObject> &result) {
      return runner.create({{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &imports},
                            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &consumerLibrary},
                            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &finalLibrary},
                            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &association},
                            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
                            {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}}, result);
    };
    hr = rootPipeline(rootedImport, namedRoot, object);
    bad += stateobj::mismatch(name, "embedded_root_renamed_pipeline", uint32_t(hr), uint32_t(S_OK));
    if (SUCCEEDED(hr)) {
      runner.raygen(object.Get(), L"LinkRayGen");
      const auto words = runner.trace(name, object.Get(), 0, std::vector<stateobj::LaneRay>(4));
      for (UINT lane = 0; lane < 4; ++lane) {
        bad += stateobj::mismatch(name, "renamed_root_uint", words[lane], 0x2320u + lane * 3u, lane);
        bad += stateobj::mismatch(name, "renamed_root_float", words[4 + lane], floatExpected[lane], lane);
        bad += stateobj::mismatch(name, "renamed_root_resource", words[21 + lane], 0xabc005u + lane, lane);
      }
    }
    auto hiddenRoot = namedRoot;
    hiddenRoot.SubobjectToAssociate = L"BridgeRoot";
    hr = rootPipeline(rootedImport, hiddenRoot, rejected);
    bad += stateobj::mismatch(name, "root_original_name_hidden", uint32_t(hr), uint32_t(E_INVALIDARG));
    auto omitted = rootedImport;
    omitted.NumExports = 1;
    hr = rootPipeline(omitted, namedRoot, rejected);
    bad += stateobj::mismatch(name, "root_omitted_by_filter", uint32_t(hr), uint32_t(E_INVALIDARG));
    selected[1].Name = L"CollectionValue";
    hr = rootPipeline(rootedImport, namedRoot, rejected);
    bad += stateobj::mismatch(name, "root_function_namespace_collision", uint32_t(hr), uint32_t(E_INVALIDARG));

    rootExports[0] = directName;
    ComPtr<ID3D12StateObject> allRootCollection;
    hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &rootedLibrary},
                        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred}}, allRootCollection,
                       D3D12_STATE_OBJECT_TYPE_COLLECTION);
    bad += stateobj::mismatch(name, "all_root_collection", uint32_t(hr), uint32_t(S_OK));
    if (SUCCEEDED(hr)) {
      D3D12_EXISTING_COLLECTION_DESC allImports{allRootCollection.Get(), 0, nullptr};
      hr = rootPipeline(allImports, hiddenRoot, object);
      bad += stateobj::mismatch(name, "all_root_pipeline", uint32_t(hr), uint32_t(S_OK));
      if (SUCCEEDED(hr)) {
        runner.raygen(object.Get(), L"LinkRayGen");
        const auto words = runner.trace(name, object.Get(), 0, std::vector<stateobj::LaneRay>(4));
        for (UINT lane = 0; lane < 4; ++lane) {
          bad += stateobj::mismatch(name, "all_root_uint", words[lane], 0x2320u + lane * 3u, lane);
          bad += stateobj::mismatch(name, "all_root_float", words[4 + lane], floatExpected[lane], lane);
          bad += stateobj::mismatch(name, "all_root_resource", words[21 + lane], 0xabc005u + lane, lane);
        }
      }
    }
  }

  rootExports[0] = directName;
  namedRoot.SubobjectToAssociate = L"BridgeRoot";
  ComPtr<ID3D12StateObject> rootedMixed;
  hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &consumerLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &rootedLibrary},
                      {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &deferred},
                      {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &namedRoot}}, rootedMixed,
                     D3D12_STATE_OBJECT_TYPE_COLLECTION);
  bad += stateobj::mismatch(name, "mixed_embedded_root_collection", uint32_t(hr), uint32_t(S_OK));
  if (SUCCEEDED(hr)) {
    D3D12_EXPORT_DESC selected[] = {{L"CollectedRootRayGen", L"LinkRayGen", D3D12_EXPORT_FLAG_NONE},
                                   {L"ImportedMixedRoot", L"BridgeRoot", D3D12_EXPORT_FLAG_NONE}};
    D3D12_EXISTING_COLLECTION_DESC mixedImport{rootedMixed.Get(), 2, selected};
    const WCHAR *finalNames[] = {L"FinalValue"};
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION finalRoot{L"ImportedMixedRoot", 1, finalNames};
    hr = runner.create({{D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &mixedImport},
                        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &finalLibrary},
                        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &finalRoot},
                        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig},
                        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig}}, object);
    bad += stateobj::mismatch(name, "mixed_embedded_root_pipeline", uint32_t(hr), uint32_t(S_OK));
    if (SUCCEEDED(hr)) {
      runner.raygen(object.Get(), L"CollectedRootRayGen");
      const auto words = runner.trace(name, object.Get(), 0, std::vector<stateobj::LaneRay>(4));
      for (UINT lane = 0; lane < 4; ++lane) {
        bad += stateobj::mismatch(name, "mixed_root_uint", words[lane], 0x2320u + lane * 3u, lane);
        bad += stateobj::mismatch(name, "mixed_root_float", words[4 + lane], floatExpected[lane], lane);
        bad += stateobj::mismatch(name, "mixed_root_resource", words[21 + lane], 0xabc005u + lane, lane);
      }
    }
  }
  return stateobj::reportCase(name, bad);
}

} // namespace linkage
