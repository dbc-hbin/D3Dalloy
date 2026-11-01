/* WaveMatrix (DXC 3e105849) and LinAlg (DXC c4d8f4f9 / published core
 * 322..342) are deliberately classified by both opcode and declaration name.
 * Matrix values become immutable scalar aggregates: phi/select/loop/storage
 * retain ordinary LLVM value semantics, with no pointer-backed SSA aliasing.
 * Wave32 operands use distributed registers; group operands communicate through
 * one reusable, barrier-protected threadgroup snapshot before each operation.
 */
#include "dxil_matrix.hpp"
#include "dxil_converter.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <algorithm>
#include <cmath>

using namespace llvm;
namespace d3dalloy::dxil {
namespace {
struct Descriptor {
  unsigned component=0, rows=0, cols=0, use=0, scope=1, fragment=0;
  unsigned size() const { return fragment==1?rows:fragment==2?cols:rows*cols; }
};
struct Operation { unsigned index; bool legacy, preview; };
constexpr const char *legacy_names[]={"waveMatrix_Annotate","waveMatrix_Depth","waveMatrix_Fill","waveMatrix_LoadRawBuf","waveMatrix_LoadGroupShared","waveMatrix_StoreRawBuf","waveMatrix_StoreGroupShared","waveMatrix_Multiply","waveMatrix_Multiply","waveMatrix_ScalarOp","waveMatrix_Accumulate","waveMatrix_Accumulate"};
constexpr unsigned legacy_args[]={3,2,3,7,6,7,6,4,4,4,3,3};
constexpr const char *matrix_names[]={"linAlgMatrixMultiplyAccumulate","linAlgFillMatrix","linAlgCopyConvertMatrix","linAlgMatrixLoadFromDescriptor","linAlgMatrixLoadFromMemory","linAlgMatrixLength","linAlgMatrixGetCoordinate","linAlgMatrixGetElement","linAlgMatrixSetElement","linAlgMatrixStoreToDescriptor","linAlgMatrixStoreToMemory","linAlgMatrixQueryAccumulatorLayout","linAlgMatrixMultiply","linAlgMatrixAccumulate","linAlgMatVecMul","linAlgMatVecMulAdd","linAlgMatrixAccumulateToDescriptor","linAlgMatrixAccumulateToMemory","linAlgMatrixOuterProduct","linAlgConvert","linAlgVectorAccumulateToDescriptor"};
constexpr unsigned matrix_args[]={4,3,3,6,5,2,3,3,4,7,6,1,3,3,5,6,7,6,4,4,5};
Expected<Operation> classify(const CallInst &call) {
  Function *function=call.getCalledFunction();
  if(!function || !function->getName().startswith("dx.op.")) return Operation{0,false,false};
  StringRef name=function->getName().drop_front(6);
  bool named_legacy=name.startswith("waveMatrix_");
  bool named_matrix=name.startswith("linAlg");
  if(!named_legacy && !named_matrix) return Operation{0,false,false};
  if(call.arg_empty()) return createError("dxil: matrix operation lacks an opcode");
  auto *number=dyn_cast<ConstantInt>(call.getArgOperand(0));
  if(!number || !number->getType()->isIntegerTy(32)) return createError("dxil: matrix opcode must be constant i32");
  uint32_t opcode=number->getZExtValue();
  auto matches=[&](StringRef base) { return name==base || (name.startswith(base) && name.size()>base.size() && name[base.size()]=='.'); };
  if(named_legacy) {
    if(opcode<226 || opcode>237 || !matches(legacy_names[opcode-226]) || call.arg_size()!=legacy_args[opcode-226])
      return createError("dxil: malformed retired WaveMatrix opcode/signature");
    return Operation{opcode-226+1,true,false};
  }
  bool preview=(opcode&0x80000000u)!=0;
  unsigned index=preview?((opcode&0x7fffffffu)-11):(opcode-322);
  if(index>=21 || !matches(matrix_names[index])) return createError("dxil: unrecognized LinAlg opcode/name ABI");
  unsigned count=matrix_args[index];
  if(preview && (index==1 || index==18)) --count;
  if(preview && index==15) ++count;
  if(call.arg_size()!=count) return createError("dxil: LinAlg final and preview signatures cannot be interchanged");
  return Operation{index+1,false,preview};
}
bool signedComponent(unsigned c) { return c==2 || c==4 || c==6 || c==17 || c==19; }
bool floatComponent(unsigned c) { return c==8 || c==9 || c==10 || c==21 || c==22 || c==23; }
unsigned componentBits(unsigned c) {
  switch(c) { case 2:case 3:case 8:case 23:return 16; case 4:case 5:case 9:return 32; case 6:case 7:case 10:return 64; case 17:case 18:case 19:case 20:case 21:case 22:return 8; default:return 0; }
}
Type *componentType(LLVMContext &context,unsigned c) {
  switch(c) { case 8:return Type::getHalfTy(context);case 9:return Type::getFloatTy(context);case 10:return Type::getDoubleTy(context);default:return IntegerType::get(context,componentBits(c)); }
}
Expected<unsigned> metadataInteger(Metadata *value) {
  auto *constant=mdconst::dyn_extract<ConstantInt>(value);
  if(!constant || !constant->getType()->isIntegerTy(32)) return createError("dxil: matrix metadata needs i32 integer fields");
  return constant->getZExtValue();
}
Expected<DenseMap<Type *,Descriptor>> descriptors(Module &module) {
  DenseMap<Type *,Descriptor> result;
  auto *metadata=module.getNamedMetadata("dx.targetTypes");
  if(!metadata) return result;
  for(MDNode *record:metadata->operands()) {
    if(!record->getNumOperands())return createError("dxil: empty target-type record");
    auto *marker=dyn_cast<ValueAsMetadata>(record->getOperand(0));
    auto *type=marker?dyn_cast<StructType>(marker->getValue()->getType()):nullptr;
    if(!type || !type->hasName() || !type->getName().startswith("dx.types.LinAlgMatrix"))continue;
    if(record->getNumOperands()!=6) return createError("dxil: malformed matrix dx.targetTypes record");
    unsigned fields[5];
    for(unsigned i=0;i<5;++i) { auto value=metadataInteger(record->getOperand(i+1));if(!value)return value.takeError();fields[i]=*value; }
    Descriptor description{fields[0],fields[1],fields[2],fields[3],fields[4],0};
    if(!componentBits(description.component) || description.component==17 || description.component==18 || !description.rows || !description.cols || description.rows>4096 || description.cols>4096 || description.size()>1048576 || description.use>2 || description.scope>2)
      return createError("dxil: invalid matrix component/dimensions/use/scope metadata");
    if(description.scope==1 && ((description.rows%16) || (description.cols%16))) return createError("dxil: wave matrix dimensions must be multiples of 16");
    if(!result.try_emplace(type,description).second) return createError("dxil: duplicate matrix type metadata");
  }
  return result;
}
class MatrixTypes final:public ValueMapTypeRemapper {
  DenseMap<Type *,Type *> mapped;
public:
  DenseMap<Type *,Descriptor> descriptions;
  unsigned group_threads;
  MatrixTypes(DenseMap<Type *,Descriptor> records,unsigned threads):descriptions(std::move(records)),group_threads(threads) {}
  unsigned cells(const Descriptor &d) const { unsigned width=d.scope==0?1:d.scope==1?32:group_threads;return (d.size()+width-1)/width; }
  bool containsMatrix(Type *type) {
    SmallPtrSet<Type *,16> visited;SmallVector<Type *,16> pending{type};
    while(!pending.empty()) {Type *next=pending.pop_back_val();if(!visited.insert(next).second)continue;if(descriptions.count(next))return true;for(Type *child:next->subtypes())pending.push_back(child);}return false;
  }
  Type *remapType(Type *type) override {
    if(auto found=mapped.find(type);found!=mapped.end())return found->second;
    if(auto found=descriptions.find(type);found!=descriptions.end()) {
      Descriptor d=found->second;auto *replacement=StructType::create(type->getContext(),(cast<StructType>(type)->getName()+".scalars").str());
      replacement->setBody(ArrayType::get(componentType(type->getContext(),d.component),cells(d)));mapped[type]=replacement;descriptions[replacement]=d;return replacement;
    }
    if(!containsMatrix(type))return type;
    Type *result=nullptr;
    if(auto *array=dyn_cast<ArrayType>(type))result=ArrayType::get(remapType(array->getElementType()),array->getNumElements());
    else if(auto *structure=dyn_cast<StructType>(type)) {auto *replacement=StructType::create(type->getContext(),structure->hasName()?(structure->getName()+".matrix").str():"");mapped[type]=replacement;SmallVector<Type *,8> fields;for(Type *field:structure->elements())fields.push_back(remapType(field));replacement->setBody(fields,structure->isPacked());result=replacement;}
    else if(auto *pointer=dyn_cast<PointerType>(type))result=PointerType::get(remapType(pointer->getNonOpaquePointerElementType()),pointer->getAddressSpace());
    else if(auto *function=dyn_cast<FunctionType>(type)) {SmallVector<Type *,8> args;for(Type *arg:function->params())args.push_back(remapType(arg));result=FunctionType::get(remapType(function->getReturnType()),args,function->isVarArg());}
    else if(auto *vector=dyn_cast<FixedVectorType>(type))result=FixedVectorType::get(remapType(vector->getElementType()),vector->getNumElements());
    assert(result && "unsupported LLVM aggregate containing a matrix");mapped[type]=result;return result;
  }
};

class MatrixLowerer {
  ModuleInfo &info;Module &module;MatrixTypes types;IRBuilder<> builder;
  DenseMap<Value *,Descriptor> old_descriptions;
  DenseMap<Value *,AllocaInst *> old_storage;
  DenseMap<Value *,Value *> snapshots;
  GlobalVariable *scratch=nullptr;uint64_t scratch_bytes=0;
  Value *lane=nullptr,*thread=nullptr;
  Function *position_function=nullptr;
  SmallVector<AllocaInst *,16> obsolete_allocas;
  Instruction *position=nullptr;

