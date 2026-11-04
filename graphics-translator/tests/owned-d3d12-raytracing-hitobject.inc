// HitObject traversal, value lifetime, deferred invocation and query snapshots.
namespace hitobject {
void atomicReadbackPhaseMarker(const char *name, const char *action) {
  char line[160];
  const int length = std::snprintf(line, sizeof(line),
      "OWNED_D3D12_ATOMIC64_READBACK_PHASE suite=raytracing case=%s %s\n", name, action);
  require(length > 0 && static_cast<size_t>(length) < sizeof(line), "atomic64 RT readback marker length");
  DWORD written = 0;
  require(WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(length), &written, nullptr) &&
          written == static_cast<DWORD>(length), "atomic64 RT readback marker write");
}

int runFirstHit(stateobj::Runner &runner, D3D12_GPU_VIRTUAL_ADDRESS scene) {
  // Keep the same shader library and inputs for native and explicit-query paths.
  // Selecting exports avoids inheriting the library's ordinary pipeline config.
  D3D12_EXPORT_DESC exports[] = {
    {L"HoFirstRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstDeferredRayGen", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstAnyHit", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstClosestHit", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstMiss", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstIntersection", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstOpaqueGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoFirstRejectGroup", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoGlobalRS", nullptr, D3D12_EXPORT_FLAG_NONE},
    {L"HoShaderConfig", nullptr, D3D12_EXPORT_FLAG_NONE},
  };
  auto library = runner.library(UINT(std::size(exports)), exports);
  int bad = 0;
  for (UINT query = 0; query != 2; ++query) {
    // ALLOW_OPACITY_MICROMAPS selects the query callback engine. The scene has
    // ordinary geometry, so this changes the execution path, not its semantics.
    D3D12_RAYTRACING_PIPELINE_CONFIG1 config{2, static_cast<D3D12_RAYTRACING_PIPELINE_FLAGS>(query ? 0x400 : 0)};
    ComPtr<ID3D12StateObject> object;
    check(runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                         {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &config}}, object),
          "trace-control: pipeline");
    runner.hitGroup(0, object.Get(), L"HoFirstRejectGroup", 0);
    runner.hitGroup(1, object.Get(), L"HoFirstRejectGroup", 0);
    runner.hitGroup(2, object.Get(), L"HoFirstGroup", 0);
    runner.hitGroup(3, object.Get(), L"HoFirstOpaqueGroup", 0);
    runner.miss(0, object.Get(), L"HoFirstMiss", 0);
    for (UINT deferred = 0; deferred != 2; ++deferred) {
      runner.raygen(object.Get(), deferred ? L"HoFirstDeferredRayGen" : L"HoFirstRayGen");
      const char *name = query ? (deferred ? "trace-control-query-hitobject" : "trace-control-query")
                               : (deferred ? "trace-control-native-hitobject" : "trace-control-native");
      const auto words = runner.trace(name, object.Get(), scene, std::vector<stateobj::LaneRay>(8));
      int runBad = 0;
      for (UINT lane = 0; lane != 8; ++lane)
        for (UINT iteration = 0; iteration != 2; ++iteration) {
          const bool miss = lane == 7 || (lane == 4 && iteration == 1);
          const bool rejectFar = lane == 2 || (lane == 3 && iteration == 1) || miss;
          const bool ended = lane == 1 || lane == 2 || lane == 5 || lane == 6 ||
                             (lane == 3 && iteration == 0);
          const bool fartherHit = ended && lane != 2;
          const UINT anyHits = lane == 6 ? 0 : lane == 7 ? iteration : (fartherHit ? 1 : 2);
          const UINT payload = lane | (iteration << 4) | (anyHits << 8);
          const UINT finalPayload = payload | (miss ? 0x20000 : lane == 5 ? 0 : 0x10000);
          const float fraction = fartherHit ? 0.75f : 0.25f;
          const UINT distance = contract::bits(1.0f + 2.0f * iteration + fraction);
          std::array<uint32_t, 16> expected;
          expected.fill(0xffffffffu);
          expected[0] = finalPayload;
          expected[1] = lane == 7 ? 0 : 1;
          expected[2] = lane == 7 || (ended && lane != 2) ? 0 : lane == 2 ? 1 : 3;
          if (!ended && lane != 7) {
            expected[3] = rejectFar ? 0 : 1;
            expected[4] = miss ? 0 : 1;
            expected[12] = contract::bits(rejectFar ? 100.0f : 1.75f + 2.0f * iteration);
            expected[13] = miss ? contract::bits(100.0f) : distance;
          } else if (lane == 2) {
            expected[3] = 0;
            expected[12] = contract::bits(100.0f);
          }
          if (!miss && lane != 5) {
            expected[5] = distance;
            expected[6] = fartherHit ? 91 : 92;
            expected[7] = contract::bits(fraction);
          }
          expected[8] = deferred ? payload : finalPayload;
          expected[9] = deferred ? (miss ? 2 : 1) : 0;
          if (deferred) {
            expected[10] = miss ? contract::bits(100.0f) : distance;
            if (!miss) expected[11] = contract::bits(fraction);
          }
          for (UINT word = 0; word != expected.size(); ++word)
            runBad += stateobj::mismatch(name, "report-order-and-control", words[lane * 32 + iteration * 16 + word],
                                         expected[word], lane, iteration * 16 + word);
        }
      std::array<uint32_t, 256> noHitExpected;
      noHitExpected.fill(0xffffffffu);
      if (deferred) {
        for (UINT kind = 0; kind != 4; ++kind) {
          auto *e = noHitExpected.data() + kind * 64;
          std::fill(e, e + 49, 0u);
          const bool nop = kind == 2, made = kind == 1;
          e[0] = nop ? 0 : made ? 9 : kind == 3 ? 6 : 2;
          e[1] = contract::bits(made ? 0.125f : 0.0f);
          e[2] = contract::bits(nop ? 0.0f : made ? 17.25f : 100.0f);
          e[3] = nop ? 0 : 2;
          const float origin[3] = {nop ? 0.0f : made ? -7.0f : kind == 0 ? 4.25f : 2.25f,
                                   nop ? 0.0f : made ? 0.375f : 0.25f,
                                   nop ? 0.0f : made ? -5.0f : -3.0f};
          const float direction[3] = {made ? 0.125f : 0.0f, made ? -0.25f : 0.0f,
                                      nop ? 0.0f : made ? 1.5f : 1.0f};
          for (UINT i = 0; i != 3; ++i) {
            e[4+i] = e[10+i] = contract::bits(origin[i]);
            e[7+i] = e[13+i] = contract::bits(direction[i]);
            e[16+i*4+i] = e[28+i*4+i] = contract::bits(1.0f);
          }
          e[48] = kind == 0 || kind == 3;
        }
      }
      for (UINT word = 256; word != words.size(); ++word)
        runBad += stateobj::mismatch(name, "miss-nop-state-and-guards", words[word],
                                     noHitExpected[word - 256], 8, word);
      std::printf("OWNED_D3D12_RAYTRACING_TRACE_CONTROL path=%s deferred=%u rays=16 words=512 status=%s\n",
                  query ? "query" : "native", deferred, runBad ? "FAIL" : "PASS");
      bad += runBad;
    }
  }
  return stateobj::reportCase("trace-control", bad);
}

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
  return originalFailed | stateobj::reportCase("hitobject-reorder-coherence", coherenceBad) |
         runFirstHit(runner, scene.tlas->GetGPUVirtualAddress());
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
  atomicReadbackPhaseMarker("base", "begin");
  const auto words = runner.run("atomic64-rt", scene.tlas->GetGPUVirtualAddress(),
    std::vector<stateobj::LaneRay>(1), [&](ID3D12GraphicsCommandList4 *list, D3D12_DISPATCH_RAYS_DESC dispatch) {
      dispatch.Width = lanes;
      dispatch.CallableShaderTable = {dispatch.MissShaderTable.StartAddress +
        callableOffset - stateobj::kMissOffset, stateobj::kRecordStride, stateobj::kRecordStride};
      list->SetPipelineState1(object.Get());
      list->DispatchRays(&dispatch);
    });
  atomicReadbackPhaseMarker("base", "end");
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
  for (UINT mode = 0; mode < 3; ++mode) {
    auto dispatch = runner.dispatchDesc(extendedLanes);
    dispatch.CallableShaderTable = {dispatch.MissShaderTable.StartAddress + callableOffset - stateobj::kMissOffset,
      stateobj::kRecordStride, stateobj::kRecordStride};
    // CallShader(0) still consumes the same record. A broadcast table exposes
    // only its identifier in the first readback and resolves the local CBV next.
    if (mode == 2) dispatch.CallableShaderTable.StrideInBytes = 0;
    auto commands = geometry.upload(&dispatch, sizeof(dispatch));
    const char *phase = mode == 2 ? "extended-indirect-zero-stride" : mode ? "extended-indirect" : "extended-direct";
    atomicReadbackPhaseMarker(phase, "begin");
    const auto extended = runner.run("atomic64-rt-all-stages", scene.tlas->GetGPUVirtualAddress(),
      std::vector<stateobj::LaneRay>(1), [&](ID3D12GraphicsCommandList4 *list, D3D12_DISPATCH_RAYS_DESC) {
        list->SetPipelineState1(object.Get());
        if (mode) list->ExecuteIndirect(signature.Get(), 1, commands.Get(), 0, nullptr, 0);
        else list->DispatchRays(&dispatch);
      });
    atomicReadbackPhaseMarker(phase, "end");
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
