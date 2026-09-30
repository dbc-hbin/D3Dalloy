/*
 * SM 6.x entry points and orchestration.
 *
 * The DXIL frontend is dispatched from the SM 5.x entry points in dxbc_converter.cpp
 * when SM50ShaderInternal carries `dxil_bytecode`. Each compile owns its parsed
 * module (ModuleInfo), so concurrent compiles never share IR.
 */

#include "dxil_converter.hpp"
#include "dxil_pipeline.hpp"

#include <algorithm>

#include <string>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Scalar/InferAddressSpaces.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DebugInfo.h"

#include "airconv_context.hpp"
#include "airconv_error.hpp"

namespace dxmt::dxil {

llvm::Error createError(const llvm::Twine &message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), message);
}

llvm::Error unsupported(StringRef what) {
  return llvm::make_error<UnsupportedFeature>(what);
}

shader::common::ResourceType to_resource_type(ResourceKind kind) {
  using shader::common::ResourceType;
  switch (kind) {
  case ResourceKind::Texture1D:
    return ResourceType::Texture1D;
  case ResourceKind::Texture1DArray:
    return ResourceType::Texture1DArray;
  case ResourceKind::Texture2D:
    return ResourceType::Texture2D;
  case ResourceKind::Texture2DArray:
    return ResourceType::Texture2DArray;
  case ResourceKind::Texture2DMS:
    return ResourceType::Texture2DMultisampled;
  case ResourceKind::Texture2DMSArray:
    return ResourceType::Texture2DMultisampledArray;
  case ResourceKind::Texture3D:
    return ResourceType::Texture3D;
  case ResourceKind::TextureCube:
    return ResourceType::TextureCube;
  case ResourceKind::TextureCubeArray:
    return ResourceType::TextureCubeArray;
  case ResourceKind::TypedBuffer:
  case ResourceKind::TBuffer:
    return ResourceType::TextureBuffer;
  default:
    /* Raw and structured buffers are bound as plain buffers, exactly like the
     * SM 5.x path declares them. */
    return ResourceType::NonApplicable;
  }
}

namespace {

/** AIR stage entry points, used to tell lowering output from leftover DXIL IR. */
llvm::SmallPtrSet<llvm::Function *, 8> collectAirEntries(llvm::Module &module) {
  static const char *kAirEntryMetadata[] = {
    "air.vertex",
    "air.fragment",
    "air.kernel",
    "air.mesh",
    "air.object",
    /* A raytracing miss / closest-hit / any-hit / callable shader is a plain
     * `[[visible]]` function (public Metal has no attribute for any of them), and
     * an intersection shader is `air.intersection`. */
    "air.visible",
    "air.intersection",
  };
  llvm::SmallPtrSet<llvm::Function *, 8> entries;
  for (const char *name : kAirEntryMetadata) {
    llvm::NamedMDNode *named = module.getNamedMetadata(name);
    if (!named)
      continue;
    for (unsigned i = 0; i < named->getNumOperands(); i++)
      if (llvm::MDNode *node = named->getOperand(i))
        if (auto *value = llvm::dyn_cast_or_null<llvm::ValueAsMetadata>(node->getOperand(0)))
          if (auto *function = llvm::dyn_cast<llvm::Function>(value->getValue()))
            entries.insert(function);
  }
  return entries;
}

} // namespace



/**
 * Removes the DXIL scaffolding so the module the metallib writer sees contains the
 * AIR stage functions and nothing else: debug info, `dx.*` metadata, the original
 * DXIL entry point and the now dead resource declarations.
 *
 * `keep` names the AIR functions that must survive; everything else that is dead is
 * erased. Raster / compute lowering passes the entries collected from the module's
 * own `air.*` metadata, the raytracing path passes the exports it just built.
 */