  Value *dxcall(StringRef name,Type *result,ArrayRef<Value *> arguments) {
    SmallVector<Type *,8> parameters;for(Value *v:arguments)parameters.push_back(v->getType());
    return builder.CreateCall(module.getOrInsertFunction(name,FunctionType::get(result,parameters,false)),arguments);
  }
  Value *laneIndex() {
    if(position_function!=position->getFunction()) {position_function=position->getFunction();lane=nullptr;thread=nullptr;}
    if(!lane) {auto save=builder.saveIP();builder.SetInsertPoint(&*position_function->getEntryBlock().getFirstInsertionPt());lane=dxcall("dx.op.waveGetLaneIndex.i32",builder.getInt32Ty(),{builder.getInt32(111)});builder.restoreIP(save);}return lane;
  }
  Value *threadIndex() {
    laneIndex();if(!thread) {auto save=builder.saveIP();builder.SetInsertPoint(&*position_function->getEntryBlock().getFirstInsertionPt());thread=dxcall("dx.op.flattenedThreadIdInGroup.i32",builder.getInt32Ty(),{builder.getInt32(96)});builder.restoreIP(save);}return thread;
  }
  Value *shuffle(Value *value,Value *owner) {
    Type *type=value->getType();unsigned bits=type->getScalarSizeInBits();
    if(bits==64) {Value *integer=builder.CreateBitCast(value,builder.getInt64Ty());Value *low=shuffle(builder.CreateTrunc(integer,builder.getInt32Ty()),owner),*high=shuffle(builder.CreateTrunc(builder.CreateLShr(integer,32),builder.getInt32Ty()),owner);return builder.CreateBitCast(builder.CreateOr(builder.CreateZExt(low,builder.getInt64Ty()),builder.CreateShl(builder.CreateZExt(high,builder.getInt64Ty()),32)),type);}
    Value *integer=type->isIntegerTy()?value:builder.CreateBitCast(value,builder.getIntNTy(bits));integer=builder.CreateZExtOrTrunc(integer,builder.getInt32Ty());
    integer=dxcall("dx.op.waveReadLaneAt.i32",builder.getInt32Ty(),{builder.getInt32(117),integer,owner});integer=builder.CreateTruncOrBitCast(integer,builder.getIntNTy(bits));return type->isIntegerTy()?integer:builder.CreateBitCast(integer,type);
  }
  Value *uniform(Value *value,const Descriptor &d) {return d.scope==0?value:shuffle(value,builder.getInt32(0));}
  void barrier() {dxcall("dx.op.barrier",builder.getVoidTy(),{builder.getInt32(80),builder.getInt32(9)});}
  Descriptor description(Value *value) const {if(auto found=old_descriptions.find(value);found!=old_descriptions.end())return found->second;return types.descriptions.lookup(value->getType());}
  unsigned cells(const Descriptor &d) const {if(d.fragment)return (d.size()+31)/32;return types.cells(d);}
  Value *cell(Value *value,unsigned index) {
    if(auto found=old_storage.find(value);found!=old_storage.end())return builder.CreateLoad(componentType(module.getContext(),description(value).component),builder.CreateInBoundsGEP(found->second->getAllocatedType(),found->second,{builder.getInt32(0),builder.getInt32(index)}));
    return builder.CreateExtractValue(value,{0,index});
  }
  Value *packet(Type *type,ArrayRef<Value *> values) {Value *result=UndefValue::get(type);for(unsigned i=0;i<values.size();++i)result=builder.CreateInsertValue(result,values[i],{0,i});return result;}
  void writeOld(Value *value,ArrayRef<Value *> values) {AllocaInst *storage=old_storage.lookup(value);for(unsigned i=0;i<values.size();++i)builder.CreateStore(values[i],builder.CreateInBoundsGEP(storage->getAllocatedType(),storage,{builder.getInt32(0),builder.getInt32(i)}));}
  std::pair<Value *,Value *> coordinate(const Descriptor &d,Value *index) {
    if(d.fragment) {Value *flat=builder.CreateAdd(laneIndex(),builder.CreateMul(index,builder.getInt32(32)));return d.fragment==1?std::pair<Value *,Value *>{flat,builder.getInt32(0)}:std::pair<Value *,Value *>{builder.getInt32(0),flat};}
    if(d.scope!=1) {Value *flat=d.scope==0?index:builder.CreateAdd(threadIndex(),builder.CreateMul(index,builder.getInt32(types.group_threads)));return {builder.CreateUDiv(flat,builder.getInt32(d.cols)),builder.CreateURem(flat,builder.getInt32(d.cols))};}
    Value *l=laneIndex();
    if(d.use==0) return {builder.CreateAdd(builder.CreateUDiv(l,builder.getInt32(2)),builder.CreateMul(builder.CreateUDiv(index,builder.getInt32(d.cols/2)),builder.getInt32(16))),builder.CreateAdd(builder.CreateURem(l,builder.getInt32(2)),builder.CreateMul(builder.CreateURem(index,builder.getInt32(d.cols/2)),builder.getInt32(2)))};
    if(d.use==1) return {builder.CreateAdd(builder.CreateUDiv(l,builder.getInt32(16)),builder.CreateMul(builder.CreateURem(index,builder.getInt32(d.rows/2)),builder.getInt32(2))),builder.CreateAdd(builder.CreateURem(l,builder.getInt32(16)),builder.CreateMul(builder.CreateUDiv(index,builder.getInt32(d.rows/2)),builder.getInt32(16)))};
    // The accumulator deliberately uses A layout, as QueryAccumulatorLayout reports.
    return {builder.CreateAdd(builder.CreateUDiv(l,builder.getInt32(2)),builder.CreateMul(builder.CreateUDiv(index,builder.getInt32(d.cols/2)),builder.getInt32(16))),builder.CreateAdd(builder.CreateURem(l,builder.getInt32(2)),builder.CreateMul(builder.CreateURem(index,builder.getInt32(d.cols/2)),builder.getInt32(2)))};
  }
  Value *choose(Value *value,Value *index) {
    unsigned count=cells(description(value));if(auto *constant=dyn_cast<ConstantInt>(index);constant && constant->getZExtValue()<count)return cell(value,constant->getZExtValue());
    Value *result=Constant::getNullValue(componentType(module.getContext(),description(value).component));for(unsigned i=0;i<count;++i)result=builder.CreateSelect(builder.CreateICmpEQ(index,builder.getInt32(i)),cell(value,i),result);return result;
  }
  Value *fetch(Value *value,Value *row,Value *col) {
    Descriptor d=description(value);Value *flat=builder.CreateAdd(builder.CreateMul(row,builder.getInt32(d.cols)),col);
    if(d.scope==2) {Value *base=snapshots.lookup(value);return builder.CreateLoad(componentType(module.getContext(),d.component),builder.CreateInBoundsGEP(componentType(module.getContext(),d.component),base,flat));}
    if(d.scope==0)return choose(value,flat);
    Value *owner,*index;
    if(d.fragment) {Value *f=d.fragment==1?row:col;owner=builder.CreateURem(f,builder.getInt32(32));index=builder.CreateUDiv(f,builder.getInt32(32));}
    else if(d.use==1) {owner=builder.CreateAdd(builder.CreateMul(builder.CreateURem(row,builder.getInt32(2)),builder.getInt32(16)),builder.CreateURem(col,builder.getInt32(16)));index=builder.CreateAdd(builder.CreateMul(builder.CreateUDiv(col,builder.getInt32(16)),builder.getInt32(d.rows/2)),builder.CreateUDiv(row,builder.getInt32(2)));}
    else {owner=builder.CreateAdd(builder.CreateMul(builder.CreateURem(row,builder.getInt32(16)),builder.getInt32(2)),builder.CreateURem(col,builder.getInt32(2)));index=builder.CreateAdd(builder.CreateMul(builder.CreateUDiv(row,builder.getInt32(16)),builder.getInt32(d.cols/2)),builder.CreateUDiv(col,builder.getInt32(2)));}
    Value *result=UndefValue::get(componentType(module.getContext(),d.component));for(unsigned i=0;i<cells(d);++i)result=builder.CreateSelect(builder.CreateICmpEQ(index,builder.getInt32(i)),shuffle(cell(value,i),owner),result);return result;
  }
  Value *floatValue(Value *value,unsigned component) {
    if(component==23)return builder.CreateBitCast(builder.CreateShl(builder.CreateZExt(value,builder.getInt32Ty()),16),builder.getFloatTy());
    if(component!=8 && component!=21 && component!=22)return value;
    unsigned fraction=component==8?10:component==21?3:2,bias=component==21?7:15;
    Value *storage=component==8?builder.CreateBitCast(value,builder.getInt16Ty()):value;
    Value *byte=builder.CreateZExt(storage,builder.getInt32Ty()),*sign=builder.CreateShl(builder.CreateAnd(byte,builder.getInt32(component==8?0x8000:128)),component==8?16:24),*exponent=builder.CreateAnd(builder.CreateLShr(byte,fraction),builder.getInt32(component==21?15:31)),*mantissa=builder.CreateAnd(byte,builder.getInt32((1u<<fraction)-1));
    Value *normal=builder.CreateBitCast(builder.CreateOr(sign,builder.CreateOr(builder.CreateShl(builder.CreateAdd(exponent,builder.getInt32(127-bias)),23),builder.CreateShl(mantissa,23-fraction))),builder.getFloatTy());
    Value *tiny=builder.CreateFMul(builder.CreateUIToFP(mantissa,builder.getFloatTy()),ConstantFP::get(builder.getFloatTy(),std::ldexp(1.0,1-static_cast<int>(bias)-static_cast<int>(fraction))));tiny=builder.CreateSelect(builder.CreateICmpNE(sign,builder.getInt32(0)),builder.CreateFNeg(tiny),tiny);
    Value *exception=component==21?builder.CreateICmpEQ(builder.CreateAnd(byte,builder.getInt32(127)),builder.getInt32(127)):builder.CreateICmpEQ(exponent,builder.getInt32(31));
    Value *special=builder.CreateBitCast(builder.CreateOr(sign,builder.CreateSelect(builder.CreateICmpEQ(mantissa,builder.getInt32(0)),builder.getInt32(component==21?0x7fc00000:0x7f800000),builder.getInt32(0x7fc00000))),builder.getFloatTy());
    return builder.CreateSelect(exception,special,builder.CreateSelect(builder.CreateICmpEQ(exponent,builder.getInt32(0)),tiny,normal));
  }
  Value *encodeFloat(Value *value,unsigned component) {
    Value *bits=builder.CreateBitCast(value,builder.getInt32Ty());
    if(component==23) {Value *rounded=builder.CreateAdd(bits,builder.CreateAdd(builder.getInt32(0x7fff),builder.CreateAnd(builder.CreateLShr(bits,16),builder.getInt32(1))));Value *nan=builder.CreateICmpUGT(builder.CreateAnd(bits,builder.getInt32(0x7fffffff)),builder.getInt32(0x7f800000));return builder.CreateTrunc(builder.CreateSelect(nan,builder.CreateOr(builder.CreateLShr(bits,16),builder.getInt32(0x40)),builder.CreateLShr(rounded,16)),builder.getInt16Ty());}
    unsigned fraction=component==8?10:component==21?3:2,bias=component==21?7:15,max_code=component==8?0x7bff:component==21?126:123;
    Value *sign=builder.CreateAnd(builder.CreateLShr(bits,component==8?16:24),builder.getInt32(component==8?0x8000:128)),*magnitude=builder.CreateAnd(bits,builder.getInt32(0x7fffffff)),*exponent=builder.CreateSub(builder.CreateLShr(magnitude,23),builder.getInt32(127-bias)),*mantissa=builder.CreateAnd(bits,builder.getInt32(0x7fffff));
    auto roundShift=[&](Value *significand,Value *shift) {Value *safe=builder.CreateSelect(builder.CreateICmpUGT(shift,builder.getInt32(31)),builder.getInt32(31),shift),*base=builder.CreateLShr(significand,safe),*mask=builder.CreateSub(builder.CreateShl(builder.getInt32(1),safe),builder.getInt32(1)),*lost=builder.CreateAnd(significand,mask),*half=builder.CreateShl(builder.getInt32(1),builder.CreateSub(safe,builder.getInt32(1)));Value *increment=builder.CreateOr(builder.CreateICmpUGT(lost,half),builder.CreateAnd(builder.CreateICmpEQ(lost,half),builder.CreateICmpNE(builder.CreateAnd(base,builder.getInt32(1)),builder.getInt32(0))));return builder.CreateSelect(builder.CreateICmpUGT(shift,builder.getInt32(31)),builder.getInt32(0),builder.CreateAdd(base,builder.CreateZExt(increment,builder.getInt32Ty())));};
    Value *normal=builder.CreateAdd(builder.CreateShl(exponent,fraction),roundShift(mantissa,builder.getInt32(23-fraction))),*subnormal=roundShift(builder.CreateOr(mantissa,builder.getInt32(0x800000)),builder.CreateSub(builder.getInt32(24-fraction),exponent));
    Value *code=builder.CreateSelect(builder.CreateICmpSGT(exponent,builder.getInt32(0)),normal,subnormal),*overflow=builder.CreateICmpUGT(code,builder.getInt32(max_code));code=builder.CreateSelect(overflow,builder.getInt32(component==8?0x7c00:component==21?127:124),code);
    Value *nan=builder.CreateICmpUGT(magnitude,builder.getInt32(0x7f800000));code=builder.CreateSelect(nan,builder.getInt32(component==8?0x7e00:127),code);Value *packed=builder.CreateTrunc(builder.CreateOr(sign,code),builder.getIntNTy(component==8?16:8));return component==8?builder.CreateBitCast(packed,builder.getHalfTy()):packed;
  }
  Value *convert(Value *value,unsigned source,unsigned target) {
    if(source==target)return value;
    Type *destination=componentType(module.getContext(),target);
    bool sf=floatComponent(source),tf=floatComponent(target);
    if(sf)value=floatValue(value,source);
    Type *arithmetic_target=(target==8 || target==21 || target==22 || target==23)?builder.getFloatTy():destination;
    if(sf && tf) {if(value->getType()!=arithmetic_target)value=builder.CreateFPCast(value,arithmetic_target);}
    else if(sf) {
      unsigned width=destination->getIntegerBitWidth();bool sign=signedComponent(target);
      Value *rounded=dxcall(value->getType()->isDoubleTy()?"dx.op.unary.f64":"dx.op.unary.f32",value->getType(),{builder.getInt32(26),value});
      Value *upper=ConstantFP::get(value->getType(),std::ldexp(1.0,width-(sign?1:0))),*lower=sign?builder.CreateFNeg(upper):Constant::getNullValue(value->getType());
      Value *nan=builder.CreateFCmpUNO(value,value),*low=builder.CreateFCmpOLT(rounded,lower),*high=builder.CreateFCmpOGE(rounded,upper);
      Value *safe=builder.CreateSelect(builder.CreateOr(nan,builder.CreateOr(low,high)),Constant::getNullValue(value->getType()),rounded);
      Value *integer=sign?builder.CreateFPToSI(safe,destination):builder.CreateFPToUI(safe,destination);
      Value *minimum=ConstantInt::get(module.getContext(),sign?APInt::getSignedMinValue(width):APInt::getMinValue(width)),*maximum=ConstantInt::get(module.getContext(),sign?APInt::getSignedMaxValue(width):APInt::getMaxValue(width));
      value=builder.CreateSelect(nan,Constant::getNullValue(destination),builder.CreateSelect(low,minimum,builder.CreateSelect(high,maximum,integer)));
    }
    else if(tf)value=signedComponent(source)?builder.CreateSIToFP(value,arithmetic_target):builder.CreateUIToFP(value,arithmetic_target);
    else {
      unsigned from=value->getType()->getIntegerBitWidth(),to=destination->getIntegerBitWidth();bool ss=signedComponent(source),ts=signedComponent(target);
      if(ss && !ts)value=builder.CreateSelect(builder.CreateICmpSLT(value,Constant::getNullValue(value->getType())),Constant::getNullValue(value->getType()),value);
      if(to<from || (!ss && ts && to==from)) {
        APInt max=(ts?APInt::getSignedMaxValue(to):APInt::getMaxValue(to)).zextOrTrunc(from);Value *maximum=ConstantInt::get(module.getContext(),max);
        value=builder.CreateSelect(ss?builder.CreateICmpSGT(value,maximum):builder.CreateICmpUGT(value,maximum),maximum,value);
        if(ss && ts) {Value *minimum=ConstantInt::get(module.getContext(),APInt::getSignedMinValue(to).sextOrTrunc(from));value=builder.CreateSelect(builder.CreateICmpSLT(value,minimum),minimum,value);}
      }
      value=ss && ts?builder.CreateSExtOrTrunc(value,destination):builder.CreateZExtOrTrunc(value,destination);
    }
    if(target==8 || target==21 || target==22 || target==23)value=encodeFloat(value,target);return value;
  }
  Value *arithmetic(Value *a,Value *b,unsigned component,unsigned operation) {
    if(floatComponent(component)) {a=floatValue(a,component);b=floatValue(b,component);Value *result=operation==0?builder.CreateFAdd(a,b):operation==1?builder.CreateFSub(a,b):operation==2?builder.CreateFMul(a,b):builder.CreateFDiv(a,b);return component==8 || component==21 || component==22 || component==23?encodeFloat(result,component):result;}
    return operation==0?builder.CreateAdd(a,b):operation==1?builder.CreateSub(a,b):operation==2?builder.CreateMul(a,b):signedComponent(component)?builder.CreateSDiv(a,b):builder.CreateUDiv(a,b);
  }
  Value *mathInput(Value *value,unsigned source,unsigned target) {
    // Dot products preserve each integer operand's signedness and reduce only
    // in accumulator width. Saturating casts belong to explicit conversions.
    if(!floatComponent(source) && !floatComponent(target))return signedComponent(source)?builder.CreateSExtOrTrunc(value,componentType(module.getContext(),target)):builder.CreateZExtOrTrunc(value,componentType(module.getContext(),target));
    return convert(value,source,target);
  }
  Value *multiplyAdd(Value *a,Value *b,Value *c,unsigned component) {
    if(!floatComponent(component))return builder.CreateAdd(builder.CreateMul(a,b),c);
    a=floatValue(a,component);b=floatValue(b,component);c=floatValue(c,component);
    Value *result=builder.CreateCall(Intrinsic::getDeclaration(&module,Intrinsic::fma,{c->getType()}),{a,b,c});return component==8 || component==21 || component==22 || component==23?encodeFloat(result,component):result;
  }
  std::string suffix(Type *type) {if(type->isHalfTy())return "f16";if(type->isFloatTy())return "f32";if(type->isDoubleTy())return "f64";return "i"+std::to_string(type->getIntegerBitWidth());}
  Value *rawLoad(Value *handle,Value *offset,Type *type) {
    auto *result=StructType::get(module.getContext(),{type,type,type,type,builder.getInt32Ty()});
    Value *load=dxcall("dx.op.rawBufferLoad."+suffix(type),result,{builder.getInt32(139),handle,offset,UndefValue::get(builder.getInt32Ty()),builder.getInt8(1),builder.getInt32(type->getScalarSizeInBits()/8)});return builder.CreateExtractValue(load,0);
  }
  void rawStore(Value *handle,Value *offset,Value *value) {
    Type *type=value->getType();Value *undef=UndefValue::get(type);
    dxcall("dx.op.rawBufferStore."+suffix(type),builder.getVoidTy(),{builder.getInt32(140),handle,offset,UndefValue::get(builder.getInt32Ty()),value,undef,undef,undef,builder.getInt8(1),builder.getInt32(type->getScalarSizeInBits()/8)});
  }
  Value *address(const Descriptor &d,Value *row,Value *col,Value *offset,Value *stride,Value *layout,bool memory,bool retired=false) {
    unsigned bytes=componentBits(d.component)/8;
    if(d.fragment) {Value *element=builder.CreateAdd(offset,builder.CreateMul(d.fragment==1?row:col,stride));return memory?builder.CreateMul(element,builder.getInt32(bytes)):element;}
    Value *transposed=builder.CreateICmpEQ(builder.CreateAnd(layout,builder.getInt32(1)),builder.getInt32(1));
    if(d.scope==0) {Value *optimal=builder.CreateICmpUGE(layout,builder.getInt32(2));Value *dense=builder.CreateSelect(transposed,builder.getInt32(d.rows*bytes),builder.getInt32(d.cols*bytes));stride=builder.CreateSelect(builder.CreateAnd(optimal,builder.CreateICmpEQ(stride,builder.getInt32(0))),dense,stride);}
    Value *major=builder.CreateSelect(transposed,col,row),*minor=builder.CreateSelect(transposed,row,col);
    if(memory && retired && bytes==1)minor=builder.CreateUDiv(minor,builder.getInt32(4));
    else if(!memory)minor=builder.CreateMul(minor,builder.getInt32(bytes));
    Value *element=builder.CreateAdd(offset,builder.CreateAdd(builder.CreateMul(major,stride),minor));
    return memory?builder.CreateMul(element,builder.getInt32(retired && bytes==1?4:bytes)):element;
  }
  Value *memoryPointer(Value *resource,Value *byte,Type *type) {
    unsigned space=cast<PointerType>(resource->getType())->getAddressSpace();
    Value *base=builder.CreateBitCast(resource,PointerType::get(builder.getInt8Ty(),space));
    return builder.CreateBitCast(builder.CreateInBoundsGEP(builder.getInt8Ty(),base,byte),PointerType::get(type,space));
  }
  Value *loadElement(const Descriptor &d,Value *resource,Value *offset,Value *stride,Value *layout,bool memory,Value *row,Value *col,bool retired=false) {
    Type *type=componentType(module.getContext(),d.component);
    BasicBlock *before=nullptr,*loaded_block=nullptr;
    if(d.fragment || d.scope==2) {
      Value *valid=builder.CreateAnd(builder.CreateICmpULT(row,builder.getInt32(d.fragment==2?1:d.rows)),builder.CreateICmpULT(col,builder.getInt32(d.fragment==1?1:d.cols)));
      Instruction *then=SplitBlockAndInsertIfThen(valid,position,false);loaded_block=then->getParent();before=loaded_block->getSinglePredecessor();builder.SetInsertPoint(then);
    }
    Value *location=address(d,row,col,offset,stride,layout,memory,retired),*value;
    if(componentBits(d.component)==8) {
      Value *word;
      if(memory)word=builder.CreateLoad(builder.getInt32Ty(),memoryPointer(resource,builder.CreateAnd(location,builder.getInt32(~3u)),builder.getInt32Ty()));
      else word=rawLoad(resource,builder.CreateAnd(location,builder.getInt32(~3u)),builder.getInt32Ty());
      Value *minor=builder.CreateSelect(builder.CreateICmpEQ(builder.CreateAnd(layout,builder.getInt32(1)),builder.getInt32(1)),row,col);
      Value *shift=builder.CreateMul(builder.CreateAnd(memory && retired?minor:location,builder.getInt32(3)),builder.getInt32(8));value=builder.CreateTrunc(builder.CreateLShr(word,shift),builder.getInt8Ty());
    } else if(!memory)value=rawLoad(resource,location,type);
    else value=builder.CreateLoad(type,memoryPointer(resource,location,type));
    if(loaded_block) {
      builder.SetInsertPoint(position);auto *merged=builder.CreatePHI(type,2,"matrix.loaded");merged->addIncoming(Constant::getNullValue(type),before);merged->addIncoming(value,loaded_block);value=merged;
    }
    return value;
  }
  void guardedStore(const Descriptor &d,Value *row,Value *col,Value *resource,Value *location,Value *value,bool memory) {
    bool guard=d.fragment || d.scope==2;
    if(guard) {Value *valid=builder.CreateAnd(builder.CreateICmpULT(row,builder.getInt32(d.fragment==2?1:d.rows)),builder.CreateICmpULT(col,builder.getInt32(d.fragment==1?1:d.cols)));builder.SetInsertPoint(SplitBlockAndInsertIfThen(valid,position,false));}
    if(memory)builder.CreateStore(value,memoryPointer(resource,location,value->getType()));
    else rawStore(resource,location,value);
    if(guard)builder.SetInsertPoint(position);
  }
  bool needsSnapshot(const CallInst &call,Operation op) const {
    if(op.legacy)return false;
    switch(op.index-1) {
    case 0:case 2:case 12:case 13:return true;
    case 9:case 10:return componentBits(description(call.getArgOperand(1)).component)==8;
    default:return false;
    }
  }
  Error snapshotInputs(CallInst *call,Operation op) {
    snapshots.clear();if(!needsSnapshot(*call,op))return Error::success();uint64_t offset=0;
    for(Value *argument:call->args()) {
      auto found=types.descriptions.find(argument->getType());if(found==types.descriptions.end() || found->second.scope!=2 || snapshots.count(argument))continue;
      Descriptor d=found->second;offset=(offset+7)&~uint64_t(7);Type *type=componentType(module.getContext(),d.component);
      if(!scratch)return createError("dxil: missing group matrix snapshot storage");
      Value *bytes=builder.CreateInBoundsGEP(scratch->getValueType(),scratch,{builder.getInt32(0),builder.getInt32(offset)}),*base=builder.CreateBitCast(bytes,PointerType::get(type,3));snapshots[argument]=base;
      for(unsigned i=0;i<cells(d);++i) {Value *index=builder.CreateAdd(threadIndex(),builder.getInt32(i*types.group_threads));builder.CreateStore(cell(argument,i),builder.CreateInBoundsGEP(type,base,index));}
      offset+=uint64_t(cells(d))*types.group_threads*(componentBits(d.component)/8);
    }
    if(!snapshots.empty())barrier();return Error::success();
  }
  Error remapMatrices() {
    bool present=!types.descriptions.empty();for(Function &function:module)present|=types.containsMatrix(function.getFunctionType());if(!present)return Error::success();
    ValueToValueMapTy values;SmallVector<Function *,32> originals;SmallVector<GlobalVariable *,16> globals;
    for(Function &function:module)originals.push_back(&function);for(GlobalVariable &global:module.globals())globals.push_back(&global);
    for(GlobalVariable *global:globals) {Type *type=types.remapType(global->getValueType());if(type==global->getValueType()){values[global]=global;continue;}auto *replacement=new GlobalVariable(module,type,global->isConstant(),global->getLinkage(),nullptr,global->getName()+".matrix",nullptr,global->getThreadLocalMode(),global->getAddressSpace());replacement->copyAttributesFrom(global);values[global]=replacement;}
    for(Function *function:originals) {auto *type=cast<FunctionType>(types.remapType(function->getFunctionType()));if(function->isDeclaration() && type==function->getFunctionType()){values[function]=function;continue;}auto *replacement=Function::Create(type,function->getLinkage(),function->getAddressSpace(),function->getName()+".matrix",&module);replacement->copyAttributesFrom(function);values[function]=replacement;auto next=replacement->arg_begin();for(Argument &argument:function->args()){values[&argument]=&*next;next->setName(argument.getName());++next;}}
    for(GlobalVariable *global:globals)if(global->hasInitializer())cast<GlobalVariable>(values[global])->setInitializer(MapValue(global->getInitializer(),values,RF_None,&types));
    for(Function *function:originals) {auto *replacement=cast<Function>(values[function]);if(replacement==function || function->isDeclaration())continue;SmallVector<ReturnInst *,8> returns;CloneFunctionInto(replacement,function,values,CloneFunctionChangeType::GlobalChanges,returns,"",nullptr,&types);}
    for(NamedMDNode &named:module.named_metadata())for(unsigned i=0;i<named.getNumOperands();++i)named.setOperand(i,cast<MDNode>(MapMetadata(named.getOperand(i),values,RF_None,&types)));
    info.entry=cast<Function>(values[info.entry]);if(info.patch_constant_function)info.patch_constant_function=cast<Function>(values[info.patch_constant_function]);
    for(Function *function:originals)if(values[function]!=function){cast<Function>(values[function])->takeName(function);function->dropAllReferences();}
    for(Function *function:originals)if(values[function]!=function)function->eraseFromParent();
    for(GlobalVariable *global:globals)if(values[global]!=global){cast<GlobalVariable>(values[global])->takeName(global);global->dropAllReferences();global->eraseFromParent();}
    return Error::success();
  }
  Error annotate(CallInst *call) {
    Value *pointer=call->getArgOperand(1);auto *allocation=dyn_cast<AllocaInst>(pointer);
    auto *props=dyn_cast<Constant>(call->getArgOperand(2));if(!allocation || !props)return createError("dxil: WaveMatrix annotation requires an allocated object and constant properties");
    unsigned fields[4];for(unsigned i=0;i<4;++i){auto *field=dyn_cast_or_null<ConstantInt>(props->getAggregateElement(i));if(!field)return createError("dxil: malformed WaveMatrix properties");fields[i]=field->getZExtValue();}
    unsigned kind=fields[0],component=fields[1],m=fields[2],n=fields[3];
    if(kind>4 || (m!=16 && m!=64) || (n!=16 && n!=64) || (component!=8 && component!=9 && component!=4 && component!=17 && component!=18))return createError("dxil: invalid WaveMatrix kind, component or dimensions");
    if((kind<2 && component==4) || (kind>=2 && (component==17 || component==18)))return createError("dxil: invalid WaveMatrix input/accumulator component pairing");
    Descriptor d{component,kind==1?16:m,kind==0?16:n,kind==1?1u:kind==4?2u:0u,1,kind==2?1u:kind==3?2u:0u};
    if(old_storage.count(pointer))return createError("dxil: duplicate WaveMatrix annotation");
    IRBuilder<> entry(&*call->getFunction()->getEntryBlock().getFirstInsertionPt());old_descriptions[pointer]=d;old_storage[pointer]=entry.CreateAlloca(ArrayType::get(componentType(module.getContext(),component),cells(d)),nullptr,"wave.matrix");obsolete_allocas.push_back(allocation);call->eraseFromParent();return Error::success();
  }
  Expected<SmallVector<Value *,16>> multiply(Value *a,Value *b,Value *c,const Descriptor &d) {
    Descriptor da=description(a),db=description(b);
    if(da.cols!=db.rows || da.rows!=d.rows || db.cols!=d.cols || da.scope!=d.scope || db.scope!=d.scope || da.use!=0 || db.use!=1 || d.use!=2 || da.fragment || db.fragment || d.fragment)return createError("dxil: matrix multiply dimensions/use/scope mismatch");
    SmallVector<Value *,16> output;Type *type=componentType(module.getContext(),d.component);
    for(unsigned i=0;i<cells(d);++i) {
      auto [row,col]=coordinate(d,builder.getInt32(i));Value *sum=c?convert(cell(c,i),description(c).component,d.component):Constant::getNullValue(type);
      for(unsigned k=0;k<da.cols;++k) {
        Value *av,*bv;
        if(d.scope==1) {
          // A-layout accumulator shares A's row/odd-even lane ownership.
          unsigned ar=(i/(d.cols/2))*(da.cols/2)+k/2;
          Value *ao=builder.CreateAdd(builder.CreateMul(builder.CreateURem(row,builder.getInt32(16)),builder.getInt32(2)),builder.getInt32(k%2));av=shuffle(cell(a,ar),ao);
          // B component index depends only on this output's column tile and k.
          unsigned base=(i%(d.cols/2))/8*(db.rows/2)+k/2;
          Value *bo=builder.CreateAdd(builder.getInt32((k%2)*16),builder.CreateURem(col,builder.getInt32(16)));bv=shuffle(cell(b,base),bo);
        } else {av=fetch(a,row,builder.getInt32(k));bv=fetch(b,builder.getInt32(k),col);}
        if(floatComponent(d.component)) {
          unsigned work=d.component==10 || da.component==10 || db.component==10?10:9;
          Value *next=multiplyAdd(convert(av,da.component,work),convert(bv,db.component,work),convert(sum,d.component,work),work);
          sum=convert(next,work,d.component);
        } else sum=multiplyAdd(mathInput(av,da.component,d.component),mathInput(bv,db.component,d.component),sum,d.component);
      }
      output.push_back(sum);
    }
    return output;
  }
  Error memoryOperation(Value *matrix,const Descriptor &d,Value *resource,Value *offset,Value *stride,Value *layout,bool memory,bool store,SmallVectorImpl<Value *> &output,bool retired=false) {
    offset=uniform(offset,d);stride=uniform(stride,d);layout=uniform(layout,d);
    if(store && componentBits(d.component)==8) {
      unsigned words=(d.size()+3)/4,width=d.scope==0?1:d.scope==1?32:types.group_threads;
      for(unsigned i=0;i<(words+width-1)/width;++i) {
        Value *word_index=builder.getInt32(i*width);if(d.scope!=0)word_index=builder.CreateAdd(word_index,d.scope==1?laneIndex():threadIndex());
        Value *flat=builder.CreateMul(word_index,builder.getInt32(4)),*transpose=builder.CreateICmpNE(builder.CreateAnd(layout,builder.getInt32(1)),builder.getInt32(0));
        Value *major=builder.CreateUDiv(flat,builder.CreateSelect(transpose,builder.getInt32(d.rows),builder.getInt32(d.cols))),*minor=builder.CreateURem(flat,builder.CreateSelect(transpose,builder.getInt32(d.rows),builder.getInt32(d.cols))),*word=builder.getInt32(0);
        for(unsigned j=0;j<4;++j) {Value *r=builder.CreateSelect(transpose,builder.CreateAdd(minor,builder.getInt32(j)),major),*c=builder.CreateSelect(transpose,major,builder.CreateAdd(minor,builder.getInt32(j)));word=builder.CreateOr(word,builder.CreateShl(builder.CreateZExt(fetch(matrix,r,c),builder.getInt32Ty()),8*j));}
        Value *row=builder.CreateSelect(transpose,minor,major),*col=builder.CreateSelect(transpose,major,minor),*location=address(d,row,col,offset,stride,layout,memory,retired);
        if(!memory)location=builder.CreateAnd(location,builder.getInt32(~3u));guardedStore(d,row,col,resource,location,word,memory);
      }
      return Error::success();
    }
    for(unsigned i=0;i<cells(d);++i) {auto [row,col]=coordinate(d,builder.getInt32(i));if(!store)output.push_back(loadElement(d,resource,offset,stride,layout,memory,row,col,retired));else guardedStore(d,row,col,resource,address(d,row,col,offset,stride,layout,memory,retired),cell(matrix,i),memory);}
    return Error::success();
  }
  Error legacy(CallInst *call,unsigned op) {
    if(op==1)return annotate(call);
    if(op==2){call->replaceAllUsesWith(builder.getInt32(16));call->eraseFromParent();return Error::success();}
    Value *matrix=call->getArgOperand(1);if(!old_storage.count(matrix))return createError("dxil: WaveMatrix operation lacks an annotation");Descriptor d=description(matrix);SmallVector<Value *,16> output;
    if(op==3) {Value *value=uniform(call->getArgOperand(2),d);for(unsigned i=0;i<cells(d);++i){Value *v=value;if(componentBits(d.component)==8){auto [r,c]=coordinate(d,builder.getInt32(i));v=builder.CreateTrunc(builder.CreateLShr(value,builder.CreateMul(builder.CreateAnd(d.use==1?r:c,builder.getInt32(3)),builder.getInt32(8))),builder.getInt8Ty());}else if(value->getType()!=componentType(module.getContext(),d.component))v=builder.CreateFPCast(value,componentType(module.getContext(),d.component));output.push_back(v);}}
    else if(op>=4 && op<=7) {bool memory=op==5 || op==7,store=op>=6;unsigned layout_index=memory?5:6;Value *layout=builder.CreateZExt(call->getArgOperand(layout_index),builder.getInt32Ty());if(auto error=memoryOperation(matrix,d,call->getArgOperand(2),call->getArgOperand(3),call->getArgOperand(4),layout,memory,store,output,true))return error;}
    else if(op==8 || op==9) {Descriptor a=description(call->getArgOperand(2)),b=description(call->getArgOperand(3));bool bytes=(a.component==17 || a.component==18) && (b.component==17 || b.component==18);if(!(bytes?d.component==4:a.component==b.component && (a.component==8?(d.component==8 || d.component==9):a.component==9 && d.component==9)))return createError("dxil: WaveMatrix multiply violates published input/accumulator types");auto result=multiply(call->getArgOperand(2),call->getArgOperand(3),op==9?matrix:nullptr,d);if(!result)return result.takeError();output=std::move(*result);}
    else if(op==10) {auto *operation=dyn_cast<ConstantInt>(call->getArgOperand(2));if(!operation || operation->getZExtValue()>3)return createError("dxil: invalid WaveMatrix scalar operation");Value *value=uniform(call->getArgOperand(3),d);for(unsigned i=0;i<cells(d);++i)output.push_back(arithmetic(cell(matrix,i),value,d.component,operation->getZExtValue()));}
    else {Value *other=call->getArgOperand(2);if(!old_storage.count(other))return createError("dxil: unannotated WaveMatrix accumulation input");Descriptor source=description(other);
      if(op==11 && (!d.fragment || source.fragment || (d.fragment==1?source.use!=0:source.use!=1)))return createError("dxil: WaveMatrix SumAccumulate sidedness mismatch");
      for(unsigned i=0;i<cells(d);++i) {auto [r,c]=coordinate(d,builder.getInt32(i));Value *sum=cell(matrix,i);
        if(op==11)for(unsigned k=0;k<16;++k)sum=arithmetic(sum,convert(fetch(other,d.fragment==1?r:builder.getInt32(k),d.fragment==1?builder.getInt32(k):c),source.component,d.component),d.component,0);
        else sum=arithmetic(sum,convert(fetch(other,source.fragment==2?builder.getInt32(0):r,source.fragment==1?builder.getInt32(0):c),source.component,d.component),d.component,0);
        output.push_back(sum);
      }
    }
    if(!output.empty())writeOld(matrix,output);call->eraseFromParent();return Error::success();
  }

