#include "dxbc_converter.hpp"
#include "DXBCParser/BlobContainer.h"
#include "DXBCParser/ShaderBinary.h"
#include "airconv_error.hpp"
#include "dxil_converter.hpp"
#include "dxil_pipeline.hpp"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/FMF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <bit>
#include <memory>
#include <string>
#include <vector>

#include "metallib_writer.hpp"
#include "nt/air_builder.hpp"
#include "shader_common.hpp"

#include "airconv_context.hpp"
#include "msaa_pixel.hpp"
#include "logic_op.hpp"
#include "msaa_vertex.hpp"
#include "msaa_mesh.hpp"

#include "ftl.hpp"

/*
 * sm50_bitcode_t / sm50_error_t are the opaque handles declared in
 * dxil_converter.hpp (dxil::SM50CompiledBitcodeInternal / dxil::SM50ErrorInternal):
 * the raytracing entry points in dxil_raytracing.cpp allocate them too, so their
 * definitions are shared rather than repeated here.
 */
using dxmt::dxil::SM50CompiledBitcodeInternal;
using dxmt::dxil::SM50ErrorInternal;

void dxmt::dxbc::initialize_sample_mask_constant(
    llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args) {
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *constant = nullptr;
  if (!args_get_data<SM50_SHADER_D3D12_SAMPLE_MASK_CONSTANT, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(args, &constant))
    return;
  auto &context = module.getContext();
  auto *type = llvm::Type::getInt32Ty(context);
  auto *global = new llvm::GlobalVariable(module, type, true, llvm::GlobalValue::LinkOnceODRLinkage,
      llvm::UndefValue::get(type), "dxmt_sample_mask.MTL_FC_INIT_0_j", nullptr,
      llvm::GlobalValue::NotThreadLocal, 2, true);
  global->setVisibility(llvm::GlobalValue::HiddenVisibility);
  global->setSection("air.fc_initializer");
  global->setAlignment(llvm::Align(4));
  module.getOrInsertNamedMetadata("air.function_constants")->addOperand(llvm::MDNode::get(context, {
      llvm::ValueAsMetadata::get(global), llvm::MDString::get(context, "uint"),
      llvm::MDString::get(context, "dxmt_sample_mask"),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(type, 0))}));
}

llvm::Value *dxmt::dxbc::pixel_sample_mask(context ctx) {
  if (auto *constant = ctx.module.getGlobalVariable("dxmt_sample_mask.MTL_FC_INIT_0_j"))
    return ctx.builder.CreateLoad(ctx.builder.getInt32Ty(), constant);
  return ctx.builder.getInt32(ctx.pso_sample_mask);
}

/** Hands the error object to the caller, or releases it when none was asked for. */
static void failCall(SM50ErrorInternal *errorObj, sm50_error_t *ppError) {
  if (ppError)
    *ppError = (sm50_error_t)errorObj;
  else
    delete errorObj;
}

/** Logs any llvm::Error into the SM50 error buffer, keeping the NUL terminator. */
static void reportError(llvm::Error error, llvm::raw_ostream &errorOut) {
  std::string message;
  llvm::raw_string_ostream stream(message);
  llvm::handleAllErrors(
    std::move(error), [&](const llvm::ErrorInfoBase &base) { base.log(stream); }
  );
  stream.flush();
  errorOut << message << '\0';
}

/**
 * Multi-stage pipeline function with at least one Shader Model 6.x stage: the DXIL
 * front end builds it through the same converters (dxil_pipeline.cpp). Returns -1 when
 * both stages are DXBC so the caller continues on the SM 5.x path.
 */
static int compileDXILPipelineStage(
  dxmt::dxil::PipelineKind kind, sm50_shader_t first, sm50_shader_t second,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name, sm50_bitcode_t *ppBitcode,
  SM50ErrorInternal *errorObj, llvm::raw_ostream &errorOut, sm50_error_t *ppError
) {
  if (!dxmt::dxil::IsDXILShader(first) && !dxmt::dxil::IsDXILShader(second))
    return -1;
  auto compiled = dxmt::dxil::CompilePipelineStage(
    kind, (dxmt::dxbc::SM50ShaderInternal *)first, (dxmt::dxbc::SM50ShaderInternal *)second, args, name
  );
  if (!compiled) {
    reportError(compiled.takeError(), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }
  delete errorObj;
  *ppBitcode = (sm50_bitcode_t)compiled->release();
  return 0;
}

namespace dxmt::dxbc {

uint32_t next_pow2(uint32_t x) {
  return x == 1 ? 1 : 1 << (32 - __builtin_clz(x - 1));
}


void setup_immediate_constant_buffer(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
) {
  if (!shader_info->immConstantBufferData.size()) {
    return;
  }
  auto &context = module.getContext();
  auto type = llvm::ArrayType::get(
    types._int4, shader_info->immConstantBufferData.size()
  );
  auto const_data = llvm::ConstantArray::get(
    type,
    shader_info->immConstantBufferData |
      [&](auto data) {
        return llvm::ConstantVector::get(
          {llvm::ConstantInt::get(context, llvm::APInt{32, data[0], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[1], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[2], false}),
           llvm::ConstantInt::get(context, llvm::APInt{32, data[3], false})}
        );
      }
  );
  llvm::GlobalVariable *icb = new llvm::GlobalVariable(
    module, type, true, llvm::GlobalValue::InternalLinkage, const_data, "icb",
    nullptr, llvm::GlobalValue::NotThreadLocal, 2
  );
  icb->setAlignment(llvm::Align(4));
  resource_map.icb = icb;
  resource_map.icb_float = builder.CreateBitCast(
    resource_map.icb, llvm::ArrayType::get(
                        types._float4, shader_info->immConstantBufferData.size()
                      )
                        ->getPointerTo(2)
  );
}

void setup_tgsm(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module
) {
  for (auto &[id, tgsm] : shader_info->tgsmMap) {
    auto type = llvm::ArrayType::get(types._int, tgsm.size_in_uint);
    llvm::GlobalVariable *tgsm_h = new llvm::GlobalVariable(
      module, type, false, llvm::GlobalValue::InternalLinkage,
      llvm::UndefValue::get(type), "g" + std::to_string(id), nullptr,
      llvm::GlobalValue::NotThreadLocal, 3
    );
    tgsm_h->setAlignment(llvm::Align(4));
    resource_map.tgsm_map[id] = {tgsm.structured ? tgsm.stride : 0, tgsm_h};
  }
}

void setup_temp_register(
  const ShaderInfo *shader_info, io_binding_map &resource_map,
  air::AirType &types, llvm::Module &module, llvm::IRBuilder<> &builder
) {
  resource_map.temp.ptr_int4 = builder.CreateAlloca(
    llvm::ArrayType::get(types._int, 4 * shader_info->tempRegisterCount)
  );
  resource_map.temp.ptr_float4 = builder.CreateBitCast(
    resource_map.temp.ptr_int4,
    llvm::ArrayType::get(types._float, 4 * shader_info->tempRegisterCount)
      ->getPointerTo()
  );
  for (auto &phase : shader_info->phases) {
    resource_map.phases.push_back({});
    auto &phase_temp = resource_map.phases.back();

    phase_temp.temp.ptr_int4 = builder.CreateAlloca(
      llvm::ArrayType::get(types._int, 4 * phase.tempRegisterCount)
    );
    phase_temp.temp.ptr_float4 = builder.CreateBitCast(
      phase_temp.temp.ptr_int4,
      llvm::ArrayType::get(types._float, 4 * phase.tempRegisterCount)
        ->getPointerTo()
    );

    for (auto &[idx, info] : phase.indexableTempRegisterCounts) {
      auto &[numRegisters, mask] = info;
      auto channel_count = std::bit_width(mask);
      auto ptr_int_vec = builder.CreateAlloca(llvm::ArrayType::get(types._int, numRegisters * channel_count));
      auto ptr_float_vec = builder.CreateBitCast(
          ptr_int_vec, llvm::ArrayType::get(types._float, numRegisters * channel_count)->getPointerTo()
      );
      phase_temp.indexable_temp_map[idx] = {
        ptr_int_vec, ptr_float_vec, (uint32_t)channel_count
      };
    }
  }
  for (auto &[idx, info] : shader_info->indexableTempRegisterCounts) {
    auto &[numRegisters, mask] = info;
    auto channel_count = std::bit_width(mask);
    auto ptr_int_vec = builder.CreateAlloca(llvm::ArrayType::get(types._int, numRegisters * channel_count));
    auto ptr_float_vec = builder.CreateBitCast(
        ptr_int_vec, llvm::ArrayType::get(types._float, numRegisters * channel_count)->getPointerTo()
    );
    resource_map.indexable_temp_map[idx] = {
      ptr_int_vec, ptr_float_vec, (uint32_t)channel_count
    };
  }
}

SM50_SHADER_METAL_VERSION required_metal_version(const llvm::Module &module, SM50_SHADER_METAL_VERSION metal_version) {
  // Metal 3.2 miscomputes dynamic cube comparison gradients. Compute comparison bias
  // uses the same gradient path; both DXIL operations need the Metal 4.0 semantics.
  for (const auto &function : module) {
    if (function.getName().startswith("dx.op.sampleCmpGrad.") ||
        function.getName().startswith("dx.op.sampleCmpBias.")) {
      metal_version = std::max(metal_version, SM50_SHADER_METAL_400);
      break;
    }
  }
  if (module.getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) metal_version = SM50_SHADER_METAL_410;
  return metal_version;
}

void setup_metal_version(llvm::Module &module, SM50_SHADER_METAL_VERSION metal_version,
                         const SM50_SHADER_D3D12_MSAA_PIXEL_DATA *msaa_pixel) {
  using namespace llvm;
  if (msaa_pixel && (msaa_pixel->variant & ~SM50_MSAA_ATOMIC_HELPERS) != SM50_MSAA_COVERAGE)
    metal_version = std::max(metal_version, SM50_SHADER_METAL_400);
  metal_version = required_metal_version(module, metal_version);
  auto &context = module.getContext();
  auto createUnsignedInteger = [&](uint32_t s) {
    return ConstantAsMetadata::get(ConstantInt::get(context, APInt{32, s, false}));
  };
  auto createString = [&](auto s) { return MDString::get(context, s); };

  auto airVersion = module.getOrInsertNamedMetadata("air.version");
  auto airLangVersion = module.getOrInsertNamedMetadata("air.language_version");
  switch (metal_version) {
  case SM50_SHADER_METAL_410: {
    airVersion->addOperand(MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(9), createUnsignedInteger(0)}));
    airLangVersion->addOperand(MDTuple::get(context, {createString("Metal"), createUnsignedInteger(4), createUnsignedInteger(1), createUnsignedInteger(0)}));
    module.setTargetTriple("air64-apple-macosx27.0.0");
    break;
  }
  case SM50_SHADER_METAL_400: {
    airVersion->addOperand(MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(8), createUnsignedInteger(0)}));
    airLangVersion->addOperand(MDTuple::get(context, {createString("Metal"), createUnsignedInteger(4), createUnsignedInteger(0), createUnsignedInteger(0)}));
    module.setTargetTriple("air64-apple-macosx26.0.0");
    break;
  }
  case SM50_SHADER_METAL_320: {
    airVersion->addOperand(
        MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(7), createUnsignedInteger(0)})
    );
    airLangVersion->addOperand(MDTuple::get(
        context, {createString("Metal"), createUnsignedInteger(3), createUnsignedInteger(2), createUnsignedInteger(0)}
    ));
    module.setTargetTriple("air64-apple-macosx15.0.0");
    break;
  }
  default: {
    airVersion->addOperand(
        MDTuple::get(context, {createUnsignedInteger(2), createUnsignedInteger(6), createUnsignedInteger(0)})
    );
    airLangVersion->addOperand(MDTuple::get(
        context, {createString("Metal"), createUnsignedInteger(3), createUnsignedInteger(1), createUnsignedInteger(0)}
    ));
    break;
  }
  }
}

llvm::Expected<StreamOutputCaptureRun> resolve_stream_output_capture(
  const SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA &capture, const SM50_SO_CAPTURE_ELEMENT &element,
  uint32_t reg, uint32_t mask
) {
  auto describe = [&] {
    return std::string(element.semantic_name) + std::to_string(element.semantic_index);
  };
  mask &= 0xFu;
  if (!mask)
    return llvm::make_error<UnsupportedFeature>("stream output capture element " + describe() + " has no components");
  if (element.component_count == 0 || element.component_count > 4)
    return llvm::make_error<UnsupportedFeature>(
      "stream output capture element " + describe() + " has an invalid component count"
    );
  uint32_t first = std::countr_zero(mask) + element.start_component;
  uint32_t range = ((1u << element.component_count) - 1u) << first;
  if (first + element.component_count > 4 || (range & mask) != range)
    return llvm::make_error<UnsupportedFeature>(
      "stream output capture element " + describe() + " selects components outside the output mask"
    );
  if (element.dword_offset > capture.record_dwords ||
      element.component_count > capture.record_dwords - element.dword_offset)
    return llvm::make_error<UnsupportedFeature>(
      "stream output capture element " + describe() + " does not fit the capture record"
    );
  return StreamOutputCaptureRun{
    .reg = reg,
    .component = first,
    .count = element.component_count,
    .dword_offset = element.dword_offset,
  };
}

VertexIndexPullArgs define_vertex_index_pull(air::FunctionSignatureBuilder &func_signature) {
  VertexIndexPullArgs args;
  args.control = func_signature.DefineInput(air::ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = SM50_BINDING_INDEX_VERTEX_INDEX_CONTROL,
    .array_size = 0,
    .memory_access = air::MemoryAccess::read,
    .address_space = air::AddressSpace::constant,
    .type = air::MSLUint{},
    .arg_name = "vertex_index_control",
    .raster_order_group = {}
  });
  args.indices = func_signature.DefineInput(air::ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = SM50_BINDING_INDEX_VERTEX_INDICES,
    .array_size = 0,
    .memory_access = air::MemoryAccess::read,
    .address_space = air::AddressSpace::device,
    .type = air::MSLUshort{},
    .arg_name = "vertex_indices",
    .raster_order_group = {}
  });
  return args;
}

uint32_t define_pipeline_statistics(air::FunctionSignatureBuilder &func_signature) {
  return func_signature.DefineInput(air::ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = SM50_BINDING_INDEX_PIPELINE_STATISTICS,
    .array_size = 0,
    .memory_access = air::MemoryAccess::read_write,
    .address_space = air::AddressSpace::device,
    .type = air::MSLUint{},
    .arg_name = "pipeline_statistics",
    .raster_order_group = {}
  });
}

void increment_pipeline_statistics(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function,
  uint32_t argument, uint32_t low_word
) {
  add_pipeline_statistics(builder, air, function, argument, low_word, builder.getInt32(1));
}

