#include "metallib_writer.hpp"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

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

/* Second integer of a metadata tuple like !{i32 2, i32 6, i32 0}. */
static bool
get_version_pair(const llvm::Module &module, StringRef md_name, uint16_t ops[2], unsigned first_op) {
  auto named = module.getNamedMetadata(md_name);
  if (!named || !named->getNumOperands())
    return false;
  auto tuple = named->getOperand(0);
  if (tuple->getNumOperands() < first_op + 2)
    return false;
  for (unsigned i = 0; i < 2; i++) {
    auto cam = dyn_cast<ConstantAsMetadata>(tuple->getOperand(first_op + i).get());
    if (!cam)
      return false;
    auto ci = dyn_cast<ConstantInt>(cam->getValue());
    if (!ci)
      return false;
    ops[i] = (uint16_t)ci->getZExtValue();
  }
  return true;
}

void MetallibWriter::Write(llvm::Module &module, raw_ostream &OS) {

  SmallVector<char, 0> bitcode;
  SmallVector<char, 0> public_metadata;
  SmallVector<char, 0> private_metadata;
  SmallVector<char, 0> function_def;

  uint32_t fn_count = 0;

  /* The optimization pipeline infers attributes the AIR bitcode readers in
   * older OSes predate; an unknown attribute is a fatal "LLVM ERROR: Unknown
   * attribute kind" inside MTLCompilerService (observed on iOS 16.3, whose
   * reader also predates macOS 14's).  Apple's own tools strip these on
   * serialization -- a metallib built with -sdk iphoneos keeps only the
   * LLVM-8-era set -- so do the same.  Hints only, no semantic loss. */
  AttributeMask incompatible;
  incompatible.addAttribute(Attribute::MustProgress);
  incompatible.addAttribute(Attribute::NoFree);
  incompatible.addAttribute(Attribute::NoSync);
  incompatible.addAttribute(Attribute::WillReturn);
  incompatible.addAttribute(Attribute::NoUndef);
  incompatible.addAttribute(Attribute::NoCallback);
  for (auto &F : module) {
    F.removeFnAttrs(incompatible);
    F.removeRetAttrs(incompatible);
    for (unsigned i = 0; i < F.arg_size(); i++)
      F.removeParamAttrs(i, incompatible);
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          CB->removeFnAttrs(incompatible);
          CB->removeRetAttrs(incompatible);
          for (unsigned i = 0; i < CB->arg_size(); i++)
            CB->removeParamAttrs(i, incompatible);
        }
  }

  raw_svector_ostream bitcode_stream(bitcode);
  WriteBitcodeToFile(module, bitcode_stream, false, nullptr, true);

  auto hash =
    compute_sha256_hash((const uint8_t *)bitcode.data(), bitcode.size());

  /* The VERS tags have to state what the bitcode actually is: the loader
   * cross-checks them against the module's own air.version. */
  MTLB_VERS_TAG vers{};
  uint16_t pair[2];
  vers.airVersionMajor = 2, vers.airVersionMinor = 6;
  if (get_version_pair(module, "air.version", pair, 0))
    vers.airVersionMajor = pair[0], vers.airVersionMinor = pair[1];
  vers.languageVersionMajor = 3, vers.languageVersionMinor = 1;
  /* air.language_version is !{!"Metal", major, minor, patch} */
  if (get_version_pair(module, "air.language_version", pair, 1))
    vers.languageVersionMajor = pair[0], vers.languageVersionMinor = pair[1];

  raw_svector_ostream public_metadata_stream(public_metadata);
  raw_svector_ostream private_metadata_stream(private_metadata);

  {

    raw_svector_ostream function_def_stream(function_def);

    auto vertexFns = module.getNamedMetadata("air.vertex");
    if (vertexFns) {
      for (auto fn : vertexFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Vertex});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
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
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Fragment});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";
        public_metadata_stream << value(4);
        public_metadata_stream << "ENDT";
        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }
    auto kernelFns = module.getNamedMetadata("air.kernel");
    if (kernelFns) {
      for (auto fn : kernelFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Kernel});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";

        std::vector<InputAttribute> attributes;

        // auto inputs = dyn_cast<MDTuple>(fn->getOperand(2).get());
        // for (auto &input : inputs->operands()) {
        //   auto inputElement = dyn_cast<MDTuple>(input.get());
        //   auto inputKind =
        //     dyn_cast<MDString>(inputElement->getOperand(1))->getString();
        //   if (inputKind == "air.vertex_input") {
        //     uint32_t location =
        //       dyn_cast<ConstantInt>(
        //         dyn_cast<ConstantAsMetadata>(inputElement->getOperand(3))
        //           ->getValue()
        //       )
        //         ->getValue()
        //         .getZExtValue();
        //     auto typeName =
        //       dyn_cast<MDString>(inputElement->getOperand(6))->getString();
        //     auto argName =
        //       dyn_cast<MDString>(inputElement->getOperand(8))->getString();
        //     attributes.push_back(InputAttribute{
        //       .attribute = (uint8_t)location,
        //       .name = (argName.str()),
        //       .type =
        //         (uint8_t)(typeName == "float4"  ? 0x06
        //                   : typeName == "uint4" ? 0x24
        //                                         : 0x20), // REFACTOR IT: hope
        //                                         it
        //                                                  // works in general?
        //     });
        //   }
        // }
        SmallVector<char, 0> fn_public_metadata;
        raw_svector_ostream fn_public_metadata_stream(fn_public_metadata);
        // fn_public_metadata_stream << value(MTLBFourCC::VertexAttribute);
        // auto lenOffset = fn_public_metadata_stream.tell();
        // fn_public_metadata_stream << value((uint16_t)0);
        // fn_public_metadata_stream << value((uint16_t)attributes.size());
        // for (auto &vattr : attributes) {
        //   fn_public_metadata_stream << vattr.name << '\0';
        //   fn_public_metadata_stream << value(MTLB_VATY{
        //     .attribute = vattr.attribute,
        //     .__ = 0,
        //     .usage = 0,
        //     .active = 1,
        //   });
        // }
        // auto vatt_written = fn_public_metadata_stream.tell() - lenOffset;
        // *(uint16_t *)(&fn_public_metadata[lenOffset]) = vatt_written - 2;
        // fn_public_metadata_stream << value(MTLBFourCC::VertexAttributeType);
        // fn_public_metadata_stream << value((uint16_t)(2 +
        // attributes.size())); fn_public_metadata_stream <<
        // value((uint16_t)(attributes.size())); for (auto &vattr : attributes)
        // {
        //   fn_public_metadata_stream << value(vattr.type);
        // }
        fn_public_metadata_stream << "ENDT";

        public_metadata_stream << value((uint32_t)fn_public_metadata.size());
        public_metadata_stream << fn_public_metadata;

        private_metadata_stream << value(4);
        private_metadata_stream << "ENDT";
      }
    }

    auto objectFns = module.getNamedMetadata("air.object");
    if (objectFns) {
      for (auto fn : objectFns->operands()) {
        fn_count++;
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Object});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";

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
        auto func = dyn_cast<Function>(
          dyn_cast<ConstantAsMetadata>(fn->getOperand(0).get())->getValue()
        );
        auto name = func->getName();
        function_def_stream << "NAME";
        function_def_stream << value((uint16_t)(name.size() + 1));
        function_def_stream << name << '\0';
        function_def_stream
          << value(MTLB_TYPE_TAG{.type = FunctionType::Mesh});
        function_def_stream << value(MTLB_HASH_TAG{.hash = hash});
        function_def_stream
          << value(MTLB_MDSZ_TAG{.bitcodeSize = bitcode.size()});
        function_def_stream << value(MTLB_OFFT_TAG{
          .PublicMetadataOffset = public_metadata_stream.tell(),
          .PrivateMetadataOffset = private_metadata_stream.tell(),
          .BitcodeOffset = 0, // ??
        });
        function_def_stream << value(vers);
        function_def_stream << "ENDT";

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
    sizeof(uint32_t) /* constant: function list size */ + function_def.size() +
    sizeof(MTLBFourCC::EndTag) /* extended header*/
    + public_metadata.size() + private_metadata.size() + bitcode.size();
  header.FunctionListOffset = sizeof(MTLBHeader);
  header.FunctionListSize = function_def.size() + 4;
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
  header.VersionMajor = 2;
  header.VersionMinor = 7;
  /* Like the AIR triple, the container names the platform it will be loaded
   * on, so it follows the platform this library was built for (a Wine cross
   * build has no TARGET_OS_* and falls through to macOS).  A mismatch is
   * "This library format is not supported on this platform" at load. */
