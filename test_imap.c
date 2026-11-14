/* Self-check for grimnotif's IMAP parts. Build: cl /utf-8 /DUNICODE /D_UNICODE test_imap.c  then run test_imap.exe */
#define wWinMain grim_wWinMain
#include "grimnotif.c"
#undef wWinMain
#include <assert.h>

int main(void)
{
    wchar_t w[256];
    char hdr[] = "From: =?UTF-8?B?SsO2cmc=?= <j@x.com>\r\nSubject: =?utf-8?Q?Caf=C3=A9_?=\r\n =?utf-8?Q?time?= ok\r\n\r\n";
    WSADATA wsa;
    Tls *t = calloc(1, sizeof *t);
    int r;

    HeaderField(hdr, "From", w, 256);
    NameOnly(w);
    assert(wcscmp(w, L"Jörg") == 0);
    HeaderField(hdr, "Subject", w, 256);
    assert(wcscmp(w, L"Café time ok") == 0);
    DecodeMime("plain =?bad", w, 256);
    assert(wcscmp(w, L"plain =?bad") == 0);
    assert(MetaNum("* 5 FETCH (UID 42 X-GM-THRID 1890 FLAGS (\\Seen)", "UID") == 42);
    assert(MetaNum("* 5 FETCH (UID 42 X-GM-THRID 1890 FLAGS ()", "X-GM-THRID") == 1890);
    {   /* preview: multipart QP plain part, html fallback, base64 single part */
        wchar_t pv[300];
        const char *b1 = "--x\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\nHi Caf=C3=A9 so=\r\nft\r\n\r\nline2\r\n--x\r\nContent-Type: text/html\r\n\r\n<b>no</b>\r\n--x--\r\n";
        const char *b2 = "--y\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<html><head><style>p{}</style></head><body><p>Hello&nbsp;<b>you</b> &amp; me</p></body></html>\r\n--y--\r\n";
        Snippet("\nContent-Type: multipart/alternative; boundary=x\r\n", b1, (int)strlen(b1), pv, 300);
        assert(wcscmp(pv, L"Hi Café soft line2") == 0);
        Snippet("\nContent-Type: multipart/alternative; boundary=y\r\n", b2, (int)strlen(b2), pv, 300);
        assert(wcscmp(pv, L"Hello you & me") == 0);
        Snippet("\nContent-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n", "SGVsbG8gd29ybGQ=", 16, pv, 300);
        assert(wcscmp(pv, L"Hello world") == 0);
    }
    strcpy_s(g_vip, sizeof g_vip, " boss@x.com ; Mom");      /* sender filters */
    assert(SenderOk(L"Big Boss <BOSS@x.com>") && SenderOk(L"mom <m@y.com>") && !SenderOk(L"Ads <ads@shop.com>"));
    g_vip[0] = 0;
    strcpy_s(g_mute, sizeof g_mute, "shop.com,");
    assert(!SenderOk(L"Ads <ads@shop.com>") && SenderOk(L"Ann <a@b.com>"));
    g_mute[0] = 0;
    {   /* LOGIN quoting with a sealed password */
        Account a = {0};
        char c[600];
        strcpy_s(a.email, sizeof a.email, "x@y.com");
        strcpy_s(a.pass, sizeof a.pass, "p\"q\\r");
        PassSeal(a.pass);
        LoginCmd(&a, c, sizeof c);
        assert(strcmp(c, "LOGIN \"x@y.com\" \"p\\\"q\\\\r\"") == 0);
    }
    printf("parsing ok\n");

    /* live: TLS to Gmail, then a login that must be refused */
    WSAStartup(MAKEWORD(2, 2), &wsa);
    {   /* DPAPI round trip */
        char hex[1024], back[128];
        assert(Protect("abcd efgh", hex, sizeof hex));
        assert(Unprotect(hex, back, sizeof back) && strcmp(back, "abcd efgh") == 0);
        assert(!Unprotect("zz", back, sizeof back));
        printf("dpapi ok\n");
    }
    t->s = INVALID_SOCKET;
    g_nacc = 1;
    t->acc = &g_acc[0];
    t->acc->sock = INVALID_SOCKET;
    strcpy_s(t->acc->email, sizeof t->acc->email, "grimnotif.selftest@gmail.com");
    strcpy_s(t->acc->pass, sizeof t->acc->pass, "aaaabbbbccccdddd");
    PassSeal(t->acc->pass);
    assert(strcmp(t->acc->pass, "aaaabbbbccccdddd") != 0);   /* sealed in memory */
    r = ImapSession(t);
    TlsClose(t);
    printf("session result %d (expect -1 = TLS ok, login refused)\n", r);
    assert(r == -1);
    printf("all ok\n");
    return 0;
}