void add_pipeline_statistics(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function,
  uint32_t argument, uint32_t low_word, llvm::Value *amount
) {
  /* Each D3D12 UINT64 is a pair of adjacent atomic uint32 words. Increment
   * high exactly once when low wraps, including with concurrent invocations. */
  auto *words = function->getArg(argument);
  auto *low = builder.CreateConstInBoundsGEP1_32(builder.getInt32Ty(), words, low_word);
  auto *previous = air.CreateAtomicRMW(llvm::AtomicRMWInst::Add, low, amount);
  auto *carry = llvm::BasicBlock::Create(builder.getContext(), "statistics_carry", function);
  auto *body = llvm::BasicBlock::Create(builder.getContext(), "statistics_body", function);
  // The add wrapped iff the new low word is below the amount added.
  builder.CreateCondBr(builder.CreateICmpULT(builder.CreateAdd(previous, amount), amount), carry, body);
  builder.SetInsertPoint(carry);
  auto *high = builder.CreateConstInBoundsGEP1_32(builder.getInt32Ty(), words, low_word + 1);
  air.CreateAtomicRMW(llvm::AtomicRMWInst::Add, high, builder.getInt32(1));
  builder.CreateBr(body);
  builder.SetInsertPoint(body);
}

PixelStatisticsDedupe define_pixel_statistics_dedupe(air::FunctionSignatureBuilder &func_signature) {
  return {func_signature.DefineInput(air::InputSampleIndex{}), func_signature.DefineInput(air::InputInputCoverage{})};
}

void increment_pixel_invocations(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function, uint32_t argument,
  const PixelStatisticsDedupe *dedupe
) {
  if (!dedupe) {
    increment_pipeline_statistics(builder, air, function, argument, 14);
    return;
  }
  /* The invocation of the lowest covered sample is the pixel's one counted invocation: no covered sample below
   * [[sample_id]] (SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL). */
  auto *sample = function->getArg(dedupe->sample_index);
  auto *coverage = function->getArg(dedupe->coverage);
  auto *below = builder.CreateSub(builder.CreateShl(builder.getInt32(1), sample), builder.getInt32(1));
  auto *count_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_pixel", function);
  auto *done_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_pixel_done", function);
  builder.CreateCondBr(builder.CreateICmpEQ(builder.CreateAnd(coverage, below), builder.getInt32(0)), count_bb,
                       done_bb);
  builder.SetInsertPoint(count_bb);
  increment_pipeline_statistics(builder, air, function, argument, 14);
  builder.CreateBr(done_bb);
  builder.SetInsertPoint(done_bb);
}

/* The flat per-primitive key of SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM (mesh primitive data 0). */
constexpr const char *kStatisticsKeyUser = "dxmt_statistics_key";

void define_mesh_statistics_key(air::FunctionSignatureBuilder &func_signature, uint32_t data_index) {
  func_signature.DefineMeshPrimitiveOutput(
    air::OutputMeshData{.user = kStatisticsKeyUser, .type = air::msl_uint4, .index = data_index}
  );
}

void set_mesh_statistics_key(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Value *primitive, llvm::Value *key0,
  llvm::Value *key1, llvm::Value *key2, uint32_t data_index
) {
  llvm::Value *key = llvm::UndefValue::get(llvm::FixedVectorType::get(builder.getInt32Ty(), 4));
  key = builder.CreateInsertElement(key, key0, uint64_t(0));
  key = builder.CreateInsertElement(key, key1, uint64_t(1));
  key = builder.CreateInsertElement(key, key2, uint64_t(2));
  key = builder.CreateInsertElement(key, builder.getInt32(0), uint64_t(3));
  air.CreateSetMeshPrimitiveData(primitive, builder.getInt32(data_index), key);
}

PixelStatisticsClaim define_pixel_statistics_claim(air::FunctionSignatureBuilder &func_signature) {
  return {
    func_signature.DefineInput(air::InputFragmentStageIn{
      .user = kStatisticsKeyUser, .type = air::msl_uint4, .interpolation = air::Interpolation::flat, .pull_mode = false
    }),
    func_signature.DefineInput(air::InputPosition{.interpolation = air::Interpolation::center_no_perspective}),
    func_signature.DefineInput(air::InputRenderTargetArrayIndex{}),
    func_signature.DefineInput(air::InputInputCoverage{}),
    func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_BINDING_INDEX_STATISTICS_CLAIM,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read_write,
      .address_space = air::AddressSpace::device,
      .type = air::MSLUint{},
      .arg_name = "statistics_claim",
      .raster_order_group = 0u
    }),
  };
}

void claim_pixel_invocation(
  llvm::IRBuilder<> &builder, llvm::air::AIRBuilder &air, llvm::Function *function, uint32_t argument,
  const PixelStatisticsClaim &claim
) {
  /* SM50_STATISTICS_CLAIM_HEADER: per sample of every pixel, the {key.xyz, epoch} of the last invocation covering
   * it. Metal runs the PS once per covered sample, all invocations of one (pixel, primitive) with the same
   * [[sample_mask]], and raster order groups order invocations only where their samples overlap. An invocation
   * counts unless one of its masked samples already holds its key, then writes its key to all of them: an earlier
   * invocation of the same (pixel, primitive) shares every masked sample and is ordered before it. Exact against the
   * D3D count with disjoint-mask interleaving at 2x/4x/8x (probe build/local/p3/p2last/probe/quads4.mm). */
  auto *i32 = builder.getInt32Ty();
  auto *header = function->getArg(claim.claim);
  auto word = [&](llvm::Value *index) { return builder.CreateGEP(i32, header, {index}); };
  auto load = [&](llvm::Value *index) { return builder.CreateLoad(i32, word(index)); };
  auto *width = load(builder.getInt32(0));
  auto *height = load(builder.getInt32(1));
  auto *layers = load(builder.getInt32(2));
  auto *epoch = load(builder.getInt32(3));
  auto *entries = load(builder.getInt32(4));
  auto *samples = load(builder.getInt32(6));
  auto *position = function->getArg(claim.position);
  auto *x = builder.CreateFPToUI(builder.CreateExtractElement(position, uint64_t(0)), i32);
  auto *y = builder.CreateFPToUI(builder.CreateExtractElement(position, uint64_t(1)), i32);
  // An out-of-range render target array index renders to slice 0 (D3D11.3 17.18).
  auto *rtai = function->getArg(claim.layer);
  auto *layer = builder.CreateSelect(builder.CreateICmpULT(rtai, layers), rtai, builder.getInt32(0));
  auto *pixel = builder.CreateAdd(builder.CreateMul(builder.CreateAdd(builder.CreateMul(layer, height), y), width), x);
  auto *base = builder.CreateAdd(entries, builder.CreateMul(builder.CreateMul(pixel, samples), builder.getInt32(4)));
  auto *mask = function->getArg(claim.coverage);
  auto *key = function->getArg(claim.key);
  llvm::Value *wanted[4] = {builder.CreateExtractElement(key, uint64_t(0)), builder.CreateExtractElement(key, uint64_t(1)),
                            builder.CreateExtractElement(key, uint64_t(2)), epoch};
  auto masked = [&](uint32_t s) {
    return builder.CreateAnd(builder.CreateICmpULT(builder.getInt32(s), samples),
                             builder.CreateICmpNE(builder.CreateAnd(mask, builder.getInt32(1u << s)), builder.getInt32(0)));
  };
  auto entry = [&](uint32_t s, uint32_t i) {
    return builder.CreateAdd(base, builder.getInt32(s * 4 + i));
  };
  // Raster sample counts are 1, 2, 4 or 8. Loads of unmasked samples read in-range entries of this pixel only.
  llvm::Value *seen = builder.getFalse();
  for (uint32_t s = 0; s < 8; s++) {
    auto *check_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_claim_check", function);
    auto *next_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_claim_next", function);
    auto *from = builder.GetInsertBlock();
    builder.CreateCondBr(masked(s), check_bb, next_bb);
    builder.SetInsertPoint(check_bb);
    llvm::Value *same = builder.getTrue();
    for (uint32_t i = 0; i < 4; i++)
      same = builder.CreateAnd(same, builder.CreateICmpEQ(load(entry(s, i)), wanted[i]));
    auto *checked = builder.CreateOr(seen, same);
    builder.CreateBr(next_bb);
    builder.SetInsertPoint(next_bb);
    auto *phi = builder.CreatePHI(builder.getInt1Ty(), 2);
    phi->addIncoming(seen, from);
    phi->addIncoming(checked, check_bb);
    seen = phi;
  }
  for (uint32_t s = 0; s < 8; s++) {
    auto *write_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_claim_write", function);
    auto *next_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_claim_written", function);
    builder.CreateCondBr(masked(s), write_bb, next_bb);
    builder.SetInsertPoint(write_bb);
    for (uint32_t i = 0; i < 4; i++)
      builder.CreateStore(wanted[i], word(entry(s, i)));
    builder.CreateBr(next_bb);
    builder.SetInsertPoint(next_bb);
  }
  auto *count_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_claim", function);
  auto *done_bb = llvm::BasicBlock::Create(builder.getContext(), "statistics_claim_done", function);
  builder.CreateCondBr(seen, done_bb, count_bb);
  builder.SetInsertPoint(count_bb);
  increment_pipeline_statistics(builder, air, function, argument, 14);
  builder.CreateBr(done_bb);
  builder.SetInsertPoint(done_bb);
}

VertexIndexPullArgs define_stream_output_capture(
  air::FunctionSignatureBuilder &func_signature, IRValue &epilogue, std::vector<StreamOutputCaptureRun> runs,
  uint32_t record_dwords
) {
  auto args = define_vertex_index_pull(func_signature);
  // MSAA-only capture uses this argument layout to pull IA indices, but has no
  // stream-output record. It must not claim keys in the caller's fallback buffer.
  if (!record_dwords) return args;
  auto scratch_arg = func_signature.DefineInput(air::ArgumentBindingBuffer{
    .buffer_size = {},
    .location_index = SM50_BINDING_INDEX_SO_CAPTURE_SCRATCH,
    .array_size = 0,
    .memory_access = air::MemoryAccess::read_write,
    .address_space = air::AddressSpace::device,
    .type = air::MSLUint{},
    .arg_name = "so_capture_scratch",
    .raster_order_group = {}
  });
  epilogue >> [=, runs = std::move(runs)](pvalue value) -> IRValue {
    return make_irvalue([=](context ctx) -> pvalue {
      auto &builder = ctx.builder;
      auto *int_ty = ctx.types._int;
      auto *control = ctx.function->getArg(args.control);
      auto *scratch = ctx.function->getArg(scratch_arg);
      auto load_control = [&](unsigned index) {
        return builder.CreateLoad(int_ty, builder.CreateConstInBoundsGEP1_32(int_ty, control, index));
      };
      auto *positions_per_instance = load_control(offsetof(SM50_SO_CAPTURE_CONTROL, positions_per_instance) / 4);
      auto *first_position = load_control(offsetof(SM50_SO_CAPTURE_CONTROL, first_position) / 4);
      auto *record_count = load_control(offsetof(SM50_SO_CAPTURE_CONTROL, record_count) / 4);
      /* The key is the IA position (see SM50_SO_CAPTURE_CONTROL); the arithmetic
       * wraps, so one unsigned compare rejects keys on both sides. */
      auto *record = builder.CreateSub(
        builder.CreateAdd(builder.CreateMul(ctx.resource.instance_id, positions_per_instance), ctx.resource.so_capture_key),
        first_position
      );
      auto *claim_bb = llvm::BasicBlock::Create(ctx.llvm, "so_capture_claim", ctx.function);
      auto *capture_bb = llvm::BasicBlock::Create(ctx.llvm, "so_capture", ctx.function);
      auto *done_bb = llvm::BasicBlock::Create(ctx.llvm, "so_capture_done", ctx.function);
      builder.CreateCondBr(builder.CreateICmpULT(record, record_count), claim_bb, done_bb);

      /* One invocation per key owns the record: a vertex shaded more than once
       * (with possibly different outputs, e.g. UAV-dependent) never mixes the
       * components of two invocations. */
      builder.SetInsertPoint(claim_bb);
      auto *record64 = builder.CreateZExt(record, builder.getInt64Ty());
      auto *claim_index = builder.CreateAdd(
        builder.CreateMul(builder.CreateZExt(record_count, builder.getInt64Ty()), builder.getInt64(record_dwords)),
        record64
      );
      auto *claim = builder.CreateAtomicCmpXchg(
        builder.CreateGEP(int_ty, scratch, {claim_index}), builder.getInt32(0), builder.getInt32(1), llvm::MaybeAlign(),
        llvm::AtomicOrdering::Monotonic, llvm::AtomicOrdering::Monotonic
      );
      builder.CreateCondBr(builder.CreateExtractValue(claim, 1), capture_bb, done_bb);

      builder.SetInsertPoint(capture_bb);
      auto *record_base = builder.CreateMul(record64, builder.getInt64(record_dwords));
      auto *registers = ctx.resource.output.ptr_int4;
      auto *registers_type = registers->getType()->getNonOpaquePointerElementType();
      for (auto &run : runs) {
        for (uint32_t i = 0; i < run.count; i++) {
          auto *source = builder.CreateGEP(
            registers_type, registers,
            {builder.getInt32(0), builder.getInt32(run.reg), builder.getInt32(run.component + i)}
          );
          auto *dword = builder.CreateAdd(record_base, builder.getInt64(run.dword_offset + i));
          builder.CreateStore(builder.CreateLoad(int_ty, source), builder.CreateGEP(int_ty, scratch, {dword}));
        }
      }
      builder.CreateBr(done_bb);
      builder.SetInsertPoint(done_bb);
      return value;
    });
  };
  return args;
}

