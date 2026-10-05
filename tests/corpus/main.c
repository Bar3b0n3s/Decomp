// The corpus program: Zydis and Zycore built without a C library, entered here. It is only linked and
// analyzed, never run.
#include <Zydis/Zydis.h>

static const ZyanU8 kCode[] = {0x51, 0x8D, 0x45, 0xFF, 0x50, 0xFF, 0x75, 0x0C, 0xFF, 0x75, 0x08, 0xFF, 0x15, 0xA0, 0xA5,
                               0x48, 0x76, 0x85, 0xC0, 0x0F, 0x88, 0xFC, 0xDA, 0x02, 0x00};

int eh_corpus(int code);
int seh_corpus(const int* p, const int* q);

int entry(void) {
    ZydisDisassembledInstruction ins;
    ZyanUSize offset = 0;
    int total = 0;
    while (ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LEGACY_32, 0x1000 + offset, kCode + offset, sizeof(kCode) - offset, &ins))) {
        total += ins.text[0];
        offset += ins.info.length;
    }
    return total + eh_corpus(total) + seh_corpus(&total, &total);
}