void StripDXILArtifacts(llvm::Module &module) {
  llvm::StripDebugInfo(module);

  llvm::SmallVector<llvm::StringRef, 8> drop;
  for (llvm::NamedMDNode &named : module.named_metadata()) {
    llvm::StringRef name = named.getName();
    if (name.startswith("dx.") || name == "llvm.ident")
      drop.push_back(name);
  }
  for (llvm::StringRef name : drop)
    if (llvm::NamedMDNode *named = module.getNamedMetadata(name))
      module.eraseNamedMetadata(named);

  auto air_entries = collectAirEntries(module);
  for (llvm::Function &function : llvm::make_early_inc_range(module)) {
    if (air_entries.contains(&function))
      continue;
    /* Unreferenced declarations are leftover dx.op intrinsics; unreferenced
     * definitions are the DXIL entry point and its helpers. */
    if (function.use_empty())
      function.eraseFromParent();
  }

  for (llvm::GlobalVariable &global : llvm::make_early_inc_range(module.globals()))
    if (global.isDeclaration() && global.use_empty())
      global.eraseFromParent();
}

/**
 * The one indirect call a lowered raytracing module may contain: the call a miss /
 * closest-hit / any-hit / callable export is reached through, i.e. the call through
 * the visible function table.
 *
 * The raytracing lowering has exactly one producer of an indirect call -
 * `EmitVisibleFunctionCall` (dxil_rt_callbacks.cpp) - and it builds that call as
 *
 *   %fp = call i8* @air.get_function_pointer_visible_function_table(vft, i32 slot)
 *   call void %fp(payload, hit_state, i32 record, i32 depth)
 *
 * so the callee is a pointer cast of a call to that AIR getter. The getter is the
 * anchor: a DXIL `call` instruction, which this frontend does not lower, has no such
 * origin and stays rejected, so this is not a blanket allowance for indirect calls.
 *
 * The signature is checked as well, because the ABI is what makes the call legal: the
 * payload and hit state are pointers (in whatever address space the caller passes -
 * the callee's parameters keep them), and the record index and depth are both i32.
 */
bool isVisibleFunctionTableCall(const llvm::CallBase &call) {
  const auto *source =
    llvm::dyn_cast<llvm::CallBase>(call.getCalledOperand()->stripPointerCasts());
  if (!source || source->arg_size() != 2 || source->getFunctionType()->isVarArg() ||
      !source->getArgOperand(0)->getType()->isPointerTy() ||
      !source->getArgOperand(1)->getType()->isIntegerTy(32))
    return false;
  const llvm::Function *getter = source->getCalledFunction();
  if (!getter || getter->getName() != "air.get_function_pointer_visible_function_table")
    return false;
  const llvm::FunctionType *type = call.getFunctionType();
  if (type->isVarArg() || !type->getReturnType()->isVoidTy() || type->getNumParams() != 4)
    return false;
  return type->getParamType(0)->isPointerTy() && type->getParamType(1)->isPointerTy() &&
         type->getParamType(2)->isIntegerTy(32) && type->getParamType(3)->isIntegerTy(32);
}

llvm::Error VerifyLoweredModule(llvm::Module &module) {
  for (llvm::Function &function : module) {
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &instruction : block) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
        if (!call)
          continue;
        if (call->isInlineAsm())
          return unsupported("inline assembly in a lowered DXIL module");
        llvm::Function *callee = call->getCalledFunction();
        if (!callee) {
          if (call->isIndirectCall()) {
            if (isVisibleFunctionTableCall(*call))
              continue;
            /* Name the offending call and where its callee came from: an indirect
             * call that is not the visible-function call is a lowering gap, and the
             * origin is what identifies the pass that left it behind. */
            std::string description;
            llvm::raw_string_ostream stream(description);
            stream << "indirect call in a lowered DXIL module: ";
            call->print(stream);
            stream << " (callee ";
            call->getCalledOperand()->print(stream);
            stream << ")";
            return unsupported(stream.str());
          }
          continue;
        }
        llvm::StringRef name = callee->getName();
        if (name.startswith("dx.op.")) {
          uint32_t opcode = ~0u;
          if (call->arg_size()) {
            if (auto *argument = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0)))
              opcode = static_cast<uint32_t>(argument->getZExtValue());
          }
          return createError(
            llvm::Twine("unlowered DXIL operation ") + name + " (opcode " +
            llvm::Twine(opcode) + ")"
          );
        }
        if (name.startswith("dx."))
          return unsupported((llvm::Twine("unlowered DXIL intrinsic ") + name).str());
      }
    }
  }
  return llvm::Error::success();
}



