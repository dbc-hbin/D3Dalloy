// CPU-only matrix trust-boundary test. Inputs must be genuine frozen DXC
// fixtures; GPU execution is covered separately by owned-d3d12-matrices.cpp.
#include "dxil_converter.hpp"
#include "dxil_matrix.hpp"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstdio>
#include <string>

using namespace llvm;
int main(int argc,char **argv) {
  if(argc<2) {std::fprintf(stderr,"usage: matrix-contract <real-matrix.dxil>...\n");return 2;}
  unsigned failures=0;
  for(int i=1;i<argc;++i) {
    auto bytes=MemoryBuffer::getFile(argv[i]);
    if(!bytes) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL unreadable=%s\n",argv[i]);++failures;continue;}
    auto parsed=dxmt::dxil::ModuleInfo::Parse((*bytes)->getBufferStart(),(*bytes)->getBufferSize());
    if(!parsed) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL parse=%s error=%s\n",argv[i],toString(parsed.takeError()).c_str());++failures;continue;}
    auto &info=**parsed;
    CallInst *matrix=nullptr;
    for(Function &function:*info.module) if(!function.isDeclaration())
      for(Instruction &instruction:instructions(function)) if(auto *call=dyn_cast<CallInst>(&instruction)) {
        auto operation=dxmt::dxil::IsMatrixOperation(*call);
        if(!operation) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL classify=%s\n",toString(operation.takeError()).c_str());++failures;}
        else if(*operation && !matrix)matrix=call;
      }
    if(!matrix) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL fixture has no matrix operations\n");++failures;continue;}
    auto *opcode=cast<ConstantInt>(matrix->getArgOperand(0));
    matrix->setArgOperand(0,ConstantInt::get(opcode->getType(),opcode->getZExtValue()+1));
    auto bad=dxmt::dxil::ValidateMatrixContract(*info.module,info.shader_model_major,info.shader_model_minor);
    if(bad) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL mismatched opcode/name accepted\n");++failures;}
    else consumeError(bad.takeError());
    matrix->setArgOperand(0,opcode);
    auto wrong_version=dxmt::dxil::ValidateMatrixContract(*info.module,6,9);
    if(wrong_version) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL final-only SM6.9 accepted matrix ABI\n");++failures;}
    else consumeError(wrong_version.takeError());
    if(auto error=dxmt::dxil::LowerMatrices(info)) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL lower=%s error=%s\n",argv[i],toString(std::move(error)).c_str());++failures;continue;}
    if(verifyModule(*info.module,&errs())) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL invalid lowered IR=%s\n",argv[i]);++failures;continue;}
    bool leftover=false;
    for(Function &function:*info.module) if(!function.isDeclaration())
      for(Instruction &instruction:instructions(function)) if(auto *call=dyn_cast<CallInst>(&instruction)) {
        auto operation=dxmt::dxil::IsMatrixOperation(*call);
        if(!operation) {consumeError(operation.takeError());leftover=true;}else leftover|=*operation;
      }
    if(leftover) {std::fprintf(stderr,"MATRIX_CONTRACT_FAIL leftover matrix call=%s\n",argv[i]);++failures;}
    else std::printf("MATRIX_CONTRACT_CASE_PASS %s\n",argv[i]);
  }
  return failures?1:0;
}
