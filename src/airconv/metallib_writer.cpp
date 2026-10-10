#include "metallib_writer.hpp"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include <cstring>
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/SHA256.h"

using namespace llvm;

namespace dxmt::metallib {

template <typename T> std::string_view value(const T &value) {
  return std::string_view((const char *)(&value), sizeof(T));
};

struct InputAttribute {
  uint8_t attribute;
  std::string name;
  uint8_t type;
  // TODO: extend patch/control point here
};

Error MetallibWriter::Write(const llvm::Module &module, raw_ostream &OS) {

  // AIR declares the initializer; CNST exposes its index/type to native specialization.
  SmallVector<char, 64> function_constant_metadata;
  if (auto *constants = module.getNamedMetadata("air.function_constants"); constants && constants->getNumOperands()) {
    if (constants->getNumOperands() > UINT16_MAX)
      return createStringError(inconvertibleErrorCode(), "metallib function constant count exceeds CNST bounds");
    raw_svector_ostream metadata(function_constant_metadata);
    metadata << "CNST" << value(uint16_t(0)) << value(uint16_t(constants->getNumOperands()));
    SmallDenseSet<uint16_t, 8> indices;
    SmallDenseSet<StringRef, 8> names;
    for (auto *declaration : constants->operands()) {
      if (declaration->getNumOperands() != 4)
        return createStringError(inconvertibleErrorCode(), "invalid AIR function constant declaration");
      auto *reference = dyn_cast_or_null<ValueAsMetadata>(declaration->getOperand(0));
      auto *global = reference ? dyn_cast<GlobalVariable>(reference->getValue()) : nullptr;
      auto *type = dyn_cast_or_null<MDString>(declaration->getOperand(1));
      auto *name = dyn_cast_or_null<MDString>(declaration->getOperand(2));
      auto *indexValue = dyn_cast_or_null<ConstantAsMetadata>(declaration->getOperand(3));
      auto *index = indexValue ? dyn_cast<ConstantInt>(indexValue->getValue()) : nullptr;
      if (!global || !global->getValueType()->isIntegerTy(32) || global->getAddressSpace() != 2 ||
          global->getSection() != "air.fc_initializer" || !global->isExternallyInitialized() ||
          !global->hasInitializer() || !isa<UndefValue>(global->getInitializer()) ||
          !type || type->getString() != "uint" ||
          !name || name->getString().empty() || name->getString().contains('\0') || !index ||
          index->getValue().getLimitedValue(UINT16_MAX + 1u) > UINT16_MAX)
        return createStringError(inconvertibleErrorCode(), "unsupported AIR function constant type/name/index");
      const auto nativeIndex = uint16_t(index->getZExtValue());
      if (!indices.insert(nativeIndex).second || !names.insert(name->getString()).second)
        return createStringError(inconvertibleErrorCode(), "duplicate AIR function constant index/name");
      if (name->getString().size() > UINT16_MAX ||
          function_constant_metadata.size() - 6 + name->getString().size() + 5 > UINT16_MAX)
        return createStringError(inconvertibleErrorCode(), "metallib function constant table exceeds CNST bounds");
      // SDK CNST scalar record: NUL name, UInt(33), uint16 index, required(1).
      metadata << name->getString() << '\0' << value(uint8_t(33)) << value(nativeIndex) << value(uint8_t(1));
    }
    const uint16_t size = function_constant_metadata.size() - 6;
    std::memcpy(function_constant_metadata.data() + 4, &size, sizeof(size));
  }

  uint16_t airVersionMajor = 2, airVersionMinor = 6;
  if (auto *version = module.getNamedMetadata("air.version"); version && version->getNumOperands()) {
    auto *fields = version->getOperand(0);
    if (fields->getNumOperands() >= 2) {
      airVersionMajor = cast<ConstantInt>(cast<ConstantAsMetadata>(fields->getOperand(0))->getValue())->getZExtValue();
      airVersionMinor = cast<ConstantInt>(cast<ConstantAsMetadata>(fields->getOperand(1))->getValue())->getZExtValue();
    }
  }

  SmallVector<char, 0> bitcode;
  SmallVector<char, 0> public_metadata;
  SmallVector<char, 0> private_metadata;
  SmallVector<char, 0> function_def;

  uint32_t fn_count = 0;

  constexpr const char *entryKinds[] = {
    "air.vertex", "air.fragment", "air.kernel", "air.object", "air.mesh",
    "air.visible", "air.intersection",
  };
  SmallPtrSet<const GlobalValue *, 8> exports;
  for (auto kind : entryKinds)
    if (auto entries = module.getNamedMetadata(kind))
      for (auto entry : entries->operands())
        exports.insert(cast<Function>(cast<ValueAsMetadata>(entry->getOperand(0))->getValue()));

  struct FunctionBitcode {
    uint64_t offset;
    uint64_t size;
    std::array<uint8_t, 32> hash;
  };
  raw_svector_ostream bitcode_stream(bitcode);
  auto appendBitcode = [&](const Function *function) -> FunctionBitcode {
    const auto offset = bitcode_stream.tell();
    if (exports.size() <= 1) {
      WriteBitcodeToFile(module, bitcode_stream, false, nullptr, true);
    } else {
      // Each export must have its own AIR definition. Reusing the whole module
      // for linked functions defines the same public symbols multiple times.
      ValueToValueMapTy values;
      auto isolated = CloneModule(module, values, [&](const GlobalValue *value) {
        return value == function || !exports.contains(value);
      });
      auto entryFunction = cast<Function>(values[function]);
      for (auto kind : entryKinds) {
        auto entries = isolated->getNamedMetadata(kind);
        if (!entries)
          continue;
        MDNode *entry = nullptr;
        for (auto candidate : entries->operands())
          if (cast<ValueAsMetadata>(candidate->getOperand(0))->getValue() == entryFunction)
            entry = candidate;
        entries->clearOperands();
        if (entry)
          entries->addOperand(entry);
        else
          isolated->eraseNamedMetadata(entries);
      }
      legacy::PassManager passes;
      passes.add(createGlobalDCEPass());
      passes.run(*isolated);
      WriteBitcodeToFile(*isolated, bitcode_stream, false, nullptr, true);
    }
    const auto size = bitcode_stream.tell() - offset;
    return {offset, size, SHA256::hash(
      ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(bitcode.data()) + offset, size))};
  };