/**
 * Publish a DXIL module's resource declarations into the ShaderInfo the
 * root-signature binding layer resolves resources through.
 *
 * Both DXIL paths build their own `SM50ShaderInternal` - the ordinary one in
 * PrepareShader and the raytracing pipeline in LowerRaytracingStage - and both hand
 * that ShaderInfo to `setup_binding_rootsig`, which walks these maps to find the range
 * a declaration resolves to. A path that skips this leaves every map empty, and then
 * every resource the shader names - a RaytracingAccelerationStructure, a UAV, a
 * constant buffer - comes back unbound. One loop serves both, so the two paths cannot
 * drift and `to_resource_type` / `to_scaler_type` keep a single definition.
 */
void PopulateShaderResourceBindings(
  dxbc::ShaderInfo &shader_info, llvm::ArrayRef<ResourceBinding> resources
) {
  for (const ResourceBinding &resource : resources) {
    /* Declaration IDs distinguish ranges that share a register number but
     * belong to different register spaces. */
    dxbc::ResourceRange range = {
      .range_id = resource.declaration_id,
      .lower_bound = resource.lower_bound,
      .size = resource.range_size,
      .space = resource.space,
    };
    switch (resource.klass) {
    case ResourceClass::CBV: {
      auto &cbv = shader_info.cbufferMap[resource.declaration_id];
      cbv.range = range;
      cbv.size_in_vec4 = (resource.size_in_bytes + 15) / 16;
      break;
    }
    case ResourceClass::Sampler: {
      auto &sampler = shader_info.samplerMap[resource.declaration_id];
      sampler.range = range;
      break;
    }
    case ResourceClass::SRV: {
      auto &srv = shader_info.srvMap[resource.declaration_id];
      srv.range = range;
      srv.resource_type = to_resource_type(resource.kind);
      srv.scaler_type = to_scaler_type(resource.component_type);
      srv.structure_stride = resource.structure_stride;
      srv.read = resource.read;
      srv.sampled = resource.sampled;
      srv.compared = resource.compared;
      break;
    }
    case ResourceClass::UAV: {
      auto &uav = shader_info.uavMap[resource.declaration_id];
      uav.range = range;
      uav.resource_type = to_resource_type(resource.kind);
      uav.scaler_type = to_scaler_type(resource.component_type);
      uav.structure_stride = resource.structure_stride;
      uav.read = resource.read;
      uav.written = resource.written;
      uav.global_coherent = resource.globally_coherent;
      uav.rasterizer_order = resource.rasterizer_order;
      uav.with_counter = resource.with_counter;
      break;
    }
    }
  }
}

llvm::Error PrepareShader(
  dxbc::SM50ShaderInternal &shader, const void *bytecode, size_t size,
  MTL_SHADER_REFLECTION *reflection
) {
  auto metadata = ParseMetadata(bytecode, size, nullptr);
  if (!metadata)
    return metadata.takeError();

  switch (metadata->stage) {
  case Stage::Vertex:
  case Stage::Pixel:
  case Stage::Compute:
  case Stage::Geometry:
  case Stage::Hull:
  case Stage::Domain:
    break;
  default:
    return unsupported("DXIL shader model 6 stage is not supported by the graphics pipeline");
  }

  auto *bytes = static_cast<const uint8_t *>(bytecode);
  shader.dxil_bytecode = std::vector<uint8_t>(bytes, bytes + size);
  shader.shader_type = to_program_type(metadata->stage);
  shader.threadgroup_size[0] = metadata->num_threads[0];
  shader.threadgroup_size[1] = metadata->num_threads[1];
  shader.threadgroup_size[2] = metadata->num_threads[2];
  shader.max_output_register = metadata->max_output_register;
  shader.pso_valid_output_reg_mask = metadata->output_register_mask;
  shader.ps_has_coverage_output = metadata->has_coverage_output ? 1 : 0;
  /* The multi-stage converters size the vertex payload and look stream output elements up
   * through the DXBC-style output signature. The signature chunk rows are the registers the
   * stage writes (storeOutput addresses elements, not registers). */
  shader.output_signature = BuildOutputSignature(metadata->output_sig);
  if (metadata->stage == Stage::Vertex)
    for (const auto &output : metadata->output_sig)
      if (output.start_row != ~0u)
        shader.max_output_register = std::max(shader.max_output_register, output.start_row + output.rows);
  if (metadata->stage == Stage::Geometry)
    if (auto error = PrepareGeometryShader(*metadata, shader))
      return error;
  if (metadata->stage == Stage::Hull)
    if (auto error = PrepareHullShader(*metadata, shader))
      return error;
  if (metadata->stage == Stage::Domain)
    if (auto error = PrepareDomainShader(*metadata, shader))
      return error;
  if (metadata->stage == Stage::Pixel)
    for (const auto &input : metadata->input_sig)
      if (input.semantic_kind == microsoft::D3D10_SB_NAME_SAMPLE_INDEX ||
          input.interp == microsoft::D3D10_SB_INTERPOLATION_LINEAR_SAMPLE ||
          input.interp == microsoft::D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE_SAMPLE) {
        shader.ps_sample_frequency = 1;
        break;
      }

  /* The resource maps the root-signature binding layer resolves declarations through.
   * The raytracing path calls the same helper (PopulateShaderResourceBindings). */
  PopulateShaderResourceBindings(shader.shader_info, metadata->resources);
  /* Same argument tables and reflection the SM 5.x path builds. */
  dxbc::ArgumentTableMasks masks = build_argument_tables(&shader);
  if (reflection) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    if (!fill_shader_reflection(&shader, masks, reflection, nullptr, nullptr, stream))
      return createError(stream.str());
  }
  return llvm::Error::success();
}