void apply_vertex_index_pull(
  llvm::IRBuilder<> &builder, llvm::Function *function, const VertexIndexPullArgs &args, io_binding_map &resource,
  uint32_t control_dword_offset
) {
  /* Native vertex ids are IA positions, from a non-indexed draw or SO position
   * buffer. The application's index value becomes the D3D
   * SV_VertexID and, with BaseVertexLocation added, the vertex fetch index. */
  auto *int_ty = builder.getInt32Ty();
  auto *short_ty = builder.getInt16Ty();
  auto *control = function->getArg(args.control);
  auto *indices = function->getArg(args.indices);
  auto load_control = [&](unsigned index) {
    return builder.CreateLoad(
      int_ty, builder.CreateConstInBoundsGEP1_32(int_ty, control, control_dword_offset + index)
    );
  };
  auto *key = resource.vertex_id;
  auto *index_size = load_control(offsetof(SM50_INDEX_PULL_CONTROL, index_size) / 4);
  auto *index_first = load_control(offsetof(SM50_INDEX_PULL_CONTROL, index_first) / 4);
  auto *base_vertex = load_control(offsetof(SM50_INDEX_PULL_CONTROL, base_vertex) / 4);
  if (resource.so_capture_key) {
    // Point capture does not discard Metal restart indices. Skip before fetching
    // the application index or executing a cut vertex's shader side effects.
    auto *restart = llvm::BasicBlock::Create(builder.getContext(), "so_restart", function);
    auto *valid = llvm::BasicBlock::Create(builder.getContext(), "so_position", function);
    builder.CreateCondBr(builder.CreateAnd(builder.CreateICmpNE(index_size, builder.getInt32(0)),
                                          builder.CreateICmpEQ(key, builder.getInt32(~0u))), restart, valid);
    builder.SetInsertPoint(restart);
    if (function->getReturnType()->isVoidTy()) builder.CreateRetVoid();
    else builder.CreateRet(llvm::Constant::getNullValue(function->getReturnType()));
    builder.SetInsertPoint(valid);
  }
  auto *entry = builder.GetInsertBlock();
  auto *pull_bb = llvm::BasicBlock::Create(builder.getContext(), "vertex_index_pull", function);
  auto *join_bb = llvm::BasicBlock::Create(builder.getContext(), "vertex_index_join", function);
  builder.CreateCondBr(builder.CreateICmpNE(index_size, builder.getInt32(0)), pull_bb, join_bb);

  builder.SetInsertPoint(pull_bb);
  auto *wide = builder.CreateICmpEQ(index_size, builder.getInt32(4));
  auto *word = builder.CreateAdd(index_first, builder.CreateSelect(wide, builder.CreateShl(key, 1), key));
  auto load_word = [&](llvm::Value *at) {
    return builder.CreateZExt(
      builder.CreateLoad(short_ty, builder.CreateGEP(short_ty, indices, {builder.CreateZExt(at, builder.getInt64Ty())})),
      int_ty
    );
  };
  auto *low = load_word(word);
  auto *high_bb = llvm::BasicBlock::Create(builder.getContext(), "vertex_index_high", function);
  auto *merge_bb = llvm::BasicBlock::Create(builder.getContext(), "vertex_index_merge", function);
  auto *low_end = builder.GetInsertBlock();
  builder.CreateCondBr(wide, high_bb, merge_bb);
  builder.SetInsertPoint(high_bb);
  auto *combined = builder.CreateOr(low, builder.CreateShl(load_word(builder.CreateAdd(word, builder.getInt32(1))), 16));
  builder.CreateBr(merge_bb);
  builder.SetInsertPoint(merge_bb);
  auto *index = builder.CreatePHI(int_ty, 2);
  index->addIncoming(low, low_end);
  index->addIncoming(combined, high_bb);
  auto *with_base = builder.CreateAdd(index, base_vertex);
  builder.CreateBr(join_bb);

  builder.SetInsertPoint(join_bb);
  auto *vertex_id = builder.CreatePHI(int_ty, 2);
  vertex_id->addIncoming(key, entry);
  vertex_id->addIncoming(index, merge_bb);
  auto *vertex_id_with_base = builder.CreatePHI(int_ty, 2);
  vertex_id_with_base->addIncoming(resource.vertex_id_with_base, entry);
  vertex_id_with_base->addIncoming(with_base, merge_bb);
  resource.vertex_id = vertex_id;
  resource.vertex_id_with_base = vertex_id_with_base;
}

RegisterComponentType
component_type_from_pixel_format(air::MTLPixelFormat format) {
  switch (format) {
  case air::MTLPixelFormat::R8Sint:
  case air::MTLPixelFormat::R16Sint:
  case air::MTLPixelFormat::RG8Sint:
  case air::MTLPixelFormat::R32Sint:
  case air::MTLPixelFormat::RG16Sint:
  case air::MTLPixelFormat::RGBA8Sint:
  case air::MTLPixelFormat::RG32Sint:
  case air::MTLPixelFormat::RGBA16Sint:
  case air::MTLPixelFormat::RGBA32Sint:
    return RegisterComponentType::Int;
  case air::MTLPixelFormat::R8Uint:
  case air::MTLPixelFormat::R16Uint:
  case air::MTLPixelFormat::RG8Uint:
  case air::MTLPixelFormat::R32Uint:
  case air::MTLPixelFormat::RG16Uint:
  case air::MTLPixelFormat::RGBA8Uint:
  case air::MTLPixelFormat::RGB10A2Uint:
  case air::MTLPixelFormat::RG32Uint:
  case air::MTLPixelFormat::RGBA16Uint:
  case air::MTLPixelFormat::RGBA32Uint:
    return RegisterComponentType::Uint;
  case air::MTLPixelFormat::A8Unorm:
  case air::MTLPixelFormat::R8Unorm:
  case air::MTLPixelFormat::R8Unorm_sRGB:
  case air::MTLPixelFormat::R8Snorm:
  case air::MTLPixelFormat::R16Unorm:
  case air::MTLPixelFormat::R16Snorm:
  case air::MTLPixelFormat::R16Float:
  case air::MTLPixelFormat::RG8Unorm:
  case air::MTLPixelFormat::RG8Unorm_sRGB:
  case air::MTLPixelFormat::RG8Snorm:
  case air::MTLPixelFormat::B5G6R5Unorm:
  case air::MTLPixelFormat::A1BGR5Unorm:
  case air::MTLPixelFormat::ABGR4Unorm:
  case air::MTLPixelFormat::BGR5A1Unorm:
  case air::MTLPixelFormat::R32Float:
  case air::MTLPixelFormat::RG16Unorm:
  case air::MTLPixelFormat::RG16Snorm:
  case air::MTLPixelFormat::RG16Float:
  case air::MTLPixelFormat::RGBA8Unorm:
  case air::MTLPixelFormat::RGBA8Unorm_sRGB:
  case air::MTLPixelFormat::RGBA8Snorm:
  case air::MTLPixelFormat::BGRA8Unorm:
  case air::MTLPixelFormat::BGRA8Unorm_sRGB:
  case air::MTLPixelFormat::RGB10A2Unorm:
  case air::MTLPixelFormat::RG11B10Float:
  case air::MTLPixelFormat::RGB9E5Float:
  case air::MTLPixelFormat::BGR10A2Unorm:
  case air::MTLPixelFormat::BGR10_XR:
  case air::MTLPixelFormat::BGR10_XR_sRGB:
  case air::MTLPixelFormat::RG32Float:
  case air::MTLPixelFormat::RGBA16Unorm:
  case air::MTLPixelFormat::RGBA16Snorm:
  case air::MTLPixelFormat::RGBA16Float:
  case air::MTLPixelFormat::BGRA10_XR:
  case air::MTLPixelFormat::BGRA10_XR_sRGB:
  case air::MTLPixelFormat::RGBA32Float:
  case air::MTLPixelFormat::BC1_RGBA:
  case air::MTLPixelFormat::BC1_RGBA_sRGB:
  case air::MTLPixelFormat::BC2_RGBA:
  case air::MTLPixelFormat::BC2_RGBA_sRGB:
  case air::MTLPixelFormat::BC3_RGBA:
  case air::MTLPixelFormat::BC3_RGBA_sRGB:
  case air::MTLPixelFormat::BC4_RUnorm:
  case air::MTLPixelFormat::BC4_RSnorm:
  case air::MTLPixelFormat::BC5_RGUnorm:
  case air::MTLPixelFormat::BC5_RGSnorm:
  case air::MTLPixelFormat::BC6H_RGBFloat:
  case air::MTLPixelFormat::BC6H_RGBUfloat:
  case air::MTLPixelFormat::BC7_RGBAUnorm:
  case air::MTLPixelFormat::BC7_RGBAUnorm_sRGB:
    return RegisterComponentType::Float;
  default:
    break;
  }
  return RegisterComponentType::Unknown;
}

llvm::Error convert_dxbc_pixel_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  uint32_t pso_sample_mask = 0xffffffff;
  bool pso_dual_source_blending = false;
  bool pso_disable_depth_output = false;
  uint32_t pso_unorm_output_reg_mask = 0;
  uint32_t *pso_pixel_formats = nullptr;
  SM50_SHADER_PSO_PIXEL_SHADER_DATA *pso_data = nullptr;
  if (args_get_data<SM50_SHADER_PSO_PIXEL_SHADER, SM50_SHADER_PSO_PIXEL_SHADER_DATA>(pArgs, &pso_data)) {
    pso_dual_source_blending = pso_data->dual_source_blending;
    pso_disable_depth_output = pso_data->disable_depth_output;
    pso_unorm_output_reg_mask = pso_data->unorm_output_reg_mask;
    pso_sample_mask = pso_data->sample_mask;
    pso_pixel_formats = pso_data->pixel_formats;
  }
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *coarse_mask_args = nullptr;
  const bool coarse_mask_output = pso_sample_mask != 0xffffffff &&
    args_get_data<SM50_SHADER_D3D12_COARSE_SHADING, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(pArgs, &coarse_mask_args);
  // SampleMask is applied per fine pixel by captured coverage/replay, not to the
  // packed coarse mask produced or consumed by the original shader body.
  if (coarse_mask_output) pso_sample_mask = 0xffffffff;
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  if (pShaderInternal->ps_has_inner_coverage_input && !rootsig)
    return llvm::make_error<UnsupportedFeature>("SV_InnerCoverage requires D3D12 ConservativeRasterization Tier3");
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *stats = nullptr;
  args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(pArgs, &stats);
  SM50_SHADER_D3D12_MSAA_PIXEL_DATA *msaa_pixel = nullptr;
  args_get_data<SM50_SHADER_D3D12_MSAA_PIXEL, SM50_SHADER_D3D12_MSAA_PIXEL_DATA>(pArgs, &msaa_pixel);

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    auto retTy = ctx.function->getReturnType();
    if (retTy->isVoidTy()) {
      return nullptr;
    }
    return llvm::ConstantAggregateZero::get(retTy);
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.dual_source_blending = pso_dual_source_blending;
    sig_ctx.disable_depth_output = pso_disable_depth_output;
    sig_ctx.captured_pixel = msaa_pixel != nullptr;
    sig_ctx.pull_mode_reg_mask = msaa_pixel ? 0xffffffffu : shader_info->pull_mode_reg_mask;
    sig_ctx.unorm_output_reg_mask = pso_unorm_output_reg_mask;
    if (pso_pixel_formats)
      memcpy(sig_ctx.pixel_formats, pso_pixel_formats, sizeof(sig_ctx.pixel_formats));
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }
  initialize_sample_mask_constant(module, pArgs);
  if (pso_sample_mask != 0xffffffff || coarse_mask_output) {
    auto assigned_index =
      func_signature.DefineOutput(air::OutputCoverageMask{});
    epilogue >> [=](pvalue value) -> IRValue {
      return make_irvalue([=](struct context ctx) {
        auto &builder = ctx.builder;
        if (ctx.resource.coverage_mask_reg)
          return value;
        return builder.CreateInsertValue(
          value, pixel_sample_mask(ctx), {assigned_index}
        );
      });
    };
  }

  const auto stats_arg = stats ? define_pipeline_statistics(func_signature) : ~0u;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *per_pixel = nullptr;
  args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS_PER_PIXEL, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(pArgs, &per_pixel);
  std::optional<PixelStatisticsDedupe> stats_dedupe;
  if (stats && per_pixel)
    stats_dedupe = define_pixel_statistics_dedupe(func_signature);
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *claim_arg = nullptr;
  args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS_PRIMITIVE_CLAIM, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(pArgs, &claim_arg);
  std::optional<PixelStatisticsClaim> stats_claim;
  if (stats && claim_arg)
    stats_claim = define_pixel_statistics_claim(func_signature);
  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_PIXEL_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);

  auto [function, function_metadata] =
    func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  setup_metal_version(module, metal_version, msaa_pixel);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  resource_map.input.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.input.ptr_float4 = builder.CreateBitCast(
    resource_map.input.ptr_int4,
    llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );
  resource_map.input_element_count = max_input_register;
  resource_map.output.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register)
    );
  resource_map.output.ptr_float4 = builder.CreateBitCast(
    resource_map.output.ptr_int4,
    llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
  );
  resource_map.output_element_count = max_output_register;

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = pso_sample_mask,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
  };

  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }
  if (stats_claim)
    claim_pixel_invocation(builder, air, function, stats_arg, *stats_claim);
  else if (stats)
    increment_pixel_invocations(builder, air, function, stats_arg, stats_dedupe ? &*stats_dedupe : nullptr);
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  auto epilogue_result = epilogue.build(ctx);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }
  auto value = epilogue_result.get();
  if (value == nullptr) {
    builder.CreateRetVoid();
  } else {
    builder.CreateRet(value);
  }

  module.getOrInsertNamedMetadata("air.fragment")
    ->addOperand(function_metadata);

  return llvm::Error::success();
};

llvm::Error convert_dxbc_compute_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *stats = nullptr;
  args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(pArgs, &stats);

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    auto retTy = ctx.function->getReturnType();
    if (retTy->isVoidTy()) {
      return nullptr;
    }
    return llvm::ConstantAggregateZero::get(retTy);
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }
  }

  const auto stats_arg = stats ? define_pipeline_statistics(func_signature) : ~0u;
  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D11_SB_COMPUTE_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);
  setup_tgsm(shader_info, resource_map, types, module);

  auto [function, function_metadata] =
    func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
  };

  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }
  if (stats)
    increment_pipeline_statistics(builder, air, function, stats_arg, 20);
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);

  if (auto err = epilogue.build(ctx).takeError()) {
    return err;
  }
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata("air.kernel")->addOperand(function_metadata);

  return llvm::Error::success();
};

