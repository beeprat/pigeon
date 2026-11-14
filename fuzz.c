/* libFuzzer harness for pigeon's mail parsers.
   Build (x64 Native Tools prompt): cl /nologo /utf-8 /DUNICODE /D_UNICODE /fsanitize=fuzzer,address /Zi fuzz.c
   Run: fuzz.exe -max_total_time=600 */
#define wWinMain grim_wWinMain
#include "pigeon.c"
#undef wWinMain

int LLVMFuzzerTestOneInput(const unsigned char *d, size_t n)
{
    static char hdr[8192], body[16384];
    wchar_t w[300], wf[300];
    size_t split = n ? d[0] % (n + 1) : 0;    /* first byte picks the header/body split */
    size_t hl = split < sizeof hdr - 1 ? split : sizeof hdr - 1;
    size_t bl = n - split < sizeof body - 1 ? n - split : sizeof body - 1;

    memcpy(hdr, d, hl);   hdr[hl] = 0;
    memcpy(body, d + split, bl); body[bl] = 0;

    DecodeMime(hdr, w, 300);
    HeaderField(hdr, "From", wf, 300);
    NameOnly(wf);
    SenderOk(wf);
    HeaderField(hdr, "Subject", w, 300);
    MetaNum(hdr, "UID");
    Snippet(hdr, body, (int)bl, w, 300);
    return 0;
}