llvm::Expected<std::unique_ptr<ModuleInfo>> LowerShader(
  dxbc::SM50ShaderInternal &shader, const char *function_name,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args
) {
  if (!shader.dxil_bytecode)
    return createError("shader was not initialized from DXIL bytecode");

  /* The caller passes a DXBC-style entry name (for example `ps_main<sha1>`), which
   * is not a DXIL entry point name: it only names the emitted AIR function. */
  const char *name = function_name && function_name[0] ? function_name : "main";

  auto info = ModuleInfo::Parse(
    shader.dxil_bytecode->data(), shader.dxil_bytecode->size(), name
  );
  if (!info)
    return info.takeError();

  // Stage construction mutates signatures and register bookkeeping. A cached
  // shader may be compiled concurrently for different PSOs; keep those fields
  // invocation-local while reusing the immutable resource declarations.
  dxbc::SM50ShaderInternal stage_shader;
  stage_shader.shader_info = shader.shader_info;
  auto binding = LowerStage(**info, stage_shader, name, args);
  if (!binding)
    return binding.takeError();
  if (auto err = LowerResources(**info, **binding))
    return std::move(err);
  if (auto *sample_position = (*info)->module->getFunction("dxmt.sample_pos.i32");
      sample_position && sample_position->isDeclaration()) {
    dxmt::linkSamplePos(*(*info)->module);
    sample_position = (*info)->module->getFunction("dxmt.sample_pos.i32");
    if (!sample_position || sample_position->isDeclaration())
      return createError("dxil: failed to link sample-position helper");
  }
  if (auto err = LowerOperations(*(*info)->module))
    return std::move(err);
  if (auto err = VerifyLoweredModule(*(*info)->module))
    return std::move(err);

  StripDXILArtifacts(*(*info)->module);
  return std::move(*info);
}