llvm::Error convert_dxbc_vertex_shader(
  SM50ShaderInternal *pShaderInternal, const char *name,
  llvm::LLVMContext &context, llvm::Module &module,
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);

  uint32_t max_input_register = pShaderInternal->max_input_register;
  uint32_t max_output_register = pShaderInternal->max_output_register;
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA *vertex_so = nullptr;
  args_get_data<SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT, SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA>(
      pArgs, &vertex_so
  );
  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_GS_PASS_THROUGH_DATA *gs_passthrough = nullptr;
  bool rasterization_disabled =
      (args_get_data<SM50_SHADER_GS_PASS_THROUGH, SM50_SHADER_GS_PASS_THROUGH_DATA>(pArgs, &gs_passthrough) &&
       gs_passthrough->RasterizationDisabled) ||
      (vertex_so != nullptr);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *stats = nullptr;
  args_get_data<SM50_SHADER_D3D12_PIPELINE_STATS, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(pArgs, &stats);
  SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA *so_capture = nullptr;
  args_get_data<SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE, SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA>(
      pArgs, &so_capture
  );
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *index_pull = nullptr;
  args_get_data<SM50_SHADER_D3D12_INDEX_PULL, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(pArgs, &index_pull);
  SM50_SHADER_D3D12_MSAA_VERTEX_DATA *msaa_vertex = nullptr;
  args_get_data<SM50_SHADER_D3D12_MSAA_VERTEX, SM50_SHADER_D3D12_MSAA_VERTEX_DATA>(pArgs, &msaa_vertex);
  if (index_pull && (so_capture || vertex_so) &&
      !(msaa_vertex && msaa_vertex->variant == SM50_MSAA_VERTEX_CAPTURE && so_capture && !vertex_so))
    return llvm::make_error<UnsupportedFeature>("standalone index pulling and stream output on the same vertex shader");
  std::vector<StreamOutputCaptureRun> so_capture_runs;
  if (so_capture) {
    if (vertex_so)
      return llvm::make_error<UnsupportedFeature>("D3D11 and D3D12 stream output on the same vertex shader");
    for (uint32_t i = 0; i < so_capture->num_elements; i++) {
      auto &element = so_capture->elements[i];
      if (!element.semantic_name)
        return llvm::make_error<UnsupportedFeature>("stream output capture element has no semantic name");
      auto output = std::find_if(
          pShaderInternal->output_signature.begin(), pShaderInternal->output_signature.end(),
          [&](const Signature &sig) {
            return sig.semanticIndex() == element.semantic_index &&
                   llvm::StringRef(sig.semanticName()).equals_insensitive(element.semantic_name);
          }
      );
      if (output == pShaderInternal->output_signature.end())
        return llvm::make_error<UnsupportedFeature>(
            "stream output capture element " + std::string(element.semantic_name) +
            std::to_string(element.semantic_index) + " is not a vertex shader output"
        );
      auto run = resolve_stream_output_capture(*so_capture, element, output->reg(), output->mask());
      if (!run)
        return run.takeError();
      so_capture_runs.push_back(*run);
    }
  }

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue epilogue([](struct context ctx) -> pvalue {
    auto retTy = ctx.function->getReturnType();
    if (retTy->isVoidTy()) {
      return nullptr;
    }
    return llvm::ConstantAggregateZero::get(retTy);
  });

  io_binding_map resource_map;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, epilogue, func_signature, resource_map);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.dense_vertex_table = rootsig != nullptr;
    sig_ctx.skip_vertex_output = rasterization_disabled;
    for (auto &p : pShaderInternal->signature_handlers) {
      p(sig_ctx);
    }

    for (auto& out : pShaderInternal->output_signature) {
      if(out.isSystemValue()) continue;
      func_signature.DefineOutput(air::OutputVertex{
        .user = out.varyingAttributeName(),
        .type = to_msl_type(out.componentType()),
      });
    }
  }
  if (vertex_so) {
    auto bv = func_signature.DefineInput(air::InputBaseVertex{});
    auto vid = func_signature.DefineInput(air::InputVertexID{});
    auto so_table = func_signature.DefineInput(air::ArgumentBindingBuffer{
      .buffer_size = {},
      .location_index = SM50_BINDING_INDEX_STREAM_OUTPUT0,
      .array_size = 0,
      .memory_access = air::MemoryAccess::read,
      .address_space = air::AddressSpace::constant,
      .type = air::MSLUint{},
      .arg_name = "stream_outputs",
      .raster_order_group = {}
    });
    epilogue << make_irvalue([=](struct context ctx) {
      auto &builder = ctx.builder;
      auto base_vertex = ctx.function->getArg(bv);
      auto vertex_id = ctx.function->getArg(vid);
      auto so_entries_type = ctx.types._dxmt_stream_output_buffer_entry;
      auto so_entries = builder.CreateBitCast(
          ctx.function->getArg(so_table), so_entries_type->getPointerTo((uint32_t)air::AddressSpace::constant)
      );
      auto slot0_entry =
          builder.CreateLoad(so_entries_type, builder.CreateConstInBoundsGEP1_32(so_entries_type, so_entries, 0));
      auto slot0 = builder.CreateExtractValue(slot0_entry, {0});
      auto adjusted_vertex_id = builder.CreateSub(vertex_id, base_vertex);
      auto output_regs = builder.CreateBitOrPointerCast(
        ctx.resource.output.ptr_int4, llvm::PointerType::get(ctx.types._int, 0)
      );
      for (unsigned i = 0; i < vertex_so->num_elements; i++) {
        auto &element = vertex_so->elements[i];
        if (element.reg_id == 0xffffffff)
          continue;
        auto ptr = ctx.builder.CreateConstGEP1_32(
          ctx.types._int, output_regs,
          (unsigned)(element.reg_id * 4 + element.component)
        );
        auto target_offset = ctx.builder.CreateAdd(
          ctx.builder.CreateMul(
            adjusted_vertex_id,
            ctx.builder.getInt32(vertex_so->strides[element.output_slot /* expected to be 0 */])
          ),
          ctx.builder.getInt32(element.offset)
        );
        auto target_ptr = ctx.builder.CreateGEP(
          ctx.types._int, slot0, {ctx.builder.CreateLShr(target_offset, 2)}
        );
        ctx.builder.CreateStore(
          ctx.builder.CreateLoad(ctx.types._int, ptr), target_ptr, true
        );
      }
      return nullptr;
    });
  }
  VertexIndexPullArgs vertex_index_args;
  if (so_capture)
    vertex_index_args =
      define_stream_output_capture(func_signature, epilogue, std::move(so_capture_runs), so_capture->record_dwords);
  else if (index_pull)
    vertex_index_args = define_vertex_index_pull(func_signature);

  uint32_t vertex_idx = func_signature.DefineInput(air::InputVertexID{});
  uint32_t base_vertex_idx = func_signature.DefineInput(air::InputBaseVertex{});
  uint32_t instance_idx = func_signature.DefineInput(air::InputInstanceID{});
  uint32_t base_instance_idx =
    func_signature.DefineInput(air::InputBaseInstance{});

  uint32_t clip_distance_out_idx = ~0u;
  if (!rasterization_disabled && pShaderInternal->clip_distance_scalars.size()) {
    clip_distance_out_idx = func_signature.DefineOutput(
      air::OutputClipDistance{pShaderInternal->clip_distance_scalars.size()}
    );
  };

  const auto stats_arg = stats ? define_pipeline_statistics(func_signature) : ~0u;
  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, D3D10_SB_VERTEX_SHADER, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);

  uint32_t rta_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.RenderTargetArrayIndexReg != 255) {
    rta_idx_out =
      func_signature.DefineOutput(air::OutputRenderTargetArrayIndex{});
  }
  uint32_t va_idx_out = ~0u;
  if (gs_passthrough && gs_passthrough->Data.ViewportArrayIndexReg != 255) {
    va_idx_out = func_signature.DefineOutput(air::OutputViewportArrayIndex{});
  }

  auto [function, function_metadata] = func_signature.CreateFunction(
    name, context, module, 0,
    rasterization_disabled
  );

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);

  resource_map.vertex_id_with_base = function->getArg(vertex_idx);
  resource_map.base_vertex_id = function->getArg(base_vertex_idx);
  resource_map.instance_id_with_base = function->getArg(instance_idx);
  resource_map.base_instance_id = function->getArg(base_instance_idx);
  resource_map.vertex_id = builder.CreateSub(
    resource_map.vertex_id_with_base, resource_map.base_vertex_id
  );
  resource_map.instance_id = builder.CreateSub(
    resource_map.instance_id_with_base, resource_map.base_instance_id
  );

  resource_map.input.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_input_register));
  resource_map.input.ptr_float4 = builder.CreateBitCast(
    resource_map.input.ptr_int4,
    llvm::ArrayType::get(types._float4, max_input_register)->getPointerTo()
  );
  resource_map.input_element_count = max_input_register;
  resource_map.output.ptr_int4 =
    builder.CreateAlloca(llvm::ArrayType::get(types._int4, max_output_register)
    );
  resource_map.output.ptr_float4 = builder.CreateBitCast(
    resource_map.output.ptr_int4,
    llvm::ArrayType::get(types._float4, max_output_register)->getPointerTo()
  );
  resource_map.output_element_count = max_output_register;

  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(
    shader_info, resource_map, types, module, builder
  );

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
      .capture_raw_distance_output = msaa_vertex && msaa_vertex->variant == SM50_MSAA_VERTEX_CAPTURE,
  };

  if (so_capture)
    resource_map.so_capture_key = resource_map.vertex_id;
  if (so_capture || index_pull)
    apply_vertex_index_pull(
      builder, function, vertex_index_args, resource_map,
      so_capture ? offsetof(SM50_SO_CAPTURE_CONTROL, index_size) / 4 : 0
    );
  if (auto err = prologue.build(ctx).takeError()) {
    return err;
  }
  if (stats)
    increment_pipeline_statistics(builder, air, function, stats_arg, 4);
  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError()) {
    return err;
  }
  builder.CreateBr(real_entry.get());

  builder.SetInsertPoint(epilogue_bb);
  auto epilogue_result = epilogue.build(ctx);
  if (auto err = epilogue_result.takeError()) {
    return err;
  }
  auto value = epilogue_result.get();

  /* A rasterization-disabled vertex function returns void: no clip distances. */
  if (clip_distance_out_idx != ~0u) {
    auto clip_distance_ty = llvm::ArrayType::get(
      types._float, pShaderInternal->clip_distance_scalars.size()
    );
    pvalue clip_distance_array = llvm::ConstantAggregateZero::get(clip_distance_ty);
    unsigned clip_distance_idx = 0;
    for (auto &scalar : pShaderInternal->clip_distance_scalars) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_float4->getType()
          ->getNonOpaquePointerElementType(),
        resource_map.output.ptr_float4,
        {builder.getInt32(0), builder.getInt32(scalar.reg),
         builder.getInt32(scalar.component)}
      );
      clip_distance_array = builder.CreateInsertValue(
        clip_distance_array, builder.CreateLoad(types._float, src_ptr), {clip_distance_idx++}
      );
    }
    value = builder.CreateInsertValue(
      value, clip_distance_array, {clip_distance_out_idx}
    );
  };

  if (value == nullptr) {
    builder.CreateRetVoid();
  } else {
    if (rta_idx_out != ~0u) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(
        ),
        resource_map.output.ptr_int4,
        {builder.getInt32(0),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.RenderTargetArrayIndexComponent)}
      );
      value = builder.CreateInsertValue(
        value, builder.CreateLoad(types._int, src_ptr), {rta_idx_out}
      );
    }
    if (va_idx_out != ~0u) {
      auto src_ptr = builder.CreateGEP(
        resource_map.output.ptr_int4->getType()->getNonOpaquePointerElementType(
        ),
        resource_map.output.ptr_int4,
        {builder.getInt32(0),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexReg),
         builder.getInt32(gs_passthrough->Data.ViewportArrayIndexComponent)}
      );
      value = builder.CreateInsertValue(
        value, builder.CreateLoad(types._int, src_ptr), {va_idx_out}
      );
    }
    builder.CreateRet(value);
  }

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_metadata);
  return llvm::Error::success();
};

llvm::Error convertDXBC(
  sm50_shader_t pShader, const char *name, llvm::LLVMContext &context,
  llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  using namespace microsoft;

  auto pShaderInternal = (SM50ShaderInternal *)pShader;

  if (pShaderInternal->dxil_bytecode) {
    /* SM 6.x shaders carry no DXBC control flow; the DXIL pipeline is entered
     * through SM50Compile. */
    return llvm::make_error<UnsupportedFeature>(
      "DXIL shader must be converted through SM50Compile."
    );
  }

  switch (pShaderInternal->shader_type) {
  case microsoft::D3D10_SB_PIXEL_SHADER:
    return convert_dxbc_pixel_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D10_SB_VERTEX_SHADER:
    return convert_dxbc_vertex_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D11_SB_COMPUTE_SHADER:
    return convert_dxbc_compute_shader(
      pShaderInternal, name, context, module, pArgs
    );
  case microsoft::D3D11_SB_HULL_SHADER:
  case microsoft::D3D11_SB_DOMAIN_SHADER:
    return llvm::make_error<UnsupportedFeature>(
      "Hull and domain shader cannot be independently converted."
    );
  case microsoft::D3D10_SB_GEOMETRY_SHADER:
    return llvm::make_error<UnsupportedFeature>(
      "Geometry shader cannot be independently converted."
    );
  case microsoft::D3D12_SB_MESH_SHADER:
  case microsoft::D3D12_SB_AMPLIFICATION_SHADER:
  case microsoft::D3D11_SB_RESERVED0:
    break;
  }
  return llvm::make_error<UnsupportedFeature>("Not supported shader type");
};
} // namespace dxmt::dxbc

bool CheckGSBBIsPassThrough(dxmt::dxbc::BasicBlock *bb) {
  using namespace dxmt::dxbc;
  if (bb->instructions.any(patterns{
        [](InstNop &) { return false; }, [](InstMov &mov) { return false; },
        [](InstEmit &) { return false; }, [](InstCut &) { return false; },
        [](auto &) { return true; }
      }))
    return false;

  return std::visit(
    patterns{
      [](dxmt::dxbc::BasicBlockReturn) { return true; },
      [](dxmt::dxbc::BasicBlockUnconditionalBranch uncond) {
        return CheckGSBBIsPassThrough(uncond.target);
      },
      [](auto) { return false; }
    },
    bb->target
  );
}

bool CheckGSSignatureIsPassThrough(
  microsoft::CSignatureParser &input, microsoft::CSignatureParser5 &output,
  MTL_GEOMETRY_SHADER_PASS_THROUGH &data
) {
  using namespace microsoft;

  data.RenderTargetArrayIndexComponent = 255;
  data.RenderTargetArrayIndexReg = 255;
  data.ViewportArrayIndexComponent = 255;
  data.ViewportArrayIndexReg = 255;

  if (output.NumStreams() > 1) {
    return false;
  }
  if (output.RasterizedStream()) {
    return false;
  }

  const D3D11_SIGNATURE_PARAMETER *input_parameters, *output_parameters;
  input.GetParameters(&input_parameters);
  output.Signature(0)->GetParameters(&output_parameters);
  size_t num_input, num_output;
  num_input = input.GetNumParameters();
  num_output = output.Signature(0)->GetNumParameters();
  if (num_output > num_input) {
    // at least it is a non-trivial shader that generates new data
    return false;
  }
  bool has_match_record;
  for (unsigned i = 0; i < num_output; i++) {
    has_match_record = false;
    const D3D11_SIGNATURE_PARAMETER &out = output_parameters[i];
    for (unsigned j = 0; j < num_input; j++) {
      const D3D11_SIGNATURE_PARAMETER &in = input_parameters[j];
      if (out.Register == in.Register && (out.Mask & in.Mask) == out.Mask) {
        if (out.SystemValue == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX) {
          data.RenderTargetArrayIndexReg = out.Register;
          data.RenderTargetArrayIndexComponent = __builtin_ctz(out.Mask);
        } else if (out.SystemValue == D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX) {
          data.ViewportArrayIndexReg = out.Register;
          data.ViewportArrayIndexComponent = __builtin_ctz(out.Mask);
        } else {
          if (out.SemanticIndex != in.SemanticIndex) {
            return false;
          }
          if (strcasecmp(out.SemanticName, in.SemanticName)) {
            return false;
          }
        }
        has_match_record = true;
        break;
      }
    }
    if (!has_match_record) {
      return false;
    }
  }

  return true;
};

/**
 * Builds the AIR argument tables from shader_info. Both the SM 5.x DXBC and the
 * SM 6.x DXIL paths feed their own ShaderInfo here so the argument layout and the
 * SM50 argument reflection are derived in exactly one place.
 */
