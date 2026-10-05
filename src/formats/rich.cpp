#include "formats/rich.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>

namespace decomp::pe {

namespace {

using T = RichTool;
using R = VsRelease;

// Microsoft's product ids, by number. Names follow the enumeration ("Utc" is the C/C++ compiler back
// end; "_Std" and "_Book" are the Standard and Introductory editions, which have no optimizer; "p" marks
// prerelease tools; "LTCG" link-time code generation; "POGO_I"/"POGO_O" profile-guided instrumentation
// and optimization; "CVTCIL" objects converted to CIL). The numbering is checked against what cl.exe
// and link.exe write (tests/corpus/build_corpus.ps1): 0x0102 for the linker, 0x0104 for the C compiler.
constexpr std::array kProducts = {
    RichProduct{0x0000, "Unknown", T::unmarked, 0, 0, R::unknown, "", ""},
    RichProduct{0x0001, "Import0", T::imports, 0, 0, R::unknown, "", ""},
    RichProduct{0x0002, "Linker510", T::linker, 5, 10, R::vc5, "", ""},
    RichProduct{0x0003, "Cvtomf510", T::cvtomf, 5, 10, R::vc5, "", ""},
    RichProduct{0x0004, "Linker600", T::linker, 6, 0, R::vc6, "", ""},
    RichProduct{0x0005, "Cvtomf600", T::cvtomf, 6, 0, R::vc6, "", ""},
    RichProduct{0x0006, "Cvtres500", T::resources, 5, 0, R::vc6, "", ""},
    RichProduct{0x0007, "Utc11_Basic", T::compiler, 11, 0, R::vc5, "Basic", ""},
    RichProduct{0x0008, "Utc11_C", T::compiler, 11, 0, R::vc5, "C", ""},
    RichProduct{0x0009, "Utc12_Basic", T::compiler, 12, 0, R::vc6, "Basic", ""},
    RichProduct{0x000a, "Utc12_C", T::compiler, 12, 0, R::vc6, "C", ""},
    RichProduct{0x000b, "Utc12_CPP", T::compiler, 12, 0, R::vc6, "C++", ""},
    RichProduct{0x000c, "AliasObj60", T::alias_object, 6, 0, R::vc6, "", ""},
    RichProduct{0x000d, "VisualBasic60", T::compiler, 6, 0, R::vc6, "Basic", ""},
    RichProduct{0x000e, "Masm613", T::assembler, 6, 13, R::vc6, "", ""},
    RichProduct{0x000f, "Masm710", T::assembler, 7, 10, R::vs2003, "", ""},
    RichProduct{0x0010, "Linker511", T::linker, 5, 11, R::vc5, "", ""},
    RichProduct{0x0011, "Cvtomf511", T::cvtomf, 5, 11, R::vc5, "", ""},
    RichProduct{0x0012, "Masm614", T::assembler, 6, 14, R::vc6, "", ""},
    RichProduct{0x0013, "Linker512", T::linker, 5, 12, R::vc5, "", ""},
    RichProduct{0x0014, "Cvtomf512", T::cvtomf, 5, 12, R::vc5, "", ""},
    RichProduct{0x0015, "Utc12_C_Std", T::compiler, 12, 0, R::vc6, "C", "Standard edition"},
    RichProduct{0x0016, "Utc12_CPP_Std", T::compiler, 12, 0, R::vc6, "C++", "Standard edition"},
    RichProduct{0x0017, "Utc12_C_Book", T::compiler, 12, 0, R::vc6, "C", "Introductory edition"},
    RichProduct{0x0018, "Utc12_CPP_Book", T::compiler, 12, 0, R::vc6, "C++", "Introductory edition"},
    RichProduct{0x0019, "Implib700", T::import_library, 7, 0, R::vs2002, "", ""},
    RichProduct{0x001a, "Cvtomf700", T::cvtomf, 7, 0, R::vs2002, "", ""},
    RichProduct{0x001b, "Utc13_Basic", T::compiler, 13, 0, R::vs2002, "Basic", ""},
    RichProduct{0x001c, "Utc13_C", T::compiler, 13, 0, R::vs2002, "C", ""},
    RichProduct{0x001d, "Utc13_CPP", T::compiler, 13, 0, R::vs2002, "C++", ""},
    RichProduct{0x001e, "Linker610", T::linker, 6, 10, R::vc6, "", ""},
    RichProduct{0x001f, "Cvtomf610", T::cvtomf, 6, 10, R::vc6, "", ""},
    RichProduct{0x0020, "Linker601", T::linker, 6, 1, R::vc6, "", ""},
    RichProduct{0x0021, "Cvtomf601", T::cvtomf, 6, 1, R::vc6, "", ""},
    RichProduct{0x0022, "Utc12_1_Basic", T::compiler, 12, 10, R::vc6, "Basic", ""},
    RichProduct{0x0023, "Utc12_1_C", T::compiler, 12, 10, R::vc6, "C", ""},
    RichProduct{0x0024, "Utc12_1_CPP", T::compiler, 12, 10, R::vc6, "C++", ""},
    RichProduct{0x0025, "Linker620", T::linker, 6, 20, R::vc6, "", ""},
    RichProduct{0x0026, "Cvtomf620", T::cvtomf, 6, 20, R::vc6, "", ""},
    RichProduct{0x0027, "AliasObj70", T::alias_object, 7, 0, R::vs2002, "", ""},
    RichProduct{0x0028, "Linker621", T::linker, 6, 21, R::vc6, "", ""},
    RichProduct{0x0029, "Cvtomf621", T::cvtomf, 6, 21, R::vc6, "", ""},
    RichProduct{0x002a, "Masm615", T::assembler, 6, 15, R::vc6, "", ""},
    RichProduct{0x002b, "Utc13_LTCG_C", T::compiler, 13, 0, R::vs2002, "C", "LTCG"},
    RichProduct{0x002c, "Utc13_LTCG_CPP", T::compiler, 13, 0, R::vs2002, "C++", "LTCG"},
    RichProduct{0x002d, "Masm620", T::assembler, 6, 20, R::vc6, "", ""},
    RichProduct{0x002e, "ILAsm100", T::ilasm, 1, 0, R::unknown, "", ""},
    RichProduct{0x002f, "Utc12_2_Basic", T::compiler, 12, 20, R::vc6, "Basic", ""},
    RichProduct{0x0030, "Utc12_2_C", T::compiler, 12, 20, R::vc6, "C", ""},
    RichProduct{0x0031, "Utc12_2_CPP", T::compiler, 12, 20, R::vc6, "C++", ""},
    RichProduct{0x0032, "Utc12_2_C_Std", T::compiler, 12, 20, R::vc6, "C", "Standard edition"},
    RichProduct{0x0033, "Utc12_2_CPP_Std", T::compiler, 12, 20, R::vc6, "C++", "Standard edition"},
    RichProduct{0x0034, "Utc12_2_C_Book", T::compiler, 12, 20, R::vc6, "C", "Introductory edition"},
    RichProduct{0x0035, "Utc12_2_CPP_Book", T::compiler, 12, 20, R::vc6, "C++", "Introductory edition"},
    RichProduct{0x0036, "Implib622", T::import_library, 6, 22, R::vc6, "", ""},
    RichProduct{0x0037, "Cvtomf622", T::cvtomf, 6, 22, R::vc6, "", ""},
    RichProduct{0x0038, "Cvtres501", T::resources, 5, 1, R::vc6, "", ""},
    RichProduct{0x0039, "Utc13_C_Std", T::compiler, 13, 0, R::vs2002, "C", "Standard edition"},
    RichProduct{0x003a, "Utc13_CPP_Std", T::compiler, 13, 0, R::vs2002, "C++", "Standard edition"},
    RichProduct{0x003b, "Cvtpgd1300", T::pgo_converter, 13, 0, R::vs2002, "", ""},
    RichProduct{0x003c, "Linker622", T::linker, 6, 22, R::vc6, "", ""},
    RichProduct{0x003d, "Linker700", T::linker, 7, 0, R::vs2002, "", ""},
    RichProduct{0x003e, "Export622", T::exports, 6, 22, R::vc6, "", ""},
    RichProduct{0x003f, "Export700", T::exports, 7, 0, R::vs2002, "", ""},
    RichProduct{0x0040, "Masm700", T::assembler, 7, 0, R::vs2002, "", ""},
    RichProduct{0x0041, "Utc13_POGO_I_C", T::compiler, 13, 0, R::vs2002, "C", "PGO instrumented"},
    RichProduct{0x0042, "Utc13_POGO_I_CPP", T::compiler, 13, 0, R::vs2002, "C++", "PGO instrumented"},
    RichProduct{0x0043, "Utc13_POGO_O_C", T::compiler, 13, 0, R::vs2002, "C", "PGO optimized"},
    RichProduct{0x0044, "Utc13_POGO_O_CPP", T::compiler, 13, 0, R::vs2002, "C++", "PGO optimized"},
    RichProduct{0x0045, "Cvtres700", T::resources, 7, 0, R::vs2002, "", ""},
    RichProduct{0x0046, "Cvtres710p", T::resources, 7, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x0047, "Linker710p", T::linker, 7, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x0048, "Cvtomf710p", T::cvtomf, 7, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x0049, "Export710p", T::exports, 7, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x004a, "Implib710p", T::import_library, 7, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x004b, "Masm710p", T::assembler, 7, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x004c, "Utc1310p_C", T::compiler, 13, 10, R::vs2003, "C", "prerelease"},
    RichProduct{0x004d, "Utc1310p_CPP", T::compiler, 13, 10, R::vs2003, "C++", "prerelease"},
    RichProduct{0x004e, "Utc1310p_C_Std", T::compiler, 13, 10, R::vs2003, "C", "prerelease, Standard edition"},
    RichProduct{0x004f, "Utc1310p_CPP_Std", T::compiler, 13, 10, R::vs2003, "C++", "prerelease, Standard edition"},
    RichProduct{0x0050, "Utc1310p_LTCG_C", T::compiler, 13, 10, R::vs2003, "C", "prerelease, LTCG"},
    RichProduct{0x0051, "Utc1310p_LTCG_CPP", T::compiler, 13, 10, R::vs2003, "C++", "prerelease, LTCG"},
    RichProduct{0x0052, "Utc1310p_POGO_I_C", T::compiler, 13, 10, R::vs2003, "C", "prerelease, PGO instrumented"},
    RichProduct{0x0053, "Utc1310p_POGO_I_CPP", T::compiler, 13, 10, R::vs2003, "C++", "prerelease, PGO instrumented"},
    RichProduct{0x0054, "Utc1310p_POGO_O_C", T::compiler, 13, 10, R::vs2003, "C", "prerelease, PGO optimized"},
    RichProduct{0x0055, "Utc1310p_POGO_O_CPP", T::compiler, 13, 10, R::vs2003, "C++", "prerelease, PGO optimized"},
    RichProduct{0x0056, "Linker624", T::linker, 6, 24, R::vc6, "", ""},
    RichProduct{0x0057, "Cvtomf624", T::cvtomf, 6, 24, R::vc6, "", ""},
    RichProduct{0x0058, "Export624", T::exports, 6, 24, R::vc6, "", ""},
    RichProduct{0x0059, "Implib624", T::import_library, 6, 24, R::vc6, "", ""},
    RichProduct{0x005a, "Linker710", T::linker, 7, 10, R::vs2003, "", ""},
    RichProduct{0x005b, "Cvtomf710", T::cvtomf, 7, 10, R::vs2003, "", ""},
    RichProduct{0x005c, "Export710", T::exports, 7, 10, R::vs2003, "", ""},
    RichProduct{0x005d, "Implib710", T::import_library, 7, 10, R::vs2003, "", ""},
    RichProduct{0x005e, "Cvtres710", T::resources, 7, 10, R::vs2003, "", ""},
    RichProduct{0x005f, "Utc1310_C", T::compiler, 13, 10, R::vs2003, "C", ""},
    RichProduct{0x0060, "Utc1310_CPP", T::compiler, 13, 10, R::vs2003, "C++", ""},
    RichProduct{0x0061, "Utc1310_C_Std", T::compiler, 13, 10, R::vs2003, "C", "Standard edition"},
    RichProduct{0x0062, "Utc1310_CPP_Std", T::compiler, 13, 10, R::vs2003, "C++", "Standard edition"},
    RichProduct{0x0063, "Utc1310_LTCG_C", T::compiler, 13, 10, R::vs2003, "C", "LTCG"},
    RichProduct{0x0064, "Utc1310_LTCG_CPP", T::compiler, 13, 10, R::vs2003, "C++", "LTCG"},
    RichProduct{0x0065, "Utc1310_POGO_I_C", T::compiler, 13, 10, R::vs2003, "C", "PGO instrumented"},
    RichProduct{0x0066, "Utc1310_POGO_I_CPP", T::compiler, 13, 10, R::vs2003, "C++", "PGO instrumented"},
    RichProduct{0x0067, "Utc1310_POGO_O_C", T::compiler, 13, 10, R::vs2003, "C", "PGO optimized"},
    RichProduct{0x0068, "Utc1310_POGO_O_CPP", T::compiler, 13, 10, R::vs2003, "C++", "PGO optimized"},
    RichProduct{0x0069, "AliasObj710", T::alias_object, 7, 10, R::vs2003, "", ""},
    RichProduct{0x006a, "AliasObj710p", T::alias_object, 7, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x006b, "Cvtpgd1310", T::pgo_converter, 13, 10, R::vs2003, "", ""},
    RichProduct{0x006c, "Cvtpgd1310p", T::pgo_converter, 13, 10, R::vs2003, "", "prerelease"},
    RichProduct{0x006d, "Utc1400_C", T::compiler, 14, 0, R::vs2005, "C", ""},
    RichProduct{0x006e, "Utc1400_CPP", T::compiler, 14, 0, R::vs2005, "C++", ""},
    RichProduct{0x006f, "Utc1400_C_Std", T::compiler, 14, 0, R::vs2005, "C", "Standard edition"},
    RichProduct{0x0070, "Utc1400_CPP_Std", T::compiler, 14, 0, R::vs2005, "C++", "Standard edition"},
    RichProduct{0x0071, "Utc1400_LTCG_C", T::compiler, 14, 0, R::vs2005, "C", "LTCG"},
    RichProduct{0x0072, "Utc1400_LTCG_CPP", T::compiler, 14, 0, R::vs2005, "C++", "LTCG"},
    RichProduct{0x0073, "Utc1400_POGO_I_C", T::compiler, 14, 0, R::vs2005, "C", "PGO instrumented"},
    RichProduct{0x0074, "Utc1400_POGO_I_CPP", T::compiler, 14, 0, R::vs2005, "C++", "PGO instrumented"},
    RichProduct{0x0075, "Utc1400_POGO_O_C", T::compiler, 14, 0, R::vs2005, "C", "PGO optimized"},
    RichProduct{0x0076, "Utc1400_POGO_O_CPP", T::compiler, 14, 0, R::vs2005, "C++", "PGO optimized"},
    RichProduct{0x0077, "Cvtpgd1400", T::pgo_converter, 14, 0, R::vs2005, "", ""},
    RichProduct{0x0078, "Linker800", T::linker, 8, 0, R::vs2005, "", ""},
    RichProduct{0x0079, "Cvtomf800", T::cvtomf, 8, 0, R::vs2005, "", ""},
    RichProduct{0x007a, "Export800", T::exports, 8, 0, R::vs2005, "", ""},
    RichProduct{0x007b, "Implib800", T::import_library, 8, 0, R::vs2005, "", ""},
    RichProduct{0x007c, "Cvtres800", T::resources, 8, 0, R::vs2005, "", ""},
    RichProduct{0x007d, "Masm800", T::assembler, 8, 0, R::vs2005, "", ""},
    RichProduct{0x007e, "AliasObj800", T::alias_object, 8, 0, R::vs2005, "", ""},
    RichProduct{0x007f, "PhoenixPrerelease", T::unknown, 0, 0, R::unknown, "", "prerelease"},
    RichProduct{0x0080, "Utc1400_CVTCIL_C", T::compiler, 14, 0, R::vs2005, "C", "CIL"},
    RichProduct{0x0081, "Utc1400_CVTCIL_CPP", T::compiler, 14, 0, R::vs2005, "C++", "CIL"},
    RichProduct{0x0082, "Utc1400_LTCG_MSIL", T::compiler, 14, 0, R::vs2005, "MSIL", "LTCG"},
    RichProduct{0x0083, "Utc1500_C", T::compiler, 15, 0, R::vs2008, "C", ""},
    RichProduct{0x0084, "Utc1500_CPP", T::compiler, 15, 0, R::vs2008, "C++", ""},
    RichProduct{0x0085, "Utc1500_C_Std", T::compiler, 15, 0, R::vs2008, "C", "Standard edition"},
    RichProduct{0x0086, "Utc1500_CPP_Std", T::compiler, 15, 0, R::vs2008, "C++", "Standard edition"},
    RichProduct{0x0087, "Utc1500_CVTCIL_C", T::compiler, 15, 0, R::vs2008, "C", "CIL"},
    RichProduct{0x0088, "Utc1500_CVTCIL_CPP", T::compiler, 15, 0, R::vs2008, "C++", "CIL"},
    RichProduct{0x0089, "Utc1500_LTCG_C", T::compiler, 15, 0, R::vs2008, "C", "LTCG"},
    RichProduct{0x008a, "Utc1500_LTCG_CPP", T::compiler, 15, 0, R::vs2008, "C++", "LTCG"},
    RichProduct{0x008b, "Utc1500_LTCG_MSIL", T::compiler, 15, 0, R::vs2008, "MSIL", "LTCG"},
    RichProduct{0x008c, "Utc1500_POGO_I_C", T::compiler, 15, 0, R::vs2008, "C", "PGO instrumented"},
    RichProduct{0x008d, "Utc1500_POGO_I_CPP", T::compiler, 15, 0, R::vs2008, "C++", "PGO instrumented"},
    RichProduct{0x008e, "Utc1500_POGO_O_C", T::compiler, 15, 0, R::vs2008, "C", "PGO optimized"},
    RichProduct{0x008f, "Utc1500_POGO_O_CPP", T::compiler, 15, 0, R::vs2008, "C++", "PGO optimized"},
    RichProduct{0x0090, "Cvtpgd1500", T::pgo_converter, 15, 0, R::vs2008, "", ""},
    RichProduct{0x0091, "Linker900", T::linker, 9, 0, R::vs2008, "", ""},
    RichProduct{0x0092, "Export900", T::exports, 9, 0, R::vs2008, "", ""},
    RichProduct{0x0093, "Implib900", T::import_library, 9, 0, R::vs2008, "", ""},
    RichProduct{0x0094, "Cvtres900", T::resources, 9, 0, R::vs2008, "", ""},
    RichProduct{0x0095, "Masm900", T::assembler, 9, 0, R::vs2008, "", ""},
    RichProduct{0x0096, "AliasObj900", T::alias_object, 9, 0, R::vs2008, "", ""},
    RichProduct{0x0097, "Resource", T::resources, 0, 0, R::unknown, "", ""},
    RichProduct{0x0098, "AliasObj1000", T::alias_object, 10, 0, R::vs2010, "", ""},
    RichProduct{0x0099, "Cvtpgd1600", T::pgo_converter, 16, 0, R::vs2010, "", ""},
    RichProduct{0x009a, "Cvtres1000", T::resources, 10, 0, R::vs2010, "", ""},
    RichProduct{0x009b, "Export1000", T::exports, 10, 0, R::vs2010, "", ""},
    RichProduct{0x009c, "Implib1000", T::import_library, 10, 0, R::vs2010, "", ""},
    RichProduct{0x009d, "Linker1000", T::linker, 10, 0, R::vs2010, "", ""},
    RichProduct{0x009e, "Masm1000", T::assembler, 10, 0, R::vs2010, "", ""},
    RichProduct{0x009f, "Phx1600_C", T::compiler, 16, 0, R::vs2010, "C", "Phoenix"},
    RichProduct{0x00a0, "Phx1600_CPP", T::compiler, 16, 0, R::vs2010, "C++", "Phoenix"},
    RichProduct{0x00a1, "Phx1600_CVTCIL_C", T::compiler, 16, 0, R::vs2010, "C", "Phoenix, CIL"},
    RichProduct{0x00a2, "Phx1600_CVTCIL_CPP", T::compiler, 16, 0, R::vs2010, "C++", "Phoenix, CIL"},
    RichProduct{0x00a3, "Phx1600_LTCG_C", T::compiler, 16, 0, R::vs2010, "C", "Phoenix, LTCG"},
    RichProduct{0x00a4, "Phx1600_LTCG_CPP", T::compiler, 16, 0, R::vs2010, "C++", "Phoenix, LTCG"},
    RichProduct{0x00a5, "Phx1600_LTCG_MSIL", T::compiler, 16, 0, R::vs2010, "MSIL", "Phoenix, LTCG"},
    RichProduct{0x00a6, "Phx1600_POGO_I_C", T::compiler, 16, 0, R::vs2010, "C", "Phoenix, PGO instrumented"},
    RichProduct{0x00a7, "Phx1600_POGO_I_CPP", T::compiler, 16, 0, R::vs2010, "C++", "Phoenix, PGO instrumented"},
    RichProduct{0x00a8, "Phx1600_POGO_O_C", T::compiler, 16, 0, R::vs2010, "C", "Phoenix, PGO optimized"},
    RichProduct{0x00a9, "Phx1600_POGO_O_CPP", T::compiler, 16, 0, R::vs2010, "C++", "Phoenix, PGO optimized"},
    RichProduct{0x00aa, "Utc1600_C", T::compiler, 16, 0, R::vs2010, "C", ""},
    RichProduct{0x00ab, "Utc1600_CPP", T::compiler, 16, 0, R::vs2010, "C++", ""},
    RichProduct{0x00ac, "Utc1600_CVTCIL_C", T::compiler, 16, 0, R::vs2010, "C", "CIL"},
    RichProduct{0x00ad, "Utc1600_CVTCIL_CPP", T::compiler, 16, 0, R::vs2010, "C++", "CIL"},
    RichProduct{0x00ae, "Utc1600_LTCG_C", T::compiler, 16, 0, R::vs2010, "C", "LTCG"},
    RichProduct{0x00af, "Utc1600_LTCG_CPP", T::compiler, 16, 0, R::vs2010, "C++", "LTCG"},
    RichProduct{0x00b0, "Utc1600_LTCG_MSIL", T::compiler, 16, 0, R::vs2010, "MSIL", "LTCG"},
    RichProduct{0x00b1, "Utc1600_POGO_I_C", T::compiler, 16, 0, R::vs2010, "C", "PGO instrumented"},
    RichProduct{0x00b2, "Utc1600_POGO_I_CPP", T::compiler, 16, 0, R::vs2010, "C++", "PGO instrumented"},
    RichProduct{0x00b3, "Utc1600_POGO_O_C", T::compiler, 16, 0, R::vs2010, "C", "PGO optimized"},
    RichProduct{0x00b4, "Utc1600_POGO_O_CPP", T::compiler, 16, 0, R::vs2010, "C++", "PGO optimized"},
    RichProduct{0x00b5, "AliasObj1010", T::alias_object, 10, 10, R::vs2010, "", ""},
    RichProduct{0x00b6, "Cvtpgd1610", T::pgo_converter, 16, 10, R::vs2010, "", ""},
    RichProduct{0x00b7, "Cvtres1010", T::resources, 10, 10, R::vs2010, "", ""},
    RichProduct{0x00b8, "Export1010", T::exports, 10, 10, R::vs2010, "", ""},
    RichProduct{0x00b9, "Implib1010", T::import_library, 10, 10, R::vs2010, "", ""},
    RichProduct{0x00ba, "Linker1010", T::linker, 10, 10, R::vs2010, "", ""},
    RichProduct{0x00bb, "Masm1010", T::assembler, 10, 10, R::vs2010, "", ""},
    RichProduct{0x00bc, "Utc1610_C", T::compiler, 16, 10, R::vs2010, "C", ""},
    RichProduct{0x00bd, "Utc1610_CPP", T::compiler, 16, 10, R::vs2010, "C++", ""},
    RichProduct{0x00be, "Utc1610_CVTCIL_C", T::compiler, 16, 10, R::vs2010, "C", "CIL"},
    RichProduct{0x00bf, "Utc1610_CVTCIL_CPP", T::compiler, 16, 10, R::vs2010, "C++", "CIL"},
    RichProduct{0x00c0, "Utc1610_LTCG_C", T::compiler, 16, 10, R::vs2010, "C", "LTCG"},
    RichProduct{0x00c1, "Utc1610_LTCG_CPP", T::compiler, 16, 10, R::vs2010, "C++", "LTCG"},
    RichProduct{0x00c2, "Utc1610_LTCG_MSIL", T::compiler, 16, 10, R::vs2010, "MSIL", "LTCG"},
    RichProduct{0x00c3, "Utc1610_POGO_I_C", T::compiler, 16, 10, R::vs2010, "C", "PGO instrumented"},
    RichProduct{0x00c4, "Utc1610_POGO_I_CPP", T::compiler, 16, 10, R::vs2010, "C++", "PGO instrumented"},
    RichProduct{0x00c5, "Utc1610_POGO_O_C", T::compiler, 16, 10, R::vs2010, "C", "PGO optimized"},
    RichProduct{0x00c6, "Utc1610_POGO_O_CPP", T::compiler, 16, 10, R::vs2010, "C++", "PGO optimized"},
    RichProduct{0x00c7, "AliasObj1100", T::alias_object, 11, 0, R::vs2012, "", ""},
    RichProduct{0x00c8, "Cvtpgd1700", T::pgo_converter, 17, 0, R::vs2012, "", ""},
    RichProduct{0x00c9, "Cvtres1100", T::resources, 11, 0, R::vs2012, "", ""},
    RichProduct{0x00ca, "Export1100", T::exports, 11, 0, R::vs2012, "", ""},
    RichProduct{0x00cb, "Implib1100", T::import_library, 11, 0, R::vs2012, "", ""},
    RichProduct{0x00cc, "Linker1100", T::linker, 11, 0, R::vs2012, "", ""},
    RichProduct{0x00cd, "Masm1100", T::assembler, 11, 0, R::vs2012, "", ""},
    RichProduct{0x00ce, "Utc1700_C", T::compiler, 17, 0, R::vs2012, "C", ""},
    RichProduct{0x00cf, "Utc1700_CPP", T::compiler, 17, 0, R::vs2012, "C++", ""},
    RichProduct{0x00d0, "Utc1700_CVTCIL_C", T::compiler, 17, 0, R::vs2012, "C", "CIL"},
    RichProduct{0x00d1, "Utc1700_CVTCIL_CPP", T::compiler, 17, 0, R::vs2012, "C++", "CIL"},
    RichProduct{0x00d2, "Utc1700_LTCG_C", T::compiler, 17, 0, R::vs2012, "C", "LTCG"},
    RichProduct{0x00d3, "Utc1700_LTCG_CPP", T::compiler, 17, 0, R::vs2012, "C++", "LTCG"},
    RichProduct{0x00d4, "Utc1700_LTCG_MSIL", T::compiler, 17, 0, R::vs2012, "MSIL", "LTCG"},
    RichProduct{0x00d5, "Utc1700_POGO_I_C", T::compiler, 17, 0, R::vs2012, "C", "PGO instrumented"},
    RichProduct{0x00d6, "Utc1700_POGO_I_CPP", T::compiler, 17, 0, R::vs2012, "C++", "PGO instrumented"},
    RichProduct{0x00d7, "Utc1700_POGO_O_C", T::compiler, 17, 0, R::vs2012, "C", "PGO optimized"},
    RichProduct{0x00d8, "Utc1700_POGO_O_CPP", T::compiler, 17, 0, R::vs2012, "C++", "PGO optimized"},
    RichProduct{0x00d9, "AliasObj1200", T::alias_object, 12, 0, R::vs2013, "", ""},
    RichProduct{0x00da, "Cvtpgd1800", T::pgo_converter, 18, 0, R::vs2013, "", ""},
    RichProduct{0x00db, "Cvtres1200", T::resources, 12, 0, R::vs2013, "", ""},
    RichProduct{0x00dc, "Export1200", T::exports, 12, 0, R::vs2013, "", ""},
    RichProduct{0x00dd, "Implib1200", T::import_library, 12, 0, R::vs2013, "", ""},
    RichProduct{0x00de, "Linker1200", T::linker, 12, 0, R::vs2013, "", ""},
    RichProduct{0x00df, "Masm1200", T::assembler, 12, 0, R::vs2013, "", ""},
    RichProduct{0x00e0, "Utc1800_C", T::compiler, 18, 0, R::vs2013, "C", ""},
    RichProduct{0x00e1, "Utc1800_CPP", T::compiler, 18, 0, R::vs2013, "C++", ""},
    RichProduct{0x00e2, "Utc1800_CVTCIL_C", T::compiler, 18, 0, R::vs2013, "C", "CIL"},
    RichProduct{0x00e3, "Utc1800_CVTCIL_CPP", T::compiler, 18, 0, R::vs2013, "C++", "CIL"},
    RichProduct{0x00e4, "Utc1800_LTCG_C", T::compiler, 18, 0, R::vs2013, "C", "LTCG"},
    RichProduct{0x00e5, "Utc1800_LTCG_CPP", T::compiler, 18, 0, R::vs2013, "C++", "LTCG"},
    RichProduct{0x00e6, "Utc1800_LTCG_MSIL", T::compiler, 18, 0, R::vs2013, "MSIL", "LTCG"},
    RichProduct{0x00e7, "Utc1800_POGO_I_C", T::compiler, 18, 0, R::vs2013, "C", "PGO instrumented"},
    RichProduct{0x00e8, "Utc1800_POGO_I_CPP", T::compiler, 18, 0, R::vs2013, "C++", "PGO instrumented"},
    RichProduct{0x00e9, "Utc1800_POGO_O_C", T::compiler, 18, 0, R::vs2013, "C", "PGO optimized"},
    RichProduct{0x00ea, "Utc1800_POGO_O_CPP", T::compiler, 18, 0, R::vs2013, "C++", "PGO optimized"},
    RichProduct{0x00eb, "AliasObj1210", T::alias_object, 12, 10, R::vs2013, "", ""},
    RichProduct{0x00ec, "Cvtpgd1810", T::pgo_converter, 18, 10, R::vs2013, "", ""},
    RichProduct{0x00ed, "Cvtres1210", T::resources, 12, 10, R::vs2013, "", ""},
    RichProduct{0x00ee, "Export1210", T::exports, 12, 10, R::vs2013, "", ""},
    RichProduct{0x00ef, "Implib1210", T::import_library, 12, 10, R::vs2013, "", ""},
    RichProduct{0x00f0, "Linker1210", T::linker, 12, 10, R::vs2013, "", ""},
    RichProduct{0x00f1, "Masm1210", T::assembler, 12, 10, R::vs2013, "", ""},
    RichProduct{0x00f2, "Utc1810_C", T::compiler, 18, 10, R::vs2013, "C", ""},
    RichProduct{0x00f3, "Utc1810_CPP", T::compiler, 18, 10, R::vs2013, "C++", ""},
    RichProduct{0x00f4, "Utc1810_CVTCIL_C", T::compiler, 18, 10, R::vs2013, "C", "CIL"},
    RichProduct{0x00f5, "Utc1810_CVTCIL_CPP", T::compiler, 18, 10, R::vs2013, "C++", "CIL"},
    RichProduct{0x00f6, "Utc1810_LTCG_C", T::compiler, 18, 10, R::vs2013, "C", "LTCG"},
    RichProduct{0x00f7, "Utc1810_LTCG_CPP", T::compiler, 18, 10, R::vs2013, "C++", "LTCG"},
    RichProduct{0x00f8, "Utc1810_LTCG_MSIL", T::compiler, 18, 10, R::vs2013, "MSIL", "LTCG"},
    RichProduct{0x00f9, "Utc1810_POGO_I_C", T::compiler, 18, 10, R::vs2013, "C", "PGO instrumented"},
    RichProduct{0x00fa, "Utc1810_POGO_I_CPP", T::compiler, 18, 10, R::vs2013, "C++", "PGO instrumented"},
    RichProduct{0x00fb, "Utc1810_POGO_O_C", T::compiler, 18, 10, R::vs2013, "C", "PGO optimized"},
    RichProduct{0x00fc, "Utc1810_POGO_O_CPP", T::compiler, 18, 10, R::vs2013, "C++", "PGO optimized"},
    RichProduct{0x00fd, "AliasObj1400", T::alias_object, 14, 0, R::vs2015_or_later, "", ""},
    RichProduct{0x00fe, "Cvtpgd1900", T::pgo_converter, 19, 0, R::vs2015_or_later, "", ""},
    RichProduct{0x00ff, "Cvtres1400", T::resources, 14, 0, R::vs2015_or_later, "", ""},
    RichProduct{0x0100, "Export1400", T::exports, 14, 0, R::vs2015_or_later, "", ""},
    RichProduct{0x0101, "Implib1400", T::import_library, 14, 0, R::vs2015_or_later, "", ""},
    RichProduct{0x0102, "Linker1400", T::linker, 14, 0, R::vs2015_or_later, "", ""},
    RichProduct{0x0103, "Masm1400", T::assembler, 14, 0, R::vs2015_or_later, "", ""},
    RichProduct{0x0104, "Utc1900_C", T::compiler, 19, 0, R::vs2015_or_later, "C", ""},
    RichProduct{0x0105, "Utc1900_CPP", T::compiler, 19, 0, R::vs2015_or_later, "C++", ""},
    RichProduct{0x0106, "Utc1900_CVTCIL_C", T::compiler, 19, 0, R::vs2015_or_later, "C", "CIL"},
    RichProduct{0x0107, "Utc1900_CVTCIL_CPP", T::compiler, 19, 0, R::vs2015_or_later, "C++", "CIL"},
    RichProduct{0x0108, "Utc1900_LTCG_C", T::compiler, 19, 0, R::vs2015_or_later, "C", "LTCG"},
    RichProduct{0x0109, "Utc1900_LTCG_CPP", T::compiler, 19, 0, R::vs2015_or_later, "C++", "LTCG"},
    RichProduct{0x010a, "Utc1900_LTCG_MSIL", T::compiler, 19, 0, R::vs2015_or_later, "MSIL", "LTCG"},
    RichProduct{0x010b, "Utc1900_POGO_I_C", T::compiler, 19, 0, R::vs2015_or_later, "C", "PGO instrumented"},
    RichProduct{0x010c, "Utc1900_POGO_I_CPP", T::compiler, 19, 0, R::vs2015_or_later, "C++", "PGO instrumented"},
    RichProduct{0x010d, "Utc1900_POGO_O_C", T::compiler, 19, 0, R::vs2015_or_later, "C", "PGO optimized"},
    RichProduct{0x010e, "Utc1900_POGO_O_CPP", T::compiler, 19, 0, R::vs2015_or_later, "C++", "PGO optimized"},
};

static_assert(std::ranges::is_sorted(kProducts, {}, &RichProduct::id));

// Visual Studio 2015 and later share product ids; the build number tells the release apart. Each row
// is the first build of a toolset (compilers 19.xx and linkers 14.xx share the minor version xx).
struct VsBuild {
    u16 first_build;
    u8 minor;
    bool minor_known;
    std::string_view visual_studio;
    std::string_view name;
};
constexpr std::array kVsBuilds = {
    VsBuild{0, 0, true, "Visual Studio 2015", "vs2015"},
    VsBuild{23506, 0, true, "Visual Studio 2015 Update 1", "vs2015"},
    VsBuild{23918, 0, true, "Visual Studio 2015 Update 2", "vs2015"},
    VsBuild{24210, 0, true, "Visual Studio 2015 Update 3", "vs2015"},
    VsBuild{25017, 10, true, "Visual Studio 2017 15.0", "vs2017"},
    VsBuild{25506, 11, true, "Visual Studio 2017 15.3", "vs2017"},
    VsBuild{25830, 12, true, "Visual Studio 2017 15.5", "vs2017"},
    VsBuild{26128, 13, true, "Visual Studio 2017 15.6", "vs2017"},
    VsBuild{26428, 14, true, "Visual Studio 2017 15.7", "vs2017"},
    VsBuild{26726, 15, true, "Visual Studio 2017 15.8", "vs2017"},
    VsBuild{27023, 16, true, "Visual Studio 2017 15.9", "vs2017"},
    VsBuild{27508, 20, true, "Visual Studio 2019 16.0", "vs2019"},
    VsBuild{27702, 21, true, "Visual Studio 2019 16.1", "vs2019"},
    VsBuild{27905, 22, true, "Visual Studio 2019 16.2", "vs2019"},
    VsBuild{28105, 23, true, "Visual Studio 2019 16.3", "vs2019"},
    VsBuild{28314, 24, true, "Visual Studio 2019 16.4", "vs2019"},
    VsBuild{28610, 25, true, "Visual Studio 2019 16.5", "vs2019"},
    VsBuild{28805, 26, true, "Visual Studio 2019 16.6", "vs2019"},
    VsBuild{29110, 27, true, "Visual Studio 2019 16.7", "vs2019"},
    VsBuild{29333, 28, true, "Visual Studio 2019 16.8", "vs2019"},
    VsBuild{29910, 28, true, "Visual Studio 2019 16.9", "vs2019"},
    VsBuild{30037, 29, true, "Visual Studio 2019 16.10", "vs2019"},
    VsBuild{30133, 29, true, "Visual Studio 2019 16.11", "vs2019"},
    VsBuild{30705, 30, true, "Visual Studio 2022 17.0", "vs2022"},
    VsBuild{31104, 31, true, "Visual Studio 2022 17.1", "vs2022"},
    VsBuild{31326, 32, true, "Visual Studio 2022 17.2", "vs2022"},
    VsBuild{31629, 33, true, "Visual Studio 2022 17.3", "vs2022"},
    VsBuild{31933, 34, true, "Visual Studio 2022 17.4", "vs2022"},
    VsBuild{32215, 35, true, "Visual Studio 2022 17.5", "vs2022"},
    VsBuild{32532, 36, true, "Visual Studio 2022 17.6", "vs2022"},
    VsBuild{32822, 37, true, "Visual Studio 2022 17.7", "vs2022"},
    VsBuild{33130, 38, true, "Visual Studio 2022 17.8", "vs2022"},
    VsBuild{33519, 39, true, "Visual Studio 2022 17.9", "vs2022"},
    VsBuild{33808, 40, true, "Visual Studio 2022 17.10", "vs2022"},
    VsBuild{34120, 41, true, "Visual Studio 2022 17.11", "vs2022"},
    VsBuild{34433, 42, true, "Visual Studio 2022 17.12", "vs2022"},
    VsBuild{34808, 43, true, "Visual Studio 2022 17.13", "vs2022"},
    VsBuild{35207, 44, true, "Visual Studio 2022 17.14", "vs2022"},
    // Toolsets 14.5x: which minor a build belongs to is not tabled; the image's linker version says.
    VsBuild{35500, 50, false, "Visual Studio 2026", "vs2026"},
};

const VsBuild& vs_build(u16 build) {
    auto it = std::ranges::upper_bound(kVsBuilds, build, {}, &VsBuild::first_build);
    return *std::prev(it);
}

std::string_view suggested_name(VsRelease release) {
    switch (release) {
    case VsRelease::vc5: return "vc5";
    case VsRelease::vc6: return "vc6";
    case VsRelease::vs2002: return "vs2002";
    case VsRelease::vs2003: return "vs2003";
    case VsRelease::vs2005: return "vs2005";
    case VsRelease::vs2008: return "vs2008";
    case VsRelease::vs2010: return "vs2010";
    case VsRelease::vs2012: return "vs2012";
    case VsRelease::vs2013: return "vs2013";
    case VsRelease::vs2015_or_later:
    case VsRelease::unknown: break;
    }
    return "";
}

u32 rol(u32 value, u32 count) {
    count &= 31;
    return count ? (value << count) | (value >> (32 - count)) : value;
}

std::string tool_noun(const RichProduct& p) {
    switch (p.tool) {
    case RichTool::compiler: return std::string(p.language) + " compiler";
    case RichTool::assembler: return "MASM";
    case RichTool::resources: return "resource converter";
    case RichTool::exports: return "export file";
    case RichTool::import_library: return "import library";
    case RichTool::alias_object: return "alias object";
    case RichTool::cvtomf: return "OMF converter";
    case RichTool::pgo_converter: return "PGO converter";
    default: return std::string(to_string(p.tool));
    }
}

} // namespace

std::optional<RichHeader> parse_rich_header(ByteSpan d, u32 pe_offset) {
    // "Rich" and the key follow the entries; the XOR-ed "DanS" marker precedes them.
    for (u32 pos = 0x40; pos + 8 <= pe_offset && pos + 8 <= d.size(); pos += 4) {
        if (read_le<u32>(d, pos) != 0x68636952u) continue;  // "Rich"
        const u32 key = read_le<u32>(d, pos + 4).value_or(0);
        for (u32 start = pos; start >= 0x44; start -= 4) {
            if ((read_le<u32>(d, start - 4).value_or(0) ^ key) != 0x536E6144u) continue;  // "DanS"
            RichHeader h;
            h.offset = start - 4;
            h.key = key;
            for (u32 e = h.offset + 16; e + 8 <= pos; e += 8) {  // after DanS and three zero dwords
                const u32 comp_id = read_le<u32>(d, e).value_or(0) ^ key;
                const u32 count = read_le<u32>(d, e + 4).value_or(0) ^ key;
                h.entries.push_back({static_cast<u16>(comp_id >> 16), static_cast<u16>(comp_id & 0xFFFF), count});
            }
            h.checksum_ok = rich_checksum(d, h.offset, h.entries) == key;
            return h;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

u32 rich_checksum(ByteSpan d, u32 offset, const std::vector<RichEntry>& entries) {
    u32 sum = offset;
    for (u32 i = 0; i < offset && i < d.size(); ++i) {
        if (i >= 0x3C && i < 0x40) continue;  // e_lfanew
        sum += rol(static_cast<u32>(d[i]), i);
    }
    for (const auto& e : entries) sum += rol((static_cast<u32>(e.product_id) << 16) | e.build, e.count);
    return sum;
}

std::string_view to_string(RichTool tool) {
    switch (tool) {
    case RichTool::unknown: return "unknown";
    case RichTool::unmarked: return "unmarked";
    case RichTool::imports: return "imports";
    case RichTool::compiler: return "compiler";
    case RichTool::linker: return "linker";
    case RichTool::assembler: return "assembler";
    case RichTool::resources: return "resources";
    case RichTool::exports: return "exports";
    case RichTool::import_library: return "import_library";
    case RichTool::alias_object: return "alias_object";
    case RichTool::cvtomf: return "cvtomf";
    case RichTool::pgo_converter: return "pgo_converter";
    case RichTool::ilasm: return "ilasm";
    }
    return "unknown";
}

std::string_view to_string(VsRelease release) {
    switch (release) {
    case VsRelease::unknown: return "";
    case VsRelease::vc5: return "Visual C++ 5.0";
    case VsRelease::vc6: return "Visual C++ 6.0";
    case VsRelease::vs2002: return "Visual Studio .NET 2002";
    case VsRelease::vs2003: return "Visual Studio .NET 2003";
    case VsRelease::vs2005: return "Visual Studio 2005";
    case VsRelease::vs2008: return "Visual Studio 2008";
    case VsRelease::vs2010: return "Visual Studio 2010";
    case VsRelease::vs2012: return "Visual Studio 2012";
    case VsRelease::vs2013: return "Visual Studio 2013";
    case VsRelease::vs2015_or_later: return "Visual Studio 2015 or later";
    }
    return "";
}

const RichProduct* rich_product(u16 id) {
    auto it = std::ranges::lower_bound(kProducts, id, {}, &RichProduct::id);
    return it != kProducts.end() && it->id == id ? &*it : nullptr;
}

std::string describe_rich_product(u16 id) {
    const RichProduct* p = rich_product(id);
    if (!p) return std::format("product {:#06x}", id);
    if (p->tool == RichTool::unmarked) return "objects without a tool id";
    if (p->tool == RichTool::imports) return "imported functions";
    std::string out = std::format("{} {}.{:02}", tool_noun(*p), p->major, p->minor);
    if (!p->variant.empty()) out += std::format(" {}", p->variant);
    if (p->release != VsRelease::unknown) out += std::format(" ({})", to_string(p->release));
    return out;
}

std::string ToolVersion::text() const {
    if (!minor_known) return std::format("{}.x.{}", major, build);
    return std::format("{}.{:02}.{}", major, minor, build);
}

ToolVersion tool_version(const RichProduct& p, u16 build, std::optional<u8> toolset_minor) {
    ToolVersion v;
    v.major = p.major;
    v.minor = p.minor;
    v.build = build;
    if (p.release != VsRelease::vs2015_or_later) {
        v.visual_studio = std::string(to_string(p.release));
        v.suggested_name = std::string(suggested_name(p.release));
        return v;
    }
    const VsBuild& row = vs_build(build);
    v.visual_studio = std::string(row.visual_studio);
    v.suggested_name = std::string(row.name);
    if (toolset_minor) v.minor = *toolset_minor;
    else if (row.minor_known) v.minor = row.minor;
    else v.minor_known = false;
    return v;
}

std::string BuildTool::description() const {
    if (!product) return std::format("product {:#06x} build {}", entry.product_id, entry.build);
    if (product->tool == RichTool::unmarked) return "objects without a tool id";
    if (product->tool == RichTool::imports) return "imported functions";
    std::string out = std::format("{} {}", tool_noun(*product), version.text());
    if (!product->variant.empty()) out += std::format(" ({})", product->variant);
    return out;
}

const BuildTool* BuildInfo::main_compiler() const {
    // Releases at the granularity toolchains are named by: VS2015 and later share product ids.
    auto family = [](const BuildTool& t) -> std::string {
        if (!t.product) return {};
        if (t.product->release == VsRelease::vs2015_or_later) return t.version.suggested_name;
        return std::string(to_string(t.product->release));
    };
    std::vector<const BuildTool*> candidates;
    for (const auto& c : compilers)
        if (c.product && (c.product->language == "C" || c.product->language == "C++")) candidates.push_back(&c);
    if (candidates.empty())
        for (const auto& c : compilers) candidates.push_back(&c);
    if (candidates.empty()) return nullptr;
    // The release: the linker's when some compiler shares it, else the one with most objects.
    std::map<std::string, u64> objects;
    for (const BuildTool* c : candidates) objects[family(*c)] += c->entry.count;
    std::string release = std::ranges::max_element(objects, {}, [](const auto& kv) { return kv.second; })->first;
    if (linker && objects.contains(family(*linker))) release = family(*linker);
    const BuildTool* best = nullptr;
    for (const BuildTool* c : candidates) {
        if (family(*c) != release) continue;
        if (!best || c->entry.build > best->entry.build || (c->entry.build == best->entry.build && c->entry.count > best->entry.count)) best = c;
    }
    return best;
}

u32 BuildInfo::objects_in(std::string_view language) const {
    u32 n = 0;
    for (const auto& c : compilers)
        if (c.product && c.product->language == language) n += c.entry.count;
    return n;
}

bool BuildInfo::has_variant(std::string_view variant) const {
    return std::ranges::any_of(compilers, [&](const BuildTool& c) { return c.product && c.product->variant.find(variant) != std::string_view::npos; });
}

std::vector<BuildTool> build_tools(const RichHeader& header, u8 linker_major, u8 linker_minor) {
    // The toolset minor version (14.xx) applies to the tools that share the image linker's build.
    std::optional<u16> linker_build;
    for (const auto& e : header.entries)
        if (const RichProduct* p = rich_product(e.product_id); p && p->tool == RichTool::linker && p->release == VsRelease::vs2015_or_later)
            linker_build = e.build;
    std::vector<BuildTool> out;
    for (const auto& e : header.entries) {
        BuildTool t;
        t.product = rich_product(e.product_id);
        t.entry = e;
        if (t.product) {
            std::optional<u8> toolset_minor;
            if (linker_major == 14 && linker_build == e.build) toolset_minor = linker_minor;
            t.version = tool_version(*t.product, e.build, toolset_minor);
        }
        out.push_back(std::move(t));
    }
    return out;
}

BuildInfo identify_build(const RichHeader& header, u8 linker_major, u8 linker_minor) {
    BuildInfo info;
    info.checksum_ok = header.checksum_ok;
    for (auto& t : build_tools(header, linker_major, linker_minor)) {
        switch (t.product ? t.product->tool : RichTool::unknown) {
        case RichTool::compiler: info.compilers.push_back(std::move(t)); break;
        case RichTool::assembler: info.assemblers.push_back(std::move(t)); break;
        case RichTool::imports: info.imports += t.entry.count; break;
        case RichTool::unmarked: info.unmarked += t.entry.count; break;
        case RichTool::linker: info.linker = std::move(t); break;  // the linker that linked the image
        default: info.others.push_back(std::move(t)); break;
        }
    }
    auto by_objects = [](const BuildTool& a, const BuildTool& b) { return a.entry.count > b.entry.count; };
    std::ranges::stable_sort(info.compilers, by_objects);
    std::ranges::stable_sort(info.assemblers, by_objects);
    return info;
}

} // namespace decomp::pe