namespace {

llvm::Error writeConstantInitializer(
  llvm::Constant *value, uint64_t offset, const llvm::DataLayout &layout,
  const llvm::DenseMap<llvm::GlobalVariable *, uint64_t> &offsets, SM50CompiledBitcodeInternal &compiled
) {
  using namespace llvm;
  if (isa<ConstantAggregateZero>(value) || isa<ConstantPointerNull>(value) || isa<UndefValue>(value))
    return Error::success();
  auto write_bits = [&](const APInt &bits) {
    for (unsigned bit = 0; bit < bits.getBitWidth(); bit += 8)
      compiled.constants[offset + bit / 8] = bits.extractBitsAsZExtValue(std::min(8u, bits.getBitWidth() - bit), bit);
    return Error::success();
  };
  if (auto *integer = dyn_cast<ConstantInt>(value))
    return write_bits(integer->getValue());
  if (auto *floating = dyn_cast<ConstantFP>(value))
    return write_bits(floating->getValueAPF().bitcastToAPInt());
  if (auto *structure = dyn_cast<StructType>(value->getType())) {
    const auto *structure_layout = layout.getStructLayout(structure);
    for (unsigned i = 0; i < structure->getNumElements(); ++i)
      if (auto error = writeConstantInitializer(value->getAggregateElement(i),
            offset + structure_layout->getElementOffset(i), layout, offsets, compiled))
        return error;
    return Error::success();
  }
  if (value->getType()->isArrayTy() || value->getType()->isVectorTy()) {
    auto *array = dyn_cast<ArrayType>(value->getType());
    auto *vector = dyn_cast<FixedVectorType>(value->getType());
    if (!array && !vector)
      return unsupported("scalable immutable constant vector");
    Type *element = array ? array->getElementType() : vector->getElementType();
    const uint64_t count = array ? array->getNumElements() : vector->getNumElements();
    if (vector && element->isIntegerTy() && element->getIntegerBitWidth() % 8) {
      const unsigned width = element->getIntegerBitWidth();
      for (uint64_t i = 0; i < count; ++i) {
        auto *lane = value->getAggregateElement(static_cast<unsigned>(i));
        if (isa<UndefValue>(lane))
          continue;
        auto *integer = dyn_cast<ConstantInt>(lane);
        if (!integer)
          return unsupported("nonliteral packed integer constant vector");
        for (unsigned bit = 0; bit < width; ++bit)
          if (integer->getValue()[bit]) {
            const uint64_t destination = i * width + bit;
            compiled.constants[offset + destination / 8] |= uint8_t(1u << (destination % 8));
          }
      }
      return Error::success();
    }
    const uint64_t stride = layout.getTypeAllocSize(element).getFixedValue();
    for (uint64_t i = 0; i < count; ++i)
      if (auto error = writeConstantInitializer(value->getAggregateElement(static_cast<unsigned>(i)),
            offset + i * stride, layout, offsets, compiled))
        return error;
    return Error::success();
  }
  if (auto *expression = dyn_cast<ConstantExpr>(value)) {
    auto *folded = ConstantFoldConstant(expression, layout);
    if (folded && folded != expression)
      return writeConstantInitializer(folded, offset, layout, offsets, compiled);
  }
  Constant *pointer = value;
  if (auto *expression = dyn_cast<ConstantExpr>(value))
    if (expression->getOpcode() == Instruction::PtrToInt && value->getType()->isIntegerTy(64))
      pointer = cast<Constant>(expression->getOperand(0));
  if (pointer->getType()->isPointerTy()) {
    APInt displacement(64, 0);
    auto *target = dyn_cast<GlobalVariable>(pointer->stripAndAccumulateConstantOffsets(layout, displacement, true));
    auto found = offsets.find(target);
    if (found != offsets.end()) {
      compiled.constant_relocations.push_back({offset, found->second + displacement.getZExtValue()});
      return Error::success();
    }
  }
  return unsupported("immutable constant initializer that cannot be represented in GPU storage");
}

} // namespace