dxmt::dxbc::ArgumentTableMasks
dxmt::dxbc::build_argument_tables(dxmt::dxbc::SM50ShaderInternal *shader) {
  using namespace dxmt::air;
  using namespace dxmt::shader::common;
  auto shader_info = &shader->shader_info;
  auto &binding_table = shader_info->binding_table;
  auto &binding_table_cbuffer = shader_info->binding_table_cbuffer;
  uint32_t visibility = 0;
  switch (shader->shader_type) {
  case microsoft::D3D10_SB_VERTEX_SHADER: visibility = 1; break;
  case microsoft::D3D11_SB_HULL_SHADER: visibility = 2; break;
  case microsoft::D3D11_SB_DOMAIN_SHADER: visibility = 3; break;
  case microsoft::D3D10_SB_GEOMETRY_SHADER: visibility = 4; break;
  case microsoft::D3D10_SB_PIXEL_SHADER: visibility = 5; break;
  case microsoft::D3D12_SB_AMPLIFICATION_SHADER: visibility = 6; break;
  case microsoft::D3D12_SB_MESH_SHADER: visibility = 7; break;
  default: break;
  }

  uint16_t binding_cbuffer_mask = 0;
  uint16_t binding_sampler_mask = 0;
  uint64_t binding_uav_mask = 0;
  uint64_t binding_srv_hi_mask = 0;
  uint64_t binding_srv_lo_mask = 0;

  for (auto &[range_id, cbv] : shader_info->cbufferMap) {
    // TODO: abstract SM 5.0 binding
    cbv.arg_index = binding_table_cbuffer.DefineBuffer(
      "cb" + std::to_string(range_id), AddressSpace::constant,
      MemoryAccess::read, msl_uint4,
      GetArgumentIndex(SM50BindingType::ConstantBuffer, range_id)
    );
    shader->args_reflection_cbuffer.push_back({
      .Type = SM50BindingType::ConstantBuffer,
      .SM50BindingSlot = range_id,
      .Flags =
        MTL_SM50_SHADER_ARGUMENT_BUFFER | MTL_SM50_SHADER_ARGUMENT_READ_ACCESS,
      .StructurePtrOffset = cbv.arg_index,
      .RegisterSpace = cbv.range.space,
      .RegisterLowerBound = cbv.range.lower_bound,
      .RegisterCount = cbv.range.size,
      .ShaderVisibilityMask = 1u << visibility,
    });
    binding_cbuffer_mask |= (1 << range_id);
  }
  for (auto &[range_id, sampler] : shader_info->samplerMap) {
    // TODO: abstract SM 5.0 binding
    auto attr_index = GetArgumentIndex(SM50BindingType::Sampler, range_id);
    sampler.arg_index =
      binding_table.DefineSampler("s" + std::to_string(range_id), attr_index);
    sampler.arg_cube_index =
      binding_table.DefineSampler("cube_s" + std::to_string(range_id), attr_index + 1);
    sampler.arg_metadata_index = binding_table.DefineInteger64(
      "meta_s" + std::to_string(range_id), attr_index + 2
    );
    shader->args_reflection.push_back({
      .Type = SM50BindingType::Sampler,
      .SM50BindingSlot = range_id,
      .Flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0,
      .StructurePtrOffset = sampler.arg_index,
    });
    binding_sampler_mask |= (1 << range_id);
  }
  for (auto &[range_id, srv] : shader_info->srvMap) {
    auto attr_index = GetArgumentIndex(SM50BindingType::SRV, range_id);
    if (srv.resource_type != ResourceType::NonApplicable) {
      // TODO: abstract SM 5.0 binding
      auto access = srv.sampled ? MemoryAccess::sample : MemoryAccess::read;
      auto texture_kind = to_air_resource_type(srv.resource_type, srv.compared);
      auto scaler_type = to_air_scaler_type(srv.scaler_type);
      srv.arg_index = binding_table.DefineTexture(
        "t" + std::to_string(range_id), texture_kind, access, scaler_type,
        attr_index
      );
    } else {
      srv.arg_index = binding_table.DefineBuffer(
        "t" + std::to_string(range_id),
        AddressSpace::device, // it needs to be `device` since `constant` has
                              // size and alignment restriction
        MemoryAccess::read, msl_uint, attr_index
      );
    }
    srv.arg_metadata_index = binding_table.DefineInteger64(
      "meta_t" + std::to_string(range_id), attr_index + 1
    );
    MTL_SM50_SHADER_ARGUMENT_FLAG flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0;
    if (srv.resource_type == ResourceType::NonApplicable) {
      flags |= MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH |
               MTL_SM50_SHADER_ARGUMENT_BUFFER;
    } else if (srv.resource_type == ResourceType::TextureBuffer) {
      flags |= MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    } else {
      flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
      switch (srv.resource_type) {
      case ResourceType::Texture1DArray:
      case ResourceType::Texture2DArray:
      case ResourceType::Texture2DMultisampledArray:
      case ResourceType::TextureCubeArray:
        flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
        break;
      default:
        break;
      }
    }
    if (srv.read || srv.sampled || srv.compared) {
      flags |= MTL_SM50_SHADER_ARGUMENT_READ_ACCESS;
    }
    shader->args_reflection.push_back({
      .Type = SM50BindingType::SRV,
      .SM50BindingSlot = range_id,
      .Flags = flags,
      .StructurePtrOffset = srv.arg_index,
      .RegisterSpace = srv.range.space,
      .RegisterLowerBound = srv.range.lower_bound,
      .RegisterCount = srv.range.size,
      .ShaderVisibilityMask = 1u << visibility,
    });
    if (range_id & 64) {
      binding_srv_hi_mask |= (1ULL << (range_id - 64));
    } else {
      binding_srv_lo_mask |= (1ULL << range_id);
    }
  }
  for (auto &[range_id, uav] : shader_info->uavMap) {
    auto access =
      uav.written ? (uav.read ? MemoryAccess::read_write : MemoryAccess::write)
                  : MemoryAccess::read;
    auto attr_index = GetArgumentIndex(SM50BindingType::UAV, range_id);
    if (uav.resource_type != ResourceType::NonApplicable) {
      auto texture_kind = to_air_resource_type(uav.resource_type);
      auto scaler_type = to_air_scaler_type(uav.scaler_type);
      uav.arg_index = binding_table.DefineTexture(
        "u" + std::to_string(range_id), texture_kind, access, scaler_type,
        attr_index, uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    } else {
      uav.arg_index = binding_table.DefineBuffer(
        "u" + std::to_string(range_id), AddressSpace::device, access, msl_uint,
        attr_index, uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    }
    uav.arg_metadata_index = binding_table.DefineInteger64(
      "meta_u" + std::to_string(range_id), attr_index + 1
    );
    if (uav.with_counter) {
      uav.arg_counter_index = binding_table.DefineBuffer(
        "counter" + std::to_string(range_id), AddressSpace::device,
        MemoryAccess::read_write, msl_uint, attr_index + 2,
        uav.rasterizer_order ? std::optional(1) : std::nullopt
      );
    }
    MTL_SM50_SHADER_ARGUMENT_FLAG flags = (MTL_SM50_SHADER_ARGUMENT_FLAG)0;
    if (uav.resource_type == ResourceType::NonApplicable) {
      flags |= MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH |
               MTL_SM50_SHADER_ARGUMENT_BUFFER;
    } else if (uav.resource_type == ResourceType::TextureBuffer) {
      flags |= MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    } else {
      flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP |
               MTL_SM50_SHADER_ARGUMENT_TEXTURE;
      switch (uav.resource_type) {
      case ResourceType::Texture1DArray:
      case ResourceType::Texture2DArray:
      case ResourceType::Texture2DMultisampledArray:
      case ResourceType::TextureCubeArray:
        flags |= MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
        break;
      default:
        break;
      }
    }
    if (uav.read) {
      flags |= MTL_SM50_SHADER_ARGUMENT_READ_ACCESS;
    }
    if (uav.written) {
      flags |= MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS;
    }
    if (uav.with_counter) {
      flags |= MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER;
    }
    shader->args_reflection.push_back({
      .Type = SM50BindingType::UAV,
      .SM50BindingSlot = range_id,
      .Flags = flags,
      .StructurePtrOffset = uav.arg_index,
      .RegisterSpace = uav.range.space,
      .RegisterLowerBound = uav.range.lower_bound,
      .RegisterCount = uav.range.size,
      .ShaderVisibilityMask = 1u << visibility,
    });
    binding_uav_mask |= (1ULL << range_id);
  }
  return {
    .constant_buffer = binding_cbuffer_mask,
    .sampler = binding_sampler_mask,
    .srv_lo = binding_srv_lo_mask,
    .srv_hi = binding_srv_hi_mask,
    .uav = binding_uav_mask,
  };
}

/**
 * Fills the MTL reflection from the shader's reflection state. The DXBC signature
 * parsers are only used by the geometry stage and may be null; it is told apart on
 * the caller side, since DXIL geometry shaders are rejected before this point.
 */
bool dxmt::dxbc::fill_shader_reflection(
  dxmt::dxbc::SM50ShaderInternal *shader, const dxmt::dxbc::ArgumentTableMasks &masks,
  MTL_SHADER_REFLECTION *pRefl, microsoft::CSignatureParser *inputParser,
  microsoft::CSignatureParser5 *outputParser, llvm::raw_ostream &error
) {
  using namespace dxmt::air;
  using namespace dxmt::shader::common;
pRefl->ConstanttBufferTableBindIndex =
    shader->args_reflection_cbuffer.size() > 0 ? 29 : ~0u;
  pRefl->ArgumentBufferBindIndex =
    shader->args_reflection.size() > 0 ? 30 : ~0u;
  pRefl->ConstantBufferSlotMask = masks.constant_buffer;
  pRefl->SamplerSlotMask = masks.sampler;
  pRefl->SRVSlotMaskHi = masks.srv_hi;
  pRefl->SRVSlotMaskLo = masks.srv_lo;
  pRefl->UAVSlotMask = masks.uav;
  pRefl->NumConstantBuffers = shader->args_reflection_cbuffer.size();
  pRefl->NumArguments = shader->args_reflection.size();
  pRefl->HasAtomic64 = 0;
  pRefl->PSCoverageInputs = 0;
  if (shader->shader_type == microsoft::D3D11_SB_COMPUTE_SHADER ||
      shader->shader_type == microsoft::D3D12_SB_MESH_SHADER ||
      shader->shader_type == microsoft::D3D12_SB_AMPLIFICATION_SHADER) {
    pRefl->ThreadgroupSize[0] = shader->threadgroup_size[0];
    pRefl->ThreadgroupSize[1] = shader->threadgroup_size[1];
    pRefl->ThreadgroupSize[2] = shader->threadgroup_size[2];
  }
  if (shader->shader_type == microsoft::D3D11_SB_HULL_SHADER) {
    auto threads_per_patch = next_pow2(shader->hull_maximum_threads_per_patch);
    if (threads_per_patch > 32) {
      error << "Threadgroup size of tessellation pipeline is too large.";
      return false;
    }
    auto patch_per_group = 32 / threads_per_patch;
    float max_tesselation_factor = shader->max_tesselation_factor;

    while (estimate_payload_size(shader, max_tesselation_factor, patch_per_group) > 16384) {
      if (patch_per_group == 1) {
        if (max_tesselation_factor > 1.0f) {
          max_tesselation_factor = std::max(max_tesselation_factor - 2.0f, 1.0f);
        } else {
          error << "Payload size of tessellation pipeline is too large.";
          return false;
        }
      } else {
        patch_per_group = patch_per_group >> 1;
      }
    }
    shader->hull_maximum_threads_per_patch = 32 / patch_per_group;
    shader->max_tesselation_factor = max_tesselation_factor;

    pRefl->ThreadsPerPatch = shader->hull_maximum_threads_per_patch;
    pRefl->Tessellator = {
      .Partition = shader->tessellation_partition,
      .MaxFactor = shader->max_tesselation_factor,
      .OutputPrimitive = (MTL_TESSELLATOR_OUTPUT_PRIMITIVE
      )shader->tessellator_output_primitive,
    };
  }
  if (shader->shader_type == microsoft::D3D11_SB_DOMAIN_SHADER) {
    uint32_t max_potential_tess_factor = 1;

    for (int tess_factor = 1; tess_factor <= 64; tess_factor++) {
      auto x = estimate_mesh_size(shader, tess_factor);
      if (x > 32768)
        break;
      max_potential_tess_factor = tess_factor;
    }

    pRefl->PostTessellator = {.MaxPotentialTessFactor = max_potential_tess_factor};
  }
  if (shader->shader_type == microsoft::D3D10_SB_GEOMETRY_SHADER) {
    if (!inputParser || !outputParser || masks.constant_buffer || masks.sampler ||
        masks.uav || masks.srv_hi || masks.srv_lo ||
        !CheckGSBBIsPassThrough(shader->entry())) {
      pRefl->GeometryShader.GSPassThrough = ~0u;
    } else {
      CheckGSSignatureIsPassThrough(
        *inputParser, *outputParser, pRefl->GeometryShader.Data
      );
    }
    pRefl->GeometryShader.Primitive = shader->gs_input_primitive;
    pRefl->GeometryShader.MaxOutputVertexCount = uint16_t(shader->gs_max_vertex_output);
    pRefl->GeometryShader.OutputTopology = uint8_t(shader->gs_output_topology);
    pRefl->GeometryShader.InstanceCount = uint8_t(shader->gs_instance_count);
  }
  if (shader->shader_type == microsoft::D3D10_SB_PIXEL_SHADER) {
    pRefl->PixelShader.ValidRenderTargets = shader->pso_valid_output_reg_mask;
    pRefl->PixelShader.HasCoverageOutput = shader->ps_has_coverage_output;
    pRefl->PixelShader.SampleFrequency = shader->ps_sample_frequency;
    pRefl->PSCoverageInputs = (shader->ps_has_coverage_input ? SM50_PIXEL_COVERAGE_INPUT : 0u) |
                             (shader->ps_has_inner_coverage_input ? SM50_PIXEL_INNER_COVERAGE_INPUT : 0u);
    pRefl->PSForceEarlyDepthStencil = shader->ps_force_early_depth_stencil;
  }
  pRefl->NumOutputElement = shader->max_output_register;
  pRefl->ArgumentTableQwords = shader->shader_info.binding_table.Size();
  return true;
}

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
) {
  using namespace microsoft;
  using namespace dxmt::dxbc;
  using namespace dxmt::air;
  using namespace dxmt::shader::common;
  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);

  if (ppShader == nullptr) {
    errorOut << "ppShader can not be null\0";
    failCall(errorObj, ppError);
    return 1;
  }
  *ppShader = nullptr;

  /* Shader Model 6.x containers have no DXBC shader blob and no DXBC signature
   * chunks, so the DXIL path has to branch before the DXBC parsing below. */
  if (dxmt::dxil::IsDXILContainer(pBytecode, BytecodeSize)) {
    auto sm50_shader = new SM50ShaderInternal();
    if (auto err = dxmt::dxil::PrepareShader(*sm50_shader, pBytecode, BytecodeSize, pRefl)) {
      reportError(std::move(err), errorOut);
      delete sm50_shader;
      failCall(errorObj, ppError);
      return 1;
    }
    delete errorObj;
    *ppShader = sm50_shader;
    return 0;
  }

  CDXBCParser DXBCParser;
  if (DXBCParser.ReadDXBC(pBytecode, BytecodeSize) != S_OK) {
    errorOut << "Invalid DXBC bytecode\0";
    failCall(errorObj, ppError);
    return 1;
  }

  UINT codeBlobIdx = DXBCParser.FindNextMatchingBlob(DXBC_GenericShaderEx);
  if (codeBlobIdx == DXBC_BLOB_NOT_FOUND) {
    codeBlobIdx = DXBCParser.FindNextMatchingBlob(DXBC_GenericShader);
  }
  if (codeBlobIdx == DXBC_BLOB_NOT_FOUND) {
    errorOut << "Invalid DXBC bytecode: shader blob not found\0";
    failCall(errorObj, ppError);
    return 1;
  }
  const void *codeBlob = DXBCParser.GetBlob(codeBlobIdx);

  CShaderToken *ShaderCode = (CShaderToken *)(BYTE *)codeBlob;
  // 1. Collect information about the shader.
  D3D10ShaderBinary::CShaderCodeParser CodeParser(ShaderCode);
  CSignatureParser inputParser;
  if (DXBCGetInputSignature(pBytecode, &inputParser) != S_OK) {
    errorOut << "Invalid DXBC bytecode: input signature not found\0";
    failCall(errorObj, ppError);
    return 1;
  }
  CSignatureParser5 outputParser;
  if (DXBCGetOutputSignature(pBytecode, &outputParser) != S_OK) {
    errorOut << "Invalid DXBC bytecode: output signature not found\0";
    failCall(errorObj, ppError);
    return 1;
  }

  auto sm50_shader = new SM50ShaderInternal();
  sm50_shader->shader_type = CodeParser.ShaderType();

  {
    // A GS lists the outputs of every stream (each Signature carries its stream); other stages have stream 0.
    const UINT streams = CodeParser.ShaderType() == D3D10_SB_GEOMETRY_SHADER ? outputParser.NumStreams() : 1;
    for (UINT s = 0; s < std::max(streams, 1u); s++) {
      const D3D11_SIGNATURE_PARAMETER *parameters;
      const UINT count = outputParser.Signature(s)->GetParameters(&parameters);
      for (unsigned i = 0; i < count; i++)
        sm50_shader->output_signature.push_back(Signature(parameters[i]));
    }
  }

  sm50_shader->bbs = read_control_flow(CodeParser, sm50_shader, inputParser, outputParser);
  if (!sm50_shader->initialization_error.empty()) {
    errorOut << sm50_shader->initialization_error << '\0';
    delete sm50_shader;
    failCall(errorObj, ppError);
    return 1;
  }

  auto masks = build_argument_tables(sm50_shader);
  if (sm50_shader->shader_type == microsoft::D3D11_SB_HULL_SHADER &&
      !sm50_shader->shader_info.no_control_point_phase_passthrough) {
    assert(sm50_shader->max_output_register == 0);
    uint32_t num_input_params = inputParser.GetNumParameters();
    D3D11_SIGNATURE_PARAMETER const * params;
    inputParser.GetParameters(&params);
    for (unsigned i = 0; i < num_input_params; i++) {
      sm50_shader->max_output_register = std::max(
        sm50_shader->max_output_register,
        params[i].Register + 1
      );
    }
  }

  if (pRefl && !fill_shader_reflection(
        sm50_shader, masks, pRefl, &inputParser, &outputParser, errorOut
      )) {
    delete sm50_shader;
    failCall(errorObj, ppError);
    return 1;
  }

  delete errorObj;
  *ppShader = sm50_shader;
  return 0;
};

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
) {
  auto sm50_shader = (dxmt::dxbc::SM50ShaderInternal *)pShader;
  if (pConstantBuffers && sm50_shader->args_reflection_cbuffer.size())
    memcpy(
      pConstantBuffers, sm50_shader->args_reflection_cbuffer.data(),
      sm50_shader->args_reflection_cbuffer.size() *
        sizeof(struct MTL_SM50_SHADER_ARGUMENT)
    );
  if (pArguments && sm50_shader->args_reflection.size())
    memcpy(
      pArguments, sm50_shader->args_reflection.data(),
      sm50_shader->args_reflection.size() *
        sizeof(struct MTL_SM50_SHADER_ARGUMENT)
    );
}