#if defined(TARGET_OS_VISION) && TARGET_OS_VISION
  header.Platform = Platform::MTLBPlatform_Embedded;
  header.OS = TARGET_OS_SIMULATOR ? OS::MTLBOS_visionOSSimulator : OS::MTLBOS_visionOS;
  header.OSVersionMajor = 1;
  header.OSVersionMinor = 0;
#elif defined(TARGET_OS_TV) && TARGET_OS_TV
  header.Platform = Platform::MTLBPlatform_Embedded;
  header.OS = TARGET_OS_SIMULATOR ? OS::MTLBOS_tvOSSimulator : OS::MTLBOS_tvOS;
  header.OSVersionMajor = 16;
  header.OSVersionMinor = 0;
#elif defined(TARGET_OS_IOS) && TARGET_OS_IOS
  header.Platform = Platform::MTLBPlatform_Embedded;
  header.OS = TARGET_OS_SIMULATOR ? OS::MTLBOS_iOSSimulator : OS::MTLBOS_iOS;
  header.OSVersionMajor = 16;
  header.OSVersionMinor = 0;
#else
  header.Platform = Platform::MTLBPlatform_macOS;
  header.OS = OS::MTLBOS_macOS;
  header.OSVersionMajor = 14;
  header.OSVersionMinor = 4;
#endif

  // write to stream
  OS << value(header);
  OS << value(fn_count);
  OS << value((uint32_t)header.FunctionListSize);
  OS << function_def;
  OS.write("ENDT", 4); // extend header
  OS << public_metadata;
  OS << private_metadata;
  OS << bitcode;
}

} // namespace dxmt::metallib