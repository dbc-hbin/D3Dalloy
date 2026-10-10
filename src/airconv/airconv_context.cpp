#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/InferAddressSpaces.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO/DeadArgumentElimination.h"
#include "llvm/Transforms/IPO/GlobalOpt.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/IPO/Annotation2Metadata.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/InferFunctionAttrs.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/InstIterator.h"

#include "airconv_context.hpp"

#include "air_msad.h"
#include "air_samplepos.h"
#include "air_tessellation.h"
#include "air_denorm.h"
#include "air_fp64.h"
#include "air_atomic64.h"
#include "air_sampler_feedback.h"
#include "air_conservative.h"

#include "transforms/lower_16bit_texread.hpp"
#include "transforms/simdgroup_implicit_membarrier.hpp"

using namespace llvm;

namespace dxmt {

void initializeModule(llvm::Module &M) {
  M.setSourceFileName("airconv_generated.metal");
  M.setTargetTriple("air64-apple-macosx14.0.0");
  M.setDataLayout(
    "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:"
    "64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-"
    "v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:"
    "16:32"
  );
  M.setSDKVersion(VersionTuple(15, 0));
  M.addModuleFlag(Module::ModFlagBehavior::Error, "wchar_size", 4);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "frame-pointer", 2);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_device_buffers", 31);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_constant_buffers", 31);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_threadgroup_buffers", 31
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_textures", 128);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_read_write_textures", 8
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_samplers", 16);

};

static std::atomic_flag llvm_overwrite = false;

void
runOptimizationPasses(llvm::Module &M) {
  // Only D3D12 root-bound shaders define null-descriptor reads as zero. D3D11
  // modules never enter this path. Guard native AIR accesses, not their results:
  // selecting zero after a query/read/sample still touches a null Metal texture.
  if (M.getModuleFlag("dxmt.d3d12.root-signature")) {
    SmallVector<CallInst *, 16> accesses;
    for (auto &function : M) for (auto &instruction : instructions(function)) {
      auto *call = dyn_cast<CallInst>(&instruction);
      auto *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || !callee->isDeclaration() || !callee->getName().startswith("air.") ||
          callee->getName().startswith("air.atomic_") || call->arg_empty())
        continue;
      auto *pointer = dyn_cast<PointerType>(call->getArgOperand(0)->getType());
      auto *type = pointer && !pointer->isOpaque() && pointer->getAddressSpace() == 1
          ? dyn_cast<StructType>(pointer->getNonOpaquePointerElementType()) : nullptr;
      // AIRBuilder::getTextureHandleType names the opaque native texture/depth
      // types this way. Raw buffers, phase metadata and workgraph pointers are not textures.
      if (type && type->isOpaque() && type->hasName() && type->getName().endswith("_t") &&
          (type->getName().startswith("struct._texture_") || type->getName().startswith("struct._depth_")))
        accesses.push_back(call);
    }
    for (auto *call : accesses) {
      IRBuilder<> builder(call);
      Value *present = builder.CreateIsNotNull(call->getArgOperand(0));
      for (auto &argument : call->args()) {
        auto *pointer = dyn_cast<PointerType>(argument->getType());
        auto *type = pointer && !pointer->isOpaque() && pointer->getAddressSpace() == 2
            ? dyn_cast<StructType>(pointer->getNonOpaquePointerElementType()) : nullptr;
        // AIR 2.8 reads use an implicit sampler, not a nullable D3D12 descriptor.
        if (type && type->hasName() && type->getName() == "struct._sampler_t" &&
            !call->getCalledFunction()->getName().startswith("air.read_"))
          present = builder.CreateAnd(present, builder.CreateIsNotNull(argument));
      }
      auto *head = call->getParent();
      auto *then_term = SplitBlockAndInsertIfThen(present, call, false);
      auto *continuation = call->getParent();
      call->moveBefore(then_term);
      if (!call->getType()->isVoidTy()) {
        builder.SetInsertPoint(continuation, continuation->getFirstInsertionPt());
        auto *merged = builder.CreatePHI(call->getType(), 2);
        call->replaceUsesWithIf(merged, [&](Use &use) { return use.getUser() != merged; });
        merged->addIncoming(call, then_term->getParent());
        // AIR residency bit0 denotes a fault; skipped access has no fault.
        merged->addIncoming(Constant::getNullValue(call->getType()), head);
      }
    }
  }
  for (auto &function : M) {
    if (function.isDeclaration() && function.getName().startswith("dxmt.fp64.")) {
      linkFP64(M);
      break;
    }
  }

  if (!llvm_overwrite.test_and_set()) {
    auto Map = cl::getRegisteredOptions();
    auto InfiniteLoopThreshold = Map["instcombine-infinite-loop-threshold"];
    if (InfiniteLoopThreshold) {
      reinterpret_cast<cl::opt<unsigned> *>(InfiniteLoopThreshold)->setValue(1000);
    }
  }

  // Following optimization passes are picked from default LLVM pipelines
  // An unoptimized shader can make PSO compilation take a very long time
  // But a full optimization pipeline is also too heavy
  // So we choose some passes that really matter to run

  // Create the analysis managers.
  // These must be declared in this order so that they are destroyed in the
  // correct order due to inter-analysis-manager references.
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Create the new pass manager builder.
  // Take a look at the PassBuilder constructor parameters for more
  // customization, e.g. specifying a TargetMachine or various debugging
  // options.
  llvm::PassInstrumentationCallbacks PIC;
  PassBuilder PB(nullptr, PipelineTuningOptions(), {}, &PIC);

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // llvm::StandardInstrumentations SI(true);
  // SI.registerCallbacks(PIC, &FAM);

  ModulePassManager MPM;

  // Convert @llvm.global.annotations to !annotation metadata.
  MPM.addPass(Annotation2MetadataPass());

  // Force any function attributes we want the rest of the pipeline to observe.
  MPM.addPass(ForceFunctionAttrsPass());
  // Do basic inference of function attributes from known properties of system
  // libraries and other oracles.
  MPM.addPass(InferFunctionAttrsPass());

  /*
   * Recover the address space of every flat (AS0) address expression that
   * reaches storage in a specific space.
   *
   * The DXIL front end relocates a module-scope immutable global into AIR
   * constant storage (AS2, see dxil::PrepareModuleStorage), but the GEP/load
   * chain the shader already built stays in AS0 and only sees the storage
   * through an addrspacecast. Dereferencing that cast would read constant
   * storage as device memory, so the flat chain has to be folded back onto the
   * storage's own address space - which is exactly what this pass infers: the
   * cast's operand is in AS2, so the cast, the GEPs and the loads that follow
   * are all rewritten into AS2. The DXBC immediate-constant path already emits
   * the AS2 form directly, so both front ends then produce the same addressing.
   *
   * The flat address space is stated explicitly: the pass otherwise asks the
   * target for it, and this pipeline has no target machine, so it would infer
   * nothing and the AS0 chain would stay behind a cast. AS0 is the right flat
   * space here because it is the default address space the DXIL front end
   * builds flat pointers in, and it also holds the private alloca-derived
   * addressing, which the pass leaves alone (join(flat, AS0) = AS0).
   *
   * It has to run before any pass can reshape the chain - InstCombine folds a
   * GEP of an addrspacecast into an addrspacecast of a GEP, which moves the
   * cast to the load instead of removing it - so it is the first thing the
   * pipeline does to the module.
   */
  MPM.addPass(createModuleToFunctionPassAdaptor(InferAddressSpacesPass(0)));

  {
    // Create an early function pass manager to cleanup the output of the
    // frontend.
    FunctionPassManager EarlyFPM;
    // Lower llvm.expect to metadata before attempting transforms.
    // Compare/branch metadata may alter the behavior of passes like SimplifyCFG.
    EarlyFPM.addPass(LowerExpectIntrinsicPass());
    EarlyFPM.addPass(SimplifyCFGPass());
    EarlyFPM.addPass(SROAPass());
    EarlyFPM.addPass(EarlyCSEPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(EarlyFPM), true /* ? */));
  }

  MPM.addPass(AlwaysInlinerPass());
  MPM.addPass(IPSCCPPass());
  MPM.addPass(GlobalOptPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
  MPM.addPass(DeadArgumentEliminationPass());

  {
    // Create a small function pass pipeline to cleanup after all the global
    // optimizations.
    FunctionPassManager GlobalCleanupPM;
    GlobalCleanupPM.addPass(InstCombinePass());

    GlobalCleanupPM.addPass(SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

    GlobalCleanupPM.addPass(air::Lower16BitTexReadPass());
    GlobalCleanupPM.addPass(air::SimdgroupImplicitMemBarrierPass());

    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(GlobalCleanupPM), true /* ? */));
  }

  // MPM.addPass(VerifierPass());

  // Optimize the IR!
  MPM.run(M, MAM);
}

