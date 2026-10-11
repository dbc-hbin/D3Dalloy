// One PAQ dispatch reuses the existing public DXR scene and fence/readback harness.
namespace paq {
int run(ID3D12Device5 *device5, Context &ctx, const std::vector<uint8_t> &dxil,
        contract::Runner &geometry) {
  stateobj::Runner runner(device5, ctx, dxil, geometry);
  auto scene = stateobj::buildScene(runner);
  auto library = runner.library();
  ComPtr<ID3D12StateObject> object;
  check(runner.create({{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library}}, object), "paq: pipeline");
  runner.raygen(object.Get(), L"PaqRayGen");
  runner.hitGroup(0, object.Get(), L"PaqGroup", 0);
  runner.hitGroup(1, object.Get(), L"PaqGroup", 0);
  runner.miss(0, object.Get(), L"PaqMiss", 0);
  runner.miss(1, object.Get(), L"PaqMissAlt", 0);
  const auto words = runner.trace("payload-access-qualifiers", object.Get(), scene.tlas->GetGPUVirtualAddress(),
                                  std::vector<stateobj::LaneRay>(6));
  const std::array<uint32_t, 6> values = {111, 102, 112, 111, 131, 110};
  int bad = 0;
  for (UINT lane = 0; lane < values.size(); ++lane) {
    const UINT base = lane * 16;
    bad += stateobj::mismatch("payload-access-qualifiers", "denied-stage-write", words[base], 0, lane);
    bad += stateobj::mismatch("payload-access-qualifiers", "permitted-stage-transfer", words[base + 1], values[lane], lane);
    bad += stateobj::mismatch("payload-access-qualifiers", "nested-state", words[base + 2], lane == 4 ? 1 : 0, lane);
    if (lane != 1) {
      bad += stateobj::mismatch("payload-access-qualifiers", "anyhit-working-copy", words[base + 4], 12, lane);
      bad += stateobj::mismatch("payload-access-qualifiers", "anyhit-read", words[base + 5], 7, lane);
    }
    if (lane != 5) {
      bad += stateobj::mismatch("payload-access-qualifiers", "stage-working-copy", words[base + 6],
                                lane == 1 || lane == 2 || lane == 4 ? 2 : 1, lane);
      bad += stateobj::mismatch("payload-access-qualifiers", "denied-anyhit-write", words[base + 7], 7, lane);
    }
    if (lane == 4)
      bad += stateobj::mismatch("payload-access-qualifiers", "nested-caller-working-value", words[base + 8], 1, lane);
    for (UINT word = 0; word < 16; ++word) {
      const bool written = word <= 2 || (lane != 1 && (word == 4 || word == 5)) ||
                           (lane != 5 && (word == 6 || word == 7)) || (lane == 4 && word == 8);
      if (!written)
        bad += stateobj::mismatch("payload-access-qualifiers", "unwritten-output", words[base + word], stateobj::kFill, lane, word);
    }
  }
  for (UINT word = 6 * 16; word < words.size(); ++word)
    bad += stateobj::mismatch("payload-access-qualifiers", "output-guard", words[word], stateobj::kFill, 0, word);
  return stateobj::reportCase("payload-access-qualifiers", bad);
}
} // namespace paq
