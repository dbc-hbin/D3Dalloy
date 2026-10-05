// HitObject traversal, value lifetime, deferred invocation and query snapshots.
namespace hitobject {
int run(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil,
        contract::Runner &geometry) {
  stateobj::Runner runner(device5, ctx, dxil, geometry);
  auto scene = stateobj::buildScene(runner);
  auto library = runner.library();
  ComPtr<ID3D12StateObject> object;
  check(runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object), "hitobject: pipeline");
  runner.raygen(object.Get(), L"HoRayGen");
  runner.hitGroup(0, object.Get(), L"HoGroup", 10);
  runner.hitGroup(1, object.Get(), L"HoGroup", 20);
  runner.hitGroup(2, object.Get(), L"HoProcGroup", 30);
  runner.miss(0, object.Get(), L"HoMiss", 100);
  runner.miss(1, object.Get(), L"HoMiss", 200);
  const auto words = runner.trace("hitobject", object.Get(), scene.tlas->GetGPUVirtualAddress(),
                                  std::vector<stateobj::LaneRay>(4));
  const std::array<uint32_t, 32> expected = {
    1, 1, 1, 1, 20, contract::bits(0.25f), contract::bits(2.0f), 0x40,
    0, 1, 41, 51, 1, 1, 251, 251, 1, 0, 1, 0, 251, 1, 271, 1,
    contract::bits(2.25f), contract::bits(1.0f), contract::bits(1.0f), contract::bits(1.0f), 0, 0, 0xff, 0};
  int bad = 0;
  for (UINT lane = 0; lane < 4; ++lane)
    for (UINT word = 0; word < expected.size(); ++word)
      bad += stateobj::mismatch("hitobject", "snapshot", words[lane * 32 + word], expected[word], lane, word);
  runner.raygen(object.Get(), L"HoProcRayGen");
  const auto procedural = runner.trace("hitobject-procedural", object.Get(), scene.tlas->GetGPUVirtualAddress(),
                                      std::vector<stateobj::LaneRay>(4));
  const std::array<uint32_t, 13> proceduralExpected = {1, 7, contract::bits(0.5f), contract::bits(0.75f), 2, 60,
    1, 123, contract::bits(0.125f), contract::bits(0.25f), contract::bits(1.5f), 1, 90};
  for (UINT lane = 0; lane < 4; ++lane)
    for (UINT word = 0; word < proceduralExpected.size(); ++word)
      bad += stateobj::mismatch("hitobject", "procedural-snapshot", procedural[lane * 32 + word],
                                proceduralExpected[word], lane, word);
  runner.raygen(object.Get(), L"HoRayStateRayGen");
  const auto state = runner.trace("hitobject-raystate", object.Get(), scene.tlas->GetGPUVirtualAddress(),
                                 std::vector<stateobj::LaneRay>(4));
  const std::array<uint32_t, 24> stateExpected = {1, 8, contract::bits(0.125f), contract::bits(1.0f),
    contract::bits(2.25f), contract::bits(0.25f), contract::bits(-1.0f), 0, 0xff,
    contract::bits(0.25f), contract::bits(0.25f), 0, 1, 1, contract::bits(100.0f), 101, 201, 8,
    contract::bits(0.125f), contract::bits(-10.0f), contract::bits(0.25f), contract::bits(-1.0f),
    contract::bits(1.0f), contract::bits(100.0f)};
  for (UINT lane = 0; lane < 4; ++lane)
    for (UINT word = 0; word < stateExpected.size(); ++word)
      bad += stateobj::mismatch("hitobject", "ray-state", state[lane * 32 + word], stateExpected[word], lane, word);
  runner.raygen(object.Get(), L"HoAggregateRayGen");
  const auto aggregate = runner.trace("hitobject-aggregate", object.Get(), 0, std::vector<stateobj::LaneRay>(4));
  for (UINT lane = 0; lane < 4; ++lane)
    bad += stateobj::mismatch("hitobject", "aggregate-value", aggregate[lane], lane & 1 ? 12 : 28, lane);
  const int originalFailed = stateobj::reportCase("hitobject", bad);