void
removeNamedMetadata(llvm::Module &M, StringRef Name) {
  if (auto MD = M.getNamedMetadata(Name)) {
    M.eraseNamedMetadata(MD);
  }
}

template<size_t N>
void linkShader(llvm::Module &M, const unsigned char (&bitcode)[N]) {
  auto buffer = MemoryBuffer::getMemBufferCopy(StringRef((const char *)bitcode, N));
  Expected<std::unique_ptr<Module>> modOrErr = parseBitcodeFile(buffer->getMemBufferRef(), M.getContext());

  if (!modOrErr) {
    // not expected to see this unless something really bad happened in compile time
    errs() << "Failed to parse air bitcode\n";
    return;
  }

  auto module = std::move(modOrErr.get());

  removeNamedMetadata(*module, "air.compile_options");
  removeNamedMetadata(*module, "air.version");
  removeNamedMetadata(*module, "air.language_version");
  removeNamedMetadata(*module, "air.source_file_name");
  removeNamedMetadata(*module, "llvm.ident");
  removeNamedMetadata(*module, "llvm.module.flags");

  // Helpers belong to this shader module, not each separately linked AIR export.
  llvm::Linker::linkModules(M, std::move(module), Linker::LinkOnlyNeeded,
    [](Module &linked, const StringSet<> &imported) {
      internalizeModule(linked, [&](const GlobalValue &value) {
        return !imported.contains(value.getName());
      });
    });
};

void
linkMSAD(llvm::Module &M) {
  linkShader(M, air_msad);
}

void
linkSamplePos(llvm::Module &M) {
  linkShader(M, air_samplepos);
}

void
linkTessellation(llvm::Module &M) {
  linkShader(M, air_tessellation);
}

void
linkDenorm(llvm::Module &M) {
  linkShader(M, air_denorm);
}

void
linkFP64(llvm::Module &M) {
  linkShader(M, air_fp64);
}

void
linkAtomic64(llvm::Module &M) {
  linkShader(M, air_atomic64);
  // Keep the allocator/reclamation loop shared: inlining it into every user
  // update makes AGX instruction selection grow pathologically on typed shaders.
  if (auto *cas = M.getFunction("dxmt_atomic64_compare_exchange"))
    cas->addFnAttr(llvm::Attribute::NoInline);
}

void
linkSamplerFeedback(llvm::Module &M) {
  linkShader(M, air_sampler_feedback);
}

void
linkConservative(llvm::Module &M) {
  linkShader(M, air_conservative);
}

} // namespace dxmt