llvm::Error LowerImmutableConstants(llvm::Module &module, SM50CompiledBitcodeInternal &compiled) {
  using namespace llvm;
  const auto &layout = module.getDataLayout();
  SmallVector<GlobalVariable *, 4> globals;
  DenseMap<GlobalVariable *, uint64_t> offsets;
  uint64_t size = 0;
  for (auto &global : module.globals()) {
    if (!global.isConstant() || !global.hasInitializer() || global.use_empty() ||
        (global.getAddressSpace() != 0 && global.getAddressSpace() != 2) || global.getName().startswith("llvm."))
      continue;
    const uint64_t alignment = std::max(layout.getABITypeAlign(global.getValueType()).value(),
                                       global.getAlign().valueOrOne().value());
    const uint64_t length = layout.getTypeAllocSize(global.getValueType()).getFixedValue();
    if (alignment > UINT32_MAX || length > UINT32_MAX || size > UINT32_MAX - (alignment - 1))
      return unsupported("immutable constant pool exceeds AIR buffer size");
    size = alignTo(size, alignment);
    if (length > UINT32_MAX - size)
      return unsupported("immutable constant pool exceeds AIR buffer size");
    offsets[&global] = size;
    globals.push_back(&global);
    size += length;
    compiled.constant_alignment = std::max(compiled.constant_alignment, alignment);
  }
  if (globals.empty())
    return Error::success();
  // A zero-sized global may still have its address taken. Give it a valid binding.
  compiled.constants.resize(std::max<uint64_t>(size, 1), 0);
  for (auto *global : globals) {
    /* The 64 bit atomic lock table address is patched in by the runtime. */
    if (global->getName() == kAtomicLockTableGlobal) {
      compiled.constant_relocations.push_back(
        {offsets.lookup(global), SM50_CONSTANT_RELOCATION_TARGET_ATOMIC_LOCK_TABLE});
      continue;
    }
    if (auto error = writeConstantInitializer(global->getInitializer(), offsets.lookup(global), layout, offsets, compiled))
      return error;
  }

  DenseMap<Constant *, bool> dependencies;
  auto references_pool = [&](auto &self, Constant *value) -> bool {
    if (auto found = dependencies.find(value); found != dependencies.end())
      return found->second;
    bool result = false;
    if (auto *global = dyn_cast<GlobalVariable>(value))
      result = offsets.count(global);
    else if (!isa<GlobalValue>(value))
      for (auto &operand : value->operands())
        result |= self(self, cast<Constant>(operand.get()));
    dependencies[value] = result;
    return result;
  };

  SmallVector<Function *, 8> functions;
  SmallPtrSet<Function *, 8> needs_pool;
  for (auto &function : module) {
    bool needed = false;
    for (auto &instruction : instructions(function))
      for (auto &operand : instruction.operands())
        if (auto *constant = dyn_cast<Constant>(operand.get()))
          needed |= references_pool(references_pool, constant);
    if (needed) {
      needs_pool.insert(&function);
      functions.push_back(&function);
    }
  }
  // Thread the pool through surviving helper calls instead of forcing inlining.
  for (size_t i = 0; i < functions.size(); ++i)
    for (auto *user : functions[i]->users()) {
      auto *call = dyn_cast<CallInst>(user);
      if (!call || call->getCalledFunction() != functions[i])
        return unsupported("escaping function address in immutable constant lowering");
      if (needs_pool.insert(call->getFunction()).second)
        functions.push_back(call->getFunction());
    }

  auto *pool_type = Type::getInt32PtrTy(module.getContext(), 2);
  SmallVector<std::pair<Function *, Function *>, 8> replacements;
  DenseMap<Function *, Function *> rewritten;
  for (auto *function : functions) {
    SmallVector<Type *, 8> parameters(function->getFunctionType()->param_begin(), function->getFunctionType()->param_end());
    parameters.push_back(pool_type);
    auto *type = FunctionType::get(function->getReturnType(), parameters, function->isVarArg());
    auto *replacement = Function::Create(type, function->getLinkage(), function->getAddressSpace(), "", &module);
    replacement->copyAttributesFrom(function);
    replacement->takeName(function);
    const bool was_readnone = replacement->hasFnAttribute(Attribute::ReadNone);
    replacement->removeFnAttr(Attribute::ReadNone);
    replacement->removeFnAttr(Attribute::WriteOnly);
    replacement->removeFnAttr(Attribute::InaccessibleMemOnly);
    replacement->removeFnAttr(Attribute::Speculatable);
    if (was_readnone)
      replacement->addFnAttr(Attribute::ReadOnly);
    for (auto &argument : function->args()) {
      auto *next = replacement->getArg(argument.getArgNo());
      next->takeName(&argument);
      argument.replaceAllUsesWith(next);
    }
    replacement->getArg(replacement->arg_size() - 1)->setName("immutable_constants");
    replacement->getBasicBlockList().splice(replacement->begin(), function->getBasicBlockList());
    replacements.emplace_back(function, replacement);
    rewritten[function] = replacement;
  }
  for (auto [previous, replacement] : replacements)
    for (auto *user : make_early_inc_range(previous->users())) {
      auto *call = cast<CallInst>(user);
      SmallVector<Value *, 8> arguments(call->args());
      auto *caller = call->getFunction();
      arguments.push_back(caller->getArg(caller->arg_size() - 1));
      SmallVector<OperandBundleDef, 2> bundles;
      call->getOperandBundlesAsDefs(bundles);
      auto *next = CallInst::Create(replacement, arguments, bundles, "", call);
      next->takeName(call);
      next->setCallingConv(call->getCallingConv());
      next->setTailCallKind(call->getTailCallKind());
      next->setAttributes(call->getAttributes());
      next->removeFnAttr(Attribute::ReadNone);
      next->removeFnAttr(Attribute::WriteOnly);
      next->removeFnAttr(Attribute::InaccessibleMemOnly);
      next->removeFnAttr(Attribute::Speculatable);
      next->copyMetadata(*call);
      next->copyIRFlags(call);
      call->replaceAllUsesWith(next);
      call->eraseFromParent();
    }

  for (const char *kind : {"air.vertex", "air.fragment", "air.kernel"}) {
    auto *entries = module.getNamedMetadata(kind);
    if (!entries)
      continue;
    for (unsigned i = 0; i < entries->getNumOperands(); ++i) {
      auto *entry = entries->getOperand(i);
      auto *previous = cast<Function>(cast<ValueAsMetadata>(entry->getOperand(0))->getValue());
      auto found = rewritten.find(previous);
      if (found == rewritten.end())
        continue;
      auto *replacement = found->second;
      auto *arguments = cast<MDNode>(entry->getOperand(2));
      SmallVector<Metadata *, 8> argument_nodes(arguments->op_begin(), arguments->op_end());
      argument_nodes.push_back(air::BuildBufferArgumentMetadata(replacement->arg_size() - 1,
        air::ArgumentBindingBuffer{
          .buffer_size = static_cast<uint32_t>(compiled.constants.size()),
          .location_index = SM50_BINDING_INDEX_IMMUTABLE_CONSTANTS,
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_uint,
          .arg_name = "immutable_constants",
        }, module.getContext(), layout));
      SmallVector<Metadata *, 4> fields(entry->op_begin(), entry->op_end());
      fields[0] = ValueAsMetadata::get(replacement);
      fields[2] = MDNode::get(module.getContext(), argument_nodes);
      entries->setOperand(i, MDNode::get(module.getContext(), fields));
    }
  }

  FunctionAnalysisManager analyses;
  PassBuilder pass_builder;
  pass_builder.registerFunctionAnalyses(analyses);
  for (auto [previous, function] : replacements) {
    IRBuilder<> builder(&*function->getEntryBlock().getFirstInsertionPt());
    auto *bytes = builder.CreateBitCast(function->getArg(function->arg_size() - 1), builder.getInt8PtrTy(2));
    DenseMap<Constant *, Value *> values;
    auto materialize = [&](auto &self, Constant *value) -> Value * {
      if (!references_pool(references_pool, value))
        return value;
      if (auto found = values.find(value); found != values.end())
        return found->second;
      Value *result;
      if (auto *global = dyn_cast<GlobalVariable>(value)) {
        auto *address = builder.CreateConstGEP1_64(builder.getInt8Ty(), bytes, offsets.lookup(global));
        result = builder.CreateBitCast(address, global->getValueType()->getPointerTo(2));
        if (global->getAddressSpace() != 2)
          result = builder.CreateAddrSpaceCast(result, global->getType());
      } else if (auto *expression = dyn_cast<ConstantExpr>(value)) {
        auto *instruction = expression->getAsInstruction();
        for (unsigned i = 0; i < instruction->getNumOperands(); ++i)
          instruction->setOperand(i, self(self, cast<Constant>(instruction->getOperand(i))));
        result = builder.Insert(instruction);
      } else {
        result = PoisonValue::get(value->getType());
        for (unsigned i = 0; i < value->getNumOperands(); ++i) {
          auto *element = self(self, cast<Constant>(value->getOperand(i)));
          result = value->getType()->isVectorTy()
            ? builder.CreateInsertElement(result, element, builder.getInt32(i))
            : builder.CreateInsertValue(result, element, {i});
        }
      }
      values[value] = result;
      return result;
    };
    for (auto &instruction : make_early_inc_range(instructions(*function)))
      for (auto &operand : instruction.operands())
        if (auto *constant = dyn_cast<Constant>(operand.get()))
          operand.set(materialize(materialize, constant));
    // Optimizer-created AS0 lookup tables must become genuine constant loads too.
    InferAddressSpacesPass(0).run(*function, analyses);
    previous->eraseFromParent();
  }
  for (auto *global : globals)
    global->setInitializer(nullptr);
  for (auto *global : globals) {
    global->removeDeadConstantUsers();
    if (!global->use_empty())
      return unsupported("immutable storage referenced outside a shader function");
    global->eraseFromParent();
  }
  return Error::success();
}


} // namespace dxmt::dxil
