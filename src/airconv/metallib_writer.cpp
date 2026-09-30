#include "metallib_writer.hpp"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include <cstring>
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"

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

void MetallibWriter::Write(const llvm::Module &module, raw_ostream &OS) {

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
    sha256_hash hash;
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
    return {offset, size, compute_sha256_hash(
      reinterpret_cast<const uint8_t *>(bitcode.data()) + offset, size)};
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
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    auto fragmentFns = module.getNamedMetadata("air.fragment");
    if (fragmentFns) {
      for (auto fn : fragmentFns->operands()) {
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
          << value(MTLB_TYPE_TAG{.type = FunctionType::Fragment});
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
        function_def_stream << "ENDT";
        const auto recordSize = uint32_t(function_def_stream.tell() - recordOffset);
        std::memcpy(function_def.data() + recordOffset, &recordSize, sizeof(recordSize));
        public_metadata_stream << value(4);
        public_metadata_stream << "ENDT";
        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    // Kernel, visible and intersection functions have no vertex attribute table.
    // These TYPE values are the public metallib export kinds, not MTLFunctionType.
    constexpr struct {
      const char *metadata;
      FunctionType type;
    } functionKinds[] = {
      {"air.kernel", FunctionType::Kernel},
      {"air.visible", FunctionType::Visible},
      {"air.intersection", FunctionType::Intersection},
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
        public_metadata_stream << value(uint32_t(4)) << "ENDT";
        private_metadata_stream << value(uint32_t(4)) << "ENDT";
      }
    }
    auto objectFns = module.getNamedMetadata("air.object");
    if (objectFns) {
      for (auto fn : objectFns->operands()) {
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
          << value(MTLB_TYPE_TAG{.type = FunctionType::Object});
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
        function_def_stream << "ENDT";
        const auto recordSize = uint32_t(function_def_stream.tell() - recordOffset);
        std::memcpy(function_def.data() + recordOffset, &recordSize, sizeof(recordSize));

        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }

    auto meshFns = module.getNamedMetadata("air.mesh");
    if (meshFns) {
      for (auto fn : meshFns->operands()) {
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
          << value(MTLB_TYPE_TAG{.type = FunctionType::Mesh});
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
        function_def_stream << "ENDT";
        const auto recordSize = uint32_t(function_def_stream.tell() - recordOffset);
        std::memcpy(function_def.data() + recordOffset, &recordSize, sizeof(recordSize));

        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
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
}

} // namespace dxmt::metallib