  constexpr UINT coherenceLanes = 64, coherenceStride = 8;
  static_assert(coherenceLanes * coherenceStride <= stateobj::kOutputWords);
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 wave{};
  check(device5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &wave, sizeof(wave)), "hitobject: wave maximum");
  require(wave.WaveLaneCountMax && wave.WaveLaneCountMax < coherenceLanes, "hitobject: coherence requires multiple waves");
  runner.raygen(object.Get(), L"HoCoherenceRayGen");
  runner.hitGroup(1, object.Get(), L"HoCoherenceGroup", 0x35);
  runner.miss(0, object.Get(), L"HoCoherenceMiss", 0x73);
  // This raygen constructs rays itself. Reuse the existing512-word UAV without
  // enlarging the common16-entry input-ray table or any legacy fixture resource.
  const auto coherence = runner.run("hitobject-reorder-coherence", scene.tlas->GetGPUVirtualAddress(),
    std::vector<stateobj::LaneRay>(1), [&](ID3D12GraphicsCommandList4 *list, D3D12_DISPATCH_RAYS_DESC dispatch) {
      dispatch.Width = coherenceLanes;
      list->SetPipelineState1(object.Get());
      list->DispatchRays(&dispatch);
    });
  int coherenceBad = 0;
  for (UINT lane = 0; lane < coherenceLanes; ++lane) {
    const uint32_t seed = 0x43210000u + lane * 0x105u;
    const uint32_t payloadBefore = seed ^ 0x13579bdfu;
    const uint32_t tag = lane & 1 ? 0x73u : 0x35u;
    const uint32_t second = seed * 7u + 0x10203040u;
    const uint32_t produced = (seed ^ 0x00ff00ffu) + (second ^ 0x55aa55aau) + (payloadBefore + tag) * 3u;
    const uint32_t payloadAfter = produced ^ 0x2468ace0u;
    const uint32_t post = payloadAfter + 0x6a09e667u;
    const std::array<uint32_t, coherenceStride> expectedCoherence = {
      seed, post, payloadBefore, payloadBefore, payloadAfter, second, post ^ 0xbb67ae85u, lane & 1 ? 2u : 1u};
    for (UINT word = 0; word < coherenceStride; ++word)
      coherenceBad += stateobj::mismatch("hitobject-reorder-coherence", "same-dispatch-index-visibility",
        coherence[lane * coherenceStride + word], expectedCoherence[word], lane, word);
    std::printf("OWNED_D3D12_RAYTRACING_HITOBJECT_COHERENCE_LANE lane=%u words=(", lane);
    for (UINT word = 0; word < coherenceStride; ++word)
      std::printf("%s0x%08x", word ? "," : "", coherence[lane * coherenceStride + word]);
    std::printf(")\n");
  }
  std::printf("OWNED_D3D12_RAYTRACING_HITOBJECT_COHERENCE lanes=64 words=512 scope=reorder wave_max=%u status=%s\n",
    wave.WaveLaneCountMax, coherenceBad ? "FAIL" : "PASS");
  return originalFailed | stateobj::reportCase("hitobject-reorder-coherence", coherenceBad);
}
int runAtomic(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil,
              contract::Runner &geometry) {
  stateobj::Runner runner(device5, ctx, dxil, geometry, sizeof(uint64_t));
  auto scene = stateobj::buildScene(runner);
  auto library = runner.library();
  ComPtr<ID3D12StateObject> object;
  check(runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object), "atomic64-rt: pipeline");
  runner.raygen(object.Get(), L"HoRayGen");
  runner.hitGroup(1, object.Get(), L"HoGroup", 0x35);
  runner.miss(0, object.Get(), L"HoMiss", 0x73);
  // Use an otherwise unreferenced record in the existing upload table. Its
  // actual dispatch table is callable, not a miss table or a synthetic fallback.
  constexpr UINT callableOffset = stateobj::kMissOffset + 3 * stateobj::kRecordStride;
  runner.writeRecord(callableOffset, object.Get(), L"HoCallable", 0x91);
  constexpr UINT lanes = 64, stride = 8;
  static_assert(lanes * stride == stateobj::kOutputWords);
  const auto words = runner.run("atomic64-rt", scene.tlas->GetGPUVirtualAddress(),
    std::vector<stateobj::LaneRay>(1), [&](ID3D12GraphicsCommandList4 *list, D3D12_DISPATCH_RAYS_DESC dispatch) {
      dispatch.Width = lanes;
      dispatch.CallableShaderTable = {dispatch.MissShaderTable.StartAddress +
        callableOffset - stateobj::kMissOffset, stateobj::kRecordStride, stateobj::kRecordStride};
      list->SetPipelineState1(object.Get());
      list->DispatchRays(&dispatch);
    });
  int bad = 0;
  for (UINT lane = 0; lane < lanes; ++lane) {
    const uint64_t seed = (uint64_t(0x24680000u + lane) << 32) | (0xfedc0000u + lane);
    const uint32_t tag = lane & 1 ? 0x73u : 0x35u;
    const uint64_t beforeStage = seed + UINT64_C(0x200000021);
    const uint64_t beforeCallable = beforeStage + UINT64_C(0x100000007) + tag;
    const uint64_t afterCallable = beforeCallable ^ UINT64_C(0x0102030405060708);
    const uint32_t payload = ((lane * 3u + 5u) * 3u) ^ uint32_t(beforeStage) ^ tag ^
      uint32_t(beforeCallable) ^ 0x91u;
    const std::array<uint64_t, 4> expected = {
      afterCallable, seed, afterCallable, (uint64_t(payload) << 32) | (lane & 1 ? 2u : 1u)};
    for (UINT wide = 0; wide < expected.size(); ++wide) {
      bad += stateobj::mismatch("atomic64-rt", "returning-rmw-cas-low", words[lane * stride + wide * 2],
        uint32_t(expected[wide]), lane, wide * 2);
      bad += stateobj::mismatch("atomic64-rt", "returning-rmw-cas-high", words[lane * stride + wide * 2 + 1],
        uint32_t(expected[wide] >> 32), lane, wide * 2 + 1);
    }
    std::printf("OWNED_D3D12_RAYTRACING_ATOMIC64_LANE lane=%u words=(", lane);
    for (UINT word = 0; word < stride; ++word)
      std::printf("%s0x%08x", word ? "," : "", words[lane * stride + word]);
    std::printf(")\n");
  }
  std::printf("OWNED_D3D12_RAYTRACING_ATOMIC64 lanes=64 words=512 stages=raygen,closesthit,miss,callable status=%s\n",
    bad ? "FAIL" : "PASS");
  runner.raygen(object.Get(), L"ExtRayGen");
  runner.writeRecord(stateobj::kHitGroupOffset + stateobj::kRecordStride, object.Get(), L"ExtGroup", 0x35, true);
  runner.writeRecord(stateobj::kHitGroupOffset + 2 * stateobj::kRecordStride, object.Get(), L"ExtProcGroup", 0x57, true);
  runner.miss(0, object.Get(), L"ExtMiss", 0x73, true);
  runner.writeRecord(callableOffset, object.Get(), L"ExtCallable", 0x91, true);
  D3D12_INDIRECT_ARGUMENT_DESC argument{};
  argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
  const D3D12_COMMAND_SIGNATURE_DESC signatureDesc{sizeof(D3D12_DISPATCH_RAYS_DESC), 1, &argument, 0};
  ComPtr<ID3D12CommandSignature> signature;
  check(runner.device()->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&signature)),
    "atomic64-rt: indirect signature");
  constexpr UINT extendedLanes = 24, extendedStride = 20;
  for (UINT mode = 0; mode < 2; ++mode) {
    auto dispatch = runner.dispatchDesc(extendedLanes);
    dispatch.CallableShaderTable = {dispatch.MissShaderTable.StartAddress + callableOffset - stateobj::kMissOffset,
      stateobj::kRecordStride, stateobj::kRecordStride};
    auto commands = geometry.upload(&dispatch, sizeof(dispatch));
    const auto extended = runner.run("atomic64-rt-all-stages", scene.tlas->GetGPUVirtualAddress(),
      std::vector<stateobj::LaneRay>(1), [&](ID3D12GraphicsCommandList4 *list, D3D12_DISPATCH_RAYS_DESC) {
        list->SetPipelineState1(object.Get());
        if (mode) list->ExecuteIndirect(signature.Get(), 1, commands.Get(), 0, nullptr, 0);
        else list->DispatchRays(&dispatch);
      });
    std::array<uint64_t, extendedLanes> tickets{};
    int extendedBad = 0;
    for (UINT lane = 0; lane < extendedLanes; ++lane) {
      const UINT kind = lane % 3;
      const uint64_t seed = (uint64_t(0x24680000u + lane) << 32) | (0xfedc0000u + lane);
      uint64_t value = seed + UINT64_C(0x200000021), intersectionOld = 0, anyHitOld = 0;
      if (kind == 1) { intersectionOld = value; value += UINT64_C(0x400000013); }
      if (kind != 2) { anyHitOld = value; value += UINT64_C(0x800000017); }
      const uint64_t stageOld = value;
      const uint32_t tag = kind == 0 ? 0x35u : kind == 1 ? 0x57u : 0x73u;
      value += (kind == 2 ? UINT64_C(0x300000009) : UINT64_C(0x100000007)) + tag;
      const uint64_t callableOld = value;
      value ^= UINT64_C(0x0102030405060708);
      const uint64_t payload = UINT64_C(0x1000091) + (kind == 2 ? 0x20000 : 0x10001);
      const std::array<uint64_t, 10> expected = {
        value, seed, anyHitOld, intersectionOld, stageOld, callableOld, 0, kind == 2 ? 2u : 1u, payload, tag};
      for (UINT wide = 0; wide < expected.size(); ++wide) {
        const UINT word = lane * extendedStride + wide * 2;
        const uint64_t actual = uint64_t(extended[word]) | (uint64_t(extended[word + 1]) << 32);
        if (wide == 6) tickets[lane] = actual;
        else {
          extendedBad += stateobj::mismatch("atomic64-rt-all-stages", "stage-once-old-low", extended[word],
            uint32_t(expected[wide]), lane, wide * 2);
          extendedBad += stateobj::mismatch("atomic64-rt-all-stages", "stage-once-old-high", extended[word + 1],
            uint32_t(expected[wide] >> 32), lane, wide * 2 + 1);
        }
      }
      std::printf("OWNED_D3D12_RAYTRACING_ATOMIC64_EXT_LANE mode=%u lane=%u words=(", mode, lane);
      for (UINT word = 0; word < extendedStride; ++word)
        std::printf("%s0x%08x", word ? "," : "", extended[lane * extendedStride + word]);
      std::printf(")\n");
    }
    std::sort(tickets.begin(), tickets.end());
    std::array<uint64_t, extendedLanes> expectedTickets{};
    for (UINT lane = 0; lane < extendedLanes; ++lane)
      expectedTickets[lane] = UINT64_MAX + uint64_t(lane) * UINT64_C(0x100000001);
    std::sort(expectedTickets.begin(), expectedTickets.end());
    extendedBad += tickets != expectedTickets;
    const uint64_t shared = uint64_t(extended[480]) | (uint64_t(extended[481]) << 32);
    const uint64_t stageCount = uint64_t(extended[482]) | (uint64_t(extended[483]) << 32);
    extendedBad += shared != UINT64_MAX + uint64_t(extendedLanes) * UINT64_C(0x100000001);
    extendedBad += stageCount != 95; // Fill UINT64_MAX plus exactly96 stage invocations.
    std::printf("OWNED_D3D12_RAYTRACING_ATOMIC64_EXT_SHARED mode=%u words=(0x%08x,0x%08x,0x%08x,0x%08x)\n",
      mode, extended[480], extended[481], extended[482], extended[483]);
    std::printf("OWNED_D3D12_RAYTRACING_ATOMIC64_EXT mode=%u lanes=24 words=484 local=cbv stages=96 tickets=24 status=%s\n",
      mode, extendedBad ? "FAIL" : "PASS");
    bad += extendedBad;
  }
  if (!bad) std::puts("OWNED_D3D12_RAYTRACING_ATOMIC64_PASS");
  return stateobj::reportCase("atomic64-rt", bad);
}
} // namespace hitobject