AIRCONV_API void SM50Destroy(sm50_shader_t pShader) {
  delete (dxmt::dxbc::SM50ShaderInternal *)pShader;
}

// A raster-ordered buffer reached through a nested argument buffer does not
// activate Metal's fragment ROG scheduler by itself. An actual read/write of
// a same-ROG texture does; the scratch texel is private to the physical pixel,
// render layer, and native sample. It is bound outside the application's root
// signature, so unbounded descriptor tables retain their layout and range.
static llvm::Error activateRasterOrderedBuffers(llvm::Module &module, unsigned samples,
                                                uint32_t sampleMask) {
  using namespace llvm;
  if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
    return createStringError(inconvertibleErrorCode(), "ROV marker: invalid native sample count");
  auto *fragments = module.getNamedMetadata("air.fragment");
  if (!fragments || fragments->getNumOperands() != 1)
    return createStringError(inconvertibleErrorCode(), "ROV marker: missing native fragment");
  auto *signature = fragments->getOperand(0);
  auto *original = dyn_cast<Function>(cast<ValueAsMetadata>(signature->getOperand(0))->getValue());
  auto *inputs = dyn_cast<MDTuple>(signature->getOperand(2));
  if (!original || !inputs || inputs->getNumOperands() != original->arg_size())
    return createStringError(inconvertibleErrorCode(), "ROV marker: invalid fragment signature");
  CallInst *logicalCall = nullptr;
  Value *logicalActive = nullptr;
  unsigned logicalPositionArgument = ~0u;
  for (auto &block : *original)
    for (auto &instruction : block)
      if (auto *call = dyn_cast<CallInst>(&instruction))
        if (auto *metadata = call->getMetadata("dxmt.rov.logical_active")) {
          auto *argument = metadata->getNumOperands() == 1
            ? mdconst::dyn_extract<ConstantInt>(metadata->getOperand(0)) : nullptr;
          if (logicalCall || !argument || !argument->getType()->isIntegerTy(32))
            return createStringError(inconvertibleErrorCode(), "ROV marker: invalid logical coverage");
          const auto argument_index = argument->getZExtValue();
          if (argument_index >= call->arg_size())
            return createStringError(inconvertibleErrorCode(), "ROV marker: invalid logical coverage");
          logicalActive = call->getArgOperand(argument_index);
          if (!logicalActive->getType()->isIntegerTy(1))
            return createStringError(inconvertibleErrorCode(), "ROV marker: invalid logical coverage");
          logicalCall = call;
          call->setMetadata("dxmt.rov.logical_active", nullptr);
          if (auto *position = call->getMetadata("dxmt.rov.logical_position")) {
            auto *index = position->getNumOperands() == 1
              ? mdconst::dyn_extract<ConstantInt>(position->getOperand(0)) : nullptr;
            if (!index || !index->getType()->isIntegerTy(32) || index->getZExtValue() >= call->arg_size())
              return createStringError(inconvertibleErrorCode(), "ROV marker: invalid logical position");
            auto *type = dyn_cast<FixedVectorType>(call->getArgOperand(index->getZExtValue())->getType());
            if (!type || type->getNumElements() != 4 || !type->getElementType()->isFloatTy())
              return createStringError(inconvertibleErrorCode(), "ROV marker: invalid logical position");
            logicalPositionArgument = index->getZExtValue();
            call->setMetadata("dxmt.rov.logical_position", nullptr);
          }
        }

  LLVMContext &context = module.getContext();
  auto *i32 = Type::getInt32Ty(context);
  auto *float4 = FixedVectorType::get(Type::getFloatTy(context), 4);
  auto *textureType = StructType::getTypeByName(context, "struct._texture_2d_array_t");
  if (!textureType) textureType = StructType::create(context, "struct._texture_2d_array_t");
  auto *texturePointer = textureType->getPointerTo(1);
  auto *arrayType = ArrayType::get(texturePointer, samples);
  Type *bindingType = samples == 1 ? static_cast<Type *>(texturePointer)
                                   : arrayType->getPointerTo();

  SmallVector<Type *, 24> parameters;
  SmallVector<Metadata *, 24> attributes(inputs->op_begin(), inputs->op_end());
  for (auto &arg : original->args()) parameters.push_back(arg.getType());
  const unsigned textureArg = parameters.size();
  parameters.push_back(bindingType);
  attributes.push_back(MDTuple::get(context, {
    ConstantAsMetadata::get(ConstantInt::get(i32, textureArg)),
    MDString::get(context, "air.texture"), MDString::get(context, "air.location_index"),
    ConstantAsMetadata::get(ConstantInt::get(i32, 31)),
    ConstantAsMetadata::get(ConstantInt::get(i32, samples)),
    MDString::get(context, "air.read_write"), MDString::get(context, "air.raster_order_group"),
    ConstantAsMetadata::get(ConstantInt::get(i32, 1)), MDString::get(context, "air.arg_type_name"),
    MDString::get(context, samples == 1 ? "texture2d_array<uint, read_write>" :
      ("array<texture2d_array<uint, read_write>, " + std::to_string(samples) + ">")),
    MDString::get(context, "air.arg_name"), MDString::get(context, "rov_marker")
  }));
  auto findBuiltin = [&](StringRef name) {
    for (unsigned i = 0; i < inputs->getNumOperands(); ++i) {
      auto *node = cast<MDNode>(inputs->getOperand(i));
      if (auto *kind = dyn_cast<MDString>(node->getOperand(1)); kind && kind->getString() == name)
        return i;
    }
    return ~0u;
  };
  auto addBuiltin = [&](StringRef name, StringRef type, StringRef arg, Type *valueType) {
    unsigned index = parameters.size();
    parameters.push_back(valueType);
    attributes.push_back(MDTuple::get(context, {
      ConstantAsMetadata::get(ConstantInt::get(i32, index)),
      MDString::get(context, name), MDString::get(context, "air.arg_type_name"),
      MDString::get(context, type), MDString::get(context, "air.arg_name"),
      MDString::get(context, arg)
    }));
    return index;
  };
  unsigned positionArg = findBuiltin("air.position");
  if (positionArg == ~0u && logicalPositionArgument == ~0u) {
    positionArg = parameters.size();
    parameters.push_back(float4);
    attributes.push_back(MDTuple::get(context, {
      ConstantAsMetadata::get(ConstantInt::get(i32, positionArg)),
      MDString::get(context, "air.position"), MDString::get(context, "air.center"),
      MDString::get(context, "air.no_perspective"), MDString::get(context, "air.arg_type_name"),
      MDString::get(context, "float4"), MDString::get(context, "air.arg_name"),
      MDString::get(context, "rov_native_position")
    }));
  }
  unsigned layerArg = findBuiltin("air.render_target_array_index");
  if (layerArg == ~0u)
    layerArg = addBuiltin("air.render_target_array_index", "uint", "rov_native_layer", i32);
  unsigned sampleArg = findBuiltin("air.sample_id");
  if (samples > 1 && sampleArg == ~0u)
    sampleArg = addBuiltin("air.sample_id", "uint", "rov_native_sample", i32);

  auto name = original->getName().str();
  original->setName(name + ".rov_body");
  auto *wrapper = Function::Create(FunctionType::get(original->getReturnType(), parameters, false),
    Function::ExternalLinkage, name, module);
  wrapper->setCallingConv(original->getCallingConv());
  original->setLinkage(Function::InternalLinkage);
  if (samples > 1)
    wrapper->addParamAttr(textureArg, Attribute::getWithByValType(context, arrayType));

  SmallVector<Metadata *, 3> fields(signature->op_begin(), signature->op_end());
  fields[0] = ValueAsMetadata::get(wrapper);
  fields[2] = MDTuple::get(context, attributes);
  fragments->setOperand(0, MDTuple::get(context, fields));

  // Keep the native fragment body in the native entry. Metal cannot always
  // compile a raster-ordered access before a separate internal fragment-body
  // call; splitting here also avoids moving derivatives into divergent code.
  for (unsigned i = 0; i < original->arg_size(); ++i)
    original->getArg(i)->replaceAllUsesWith(wrapper->getArg(i));
  wrapper->getBasicBlockList().splice(wrapper->begin(), original->getBasicBlockList());
  original->eraseFromParent();
  // Virtual sample APP computes its logical coverage before its body call.
  // Ordinary shaders mark at their entry, before any application UAV access.
  auto *continuation = logicalCall ? logicalCall->getParent()->splitBasicBlock(
    logicalCall->getIterator(), "rov_body") : wrapper->getEntryBlock().splitBasicBlock(
      wrapper->getEntryBlock().begin(), "rov_body");
  auto *entry = continuation->getSinglePredecessor();
  auto *mark = BasicBlock::Create(context, "rov_mark", wrapper, continuation);
  entry->getTerminator()->eraseFromParent();
  IRBuilder<> builder(entry);
  auto helper = module.getOrInsertFunction("air.simd_is_helper_thread",
    FunctionType::get(builder.getInt1Ty(), {}, false));
  Value *active = builder.CreateNot(builder.CreateCall(helper));
  if (logicalCall) active = builder.CreateAnd(active, logicalActive);
  else if (sampleMask != 0xffffffffu) {
    Value *nativeSample = samples > 1 ? static_cast<Value *>(wrapper->getArg(sampleArg))
                                      : builder.getInt32(0);
    active = builder.CreateAnd(active, builder.CreateICmpNE(builder.CreateAnd(
      builder.CreateLShr(builder.getInt32(sampleMask), nativeSample), builder.getInt32(1)),
      builder.getInt32(0)));
  }
  builder.CreateCondBr(active, mark, continuation);
  builder.SetInsertPoint(mark);
  auto *texture = samples == 1 ? static_cast<Value *>(wrapper->getArg(textureArg)) :
    builder.CreateLoad(texturePointer, builder.CreateInBoundsGEP(arrayType,
      wrapper->getArg(textureArg), {builder.getInt32(0), wrapper->getArg(sampleArg)}));
  auto *position = logicalPositionArgument == ~0u ? static_cast<Value *>(wrapper->getArg(positionArg))
    : logicalCall->getArgOperand(logicalPositionArgument);
  Value *coordinate = PoisonValue::get(FixedVectorType::get(i32, 2));
  for (unsigned axis = 0; axis < 2; ++axis)
    coordinate = builder.CreateInsertElement(coordinate,
      builder.CreateFPToUI(builder.CreateExtractElement(position, builder.getInt32(axis)), i32),
      builder.getInt32(axis));
  auto *rgba = FixedVectorType::get(i32, 4);
  auto *readResult = StructType::get(context, {rgba, Type::getInt8Ty(context)});
  auto *version = module.getNamedMetadata("air.version");
  auto *minor = version && version->getNumOperands() == 1 &&
      version->getOperand(0)->getNumOperands() == 3
    ? mdconst::dyn_extract<ConstantInt>(version->getOperand(0)->getOperand(1)) : nullptr;
  if (!minor)
    return createStringError(inconvertibleErrorCode(), "ROV marker: missing AIR version");
  // AIR 2.8's array-texture read carries a sampler and a uint2 offset;
  // AIR 2.7 uses the five-argument form. Mismatching the version can load
  // normally but crash the Metal ShaderValidation PSO compiler.
  const bool extendedRead = minor->getZExtValue() >= 8;
  SmallVector<Type *, 7> readTypes{texturePointer};
  SmallVector<Value *, 7> readArgs{texture};
  if (extendedRead) {
    auto *samplerType = StructType::getTypeByName(context, "struct._sampler_t");
    if (!samplerType) samplerType = StructType::create(context, "struct._sampler_t");
    auto sampler = module.getOrInsertFunction("air.get_read_sampler",
      FunctionType::get(samplerType->getPointerTo(2), {}, false));
    readTypes.push_back(samplerType->getPointerTo(2));
    readArgs.push_back(builder.CreateCall(sampler));
  }
  readTypes.append({coordinate->getType(), i32});
  readArgs.append({coordinate, wrapper->getArg(layerArg)});
  if (extendedRead) {
    readTypes.push_back(coordinate->getType());
    readArgs.push_back(Constant::getNullValue(coordinate->getType()));
  }
  readTypes.append({i32, i32});
  readArgs.append({builder.getInt32(0), builder.getInt32(3)});
  auto read = module.getOrInsertFunction("air.read_texture_2d_array.u.v4i32",
    FunctionType::get(readResult, readTypes, false));
  auto write = module.getOrInsertFunction("air.write_texture_2d_array.u.v4i32",
    FunctionType::get(builder.getVoidTy(), {texturePointer, coordinate->getType(),
      i32, rgba, i32, i32}, false));
  auto *oldValue = builder.CreateCall(read, readArgs);
  auto *newValue = builder.CreateAdd(builder.CreateExtractElement(
    builder.CreateExtractValue(oldValue, 0), builder.getInt32(0)), builder.getInt32(1));
  auto *color = builder.CreateInsertElement(PoisonValue::get(rgba), newValue, builder.getInt32(0));
  for (unsigned axis = 1; axis < 4; ++axis)
    color = builder.CreateInsertElement(color, builder.getInt32(0), builder.getInt32(axis));
  builder.CreateCall(write, {texture, coordinate, wrapper->getArg(layerArg), color,
    builder.getInt32(0), builder.getInt32(3)});
  builder.CreateBr(continuation);
  return Error::success();
}

