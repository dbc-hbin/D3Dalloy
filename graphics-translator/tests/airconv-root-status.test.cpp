#include "dxbc_converter.hpp"
#include "dxbc_root_signature.hpp"
#include "airconv_context.hpp"
#include <llvm/IR/Verifier.h>
#include <cstddef>
#include <cstdio>

int main() {
  using namespace dxmt;
  using namespace dxmt::dxbc;
  using namespace llvm;
  unsigned cases = 0;
  bool okay = true;
  for (bool uav : {false, true}) {
    for (unsigned count : {1u, 2u}) {
      for (bool reverse : {false, true}) {
        if (count == 1 && reverse) continue;
        for (bool heaps : {false, true}) {
          for (unsigned visible = 0; visible < count; ++visible) {
            struct Root {
              RawRootSignatureDesc header;
              RawRootParameter parameters[2];
              D3D12_ROOT_DESCRIPTOR1 descriptors[2];
            } root{};
            root.header = {D3D_ROOT_SIGNATURE_VERSION_1_1, count, offsetof(Root, parameters), 0, 0,
                           heaps ? kRootSignatureFlagsDirectlyIndexed : 0u};
            const auto kind = uav ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_SRV;
            for (unsigned i = 0; i < count; ++i) {
              const auto visibility = i == unsigned(reverse) ? D3D12_SHADER_VISIBILITY_VERTEX
                                                            : D3D12_SHADER_VISIBILITY_PIXEL;
              root.parameters[i] = {uint32_t(kind), uint32_t(visibility),
                  uint32_t(offsetof(Root, descriptors) + i * sizeof(D3D12_ROOT_DESCRIPTOR1))};
              root.descriptors[i] = {7, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
            }
            ShaderInfo shader;
            if (uav) {
              auto &info = shader.uavMap[0];
              info.range = {0, 7, 1, 3};
              info.resource_type = shader::common::ResourceType::NonApplicable;
              info.scaler_type = shader::common::ScalerDataType::Uint;
              info.read = true;
            } else {
              auto &info = shader.srvMap[0];
              info.range = {0, 7, 1, 3};
              info.resource_type = shader::common::ResourceType::NonApplicable;
              info.scaler_type = shader::common::ScalerDataType::Uint;
              info.read = true;
            }
            LLVMContext context;
            context.setOpaquePointers(false);
            Module module("root-status-regression", context);
            initializeModule(module);
            dxmt::air::FunctionSignatureBuilder signature;
            const bool vertex = root.parameters[visible].ShaderVisibility == D3D12_SHADER_VISIBILITY_VERTEX;
            auto map = setup_binding_rootsig(&shader, signature, module,
                vertex ? microsoft::D3D10_SB_VERTEX_SHADER : microsoft::D3D10_SB_PIXEL_SHADER,
                &root, sizeof(root), 16, 17, nullptr, 0, false);
            if (!map) return 1;
            auto *function = Function::Create(FunctionType::get(Type::getVoidTy(context),
                {Type::getInt8PtrTy(context, 2)}, false), GlobalValue::ExternalLinkage, "probe", module);
            IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", function));
            llvm::air::AIRBuilder air(builder, errs());
            map->SetGlobalRootArgumentPointer(function->getArg(0));
            auto descriptor = uav ? map->GetUAVBuffer(air, 0, builder.getInt32(7))
                                  : map->GetSRVBuffer(air, 0, builder.getInt32(7));
            if (!descriptor) return 1;
            // Inspect the generated address, not the mapper's internal bookkeeping.
            auto offset = [&](Value *value) -> uint64_t {
              auto *load = dyn_cast_or_null<LoadInst>(value);
              auto *gep = load ? dyn_cast<GetElementPtrInst>(load->getPointerOperand()) : nullptr;
              APInt bytes(64, 0);
              if (!gep || !gep->accumulateConstantOffset(module.getDataLayout(), bytes)) return UINT64_MAX;
              return bytes.getZExtValue();
            };
            const auto data = offset(descriptor->Pointer);
            const auto status = offset(descriptor->StatusBase);
            const uint64_t expected_data = visible * sizeof(uint64_t);
            const uint64_t expected_status = (count + (heaps ? 4 : 0) + visible) * sizeof(uint64_t);
            const bool pass = data == expected_data && status == expected_status;
            okay &= pass;
            builder.CreateRetVoid();
            if (verifyModule(module, &errs())) return 1;
            ++cases;
            std::printf("ROOT_STATUS %s count=%u reverse=%u heaps=%u stage=%s data=%llu/%llu status=%llu/%llu %s\n",
                uav ? "UAV" : "SRV", count, unsigned(reverse), unsigned(heaps), vertex ? "VS" : "PS",
                (unsigned long long)data, (unsigned long long)expected_data,
                (unsigned long long)status, (unsigned long long)expected_status, pass ? "PASS" : "FAIL");
          }
        }
      }
    }
  }
  std::printf("ROOT_STATUS_%s cases=%u\n", okay ? "PASS" : "FAIL", cases);
  return okay ? 0 : 1;
}