  Value *vectorElement(Value *value,unsigned component,unsigned index) {
    unsigned packing=componentBits(component)==8?4:1;Value *element=value->getType()->isVectorTy()?builder.CreateExtractElement(value,index/packing):value;
    if(packing==4)return builder.CreateTrunc(builder.CreateLShr(element,(index%4)*8),builder.getInt8Ty());return element;
  }
  Value *vectorPacket(Type *type,unsigned component,ArrayRef<Value *> elements) {
    auto *vector=dyn_cast<FixedVectorType>(type);unsigned n=vector?vector->getNumElements():1,packing=componentBits(component)==8?4:1;Type *scalar=vector?vector->getElementType():type;Value *result=UndefValue::get(type);
    for(unsigned i=0;i<n;++i) {Value *v=packing==1?(i<elements.size()?elements[i]:Constant::getNullValue(scalar)):builder.getInt32(0);if(packing==4)for(unsigned j=0;j<4 && i*4+j<elements.size();++j)v=builder.CreateOr(v,builder.CreateShl(builder.CreateZExt(elements[i*4+j],builder.getInt32Ty()),j*8));if(vector)result=builder.CreateInsertElement(result,v,i);else result=v;}return result;
  }
  unsigned inferComponent(Type *type,bool sign) {type=type->getScalarType();if(type->isHalfTy())return 8;if(type->isFloatTy())return 9;if(type->isDoubleTy())return 10;switch(type->getIntegerBitWidth()){case 8:return sign?19:20;case 16:return sign?2:3;case 32:return sign?4:5;case 64:return sign?6:7;default:return 0;}}
  Value *compareExchange(Value *resource,Value *byte,Value *expected,Value *desired,bool memory) {
    Type *type=expected->getType();
    if(memory) {auto *exchange=builder.CreateAtomicCmpXchg(memoryPointer(resource,byte,type),expected,desired,MaybeAlign(type->getScalarSizeInBits()/8),AtomicOrdering::Monotonic,AtomicOrdering::Monotonic);return builder.CreateExtractValue(exchange,0);}
    return dxcall("dx.op.atomicCompareExchange."+suffix(type),type,{builder.getInt32(79),resource,byte,UndefValue::get(builder.getInt32Ty()),UndefValue::get(builder.getInt32Ty()),expected,desired});
  }
  void atomicAccumulate(Value *resource,Value *byte,Value *value,unsigned component,bool memory) {
    unsigned bits=componentBits(component);Type *integer=builder.getIntNTy(bits==64?64:32),*element=componentType(module.getContext(),component);
    Value *location=bits<32?builder.CreateAnd(byte,builder.getInt32(~3u)):byte;
    Value *shift=bits<32?builder.CreateMul(builder.CreateAnd(byte,builder.getInt32(3)),builder.getInt32(8)):builder.getInt32(0);
    Value *zero=Constant::getNullValue(integer),*initial=compareExchange(resource,location,zero,zero,memory);
    BasicBlock *before=position->getParent(),*after=before->splitBasicBlock(position,"matrix.atomic.done"),*loop=BasicBlock::Create(module.getContext(),"matrix.atomic.cas",position->getFunction(),after);before->getTerminator()->eraseFromParent();builder.SetInsertPoint(before);builder.CreateBr(loop);builder.SetInsertPoint(loop);
    auto *old=builder.CreatePHI(integer,2,"matrix.atomic.old");old->addIncoming(initial,before);
    Value *previous=old;if(bits<32)previous=builder.CreateTrunc(builder.CreateLShr(old,shift),builder.getIntNTy(bits));
    if(previous->getType()!=element)previous=builder.CreateBitCast(previous,element);
    Value *sum=arithmetic(previous,value,component,0),*desired=sum->getType()->isIntegerTy()?sum:builder.CreateBitCast(sum,builder.getIntNTy(bits));
    if(bits<32) {Value *mask=builder.CreateShl(builder.getInt32((1u<<bits)-1),shift);desired=builder.CreateOr(builder.CreateAnd(old,builder.CreateNot(mask)),builder.CreateShl(builder.CreateZExt(desired,builder.getInt32Ty()),shift));}
    Value *observed=compareExchange(resource,location,old,desired,memory);old->addIncoming(observed,loop);builder.CreateCondBr(builder.CreateICmpEQ(observed,old),after,loop);builder.SetInsertPoint(position);
  }
  Error atomicMatrix(CallInst *call,bool memory) {
    Value *matrix=call->getArgOperand(1);Descriptor d=description(matrix);if(d.use!=2)return createError("dxil: only accumulator matrices support interlocked storage");
    Value *resource=call->getArgOperand(2),*offset=uniform(call->getArgOperand(3),d),*stride=uniform(call->getArgOperand(4),d),*layout=uniform(call->getArgOperand(5),d);
    for(unsigned i=0;i<cells(d);++i) {auto [row,col]=coordinate(d,builder.getInt32(i));Value *byte=address(d,row,col,offset,stride,layout,memory),*value=cell(matrix,i);
      if(d.scope==2) {Value *valid=builder.CreateAnd(builder.CreateICmpULT(row,builder.getInt32(d.rows)),builder.CreateICmpULT(col,builder.getInt32(d.cols)));auto *then=SplitBlockAndInsertIfThen(valid,position,false);builder.SetInsertPoint(then);
        Instruction *saved=position;position=then;atomicAccumulate(resource,byte,value,d.component,memory);position=saved;builder.SetInsertPoint(position);
      } else atomicAccumulate(resource,byte,value,d.component,memory);
    }
    return Error::success();
  }
  Error successor(CallInst *call,Operation op) {
    unsigned index=op.index-1;Value *result=nullptr;SmallVector<Value *,16> output;Descriptor d=types.descriptions.lookup(call->getType());
    if(index==0 || index==12) {if(!d.rows)return createError("dxil: matrix multiply result lacks metadata");auto product=multiply(call->getArgOperand(1),call->getArgOperand(2),index==0?call->getArgOperand(3):nullptr,d);if(!product)return product.takeError();output=std::move(*product);}
    else if(index==1) {unsigned argument=op.preview?1:2;Value *value=uniform(call->getArgOperand(argument),d);bool sign=op.preview?signedComponent(d.component):cast<ConstantInt>(call->getArgOperand(1))->isOne();unsigned source=inferComponent(value->getType(),sign);for(unsigned i=0;i<cells(d);++i)output.push_back(convert(value,source,d.component));}
    else if(index==2) {Value *source=call->getArgOperand(1);Descriptor from=description(source);auto *transpose=dyn_cast<ConstantInt>(call->getArgOperand(2));if(!transpose || from.scope!=d.scope || (transpose->isZero()?(from.rows!=d.rows || from.cols!=d.cols):(from.rows!=d.cols || from.cols!=d.rows)))return createError("dxil: Matrix CopyConvert dimensions/scope/transpose mismatch");for(unsigned i=0;i<cells(d);++i){auto [r,c]=coordinate(d,builder.getInt32(i));output.push_back(convert(fetch(source,transpose->isZero()?r:c,transpose->isZero()?c:r),from.component,d.component));}}
    else if(index==3 || index==4) {if(auto error=memoryOperation(nullptr,d,call->getArgOperand(1),call->getArgOperand(2),call->getArgOperand(3),call->getArgOperand(4),index==4,false,output))return error;}
    else if(index>=5 && index<=8) {Value *matrix=call->getArgOperand(1);d=description(matrix);unsigned count=cells(d);
      if(index==5) {result=builder.getInt32(count);if(d.scope==2 && d.size()%types.group_threads)result=builder.CreateAdd(builder.getInt32(d.size()/types.group_threads),builder.CreateZExt(builder.CreateICmpULT(threadIndex(),builder.getInt32(d.size()%types.group_threads)),builder.getInt32Ty()));}
      else if(index==6) {Value *local=call->getArgOperand(2);auto [r,c]=coordinate(d,local);Value *valid=builder.CreateAnd(builder.CreateICmpULT(local,builder.getInt32(count)),builder.CreateAnd(builder.CreateICmpULT(r,builder.getInt32(d.rows)),builder.CreateICmpULT(c,builder.getInt32(d.cols))));Value *coordinates=UndefValue::get(call->getType());coordinates=builder.CreateInsertElement(coordinates,builder.CreateSelect(valid,c,builder.getInt32(~0u)),uint64_t(0));result=builder.CreateInsertElement(coordinates,builder.CreateSelect(valid,r,builder.getInt32(~0u)),1);}
      else if(index==7) {Value *local=call->getArgOperand(2);result=choose(matrix,local);if(d.scope==2){auto [r,c]=coordinate(d,local);Value *valid=builder.CreateAnd(builder.CreateICmpULT(r,builder.getInt32(d.rows)),builder.CreateICmpULT(c,builder.getInt32(d.cols)));result=builder.CreateSelect(valid,result,Constant::getNullValue(result->getType()));}}
      else {for(unsigned i=0;i<count;++i)output.push_back(builder.CreateSelect(builder.CreateICmpEQ(call->getArgOperand(2),builder.getInt32(i)),call->getArgOperand(3),cell(matrix,i)));}
    }
    else if(index==9 || index==10) {Value *matrix=call->getArgOperand(1);d=description(matrix);if(auto error=memoryOperation(matrix,d,call->getArgOperand(2),call->getArgOperand(3),call->getArgOperand(4),call->getArgOperand(5),index==10,true,output))return error;}
    else if(index==11)result=builder.getInt32(0);
    else if(index==13) {Value *lhs=call->getArgOperand(1),*rhs=call->getArgOperand(2);Descriptor source=description(rhs);if(d.rows!=source.rows || d.cols!=source.cols || d.scope!=source.scope)return createError("dxil: Matrix Accumulate shape/scope mismatch");for(unsigned i=0;i<cells(d);++i){auto [r,c]=coordinate(d,builder.getInt32(i));output.push_back(arithmetic(cell(lhs,i),convert(fetch(rhs,r,c),source.component,d.component),d.component,0));}}
    else if(index==14 || index==15) {Value *matrix=call->getArgOperand(1);Descriptor source=description(matrix);auto *sign=dyn_cast<ConstantInt>(call->getArgOperand(2));auto *interpretation=dyn_cast<ConstantInt>(call->getArgOperand(4));if(!sign || !interpretation)return createError("dxil: Matrix-vector interpretation must be constant");unsigned input=interpretation->getZExtValue(),target=inferComponent(call->getType(),sign->isOne());Type *type=componentType(module.getContext(),target);unsigned bias=index==15?inferComponent(call->getArgOperand(5)->getType(),sign->isOne()):target;if(index==15 && op.preview){auto *tag=dyn_cast<ConstantInt>(call->getArgOperand(6));if(!tag)return createError("dxil: bias interpretation must be constant");bias=tag->getZExtValue();}
      for(unsigned r=0;r<source.rows;++r) {Value *sum=index==15?convert(vectorElement(call->getArgOperand(5),bias,r),bias,target):Constant::getNullValue(type);
        Value *vector=call->getArgOperand(3);Type *physical=vector->getType()->getScalarType();
        unsigned physical_component=input;
        if(physical->isFloatingPointTy())physical_component=inferComponent(physical,true);
        else if(componentBits(input)==8 && vector->getType()->isVectorTy() && cast<FixedVectorType>(vector->getType())->getNumElements()==source.cols)
          physical_component=inferComponent(physical,signedComponent(input));
        for(unsigned k=0;k<source.cols;++k) {
          Value *a=fetch(matrix,builder.getInt32(r),builder.getInt32(k)),*v=vectorElement(vector,physical_component,k);
          // Packed inputs already have the matrix format. Native vectors are
          // converted to it first (Tier 1 requires FP32 -> saturated SInt8).
          bool same_integer_width=!floatComponent(physical_component) && !floatComponent(source.component) && componentBits(physical_component)==componentBits(source.component);
          unsigned vector_component=same_integer_width?physical_component:source.component;
          if(!same_integer_width)v=convert(v,physical_component,source.component);
          if(floatComponent(target)) {unsigned work=target==10 || source.component==10?10:9;Value *next=multiplyAdd(convert(a,source.component,work),convert(v,vector_component,work),convert(sum,target,work),work);sum=convert(next,work,target);}
          else sum=multiplyAdd(mathInput(a,source.component,target),mathInput(v,vector_component,target),sum,target);
        }
        output.push_back(sum);
      }result=vectorPacket(call->getType(),target,output);output.clear();}
    else if(index==16 || index==17) {if(auto error=atomicMatrix(call,index==17))return error;}
    else if(index==18) {unsigned first=op.preview?1:2;bool sign=op.preview?signedComponent(d.component):cast<ConstantInt>(call->getArgOperand(1))->isOne();Value *a=call->getArgOperand(first),*b=call->getArgOperand(first+1);unsigned ca=inferComponent(a->getType(),sign),cb=inferComponent(b->getType(),sign);for(unsigned i=0;i<cells(d);++i){auto [r,c]=coordinate(d,builder.getInt32(i));Value *av=UndefValue::get(a->getType()->getScalarType()),*bv=UndefValue::get(b->getType()->getScalarType());auto *va=cast<FixedVectorType>(a->getType()),*vb=cast<FixedVectorType>(b->getType());for(unsigned j=0;j<va->getNumElements();++j)av=builder.CreateSelect(builder.CreateICmpEQ(r,builder.getInt32(j)),builder.CreateExtractElement(a,j),av);for(unsigned j=0;j<vb->getNumElements();++j)bv=builder.CreateSelect(builder.CreateICmpEQ(c,builder.getInt32(j)),builder.CreateExtractElement(b,j),bv);output.push_back(arithmetic(convert(av,ca,d.component),convert(bv,cb,d.component),d.component,2));}}
    else if(index==19) {auto *source=dyn_cast<ConstantInt>(call->getArgOperand(2)),*target=dyn_cast<ConstantInt>(call->getArgOperand(3));if(!source || !target || !componentBits(source->getZExtValue()) || !componentBits(target->getZExtValue()))return createError("dxil: invalid LinAlg Convert interpretations");Value *vector=call->getArgOperand(1);unsigned count=vector->getType()->isVectorTy()?cast<FixedVectorType>(vector->getType())->getNumElements():1;count*=componentBits(source->getZExtValue())==8?4:1;for(unsigned i=0;i<count;++i)output.push_back(convert(vectorElement(vector,source->getZExtValue(),i),source->getZExtValue(),target->getZExtValue()));result=vectorPacket(call->getType(),target->getZExtValue(),output);output.clear();}
    else if(index==20) {unsigned vi=op.preview?1:4,hi=op.preview?2:1,oi=op.preview?3:2;Value *vector=call->getArgOperand(vi);auto *type=dyn_cast<FixedVectorType>(vector->getType());if(!type)return createError("dxil: vector interlocked accumulation needs a fixed vector");unsigned component=inferComponent(type,true);for(unsigned i=0;i<type->getNumElements();++i)atomicAccumulate(call->getArgOperand(hi),builder.CreateAdd(call->getArgOperand(oi),builder.getInt32(i*componentBits(component)/8)),builder.CreateExtractElement(vector,i),component,false);}
    if(!output.empty())result=packet(call->getType(),output);if(result)call->replaceAllUsesWith(result);
    if(!snapshots.empty())barrier();call->eraseFromParent();return Error::success();
  }
public:
  MatrixLowerer(ModuleInfo &input,DenseMap<Type *,Descriptor> records):info(input),module(*input.module),types(std::move(records),input.num_threads[0]*input.num_threads[1]*input.num_threads[2]),builder(module.getContext()) {}
  Error run() {
    if(!types.group_threads)types.group_threads=32;
    if(auto error=remapMatrices())return error;
    SmallVector<CallInst *,32> calls;
    for(Function &function:module)if(!function.isDeclaration())for(Instruction &instruction:instructions(function))if(auto *call=dyn_cast<CallInst>(&instruction)){auto operation=classify(*call);if(!operation)return operation.takeError();if(operation->index){calls.push_back(call);if(!needsSnapshot(*call,*operation))continue;uint64_t bytes=0;SmallPtrSet<Value *,4> inputs;for(Value *argument:call->args())if(inputs.insert(argument).second){auto found=types.descriptions.find(argument->getType());if(found!=types.descriptions.end() && found->second.scope==2)bytes=((bytes+7)&~uint64_t(7))+uint64_t(cells(found->second))*types.group_threads*(componentBits(found->second.component)/8);}scratch_bytes=std::max(scratch_bytes,bytes);}}
    if(scratch_bytes) {scratch=new GlobalVariable(module,ArrayType::get(builder.getInt8Ty(),scratch_bytes),false,GlobalValue::InternalLinkage,UndefValue::get(ArrayType::get(builder.getInt8Ty(),scratch_bytes)),"d3dalloy.matrix.snapshot",nullptr,GlobalVariable::NotThreadLocal,3);scratch->setAlignment(Align(8));}
    for(CallInst *call:calls) {position=call;builder.SetInsertPoint(call);auto operation=classify(*call);if(!operation)return operation.takeError();if(auto error=snapshotInputs(call,*operation))return error;if(auto error=operation->legacy?legacy(call,operation->index):successor(call,*operation))return error;}
    for(AllocaInst *allocation:obsolete_allocas) {SmallVector<Instruction *,8> dead;for(User *user:allocation->users())if(auto *cast=dyn_cast<BitCastInst>(user)){for(User *use:cast->users())if(auto *intrinsic=dyn_cast<IntrinsicInst>(use);intrinsic && (intrinsic->getIntrinsicID()==Intrinsic::lifetime_start || intrinsic->getIntrinsicID()==Intrinsic::lifetime_end))dead.push_back(intrinsic);else return createError("dxil: WaveMatrix object escapes through a pointer cast");dead.push_back(cast);}else return createError("dxil: WaveMatrix object escapes its intrinsic contract");for(Instruction *instruction:dead)instruction->eraseFromParent();allocation->eraseFromParent();}
    return Error::success();
  }
};
} // namespace
Expected<bool> IsMatrixOperation(const CallInst &call) {auto op=classify(call);if(!op)return op.takeError();return op->index!=0;}
Expected<bool> ValidateMatrixContract(Module &module,uint32_t major,uint32_t minor) {
  auto records=descriptors(module);if(!records)return records.takeError();bool legacy=false,modern=false,wave32=false,preview=false;
  for(Function &function:module)if(!function.isDeclaration())for(Instruction &instruction:instructions(function))if(auto *call=dyn_cast<CallInst>(&instruction)) {
    auto operation=classify(*call);if(!operation)return operation.takeError();if(!operation->index)continue;
    legacy|=operation->legacy;modern|=!operation->legacy;preview|=operation->preview;wave32|=operation->legacy;
    if(operation->legacy) {
      if(operation->index==2) {if(!call->getType()->isIntegerTy(32))return createError("dxil: WaveMatrix Depth must return i32");}
      else if(!call->getType()->isVoidTy() || !call->getArgOperand(1)->getType()->isPointerTy())return createError("dxil: malformed WaveMatrix object signature");
      continue;
    }
    auto has_matrix=[&](Value *value) {return records->count(value->getType())!=0;};
    for(Value *argument:call->args())if(auto *type=dyn_cast<StructType>(argument->getType());type && type->hasName() && type->getName().startswith("dx.types.LinAlgMatrix") && !has_matrix(argument))return createError("dxil: matrix operand lacks dx.targetTypes metadata");
    unsigned index=operation->index-1;
    if((index<=4 || index==8 || index==12 || index==13 || index==18) && !has_matrix(call))return createError("dxil: matrix result lacks dx.targetTypes metadata");
    if((index==0 || index==2 || (index>=5 && index<=17 && index!=11)) && !has_matrix(call->getArgOperand(1)))return createError("dxil: operation requires a matrix operand");
    if((index==0 || index==12 || index==13) && !has_matrix(call->getArgOperand(2)))return createError("dxil: operation requires its second matrix operand");
    if(index==0 && !has_matrix(call->getArgOperand(3)))return createError("dxil: matrix multiply-accumulate requires a matrix accumulator");
    if(index==5 || index==11) {if(!call->getType()->isIntegerTy(32))return createError("dxil: matrix length/layout query must return i32");}
    if(index==6) {auto *type=dyn_cast<FixedVectorType>(call->getType());if(!type || type->getNumElements()!=2 || !type->getElementType()->isIntegerTy(32))return createError("dxil: matrix coordinates must return <2 x i32>");}
    if((index==1 || index==18) && !operation->preview) {auto *flag=dyn_cast<ConstantInt>(call->getArgOperand(1));if(!flag || !flag->getType()->isIntegerTy(1))return createError("dxil: final matrix input signedness must be constant i1");}
    if(index==14 || index==15) {Descriptor d=records->lookup(call->getArgOperand(1)->getType());if(d.scope!=0 || d.use!=0)return createError("dxil: matrix-vector multiply requires a thread-scope A matrix");}
    if(index==18 && records->lookup(call->getType()).scope!=0)return createError("dxil: outer product requires thread-scope result");
  }
  if(legacy && (major!=6 || minor!=8))return createError("dxil: retired WaveMatrix contract requires its original experimental SM6.8 producer");
  if(modern && (major!=6 || minor!=10))return createError("dxil: linear algebra requires its explicit SM6.10 contract");
  if(!legacy && !modern)return false;
  auto *version=module.getNamedMetadata("dx.version");
  if(!version || version->getNumOperands()!=1 || version->getOperand(0)->getNumOperands()!=2)return createError("dxil: matrix contract requires an explicit DXIL version");
  auto vm=metadataInteger(version->getOperand(0)->getOperand(0)),vn=metadataInteger(version->getOperand(0)->getOperand(1));
  if(!vm)return vm.takeError();if(!vn)return vn.takeError();
  if(*vm!=1 || *vn!=(modern?10u:8u))return createError("dxil: matrix producer DXIL version does not match its opcode ABI");
  for(const auto &record:*records) {wave32|=record.second.scope==1;if(preview && record.second.component==23)return createError("dxil: pinned preview matrix ABI does not define bfloat16");}
  if(wave32) if(auto *entries=module.getNamedMetadata("dx.entryPoints"))for(MDNode *entry:entries->operands()) {
    if(entry->getNumOperands()<5)return createError("dxil: malformed matrix entry metadata");auto *properties=dyn_cast_or_null<MDNode>(entry->getOperand(4));if(!properties)continue;
    for(unsigned i=0;i+1<properties->getNumOperands();i+=2) {auto tag=metadataInteger(properties->getOperand(i));if(!tag)return tag.takeError();
      if(*tag!=11 && *tag!=23 && *tag!=4)continue;auto *values=dyn_cast<MDNode>(properties->getOperand(i+1));if(!values)return createError("dxil: malformed matrix wave/thread metadata");
      if(*tag==4 && legacy) {uint64_t count=1;for(Metadata *value:values->operands()){auto n=metadataInteger(value);if(!n)return n.takeError();count*=*n;}if(!count || count%32)return createError("dxil: retired WaveMatrix requires complete 32-lane waves");}
      if(*tag==11) {if(values->getNumOperands()!=1)return createError("dxil: malformed matrix WaveSize");auto n=metadataInteger(values->getOperand(0));if(!n)return n.takeError();if(*n!=32)return createError("dxil: wave matrices require WaveSize 32");}
      if(*tag==23) {if(values->getNumOperands()!=3)return createError("dxil: malformed matrix WaveSize range");auto low=metadataInteger(values->getOperand(0)),high=metadataInteger(values->getOperand(1));if(!low)return low.takeError();if(!high)return high.takeError();if(*low>32 || (*high?*high<32:*low!=32))return createError("dxil: matrix WaveSize range excludes 32");}
    }
  }
  return legacy || modern;
}
Error LowerMatrices(ModuleInfo &info) {
  auto valid=ValidateMatrixContract(*info.module,info.shader_model_major,info.shader_model_minor);if(!valid)return valid.takeError();if(!*valid)return Error::success();
  auto records=descriptors(*info.module);if(!records)return records.takeError();
  bool wave=false;
  for(const auto &record:*records)wave|=record.second.scope==1;
  for(Function &function:*info.module)for(BasicBlock &block:function)for(Instruction &instruction:block)
    if(auto *call=dyn_cast<CallInst>(&instruction);call && call->getCalledFunction() && call->getCalledFunction()->getName().startswith("dx.op.waveMatrix_"))wave=true;
  // Matrix distribution is fixed to 32, even when a ranged WaveSize prefers
  // fewer lanes. Validation above proves the declared range includes 32.
  if(wave)info.module->setModuleFlag(Module::Error,"d3dalloy.wave-size",ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(info.module->getContext()),32)));
  return MatrixLowerer(info,std::move(*records)).run();
}
} // namespace d3dalloy::dxil