static llvm::Error applyIndirectViewLocations(
  llvm::Module &module, const SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA &options
) {
  using namespace llvm;
  auto invalid = [](const char *message) {
    return createStringError(inconvertibleErrorCode(), "indirect view locations: %s", message);
  };
  if (!options.view_count || options.view_count > 4)
    return invalid("view count must be in 1..4");
  auto *vertices = module.getNamedMetadata("air.vertex");
  if (!vertices || !vertices->getNumOperands()) return Error::success();
  if (vertices->getNumOperands() != 1) return invalid("expected one native vertex entry");
  auto *signature = vertices->getOperand(0);
  if (signature->getNumOperands() < 3) return invalid("incomplete AIR vertex signature");
  auto *functionMetadata = dyn_cast<ValueAsMetadata>(signature->getOperand(0));
  auto *original = functionMetadata ? dyn_cast<Function>(functionMetadata->getValue()) : nullptr;
  if (!original) return invalid("missing AIR vertex function");
  // Metal forbids outputs on a rasterization-disabled vertex entry. Routing is unused there.
  if (original->getReturnType()->isVoidTy()) return Error::success();
  auto *returnType = dyn_cast<StructType>(original->getReturnType());
  auto *outputs = dyn_cast<MDTuple>(signature->getOperand(1));
  auto *inputs = dyn_cast<MDTuple>(signature->getOperand(2));
  if (!returnType || !outputs || !inputs || original->empty() ||
      returnType->getNumElements() != outputs->getNumOperands() ||
      original->arg_size() != inputs->getNumOperands())
    return invalid("inconsistent AIR vertex signature");

  unsigned rootArgument = ~0u, viewField = ~0u;
  StructType *rootType = nullptr;
  for (unsigned input = 0; input < inputs->getNumOperands(); ++input) {
    auto *metadata = dyn_cast<MDNode>(inputs->getOperand(input));
    if (!metadata || !metadata->getNumOperands()) continue;
    bool indirectBuffer = false, rootLocation = false;
    MDNode *structure = nullptr;
    for (unsigned i = 1; i < metadata->getNumOperands(); ++i) {
      auto *key = dyn_cast<MDString>(metadata->getOperand(i));
      if (!key) continue;
      if (key->getString() == "air.indirect_buffer") indirectBuffer = true;
      else if (key->getString() == "air.location_index" && i + 1 < metadata->getNumOperands()) {
        auto *location = mdconst::dyn_extract<ConstantInt>(metadata->getOperand(i + 1));
        rootLocation = location && location->getZExtValue() == SM50_BINDING_INDEX_ROOT_ARGUMENTS;
      } else if (key->getString() == "air.struct_type_info" && i + 1 < metadata->getNumOperands())
        structure = dyn_cast<MDNode>(metadata->getOperand(i + 1));
    }
    if (!indirectBuffer || !rootLocation || !structure) continue;
    auto *argumentIndex = mdconst::dyn_extract<ConstantInt>(metadata->getOperand(0));
    if (!argumentIndex || argumentIndex->getZExtValue() >= original->arg_size())
      return invalid("root argument index is out of range");
    rootArgument = unsigned(argumentIndex->getZExtValue());
    auto *pointer = dyn_cast<PointerType>(original->getArg(rootArgument)->getType());
    rootType = pointer ? dyn_cast<StructType>(pointer->getNonOpaquePointerElementType()) : nullptr;
    // ArgumentBufferBuilder::Build emits seven metadata operands per named struct member.
    if (!rootType || structure->getNumOperands() != rootType->getNumElements() * 7)
      return invalid("root argument struct metadata does not match its type");
    for (unsigned field = 0; field < rootType->getNumElements(); ++field) {
      auto *name = dyn_cast<MDString>(structure->getOperand(field * 7 + 4));
      if (!name || name->getString() != "view_id") continue;
      auto *offset = mdconst::dyn_extract<ConstantInt>(structure->getOperand(field * 7));
      if (!rootType->getElementType(field)->isIntegerTy(64) || !offset ||
          offset->getZExtValue() != module.getDataLayout().getStructLayout(rootType)->getElementOffset(field))
        return invalid("view_id must be a matching root qword");
      viewField = field;
      break;
    }
    break;
  }
  if (rootArgument == ~0u || viewField == ~0u)
    return invalid("native ICB routing requires the root view_id qword");

  auto &context = module.getContext();
  const StringRef kinds[] = {"air.viewport_array_index", "air.render_target_array_index"};
  const StringRef names[] = {"mtl_viewport_array_index", "mtl_render_target_array_index"};
  unsigned indices[] = {~0u, ~0u};
  for (unsigned field = 0; field < outputs->getNumOperands(); ++field) {
    auto *metadata = dyn_cast<MDNode>(outputs->getOperand(field));
    auto *kind = metadata && metadata->getNumOperands() ? dyn_cast<MDString>(metadata->getOperand(0)) : nullptr;
    if (!kind) return invalid("malformed AIR vertex output");
    for (unsigned axis = 0; axis < 2; ++axis) {
      if (kind->getString() != kinds[axis]) continue;
      if (indices[axis] != ~0u || !returnType->getElementType(field)->isIntegerTy(32))
        return invalid("view location output must be a single uint");
      indices[axis] = field;
    }
  }
  SmallVector<Type *, 48> types(returnType->element_begin(), returnType->element_end());
  SmallVector<Metadata *, 48> outputMetadata(outputs->op_begin(), outputs->op_end());
  for (unsigned axis = 0; axis < 2; ++axis) {
    if (indices[axis] != ~0u) continue;
    indices[axis] = types.size();
    types.push_back(Type::getInt32Ty(context));
    outputMetadata.push_back(MDTuple::get(context, {
      MDString::get(context, kinds[axis]), MDString::get(context, "air.arg_type_name"),
      MDString::get(context, "uint"), MDString::get(context, "air.arg_name"), MDString::get(context, names[axis])}));
  }
  auto *entry = original;
  if (types.size() != returnType->getNumElements()) {
    auto *enlargedReturn = StructType::get(context, types, returnType->isPacked());
    entry = Function::Create(FunctionType::get(enlargedReturn, original->getFunctionType()->params(), false),
      original->getLinkage(), original->getAddressSpace(), "", &module);
    entry->takeName(original);
    entry->setCallingConv(original->getCallingConv());
    entry->setAttributes(original->getAttributes());
    entry->setSubprogram(original->getSubprogram());
    for (unsigned i = 0; i < original->arg_size(); ++i) {
      entry->getArg(i)->takeName(original->getArg(i));
      original->getArg(i)->replaceAllUsesWith(entry->getArg(i));
    }
    entry->getBasicBlockList().splice(entry->end(), original->getBasicBlockList());
  }
  // Added root reads must not retain inferred no-read/write-only memory attributes.
  entry->removeFnAttr(Attribute::ReadNone);
  entry->removeFnAttr(Attribute::WriteOnly);
  entry->removeFnAttr(Attribute::InaccessibleMemOnly);
  entry->removeParamAttr(rootArgument, Attribute::ReadNone);
  entry->removeParamAttr(rootArgument, Attribute::WriteOnly);
  IRBuilder<> prologue(&*entry->getEntryBlock().getFirstInsertionPt());
  auto *packedView = prologue.CreateLoad(prologue.getInt64Ty(),
    prologue.CreateStructGEP(rootType, entry->getArg(rootArgument), viewField));
  auto *view = prologue.CreateTrunc(packedView, prologue.getInt32Ty());
  Value *offsets[2], *bounds[2];
  for (unsigned axis = 0; axis < 2; ++axis) {
    offsets[axis] = prologue.getInt32(0);
    for (unsigned logicalView = 0; logicalView < options.view_count; ++logicalView) {
      uint32_t offset = axis ? options.render_target_offsets[logicalView] : options.viewport_offsets[logicalView];
      offsets[axis] = prologue.CreateSelect(prologue.CreateICmpEQ(view, prologue.getInt32(logicalView)),
        prologue.getInt32(offset), offsets[axis]);
    }
    bounds[axis] = prologue.CreateTrunc(
      prologue.CreateAnd(prologue.CreateLShr(packedView, prologue.getInt64(axis ? 48 : 32)),
        prologue.getInt64(0xffff)), prologue.getInt32Ty());
  }
  for (auto &block : *entry) {
    auto *ret = dyn_cast<ReturnInst>(block.getTerminator());
    if (!ret) continue;
    IRBuilder<> builder(ret);
    auto *oldResult = ret->getReturnValue();
    if (!oldResult) return invalid("rasterizing vertex return has no value");
    Value *result = oldResult;
    if (entry != original) {
      result = UndefValue::get(entry->getReturnType());
      for (unsigned field = 0; field < returnType->getNumElements(); ++field)
        result = builder.CreateInsertValue(result, builder.CreateExtractValue(oldResult, field), field);
    }
    for (unsigned axis = 0; axis < 2; ++axis) {
      Value *base = indices[axis] < returnType->getNumElements()
        ? builder.CreateExtractValue(oldResult, indices[axis])
        : builder.getInt32(0);
      // View Instancing adds shader indices and PSO offsets as wrapping UINT32 before checking bounds.
      auto *sum = builder.CreateAdd(base, offsets[axis]);
      auto *clamped = builder.CreateSelect(builder.CreateICmpULT(sum, bounds[axis]),
        sum, builder.getInt32(0));
      result = builder.CreateInsertValue(result, clamped, indices[axis]);
    }
    ret->setOperand(0, result);
  }
  SmallVector<Metadata *, 8> signatureFields(signature->op_begin(), signature->op_end());
  signatureFields[0] = ValueAsMetadata::get(entry);
  signatureFields[1] = MDTuple::get(context, outputMetadata);
  vertices->setOperand(0, MDTuple::get(context, signatureFields));
  if (entry != original) original->eraseFromParent();
  return Error::success();
}

AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    failCall(errorObj, ppError);
    return 1;
  }

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info = ((dxmt::dxbc::SM50ShaderInternal *)pShader)->shader_info;
  SM50_SHADER_D3D12_MSAA_PIXEL_DATA *msaa_pixel = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_PIXEL, SM50_SHADER_D3D12_MSAA_PIXEL_DATA>(pArgs, &msaa_pixel);
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *coarse_shading = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_COARSE_SHADING, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(
      pArgs, &coarse_shading);
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *conservative_raster = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_CONSERVATIVE_RASTER, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(
      pArgs, &conservative_raster);
  SM50_SHADER_D3D12_MSAA_VERTEX_DATA *msaa_vertex = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_VERTEX, SM50_SHADER_D3D12_MSAA_VERTEX_DATA>(pArgs, &msaa_vertex);
  SM50_SHADER_D3D12_MSAA_MESH_DATA *msaa_mesh = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_MESH, SM50_SHADER_D3D12_MSAA_MESH_DATA>(pArgs, &msaa_mesh);
  SM50_SHADER_PSO_PIXEL_SHADER_DATA *pixel_pso = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_PSO_PIXEL_SHADER, SM50_SHADER_PSO_PIXEL_SHADER_DATA>(pArgs, &pixel_pso);
  SM50_SHADER_ROOT_SIGNATURE_DATA *root_signature = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &root_signature);
  SM50_SHADER_D3D12_LOGIC_OP_DATA *logic_op = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_LOGIC_OP, SM50_SHADER_D3D12_LOGIC_OP_DATA>(pArgs, &logic_op);
  SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA *indirect_views = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS,
    SM50_SHADER_D3D12_INDIRECT_VIEW_LOCATIONS_DATA>(pArgs, &indirect_views);
  const uint32_t sample_frequency_mask = !msaa_pixel && !msaa_vertex && pixel_pso &&
    ((dxmt::dxbc::SM50ShaderInternal *)pShader)->ps_sample_frequency
      ? pixel_pso->sample_mask : 0xffffffffu;
  const bool mark_rov_buffers = !msaa_vertex &&
    (!msaa_pixel || (msaa_pixel->variant & ~SM50_MSAA_ATOMIC_HELPERS) == SM50_MSAA_PIXEL ||
      (msaa_pixel->variant & ~SM50_MSAA_ATOMIC_HELPERS) == SM50_MSAA_SAMPLE_APP) &&
    root_signature && SM50NeedsRasterOrderedBufferMarker(pShader, root_signature->bytecode,
      root_signature->bytecode_length);
  const uint32_t marker_samples = pixel_pso && pixel_pso->native_sample_count
    ? pixel_pso->native_sample_count : 1;
  if (msaa_pixel && msaa_vertex) {
    errorOut << "MSAA pixel and vertex variants cannot be combined\0";
    failCall(errorObj, ppError);
    return 1;
  }
  SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA *existing_capture = nullptr;
  dxmt::dxbc::args_get_data<SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE, SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA>(pArgs, &existing_capture);
  SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE_DATA synthetic_capture{};
  if (msaa_vertex && msaa_vertex->variant == SM50_MSAA_VERTEX_CAPTURE && !existing_capture) {
    synthetic_capture.next = pArgs;
    synthetic_capture.type = SM50_SHADER_D3D12_STREAM_OUTPUT_CAPTURE;
    pArgs = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&synthetic_capture;
  }

  if (auto *sm50_shader = (dxmt::dxbc::SM50ShaderInternal *)pShader;
      sm50_shader->dxil_bytecode) {
    /* Shader Model 6.x: the parsed DXIL module is lowered in place and becomes the
     * AIR module, so it uses its own context and module instead of `shader.air`. */
    auto lowered = dxmt::dxil::LowerShader(*sm50_shader, FunctionName, pArgs);
    if (!lowered) {
      reportError(lowered.takeError(), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
    auto info = std::move(*lowered);
    auto &pModule = info->module;
    if (indirect_views) {
      if (auto error = applyIndirectViewLocations(*pModule, *indirect_views)) {
        reportError(std::move(error), errorOut);
        failCall(errorObj, ppError);
        return 1;
      }
    }
    if (msaa_mesh) {
      if (auto error = dxmt::msaa::transformMesh(*pModule, *msaa_mesh, conservative_raster != nullptr)) {
        reportError(std::move(error), errorOut);
        failCall(errorObj, ppError);
        return 1;
      }
    }
    if (msaa_pixel) {
      if (auto error = dxmt::msaa::transformPixel(*pModule, *msaa_pixel, coarse_shading != nullptr,
                                                   conservative_raster != nullptr)) {
        reportError(std::move(error), errorOut);
        failCall(errorObj, ppError);
        return 1;
      }
    } else if (msaa_vertex) {
      if (auto error = dxmt::msaa::transformVertex(*pModule, *msaa_vertex, conservative_raster != nullptr)) {
        reportError(std::move(error), errorOut);
        failCall(errorObj, ppError);
        return 1;
      }
    } else if (sample_frequency_mask != 0xffffffffu) {
      if (auto error = dxmt::msaa::maskSampleFrequency(*pModule, sample_frequency_mask)) {
        reportError(std::move(error), errorOut);
        failCall(errorObj, ppError);
        return 1;
      }
    }
    if (logic_op) {
      if (auto error = dxmt::logic_op::applyLogicOp(*pModule, *logic_op)) {
        reportError(std::move(error), errorOut);
        failCall(errorObj, ppError);
        return 1;
      }
    }
    if (mark_rov_buffers) {
      if (auto error = activateRasterOrderedBuffers(*pModule, marker_samples, sample_frequency_mask)) {
        reportError(std::move(error), errorOut);
        failCall(errorObj, ppError);
        return 1;
      }
    }

    if (shader_info.use_msad)
      linkMSAD(*pModule);
    if (shader_info.use_samplepos)
      linkSamplePos(*pModule);

    if (auto error = dxmt::dxil::LowerAtomic64Phase(*pModule)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
    runOptimizationPasses(*pModule);
    auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
    if (auto error = dxmt::dxil::LowerImmutableConstants(*pModule, *compiled)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }

    raw_svector_ostream OS(compiled->vec);

    metallib::MetallibWriter writer;

    if (auto error = writer.Write(*pModule, OS)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }

    info.reset();

    *ppBitcode = (sm50_bitcode_t)compiled.release();
    return 0;
  }

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);
  dxmt::dxil::InitializeAtomic64Phase(*pModule, pArgs);
  if (coarse_shading) pModule->addModuleFlag(Module::Error, "dxmt.coarse.snapped", 1);

  if (auto err = dxmt::dxbc::convertDXBC(
        pShader, FunctionName, context, *pModule, pArgs
      )) {
    llvm::handleAllErrors(std::move(err), [&](const UnsupportedFeature &u) {
      errorOut << u.msg;
    });
    failCall(errorObj, ppError);
    return 1;
  }
  if (indirect_views) {
    if (auto error = applyIndirectViewLocations(*pModule, *indirect_views)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }
  if (msaa_mesh) {
    if (auto error = dxmt::msaa::transformMesh(*pModule, *msaa_mesh, conservative_raster != nullptr)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }
  if (msaa_pixel) {
    if (auto error = dxmt::msaa::transformPixel(*pModule, *msaa_pixel, coarse_shading != nullptr,
                                                   conservative_raster != nullptr)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  } else if (msaa_vertex) {
    if (auto error = dxmt::msaa::transformVertex(*pModule, *msaa_vertex, conservative_raster != nullptr)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  } else if (sample_frequency_mask != 0xffffffffu) {
    if (auto error = dxmt::msaa::maskSampleFrequency(*pModule, sample_frequency_mask)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }
  if (logic_op) {
    if (auto error = dxmt::logic_op::applyLogicOp(*pModule, *logic_op)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }
  if (mark_rov_buffers) {
    if (auto error = activateRasterOrderedBuffers(*pModule, marker_samples, sample_frequency_mask)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);

  if (auto error = dxmt::dxil::LowerAtomic64Phase(*pModule)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }
  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  if (pModule->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    if (auto error = dxmt::dxil::LowerImmutableConstants(*pModule, *compiled)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  if (auto error = writer.Write(*pModule, OS)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled.release();
  return 0;
}

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    failCall(errorObj, ppError);
    return 1;
  }

  if (int dxil = compileDXILPipelineStage(
        dxmt::dxil::PipelineKind::TessellationObject, pVertexShader, pHullShader, pHullShaderArgs, FunctionName,
        ppBitcode, errorObj, errorOut, ppError
      );
      dxil >= 0)
    return dxil;

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info =
    ((dxmt::dxbc::SM50ShaderInternal *)pHullShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);
  dxmt::dxil::InitializeAtomic64Phase(*pModule, pHullShaderArgs);

  if (auto err = dxmt::dxbc::convert_dxbc_vertex_hull_shader(
        (dxbc::SM50ShaderInternal *)pVertexShader, (dxbc::SM50ShaderInternal *)pHullShader, 
        FunctionName, context, *pModule, pHullShaderArgs
      )) {
    llvm::handleAllErrors(std::move(err), [&](const UnsupportedFeature &u) {
      errorOut << u.msg;
    });
    failCall(errorObj, ppError);
    return 1;
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  linkTessellation(*pModule);

  if (auto error = dxmt::dxil::LowerAtomic64Phase(*pModule)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }
  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  if (pModule->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    if (auto error = dxmt::dxil::LowerImmutableConstants(*pModule, *compiled)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  if (auto error = writer.Write(*pModule, OS)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled.release();
  return 0;
}

AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    failCall(errorObj, ppError);
    return 1;
  }

  if (int dxil = compileDXILPipelineStage(
        dxmt::dxil::PipelineKind::TessellationMesh, pHullShader, pDomainShader, pDomainShaderArgs, FunctionName,
        ppBitcode, errorObj, errorOut, ppError
      );
      dxil >= 0)
    return dxil;

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info =
    ((dxmt::dxbc::SM50ShaderInternal *)pDomainShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);
  dxmt::dxil::InitializeAtomic64Phase(*pModule, pDomainShaderArgs);

  if (auto err = dxmt::dxbc::convert_dxbc_tesselator_domain_shader(
        (dxbc::SM50ShaderInternal *)pDomainShader, FunctionName,
        (dxbc::SM50ShaderInternal *)pHullShader, context, *pModule,
        pDomainShaderArgs
      )) {
    llvm::handleAllErrors(std::move(err), [&](const UnsupportedFeature &u) {
      errorOut << u.msg;
    });
    failCall(errorObj, ppError);
    return 1;
  }
  SM50_SHADER_D3D12_MSAA_MESH_DATA *msaa_mesh = nullptr;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *conservative = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_CONSERVATIVE_RASTER, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(
      pDomainShaderArgs, &conservative);
  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  linkTessellation(*pModule);
  if (dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_MESH, SM50_SHADER_D3D12_MSAA_MESH_DATA>(pDomainShaderArgs, &msaa_mesh))
    if (auto error = msaa::transformMesh(*pModule, *msaa_mesh, conservative != nullptr)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }

  if (auto error = dxmt::dxil::LowerAtomic64Phase(*pModule)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }
  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  if (pModule->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    if (auto error = dxmt::dxil::LowerImmutableConstants(*pModule, *compiled)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  if (auto error = writer.Write(*pModule, OS)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled.release();
  return 0;
}

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    failCall(errorObj, ppError);
    return 1;
  }

  if (int dxil = compileDXILPipelineStage(
        dxmt::dxil::PipelineKind::GeometryObject, pVertexShader, pGeometryShader, pVertexShaderArgs, FunctionName,
        ppBitcode, errorObj, errorOut, ppError
      );
      dxil >= 0)
    return dxil;

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info = ((dxmt::dxbc::SM50ShaderInternal *)pGeometryShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);
  dxmt::dxil::InitializeAtomic64Phase(*pModule, pVertexShaderArgs);

  SM50_SHADER_GS_TESSELLATION_INPUT_DATA *tess_input = nullptr;
  const bool tessellation_input =
      dxmt::dxbc::args_get_data<SM50_SHADER_GS_TESSELLATION_INPUT, SM50_SHADER_GS_TESSELLATION_INPUT_DATA>(
          pVertexShaderArgs, &tess_input
      );
  // With tessellation input the first stage is the DS and the object fetches the pass-1 capture.
  auto convert = tessellation_input ? dxmt::dxbc::convert_dxbc_domain_for_geometry_shader
                                    : dxmt::dxbc::convert_dxbc_vertex_for_geometry_shader;
  if (auto err = convert(
        (dxbc::SM50ShaderInternal *)pVertexShader, FunctionName,
        (dxbc::SM50ShaderInternal *)pGeometryShader, context, *pModule,
        pVertexShaderArgs
      )) {
    llvm::handleAllErrors(std::move(err), [&](const UnsupportedFeature &u) {
      errorOut << u.msg;
    });
    failCall(errorObj, ppError);
    return 1;
  }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);

  if (auto error = dxmt::dxil::LowerAtomic64Phase(*pModule)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }
  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  if (pModule->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    if (auto error = dxmt::dxil::LowerImmutableConstants(*pModule, *compiled)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  if (auto error = writer.Write(*pModule, OS)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled.release();
  return 0;
}

AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  using namespace llvm;
  using namespace dxmt;

  if (ppError) {
    *ppError = nullptr;
  }
  auto errorObj = new SM50ErrorInternal();
  llvm::raw_svector_ostream errorOut(errorObj->buf);
  if (ppBitcode == nullptr) {
    errorOut << "ppBitcode can not be null\0";
    failCall(errorObj, ppError);
    return 1;
  }

  if (int dxil = compileDXILPipelineStage(
        dxmt::dxil::PipelineKind::GeometryMesh, pVertexShader, pGeometryShader, pGeometryShaderArgs, FunctionName,
        ppBitcode, errorObj, errorOut, ppError
      );
      dxil >= 0)
    return dxil;

  // pArgs is ignored for now
  LLVMContext context;

  context.setOpaquePointers(false); // I suspect Metal uses LLVM 14...

  auto &shader_info =
    ((dxmt::dxbc::SM50ShaderInternal *)pGeometryShader)->shader_info;

  auto pModule = std::make_unique<Module>("shader.air", context);
  initializeModule(*pModule);
  dxmt::dxil::InitializeAtomic64Phase(*pModule, pGeometryShaderArgs);

  if (auto err = dxmt::dxbc::convert_dxbc_geometry_shader(
        (dxbc::SM50ShaderInternal *)pGeometryShader, FunctionName,
        (dxbc::SM50ShaderInternal *)pVertexShader, context, *pModule,
        pGeometryShaderArgs
      )) {
    llvm::handleAllErrors(std::move(err), [&](const UnsupportedFeature &u) {
      errorOut << u.msg;
    });
    failCall(errorObj, ppError);
    return 1;
  }
  SM50_SHADER_D3D12_MSAA_MESH_DATA *msaa_mesh = nullptr;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *conservative = nullptr;
  dxbc::args_get_data<SM50_SHADER_D3D12_CONSERVATIVE_RASTER, SM50_SHADER_COMPILATION_ARGUMENT_DATA>(
      pGeometryShaderArgs, &conservative);
  if (dxbc::args_get_data<SM50_SHADER_D3D12_MSAA_MESH, SM50_SHADER_D3D12_MSAA_MESH_DATA>(pGeometryShaderArgs, &msaa_mesh))
    if (auto error = msaa::transformMesh(*pModule, *msaa_mesh, conservative != nullptr)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }

  if (shader_info.use_msad)
    linkMSAD(*pModule);
  if (shader_info.use_samplepos)
    linkSamplePos(*pModule);
  
  if (auto error = dxmt::dxil::LowerAtomic64Phase(*pModule)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }
  runOptimizationPasses(*pModule);

  // Serialize AIR
  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  if (pModule->getModuleFlag(dxmt::dxil::kAtomic64PhaseFlag)) {
    if (auto error = dxmt::dxil::LowerImmutableConstants(*pModule, *compiled)) {
      reportError(std::move(error), errorOut);
      failCall(errorObj, ppError);
      return 1;
    }
  }

  raw_svector_ostream OS(compiled->vec);

  metallib::MetallibWriter writer;

  if (auto error = writer.Write(*pModule, OS)) {
    reportError(std::move(error), errorOut);
    failCall(errorObj, ppError);
    return 1;
  }

  pModule.reset();

  *ppBitcode = (sm50_bitcode_t)compiled.release();
  return 0;
}

AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, SM50_COMPILED_BITCODE *pData
) {
  auto pBitcodeInternal = (SM50CompiledBitcodeInternal *)pBitcode;
  pData->Data = pBitcodeInternal->vec.data();
  pData->Size = pBitcodeInternal->vec.size();
  pData->ConstantData = pBitcodeInternal->constants.empty() ? nullptr : pBitcodeInternal->constants.data();
  pData->ConstantDataSize = pBitcodeInternal->constants.size();
  pData->ConstantDataAlignment = pBitcodeInternal->constant_alignment;
  pData->ConstantRelocations = pBitcodeInternal->constant_relocations.empty()
    ? nullptr : pBitcodeInternal->constant_relocations.data();
  pData->ConstantRelocationCount = pBitcodeInternal->constant_relocations.size();
}

AIRCONV_API int SM50CopyCompiledBitcode(
  sm50_bitcode_t pBitcode, SM50_COMPILED_BITCODE_PART Part, void *pDestination, uint64_t Size
) {
  auto pBitcodeInternal = (SM50CompiledBitcodeInternal *)pBitcode;
  const void *source;
  uint64_t size;
  switch (Part) {
  case SM50_COMPILED_BITCODE_PART_LIBRARY:
    source = pBitcodeInternal->vec.data();
    size = pBitcodeInternal->vec.size();
    break;
  case SM50_COMPILED_BITCODE_PART_CONSTANTS:
    source = pBitcodeInternal->constants.data();
    size = pBitcodeInternal->constants.size();
    break;
  case SM50_COMPILED_BITCODE_PART_RELOCATIONS:
    source = pBitcodeInternal->constant_relocations.data();
    size = pBitcodeInternal->constant_relocations.size() * sizeof(SM50_CONSTANT_RELOCATION);
    break;
  default:
    return 1;
  }
  if (Size != size || (size && !pDestination))
    return 1;
  if (size)
    memcpy(pDestination, source, size);
  return 0;
}

AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode) {
  auto pBitcodeInternal = (SM50CompiledBitcodeInternal *)pBitcode;
  delete pBitcodeInternal;
}

AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) {
  if (!pBuffer || !BufferSize)
    return 0;
  if (!pError) {
    constexpr char message[] = "Out of memory converting shader arguments";
    auto str_len = std::min(sizeof(message) - 1, BufferSize - 1);
    memcpy(pBuffer, message, str_len);
    pBuffer[str_len] = '\0';
    return str_len;
  }
  auto pInternal = (SM50ErrorInternal *)pError;
  // LLVM diagnostics may already contain a terminator; never count it as message text.
  auto end = std::find(pInternal->buf.begin(), pInternal->buf.end(), '\0');
  auto str_len = std::min(static_cast<size_t>(end - pInternal->buf.begin()), BufferSize - 1);
  memcpy(pBuffer, pInternal->buf.data(), str_len);
  pBuffer[str_len] = '\0';
  return str_len;
}

AIRCONV_API void SM50FreeError(sm50_error_t pError) {
  if (pError == nullptr)
    return;
  auto pInternal = (SM50ErrorInternal *)pError;
  delete pInternal;
}