  raw_svector_ostream public_metadata_stream(public_metadata);
  raw_svector_ostream private_metadata_stream(private_metadata);

  {

    raw_svector_ostream function_def_stream(function_def);

    auto vertexFns = module.getNamedMetadata("air.vertex");
    if (vertexFns) {
      for (auto fn : vertexFns->operands()) {
        fn_count++;
        const auto recordOffset = function_def_stream.tell();
        function_def_stream << value(uint32_t(0));
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        const auto compiled = appendBitcode(func);
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Vertex});
        function_def_stream << value(MTLB_HASH_TAG{.hash = compiled.hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = compiled.size});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = compiled.offset,
        });
        function_def_stream << value(MTLB_VERS_TAG{
                  .airVersionMajor = airVersionMajor,
                  .airVersionMinor = airVersionMinor,
                  .languageVersionMajor = 3,
                  .languageVersionMinor = 1,
                });
        while (fn->getNumOperands() > 3 && isa<MDTuple>(fn->getOperand(3).get())
        ) {
          auto maybe_patch_tuple = cast<MDTuple>(fn->getOperand(3).get());
          if (maybe_patch_tuple->getNumOperands() != 4)
            break;
          if (!isa<MDString>(maybe_patch_tuple->getOperand(0).get()))
            break;
          auto air_patch =
            cast<MDString>(maybe_patch_tuple->getOperand(0).get());
          if (air_patch->getString() != "air.patch")
            break;
          if (!isa<MDString>(maybe_patch_tuple->getOperand(1).get()))
            break;
          auto air_patch_value =
            cast<MDString>(maybe_patch_tuple->getOperand(1).get());
          if (air_patch_value->getString() == "triangle") {
            function_def_stream
              << value(MTLB_TESS_TAG{.patchType = 1, .controlPointCount = 0});
          } else {
            function_def_stream
              << value(MTLB_TESS_TAG{.patchType = 2, .controlPointCount = 0});
          }
          break;
        }
        function_def_stream << "ENDT";
        const auto recordSize = uint32_t(function_def_stream.tell() - recordOffset);
        std::memcpy(function_def.data() + recordOffset, &recordSize, sizeof(recordSize));
        auto inputs = dyn_cast<MDTuple>(fn->getOperand(2).get());

        std::vector<InputAttribute> attributes;

        for (auto &input : inputs->operands()) {
          auto inputElement = dyn_cast<MDTuple>(input.get());
          auto inputKind =
            dyn_cast<MDString>(inputElement->getOperand(1))->getString();
          if (inputKind == "air.vertex_input") {
            uint32_t location =
              dyn_cast<ConstantInt>(
                dyn_cast<ConstantAsMetadata>(inputElement->getOperand(3))
                  ->getValue()
              )
                ->getValue()
                .getZExtValue();
            auto typeName =
              dyn_cast<MDString>(inputElement->getOperand(6))->getString();
            auto argName =
              dyn_cast<MDString>(inputElement->getOperand(8))->getString();
            attributes.push_back(InputAttribute{
              .attribute = (uint8_t)location,
              .name = (argName.str()),
              .type =
                (uint8_t)(typeName == "float4"  ? 0x06
                          : typeName == "uint4" ? 0x24
                                                : 0x20), // REFACTOR IT: hope it
                                                         // works in general?
            });
          }
        }
        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        if (attributes.size()) {
          // if no vertex attributes, then don't emit VATY, otherwise PSO
          // doesn't compile
          fn_public_metadata_stream << value(MTLBFourCC::VertexAttribute);
          auto lenOffset = fn_public_metadata_stream.tell();
          fn_public_metadata_stream << value((uint16_t)0);
          fn_public_metadata_stream << value((uint16_t)attributes.size());
          for (auto &vattr : attributes) {
            fn_public_metadata_stream << vattr.name << '\0';
            fn_public_metadata_stream << value(MTLB_VATY{
              .attribute = vattr.attribute,
              .__ = 0,
              .usage = 0,
              .active = 1,
            });
          }
          auto vatt_written = fn_public_metadata_stream.tell() - lenOffset;
          *(uint16_t *)(&fn_public_metadata[lenOffset]) = vatt_written - 2;
          fn_public_metadata_stream << value(MTLBFourCC::VertexAttributeType);
          fn_public_metadata_stream << value((uint16_t)(2 + attributes.size()));
          fn_public_metadata_stream << value((uint16_t)(attributes.size()));
          for (auto &vattr : attributes) {
            fn_public_metadata_stream << value(vattr.type);
          }
        }
        fn_public_metadata_stream << function_constant_metadata << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    // Non-vertex functions have no vertex attribute table.
    // These TYPE values are the public metallib export kinds, not MTLFunctionType.
    constexpr struct {
      const char *metadata;
      FunctionType type;
    } functionKinds[] = {
      {"air.fragment", FunctionType::Fragment},
      {"air.kernel", FunctionType::Kernel},
      {"air.visible", FunctionType::Visible},
      {"air.intersection", FunctionType::Intersection},
      {"air.object", FunctionType::Object},
      {"air.mesh", FunctionType::Mesh},
    };
    for (const auto &kind : functionKinds) {
      auto functions = module.getNamedMetadata(kind.metadata);
      if (!functions)
        continue;
      for (auto fn : functions->operands()) {
        fn_count++;
        const auto recordOffset = function_def_stream.tell();
        function_def_stream << value(uint32_t(0));
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        const auto compiled = appendBitcode(func);
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream << value(MTLB_TYPE_TAG{.type = kind.type});
        function_def_stream << value(MTLB_HASH_TAG{.hash = compiled.hash});
        function_def_stream << value(MTLB_MDSZ_TAG{.bitcodeSize = compiled.size});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = compiled.offset,
        });
        function_def_stream << value(MTLB_VERS_TAG{
                  .airVersionMajor = airVersionMajor,
                  .airVersionMinor = airVersionMinor,
                  .languageVersionMajor = 3,
                  .languageVersionMinor = 1,
                });
        function_def_stream << "ENDT";
        const auto recordSize = uint32_t(function_def_stream.tell() - recordOffset);
        std::memcpy(function_def.data() + recordOffset, &recordSize, sizeof(recordSize));
        public_metadata_stream << value(uint32_t(4 + function_constant_metadata.size()))
                               << function_constant_metadata << "ENDT";
        private_metadata_stream << value(uint32_t(4)) << "ENDT";
      }
    }
  }

  MTLBHeader header;
  header.Magic = MTLB_Magic;
  header.FileSize =
    sizeof(MTLBHeader) + sizeof(uint32_t) /* fn count */ +
    function_def.size() +
    sizeof(MTLBFourCC::EndTag) /* extended header*/
    + public_metadata.size() + private_metadata.size() + bitcode.size();
  header.FunctionListOffset = sizeof(MTLBHeader);
  header.FunctionListSize = function_def.size();
  header.PublicMetadataOffset =
    header.FunctionListOffset + header.FunctionListSize +
    sizeof(uint32_t) // extra room for function count
    + sizeof(MTLBFourCC::EndTag);
  header.PublicMetadataSize = public_metadata.size();
  header.PrivateMetadataOffset =
    header.PublicMetadataOffset + header.PublicMetadataSize;
  header.PrivateMetadataSize = private_metadata.size();
  header.BitcodeOffset =
    header.PrivateMetadataOffset + header.PrivateMetadataSize;
  header.BitcodeSize = bitcode.size();

  header.Type = FileType::MTLBType_Executable; // executable
  header.Platform = Platform::MTLBPlatform_macOS;
  header.VersionMajor = 2;
  header.VersionMinor = 7;
  header.OS = OS::MTLBOS_macOS;
  header.OSVersionMajor = 14;
  header.OSVersionMinor = 4;

  // write to stream
  OS << value(header);
  OS << value(fn_count);
  OS << function_def;
  OS.write("ENDT", 4); // extend header
  OS << public_metadata;
  OS << private_metadata;
  OS << bitcode;
  return Error::success();
}

} // namespace dxmt::metallib