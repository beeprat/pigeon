/* pigeon: Gmail new-mail popup at the top right.
   With email= and password= set it holds an IMAP IDLE connection, so Gmail pushes new mail instantly.
   Otherwise it polls the Apps Script web app at url=.
   Settings live in %APPDATA%\pigeon\config.ini, created on first run. Plain Win32, no runtime needed. */
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <security.h>
#include <schannel.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <wincrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <uxtheme.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "uxtheme.lib")

#define TIMER_HIDE 2
#define TIMER_FADE 3
#define TIMER_FADEIN 4
#define TIMER_BAR 5
#define TIMER_ANIM 9              /* popup: button animation, then the fade */
#define TIMER_TIP 8              /* popup: show the button tooltip after a rest */
#define TIMER_QUIET 7            /* main window: checks once a minute whether quiet hours ended */
#define TIMER_UPD 10             /* main window: daily check for a new release */
#define TIMER_DND 6              /* main window: retry held popups once the fullscreen app is gone */
static int g_dwm;               /* 1 = Windows 11 draws corners, border and shadow */
static int g_px, g_py;          /* popup resting position */
static int g_alpha = 255;       /* popup opacity during fade-out */
#define WM_TRAY (WM_APP + 1)
#define WM_NEXTMAIL (WM_APP + 3)   /* popup closed: show the next queued mail */
#define WM_NEWMAIL (WM_APP + 2)    /* from the IMAP thread, lParam = NewMail* */
#define WM_UNREAD (WM_APP + 4)     /* from the IMAP thread: an unread count changed */
#define WM_SAVECFG (WM_APP + 5)    /* from the IMAP thread: Microsoft sent a new refresh token */
#define IDM_EXIT 101
#define IDM_TEST 102
#define IDM_SETTINGS 103
#define IDM_TESTMENU 104
#define IDM_PAUSE 105
#define IDM_PAUSEDAY 106
#define IDM_HIST 110            /* 110..119: recent popups */
static NOTIFYICONDATAW g_nid;
static UINT g_taskbarMsg;       /* "TaskbarCreated": Explorer restarted, the tray icon is gone */



static wchar_t g_cfgPath[MAX_PATH];
static int g_popupMs = 4000;    /* popup visible time */
static int g_sound = 1;         /* 1 = play a sound */
static wchar_t g_soundFile[MAX_PATH];   /* custom .wav, empty = Windows notification sound */
static HWND g_main, g_popup;
static HFONT g_font, g_fontBig, g_fontSmall, g_fontBigU;   /* g_fontBigU: underlined subject on hover */
static RECT g_subjRect;         /* where the subject text is drawn */
static int g_subjHot;           /* mouse over the subject */
static wchar_t g_text[512], g_subj[512], g_to[300], g_prev[300];   /* g_prev: first words of the mail */
static int g_dpi = 96;
#define DP(x) MulDiv((x), g_dpi, 96)   /* 96-dpi pixels to screen pixels */
static int g_hover;
static int g_corner;            /* 0 top right, 1 top left, 2 bottom right, 3 bottom left */
static int g_fullHold = 1;      /* hold popups while a fullscreen app or game runs */
static int g_quiet, g_quietFrom = 22 * 60, g_quietTo = 7 * 60;   /* minutes after midnight */
static ULONGLONG g_pauseUntil;  /* tray "Pause popups": until then (NowMs), held like quiet hours; saved, so it survives a restart */

/* "22:30" or "22" -> minutes after midnight; -1 if not a time */
static int ParseHM(const char *v)
{
    int h, mi = 0, n = 0;
    if (sscanf_s(v, " %d%n", &h, &n) != 1) return -1;
    v += n;
    if (*v == ':') {
        if (sscanf_s(v + 1, "%d%n", &mi, &n) != 1) return -1;
        v += 1 + n;
    }
    while (*v == ' ') v++;
    return !*v && h >= 0 && h < 24 && mi >= 0 && mi < 60 ? h * 60 + mi : -1;
}
static int g_colOrder[3] = {0, 1, 2}, g_colW[3];  /* account list: column order, widths in 96-dpi px (0 = default) */
static int g_sortCol = -1, g_sortDir = 1;         /* -1: unsorted */
static char g_vip[512], g_mute[512];   /* sender filters, UTF-8, comma separated parts of "Name <address>" */
static int g_muteCat;           /* no popups for Gmail's Promotions and Social tabs */   /* quiet hours: no popups, no sound */
static wchar_t g_mailUrl[512] = L"https://mail.google.com/mail/u/0/#inbox";
static int g_imap;              /* 1 = IMAP IDLE mode (some account enabled) */
static HANDLE g_mutex;
static HINSTANCE g_inst;

#define MAX_ACC 8
#define PASSMAX 4096            /* app password or Microsoft refresh token; multiple of 16 for CryptProtectMemory */
typedef struct Account Account;
struct Account {
    char email[256], pass[PASSMAX];
    char nick[64];              /* optional name shown instead of the address, e.g. "Work" */
    volatile int enabled;       /* notifications on for this account */
    volatile int removed;       /* removed in Settings: the slot stays, its thread idles */
    HANDLE wake;                /* wakes the account's thread early (new password, re-enabled) */
    volatile SOCKET sock;       /* live IMAP socket, for ImapKick */
    volatile int state;         /* 0 connecting, 1 connected, 2 offline, 3 login failed */
    unsigned long uidnext;      /* next UID not yet seen, kept across reconnects */
    int tag;
    volatile int unread;        /* unread mails in the inbox, for the tray badge */
    int muted;                  /* no popups for this account; still counted in the badge */
};
static Account g_acc[MAX_ACC];
static int g_nacc;

/* What popups call an account: its nickname, else its address. */
static void AccLabel(const Account *a, wchar_t *w)
{
    MultiByteToWideChar(CP_UTF8, 0, a->nick[0] ? a->nick : a->email, -1, w, 256);
}

static char *Trim(char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t') s++;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* Password -> hex of a DPAPI blob, readable only by this Windows user. */
static int Protect(const char *pass, char *hex, size_t cap)
{
    DATA_BLOB in, out;
    DWORD i;
    in.pbData = (BYTE *)pass;
    in.cbData = (DWORD)strlen(pass);
    if (!CryptProtectData(&in, L"pigeon", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;
    if ((size_t)out.cbData * 2 + 1 > cap) { LocalFree(out.pbData); return 0; }
    for (i = 0; i < out.cbData; i++) sprintf_s(hex + i * 2, 3, "%02x", out.pbData[i]);
    hex[out.cbData * 2] = 0;
    LocalFree(out.pbData);
    return 1;
}

static int Unprotect(const char *hex, char *pass, size_t cap)
{
    static BYTE buf[PASSMAX + 2048];
    DATA_BLOB in, out;
    size_t i, n = strlen(hex) / 2;
    if (n == 0 || n > sizeof buf) return 0;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf_s(hex + i * 2, "%2x", &v) != 1) return 0;
        buf[i] = (BYTE)v;
    }
    in.pbData = buf;
    in.cbData = (DWORD)n;
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return 0;
    if (out.cbData < cap) {
        memcpy(pass, out.pbData, out.cbData);
        pass[out.cbData] = 0;
    }
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return out.cbData < cap;
}

/* Account.pass is kept encrypted in memory (CryptProtectMemory); plain text only lives in short local buffers. */
static SRWLOCK g_passLock = SRWLOCK_INIT;
static void PassSeal(char *pass)   { CryptProtectMemory(pass, PASSMAX, CRYPTPROTECTMEMORY_SAME_PROCESS); }
static void PassOpen(const char *sealed, char *plain)
{
    AcquireSRWLockShared(&g_passLock);
    memcpy(plain, sealed, PASSMAX);
    ReleaseSRWLockShared(&g_passLock);
    if (!CryptUnprotectMemory(plain, PASSMAX, CRYPTPROTECTMEMORY_SAME_PROCESS)) plain[0] = 0;
    plain[PASSMAX - 1] = 0;
}

/* Appends s as an IMAP quoted string ("..." with \ and " escaped). */
static void QuoteTo(char *o, size_t cap, const char *s)
{
    size_t n = strlen(o);
    if (n + 1 < cap) o[n++] = '"';
    for (; *s && n + 3 < cap; s++) {
        if (*s == '"' || *s == '\\') o[n++] = '\\';
        o[n++] = *s;
    }
    if (n + 1 < cap) o[n++] = '"';
    o[n] = 0;
}

/* "LOGIN <email> <pass>" with both quoted; caller wipes cmd after sending. */
static void LoginCmd(const Account *a, char *cmd, size_t cap)
{
    char plain[PASSMAX];
    PassOpen(a->pass, plain);
    strcpy_s(cmd, cap, "LOGIN ");
    QuoteTo(cmd, cap, a->email);
    strcat_s(cmd, cap, " ");
    QuoteTo(cmd, cap, plain);
    SecureZeroMemory(plain, sizeof plain);
}

/* ---------- Mail providers ---------- */

/* kind: 0 Gmail (X-GM extensions), 1 plain IMAP with app password, 2 Microsoft sign-in (XOAUTH2) */
typedef struct {
    const char *dom, *host, *web, *help;
    int kind;
    const wchar_t *name;        /* shown in Settings' Provider column */
    const wchar_t *mark;        /* popup avatar's corner logo: this text in ink color; "" = Gmail's own logo, "~" = a cloud */
    COLORREF ink;
} Provider;
static const Provider g_prov[] = {
    {"gmail.com",      "imap.gmail.com",       "", "https://myaccount.google.com/apppasswords", 0, L"Gmail", L"", 0},
    {"googlemail.com", "imap.gmail.com",       "", "https://myaccount.google.com/apppasswords", 0, L"Gmail", L"", 0},
    {"yahoo.*",        "imap.mail.yahoo.com",  "https://mail.yahoo.com/", "https://login.yahoo.com/account/security/app-passwords", 1, L"Yahoo", L"Y!", RGB(96, 1, 210)},
    {"ymail.com",      "imap.mail.yahoo.com",  "https://mail.yahoo.com/", "https://login.yahoo.com/account/security/app-passwords", 1, L"Yahoo", L"Y!", RGB(96, 1, 210)},
    {"rocketmail.com", "imap.mail.yahoo.com",  "https://mail.yahoo.com/", "https://login.yahoo.com/account/security/app-passwords", 1, L"Yahoo", L"Y!", RGB(96, 1, 210)},
    {"aol.com",        "imap.aol.com",         "https://mail.aol.com/", "https://login.aol.com/account/security/app-passwords", 1, L"AOL", L"Aol", RGB(51, 102, 255)},
    {"icloud.com",     "imap.mail.me.com",     "https://www.icloud.com/mail", "https://account.apple.com/account/manage", 1, L"iCloud", L"~", RGB(0, 122, 255)},
    {"me.com",         "imap.mail.me.com",     "https://www.icloud.com/mail", "https://account.apple.com/account/manage", 1, L"iCloud", L"~", RGB(0, 122, 255)},
    {"mac.com",        "imap.mail.me.com",     "https://www.icloud.com/mail", "https://account.apple.com/account/manage", 1, L"iCloud", L"~", RGB(0, 122, 255)},
    {"zoho.com",       "imap.zoho.com",        "https://mail.zoho.com/", "https://accounts.zoho.com/home#security/app_password", 1, L"Zoho", L"Z", RGB(226, 34, 40)},
    {"zohomail.com",   "imap.zoho.com",        "https://mail.zoho.com/", "https://accounts.zoho.com/home#security/app_password", 1, L"Zoho", L"Z", RGB(226, 34, 40)},
    {"gmx.com",        "imap.gmx.com",         "https://www.gmx.com/", "https://www.gmx.com/", 1, L"GMX", L"GMX", RGB(28, 68, 155)},
    {"gmx.us",         "imap.gmx.com",         "https://www.gmx.com/", "https://www.gmx.com/", 1, L"GMX", L"GMX", RGB(28, 68, 155)},
    {"gmx.*",          "imap.gmx.net",         "https://www.gmx.net/", "https://www.gmx.net/", 1, L"GMX", L"GMX", RGB(28, 68, 155)},
    {"web.de",         "imap.web.de",          "https://web.de/", "https://web.de/", 1, L"WEB.DE", L"W", RGB(235, 185, 0)},
    {"yandex.*",       "imap.yandex.com",      "https://mail.yandex.com/", "https://id.yandex.com/security/app-passwords", 1, L"Yandex", L"\x042f", RGB(252, 63, 29)},
    {"ya.ru",          "imap.yandex.com",      "https://mail.yandex.com/", "https://id.yandex.com/security/app-passwords", 1, L"Yandex", L"\x042f", RGB(252, 63, 29)},
    {"fastmail.com",   "imap.fastmail.com",    "https://app.fastmail.com/", "https://app.fastmail.com/settings/security/apps", 1, L"Fastmail", L"F", RGB(0, 103, 185)},
    {"fastmail.fm",    "imap.fastmail.com",    "https://app.fastmail.com/", "https://app.fastmail.com/settings/security/apps", 1, L"Fastmail", L"F", RGB(0, 103, 185)},
    {"outlook.*",      "outlook.office365.com", "https://outlook.live.com/mail/0/inbox", "", 2, L"Outlook", L"O", RGB(0, 120, 212)},
    {"hotmail.*",      "outlook.office365.com", "https://outlook.live.com/mail/0/inbox", "", 2, L"Outlook", L"O", RGB(0, 120, 212)},
    {"live.*",         "outlook.office365.com", "https://outlook.live.com/mail/0/inbox", "", 2, L"Outlook", L"O", RGB(0, 120, 212)},
    {"msn.com",        "outlook.office365.com", "https://outlook.live.com/mail/0/inbox", "", 2, L"Outlook", L"O", RGB(0, 120, 212)},
};

/* Provider by the address's domain. "x.*" matches any ending. Unknown domains are Gmail: Google Workspace uses custom domains. */
static const Provider *Prov(const char *email)
{
    const char *d = strrchr(email, '@');
    size_t i;
    if (d) {
        d++;
        for (i = 0; i < sizeof g_prov / sizeof g_prov[0]; i++) {
            const char *m = g_prov[i].dom;
            size_t n = strlen(m);
            if (n > 2 && m[n - 1] == '*' ? _strnicmp(d, m, n - 1) == 0 : _stricmp(d, m) == 0) return &g_prov[i];
        }
    }
    return &g_prov[0];
}

/* Web inbox of an account */
static void InboxUrl(const char *email, wchar_t *out)
{
    const Provider *pv = Prov(email);
    if (pv->kind == 0) swprintf_s(out, 512, L"https://mail.google.com/mail/?authuser=%hs#inbox", email);
    else swprintf_s(out, 512, L"%hs", pv->web);
}

/* Microsoft sign-in. Register a free app at entra.microsoft.com: "Personal Microsoft accounts only",
   platform "Mobile and desktop", "Allow public client flows" = Yes. The client ID is not a secret. */
#ifndef MS_CLIENT_ID
#define MS_CLIENT_ID "ac33a11d-4949-4f41-b8fe-f2c956002266"
#endif
#define MS_SCOPE "https://outlook.office.com/IMAP.AccessAsUser.All offline_access"

/* POST a form to login.microsoftonline.com. Returns the reply length, -1 on network error. */
static int HttpPost(const wchar_t *path, const char *body, char *out, int cap)
{
    HINTERNET s, c = NULL, r = NULL;
    DWORD got = 0;
    int len = -1;
    s = WinHttpOpen(L"pigeon", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, 15000, 15000, 15000, 30000);
    c = WinHttpConnect(s, L"login.microsoftonline.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) r = WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (r && WinHttpSendRequest(r, L"Content-Type: application/x-www-form-urlencoded\r\n", (DWORD)-1, (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0)
          && WinHttpReceiveResponse(r, NULL)) {
        len = 0;
        while (len < cap - 1 && WinHttpReadData(r, out + len, cap - 1 - len, &got) && got) len += got;
        out[len] = 0;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return len;
}

/* Appends s form-encoded */
static void UrlEnc(char *o, size_t cap, const char *s)
{
    size_t n = strlen(o);
    for (; *s && n + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o[n++] = (char)c;
        else n += sprintf_s(o + n, cap - n, "%%%02X", c);
    }
    o[n] = 0;
}

/* String value of "key" in a flat JSON object (enough for OAuth replies) */
static int JStr(const char *j, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;
    size_t n = 0;
    out[0] = 0;
    sprintf_s(pat, sizeof pat, "\"%s\"", key);
    if (!(p = strstr(j, pat))) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') {                    /* number */
        while (*p >= '0' && *p <= '9' && n < cap - 1) out[n++] = *p++;
        out[n] = 0;
        return n > 0;
    }
    for (p++; *p && *p != '"' && n < cap - 1; p++) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p;
    }
    out[n] = 0;
    return n > 0;
}

/* Swaps the stored refresh token for an access token. keep: store the rotated refresh token.
   1 = ok, 0 = sign-in no longer valid, -1 = network. */
static int MsToken(Account *a, int keep, char *tok, size_t cap)
{
    static char body[PASSMAX * 3 + 512];
    char rt[PASSMAX], *resp = malloc(32768);
    int n, r = -1;
    static SRWLOCK lk = SRWLOCK_INIT;   /* body is static: one refresh at a time */
    tok[0] = 0;
    if (!resp) return -1;
    AcquireSRWLockExclusive(&lk);
    PassOpen(a->pass, rt);
    strcpy_s(body, sizeof body, "client_id=" MS_CLIENT_ID "&grant_type=refresh_token&scope=");
    UrlEnc(body, sizeof body, MS_SCOPE);
    strcat_s(body, sizeof body, "&refresh_token=");
    UrlEnc(body, sizeof body, rt);
    n = HttpPost(L"/consumers/oauth2/v2.0/token", body, resp, 32768);
    SecureZeroMemory(body, sizeof body);
    ReleaseSRWLockExclusive(&lk);
    if (n > 0) {
        if (JStr(resp, "access_token", tok, cap)) {
            r = 1;
            if (keep && JStr(resp, "refresh_token", rt, sizeof rt) && strlen(rt) < PASSMAX - 1) {
                char sealed[PASSMAX];
                memset(sealed, 0, sizeof sealed);
                strcpy_s(sealed, sizeof sealed, rt);
                PassSeal(sealed);
                AcquireSRWLockExclusive(&g_passLock);
                memcpy(a->pass, sealed, PASSMAX);
                ReleaseSRWLockExclusive(&g_passLock);
                PostMessageW(g_main, WM_SAVECFG, 0, 0);
            }
        } else if (strstr(resp, "invalid_grant") || strstr(resp, "unauthorized_client") || strstr(resp, "invalid_client")) r = 0;
    }
    SecureZeroMemory(rt, sizeof rt);
    SecureZeroMemory(resp, 32768);
    free(resp);
    return r;
}

/* base64("user=<email>^Aauth=Bearer <token>^A^A"), the XOAUTH2 initial response */
static int XoauthB64(const char *email, const char *tok, char *out, DWORD cap)
{
    static char raw[PASSMAX + 600];
    int n = sprintf_s(raw, sizeof raw, "user=%s\001auth=Bearer %s\001\001", email, tok);
    int ok = n > 0 && CryptBinaryToStringA((const BYTE *)raw, (DWORD)n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &cap);
    SecureZeroMemory(raw, sizeof raw);
    return ok;
}

/* Device-code sign-in from the Add dialog. Stores the refresh token sealed in a->pass. */
static int MsSignIn(HWND d, Account *a)
{
    char body[8192], *resp = malloc(32768), code[32], dev[4096], uri[256], rt[PASSMAX];
    wchar_t msg[512];
    int ok = 0;
    if (!resp) return 0;
    if (!MS_CLIENT_ID[0]) {
        MessageBoxW(d, L"This build has no Microsoft app ID, so Outlook/Hotmail sign-in is off.\n\nSee the README to set MS_CLIENT_ID.", L"pigeon", MB_ICONWARNING);
        free(resp);
        return 0;
    }
    strcpy_s(body, sizeof body, "client_id=" MS_CLIENT_ID "&scope=");
    UrlEnc(body, sizeof body, MS_SCOPE);
    if (HttpPost(L"/consumers/oauth2/v2.0/devicecode", body, resp, 32768) <= 0 ||
        !JStr(resp, "user_code", code, sizeof code) || !JStr(resp, "device_code", dev, sizeof dev) || !JStr(resp, "verification_uri", uri, sizeof uri)) {
        MessageBoxW(d, L"Could not reach Microsoft sign-in. Check the connection and try again.", L"pigeon", MB_ICONWARNING);
        free(resp);
        return 0;
    }
    if (OpenClipboard(d)) {             /* code on the clipboard: paste it on the page */
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof code);
        EmptyClipboard();
        if (g) { memcpy(GlobalLock(g), code, sizeof code); GlobalUnlock(g); if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g); }
        CloseClipboard();
    }
    {
        wchar_t wuri[256];
        MultiByteToWideChar(CP_UTF8, 0, uri, -1, wuri, 256);
        if (wcsncmp(wuri, L"https://", 8) == 0) ShellExecuteW(d, L"open", wuri, NULL, NULL, SW_SHOWNORMAL);
    }
    swprintf_s(msg, 512, L"Code %hs is copied.\n\nPaste it on the Microsoft page that just opened, sign in as %hs and allow access.\n\nThen click OK.", code, a->email);
    while (MessageBoxW(d, msg, L"Sign in to Outlook", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK) {
        strcpy_s(body, sizeof body, "client_id=" MS_CLIENT_ID "&grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&device_code=");
        UrlEnc(body, sizeof body, dev);
        if (HttpPost(L"/consumers/oauth2/v2.0/token", body, resp, 32768) > 0 && JStr(resp, "refresh_token", rt, sizeof rt) && strlen(rt) < PASSMAX - 1) {
            memset(a->pass, 0, PASSMAX);
            strcpy_s(a->pass, PASSMAX, rt);
            PassSeal(a->pass);
            ok = 1;
            break;
        }
        if (!strstr(resp, "authorization_pending")) {
            char why[400];
            if (!JStr(resp, "error_description", why, sizeof why)) strcpy_s(why, sizeof why, resp[0] ? "no refresh token in reply" : "no reply");
            swprintf_s(msg, 512, L"Microsoft sign-in failed:\n\n%.380hs", why);
            MessageBoxW(d, msg, L"pigeon", MB_ICONWARNING);
            break;
        }
        swprintf_s(msg, 512, L"Not finished yet. Enter code %hs on the Microsoft page, sign in, then click OK.", code);
    }
    SecureZeroMemory(rt, sizeof rt);
    SecureZeroMemory(resp, 32768);
    free(resp);
    return ok;
}

/* Writes config.ini (via a temp file, so a failed write never loses the old one). */
static int SaveConfig(void)
{
    wchar_t tmp[MAX_PATH + 8];
    char u8[2048];
    static char hex[PASSMAX * 2 + 1024];
    FILE *f;
    int i, ok = 1;

    swprintf_s(tmp, MAX_PATH + 8, L"%s.tmp", g_cfgPath);
    f = _wfopen(tmp, L"wb");
    if (!f) return 0;
    fputs("; pigeon settings. Edit accounts with Settings in the tray menu;\r\n"
          "; passwords are encrypted and only work for this Windows user.\r\n", f);
    for (i = 0; i < g_nacc; i++) {
        char plain[PASSMAX];
        if (g_acc[i].removed) continue;
        PassOpen(g_acc[i].pass, plain);
        ok = Protect(plain, hex, sizeof hex);
        SecureZeroMemory(plain, sizeof plain);
        if (!ok) break;
        fprintf(f, "account=%s|%d|%s\r\n", g_acc[i].email, g_acc[i].enabled, hex);
    }
    for (i = 0; i < g_nacc; i++)        /* own key, not a 4th account field: older versions would misread that */
        if (!g_acc[i].removed && g_acc[i].nick[0]) fprintf(f, "nick=%s|%s\r\n", g_acc[i].email, g_acc[i].nick);
    for (i = 0; i < g_nacc; i++)
        if (!g_acc[i].removed && g_acc[i].muted) fprintf(f, "muted=%s\r\n", g_acc[i].email);
    fprintf(f, "popup=%d\r\nsound=%d\r\ncorner=%d\r\n", g_popupMs / 1000, g_sound, g_corner);
    fprintf(f, "fullhold=%d\r\nquiet=%d\r\nquietfrom=%02d:%02d\r\nquietto=%02d:%02d\r\n", g_fullHold, g_quiet,
            g_quietFrom / 60, g_quietFrom % 60, g_quietTo / 60, g_quietTo % 60);
    fprintf(f, "vip=%s\r\nmute=%s\r\nmutecat=%d\r\npauseuntil=%llu\r\n", g_vip, g_mute, g_muteCat, g_pauseUntil);
    fprintf(f, "cols=%d %d %d %d %d %d %d %d\r\n", g_colOrder[0], g_colOrder[1], g_colOrder[2], g_colW[0], g_colW[1], g_colW[2], g_sortCol, g_sortDir);
    WideCharToMultiByte(CP_UTF8, 0, g_soundFile, -1, u8, sizeof u8, NULL, NULL);
    fprintf(f, "soundfile=%s\r\n", u8);
    if (fclose(f) != 0) ok = 0;
    if (!ok || !MoveFileExW(tmp, g_cfgPath, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); return 0; }
    return 1;
}

static int AnyEnabled(void)
{
    int i;
    for (i = 0; i < g_nacc; i++) if (g_acc[i].enabled) return 1;
    return 0;
}

/* Loads %APPDATA%\pigeon\config.ini. Returns 1 if there is something to watch. */
static int LoadConfig(void)
{
    wchar_t appdata[MAX_PATH];
    static char line[PASSMAX * 2 + 4096];
    char oldEmail[256] = "", oldPass[128] = "";
    int dropUrl = 0;
    FILE *f;

    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return 0;
    wcscpy_s(g_cfgPath, MAX_PATH, appdata);
    {   /* renamed from grimnotif: carry the old settings over once */
        wchar_t old[MAX_PATH];
        swprintf_s(old, MAX_PATH, L"%s\\grimnotif", appdata);
        wcscat_s(g_cfgPath, MAX_PATH, L"\\pigeon");
        if (GetFileAttributesW(g_cfgPath) == INVALID_FILE_ATTRIBUTES) MoveFileW(old, g_cfgPath);
    }
    CreateDirectoryW(g_cfgPath, NULL);
    wcscat_s(g_cfgPath, MAX_PATH, L"\\config.ini");

    g_popupMs = 6000;
    g_sound = 1;
    g_nacc = 0;
    f = _wfopen(g_cfgPath, L"rb");
    if (!f) return 0;                   /* first run: Settings opens */
    while (fgets(line, sizeof line, f)) {
        char *eq, *k, *v;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        k = Trim(line);
        v = Trim(eq + 1);
        if (k[0] == ';' || k[0] == '#') continue;
        if (strcmp(k, "url") == 0)           dropUrl = 1;   /* old web app mode removed: rewrite config without its plain-text key */
        else if (strcmp(k, "popup") == 0)    g_popupMs = atoi(v) * 1000;
        else if (strcmp(k, "sound") == 0)    g_sound = atoi(v);
        else if (strcmp(k, "corner") == 0)   g_corner = atoi(v) & 3;
        else if (strcmp(k, "fullhold") == 0) g_fullHold = atoi(v);
        else if (strcmp(k, "quiet") == 0)    g_quiet = atoi(v);
        else if (strcmp(k, "pauseuntil") == 0) g_pauseUntil = _strtoui64(v, NULL, 10);
        else if (strcmp(k, "cols") == 0) {
            int o[3], wd[3], sc, sd, j;
            if (sscanf_s(v, "%d %d %d %d %d %d %d %d", &o[0], &o[1], &o[2], &wd[0], &wd[1], &wd[2], &sc, &sd) == 8
                && o[0] >= 0 && o[1] >= 0 && o[2] >= 0 && o[0] + o[1] + o[2] == 3
                && o[0] != o[1] && o[1] != o[2] && o[0] != o[2] && sc >= -1 && sc < 3) {
                for (j = 0; j < 3; j++) {
                    g_colOrder[j] = o[j];
                    g_colW[j] = wd[j] > 0 && wd[j] < 2000 ? wd[j] : 0;
                }
                g_sortCol = sc;
                g_sortDir = sd < 0 ? -1 : 1;
            }
        }
        else if (strcmp(k, "quietfrom") == 0) { int t = ParseHM(v); if (t >= 0) g_quietFrom = t; }
        else if (strcmp(k, "quietto") == 0)  { int t = ParseHM(v); if (t >= 0) g_quietTo = t; }
        else if (strcmp(k, "vip") == 0)      strcpy_s(g_vip, sizeof g_vip, v);
        else if (strcmp(k, "mute") == 0)     strcpy_s(g_mute, sizeof g_mute, v);
        else if (strcmp(k, "mutecat") == 0)  g_muteCat = atoi(v);
        else if (strcmp(k, "soundfile") == 0) MultiByteToWideChar(CP_UTF8, 0, v, -1, g_soundFile, MAX_PATH);
        else if (strcmp(k, "account") == 0 && g_nacc < MAX_ACC) {   /* email|enabled|dpapi-hex */
            char *p1 = strchr(v, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
            Account *ac = &g_acc[g_nacc];
            if (!p2) continue;
            *p1 = *p2 = 0;
            memset(ac, 0, sizeof *ac);
            ac->sock = INVALID_SOCKET;
            strncpy_s(ac->email, sizeof ac->email, v, _TRUNCATE);
            ac->enabled = atoi(p1 + 1);
            if (!Unprotect(p2 + 1, ac->pass, sizeof ac->pass)) ac->pass[0] = 0;   /* other user/PC: re-add in Settings */
            PassSeal(ac->pass);
            g_nacc++;
        }
        else if (strcmp(k, "nick") == 0) {   /* email|name */
            char *bar = strchr(v, '|');
            int j;
            if (!bar) continue;
            *bar = 0;
            for (j = 0; j < g_nacc; j++)
                if (_stricmp(g_acc[j].email, v) == 0) strncpy_s(g_acc[j].nick, sizeof g_acc[j].nick, bar + 1, _TRUNCATE);
        }
        else if (strcmp(k, "muted") == 0) {
            int j;
            for (j = 0; j < g_nacc; j++) if (_stricmp(g_acc[j].email, v) == 0) g_acc[j].muted = 1;
        }
        else if (strcmp(k, "email") == 0)    strncpy_s(oldEmail, sizeof oldEmail, v, _TRUNCATE);   /* old plain-text format */
        else if (strcmp(k, "password") == 0) {
            size_t i, o = 0;                /* app passwords are shown with spaces: drop them */
            for (i = 0; v[i] && o < sizeof oldPass - 1; i++) if (v[i] != ' ') oldPass[o++] = v[i];
            oldPass[o] = 0;
        }
    }
    fclose(f);
    if (g_popupMs < 1000) g_popupMs = 1000;
    if (oldEmail[0] && oldPass[0] && g_nacc < MAX_ACC) {   /* migrate: encrypt and drop the plain-text password */
        Account *ac = &g_acc[g_nacc++];
        memset(ac, 0, sizeof *ac);
        ac->sock = INVALID_SOCKET;
        strcpy_s(ac->email, sizeof ac->email, oldEmail);
        strcpy_s(ac->pass, sizeof ac->pass, oldPass);
        PassSeal(ac->pass);
        ac->enabled = 1;
        SaveConfig();
    }
    SecureZeroMemory(oldPass, sizeof oldPass);
    g_imap = AnyEnabled();
    if (dropUrl) SaveConfig();
    return g_imap;
}



/* keep only the name: "Name" <addr> -> Name */
static void NameOnly(wchar_t *from)
{
    size_t n;
    wchar_t *lt = wcschr(from, L'<');
    if (lt && lt > from) *lt = 0;
    n = wcslen(from);
    while (n && (from[n - 1] == L' ' || from[n - 1] == L'"')) from[--n] = 0;
    if (from[0] == L'"') memmove(from, from + 1, (wcslen(from)) * sizeof(wchar_t));
}


#define PAD 12          /* popup padding, same on all four sides */
#define AV 40           /* avatar circle */
#define BADGE 14        /* small Gmail logo width */
#define LOGO_W 44
#define LOGO_H 33
#define GAP 12          /* avatar to text, same as PAD */
static int g_h1, g_h2, g_h3;    /* line heights: sender, subject, address */
#define BTN 26          /* hover action button */
#define NBTN 4          /* seen, archive, star, snooze */
#define NOMAIL ((unsigned long)-1)   /* NewMail.uid of the quiet-hours summary: no button bar */
#define HASBAR (g_cur.uid != NOMAIL)
#define BAR (BTN + PAD)  /* height the popup grows by on hover */
static int g_pw, g_ph;          /* popup size without the button bar */
static int g_more;              /* extra preview lines shown while hovered, in pixels */
#define BARH (DP(BAR) + g_more)  /* height the popup grows by on hover */
static int g_bar, g_barOn;      /* button bar: current height in px, sliding open or shut */
static HFONT g_fontIcon, g_fontIconS;   /* g_fontIconS: the small close cross */
static int g_hot = -1;          /* button under the mouse */
#define SLIDE_MS 630             /* the slide to the middle */
#define ANIM_MS 1000              /* "mark as read" animation length */
static int g_anim;               /* button + 1 whose animation is playing, 0 none */
static int g_animSnz;            /* it was a snooze time: label melts into a clock */
static ULONGLONG g_animStart;    /* its GetTickCount64 start: frames follow the clock, not the timer's uneven ticks */
static int g_snzMode;           /* bar shows snooze times instead of actions */
typedef struct { wchar_t from[256], subj[256], url[512], to[256], prev[300]; Account *acc; unsigned long uid;
                 ULONGLONG when; } NewMail;   /* when: server receive time, FILETIME UTC; 0 = unknown */
static NewMail g_cur;           /* mail shown in the popup now */
static void PopupAction(int b);
static void ShowNext(void);
static int g_light;             /* Windows apps mode is light: we follow it */
/* A grey picked for the dark look; in light mode the mirror shade, so contrast stays the same */
static COLORREF Gr(int v) { return g_light ? RGB(255 - v, 255 - v, 255 - v) : RGB(v, v, v); }
#define BG (g_light ? RGB(243, 243, 243) : RGB(32, 32, 32))

static int ReadLight(void)
{
    DWORD v = 0, n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &n) == ERROR_SUCCESS && v;
}

static void FillPoly(HDC m, COLORREF c, POINT *pt, int n)
{
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(m, b);
    Polygon(m, pt, n);
    SelectObject(m, ob);
    DeleteObject(b);
}

/* Gmail "M", drawn 4x large then shrunk with HALFTONE for smooth edges */
static void DrawLogo(HDC dc, int x, int y, int w, int h)
{
    const int S = 4;
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, LOGO_W * S, LOGO_H * S);
    HGDIOBJ ob = SelectObject(m, bmp);
    HBRUSH b;
    RECT r = {0, 0, LOGO_W * S, LOGO_H * S};
    POINT blu[4] = {{0,10},{10,10},{10,33},{0,33}};
    POINT grn[4] = {{34,10},{44,10},{44,33},{34,33}};
    POINT red[8] = {{0,5},{9,3},{22,16},{35,3},{44,5},{44,14},{22,30},{0,14}};
    POINT yel[5] = {{34,4},{35,3},{44,5},{44,14},{34,21}};
    POINT drk[5] = {{0,5},{9,3},{10,4},{10,21},{0,14}};
    POINT *all[5] = {blu, grn, red, yel, drk};
    int cnt[5] = {4, 4, 8, 5, 5}, i, k;

    for (k = 0; k < 5; k++)
        for (i = 0; i < cnt[k]; i++) { all[k][i].x *= S; all[k][i].y *= S; }

    b = CreateSolidBrush(BG); FillRect(m, &r, b); DeleteObject(b);
    SelectObject(m, GetStockObject(NULL_PEN));
    FillPoly(m, RGB(66, 133, 244), blu, 4);
    FillPoly(m, RGB(52, 168, 83), grn, 4);
    FillPoly(m, RGB(234, 67, 53), red, 8);
    FillPoly(m, RGB(251, 188, 4), yel, 5);
    FillPoly(m, RGB(197, 34, 31), drk, 5);

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, x, y, w, h, m, 0, 0, LOGO_W * S, LOGO_H * S, SRCCOPY);
    SelectObject(m, ob);
    DeleteObject(bmp);
    DeleteDC(m);
}

static HFONT MakeFont(int h, int weight);
static int TextW(HFONT f, const wchar_t *t);

/* Gmail-style letter avatar: colored circle with the sender's initial, small provider badge bottom right
   (kind: Gmail logo for 0, blue "O" for Microsoft, none otherwise) */
static void DrawAvatar(HDC dc, int x, int y, int d, const wchar_t *name, const Provider *pv)
{
    static const COLORREF pal[] = {RGB(66,133,244), RGB(219,68,55), RGB(244,160,0), RGB(15,157,88),
                                   RGB(171,71,188), RGB(0,172,193), RGB(255,112,67), RGB(92,107,192)};
    const int S = 4, D = d * S, E = DP(3) * S;   /* E: room right of the circle for the logo to stick out */
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, D + E, D);
    HGDIOBJ ob = SelectObject(m, bmp), of;
    RECT r = {0, 0, D + E, D};
    HBRUSH b;
    HFONT f;
    wchar_t ch[2] = {L'?', 0};
    unsigned hsh = 0;
    const wchar_t *p;

    for (p = name; *p && wcsncmp(p, L"  +", 3) != 0; p++) hsh = hsh * 31 + *p;   /* same sender, same color */
    for (p = name; *p; p++) if (iswalnum(*p)) { ch[0] = towupper(*p); break; }

    b = CreateSolidBrush(BG); FillRect(m, &r, b); DeleteObject(b);
    SelectObject(m, GetStockObject(NULL_PEN));
    b = CreateSolidBrush(pal[hsh % 8]);
    SelectObject(m, b);
    Ellipse(m, 0, 0, D + 1, D + 1);
    SelectObject(m, GetStockObject(NULL_BRUSH));
    DeleteObject(b);
    f = MakeFont(-D * 48 / 100, FW_SEMIBOLD);
    of = SelectObject(m, f);
    SetBkMode(m, TRANSPARENT);
    SetTextColor(m, RGB(255, 255, 255));
    r.right = D;
    DrawTextW(m, ch, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(m, of);
    DeleteObject(f);
    if (pv->mark[0]) {   /* provider's mark in its color, BG-colored outline like the Gmail logo */
        int ow = DP(3) * S, lw = DP(BADGE - 2) * S, ox = D + E - lw - ow / 2, oy = D - lw - ow / 2, pass;
        int len = (int)wcslen(pv->mark), u = lw / 10, cx = ox + lw / 2, cy = oy + lw / 2 + 3 * u / 2;   /* cloud spans cy-5u..cy+2u */
        RECT br = {ox, oy, ox + lw, oy + lw};
        LOGBRUSH lb = {BS_SOLID, BG, 0};
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, ow * 2, &lb, 0, NULL);
        b = CreateSolidBrush(pv->ink);
        f = MakeFont(-lw * (len == 1 ? 95 : len == 2 ? 85 : 55) / 100, FW_BOLD);   /* longer marks smaller so they fit */
        of = SelectObject(m, f);
        SetPolyFillMode(m, WINDING);           /* overlapping cloud parts fill as one */
        for (pass = 0; pass < 2; pass++) {     /* pass 0: thick outline, pass 1: fill */
            BeginPath(m);
            if (wcscmp(pv->mark, L"~") == 0) {
                Ellipse(m, cx - 5 * u, cy - 2 * u, cx - u, cy + 2 * u + 1);
                Ellipse(m, cx - 3 * u, cy - 5 * u, cx + 3 * u, cy + u);
                Ellipse(m, cx + u, cy - 3 * u, cx + 5 * u, cy + 2 * u + 1);
                Rectangle(m, cx - 3 * u, cy - u, cx + 3 * u, cy + 2 * u + 1);
            } else DrawTextW(m, pv->mark, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            EndPath(m);
            if (pass == 0) { SelectObject(m, pen); StrokePath(m); }
            else { SelectObject(m, b); FillPath(m); }
        }
        SelectObject(m, GetStockObject(NULL_PEN));
        SelectObject(m, GetStockObject(NULL_BRUSH));
        SelectObject(m, of);
        DeleteObject(f);
        DeleteObject(b);
        DeleteObject(pen);
    } else {   /* small Gmail logo bottom right, drawn over the circle with a BG-colored outline */
        static const POINT src[5][8] = {
            {{0,10},{10,10},{10,33},{0,33}},
            {{34,10},{44,10},{44,33},{34,33}},
            {{0,5},{9,3},{22,16},{35,3},{44,5},{44,14},{22,30},{0,14}},
            {{34,4},{35,3},{44,5},{44,14},{34,21}},
            {{0,5},{9,3},{10,4},{10,21},{0,14}}};
        static const int cnt[5] = {4, 4, 8, 5, 5};
        static const COLORREF col[5] = {RGB(66,133,244), RGB(52,168,83), RGB(234,67,53), RGB(251,188,4), RGB(197,34,31)};
        int ow = DP(3) * S, lw = DP(BADGE - 2) * S, lh = lw * 33 / 44;
        int ox = D + E - lw - ow / 2, oy = D - lh - ow / 2, k, j, pass;
        HPEN pen = CreatePen(PS_SOLID, ow * 2, BG);
        POINT pt[8];
        for (pass = 0; pass < 2; pass++)            /* pass 0: thick outline under everything, pass 1: fills */
            for (k = 0; k < 5; k++) {
                for (j = 0; j < cnt[k]; j++) { pt[j].x = ox + src[k][j].x * lw / 44; pt[j].y = oy + src[k][j].y * lh / 33; }
                if (pass == 0) {
                    SelectObject(m, pen);
                    SelectObject(m, GetStockObject(NULL_BRUSH));
                    Polygon(m, pt, cnt[k]);
                } else {
                    SelectObject(m, GetStockObject(NULL_PEN));
                    FillPoly(m, col[k], pt, cnt[k]);
                }
            }
        SelectObject(m, GetStockObject(NULL_PEN));
        DeleteObject(pen);
    }

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, x, y, d + E / S, d, m, 0, 0, D + E, D, SRCCOPY);
    SelectObject(m, ob);
    DeleteObject(bmp);
    DeleteDC(m);

}

static void ImapKick(void);

static void PlayNotify(void)
{
    wchar_t def[MAX_PATH];
    if (!g_sound) return;
    if (g_soundFile[0] && PlaySoundW(g_soundFile, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) return;
    GetWindowsDirectoryW(def, MAX_PATH);    /* Windows' own new-mail sound, louder than Notification.Default */
    wcscat_s(def, MAX_PATH, L"\\Media\\Windows Notify Email.wav");
    if (PlaySoundW(def, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) return;
    PlaySoundW(L"Notification.Default", NULL, SND_ALIAS | SND_ASYNC);
}

/* Button b's rect: a row of equal buttons along the bottom (only while hovered) */
static RECT BtnRect(HWND h, int b)
{
    RECT rc, r;
    int gap = DP(6), pad = DP(PAD), bw;
    GetClientRect(h, &rc);
    bw = (rc.right - 2 * pad - (NBTN - 1) * gap) / NBTN;
    r.left = (rc.right - NBTN * bw - (NBTN - 1) * gap) / 2 + b * (bw + gap);   /* centered: rounding leftover split on both sides */
    r.right = r.left + bw;
    r.top = g_ph + g_more;              /* fixed under the content: the shrinking window clips it, never slides it over the text */
    r.bottom = r.top + DP(BTN);
    return r;
}

/* Small close cross at the top right; BtnAt returns NBTN for it */
static RECT CloseRect(HWND h)
{
    RECT rc, r;
    GetClientRect(h, &rc);
    r.right = rc.right - DP(6);
    r.left = r.right - DP(22);
    r.top = DP(6);
    r.bottom = r.top + DP(22);
    return r;
}

static int BtnAt(HWND h, int x, int y)
{
    int b;
    POINT pt = {x, y};
    RECT cr = CloseRect(h);
    if (!g_hover) return -1;
    if (PtInRect(&cr, pt)) return NBTN;
    if (!HASBAR) return -1;
    for (b = 0; b < NBTN; b++) {
        RECT r = BtnRect(h, b);
        if (PtInRect(&r, pt)) return b;
    }
    return -1;
}

/* Antialiased rounded rect: drawn 4x and shrunk, like the avatar */
static void SmoothRound(HDC dc, RECT r, int rad, COLORREF fill)
{
    const int S = 4;
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, w * S, hh * S);
    HGDIOBJ obm = SelectObject(m, bm);
    HBRUSH bg = CreateSolidBrush(BG), fb = CreateSolidBrush(fill);
    RECT all = {0, 0, w * S, hh * S};
    HGDIOBJ ob, op;
    FillRect(m, &all, bg);
    ob = SelectObject(m, fb);
    op = SelectObject(m, GetStockObject(NULL_PEN));
    RoundRect(m, 0, 0, w * S + 1, hh * S + 1, rad * S, rad * S);
    SelectObject(m, ob);
    SelectObject(m, op);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, r.left, r.top, w, hh, m, 0, 0, w * S, hh * S, SRCCOPY);
    SelectObject(m, obm);
    DeleteObject(bm);
    DeleteObject(bg);
    DeleteObject(fb);
    DeleteDC(m);
}

/* a..b by k/255 */
static COLORREF Mix(COLORREF a, COLORREF b, int k)
{
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * k / 255,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * k / 255,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * k / 255);
}

/* Antialiased clock face at (cx, cy): drawn 4x and shrunk; ang turns the minute hand (0 = 12 o'clock) */
/* glyph ch of font f centered at (x, y), to a quarter pixel: drawn 4x over a 4x copy of what's there, scaled back */
static void GlyphAt(HDC dc, HFONT f, wchar_t ch, double x, double y, COLORREF ink)
{
    const int S = 4;
    LOGFONTW lf;
    HFONT big;
    HDC m;
    HBITMAP bm;
    HGDIOBJ obm, of;
    int half, L, T, w;
    RECT tr;
    GetObjectW(f, sizeof lf, &lf);
    half = abs(lf.lfHeight) / 2 + 3;
    lf.lfHeight *= S; lf.lfQuality = ANTIALIASED_QUALITY;   /* no ClearType fringes once scaled */
    big = CreateFontIndirectW(&lf);
    L = (int)floor(x) - half; T = (int)floor(y) - half; w = 2 * half;
    m = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, w * S, w * S);
    obm = SelectObject(m, bm);
    SetStretchBltMode(m, COLORONCOLOR);   /* exact pixel copies: the round trip leaves the background as it was */
    StretchBlt(m, 0, 0, w * S, w * S, dc, L, T, w, w, SRCCOPY);
    of = SelectObject(m, big);
    SetBkMode(m, TRANSPARENT);
    SetTextColor(m, ink);
    tr.left = (int)((x - L) * S) - w * S; tr.right = tr.left + 2 * w * S;
    tr.top = (int)((y - T) * S) - w * S; tr.bottom = tr.top + 2 * w * S;
    DrawTextW(m, &ch, 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, L, T, w, w, m, 0, 0, w * S, w * S, SRCCOPY);
    SelectObject(m, of); SelectObject(m, obm);
    DeleteObject(big); DeleteObject(bm); DeleteDC(m);
}

static void DrawClock(HDC dc, double fx, double fy, int rad, double ang, COLORREF ink, COLORREF bgc)
{
    const int S = 4;
    int sz = 2 * rad + 4, cx = (int)floor(fx), cy = (int)floor(fy);
    int c = sz * S / 2 + (int)((fx - cx) * S), cv = sz * S / 2 + (int)((fy - cy) * S);   /* quarter-pixel position */
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, sz * S, sz * S);
    HGDIOBJ obm = SelectObject(m, bm), op, ob;
    HBRUSH bb = CreateSolidBrush(bgc);
    HPEN pen = CreatePen(PS_SOLID, DP(1) * S + S / 2, ink);
    RECT all = {0, 0, sz * S, sz * S};
    double ha = 3.14159 / 2;            /* hour hand stays at 3, like the icon */
    FillRect(m, &all, bb);
    op = SelectObject(m, pen);
    ob = SelectObject(m, GetStockObject(NULL_BRUSH));
    Ellipse(m, c - rad * S, cv - rad * S, c + rad * S, cv + rad * S);
    MoveToEx(m, c, cv, NULL); LineTo(m, c + (int)(sin(ang) * rad * S * 0.62), cv - (int)(cos(ang) * rad * S * 0.62));
    MoveToEx(m, c, cv, NULL); LineTo(m, c + (int)(sin(ha) * rad * S * 0.4), cv - (int)(cos(ha) * rad * S * 0.4));
    SelectObject(m, op); SelectObject(m, ob);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, cx - sz / 2, cy - sz / 2, sz, sz, m, 0, 0, sz * S, sz * S, SRCCOPY);
    SelectObject(m, obm);
    DeleteObject(bm); DeleteObject(bb); DeleteObject(pen);
    DeleteDC(m);
}

/* c faded toward the background by how far the bar has slid open */
static COLORREF BarFade(COLORREF c)
{
    int a = g_bar * 255 / BARH;
    a = a * a / 255;                    /* lags the slide a little, so it fades in last */
    return RGB(GetRValue(BG) + (GetRValue(c) - GetRValue(BG)) * a / 255,
               GetGValue(BG) + (GetGValue(c) - GetGValue(BG)) * a / 255,
               GetBValue(BG) + (GetBValue(c) - GetBValue(BG)) * a / 255);
}

static void DrawButtons(HWND h, HDC dc)
{
    static const wchar_t glyph[NBTN] = {0xE8C3, 0xE7B8, 0xE734, 0xE823};   /* read, archive, star, clock */
    HGDIOBJ of;
    int i;
    if (g_anim) {                       /* clicked: the others fade, it slides to the middle, its icon plays */
        static const wchar_t *times[NBTN] = {L"10 min", L"1 hour", L"3 hours", L"8 hours"};
        int b = g_anim - 1, k;
        double cx, cy, off;
        double el = (double)(GetTickCount64() - g_animStart), p, e, q;
        RECT rc, r = BtnRect(h, b);
        COLORREF fg = BarFade(Gr(255)), bg = BarFade(Gr(48));
        if (el > ANIM_MS) el = ANIM_MS;
        p = el < SLIDE_MS ? el / SLIDE_MS : 1;
        e = 1 - pow(1 - p, 5);          /* quick start, long gentle arrival */
        q = el < SLIDE_MS ? 0 : (el - SLIDE_MS) / (ANIM_MS - SLIDE_MS);   /* after arriving, 0..1 */
        k = el < 250 ? (int)(255 * (1 - el / 250)) : 0;   /* the others are gone in a quarter second */
        GetClientRect(h, &rc);
        for (i = 0; i < NBTN; i++)
            if (i != b) SmoothRound(dc, BtnRect(h, i), DP(8), Mix(BG, BarFade(Gr(22)), k));
        off = ((rc.right - (r.right - r.left)) / 2 - r.left) * e;
        cx = (r.left + r.right) / 2.0 + off; cy = (r.top + r.bottom) / 2.0;   /* icons move by fractions of a pixel */
        OffsetRect(&r, (int)(off + 0.5), 0);
        SmoothRound(dc, r, DP(8), bg);
        SetBkMode(dc, TRANSPARENT);
        of = SelectObject(dc, g_fontIcon);
        if (g_animSnz) {                /* the time label fades into a clock on the way; its hands go round once */
            double f = p < 0.5 ? p * 2 : 1, s2 = el < 100 ? 0 : el > 900 ? 1 : (el - 100) / 800;   /* one turn, home at 0.9 s */
            s2 = s2 < 0.5 ? 2 * s2 * s2 : 1 - 2 * (1 - s2) * (1 - s2);   /* ease in and out */
            DrawClock(dc, cx, cy, DP(7), 6.28318 * s2, Mix(bg, fg, (int)(255 * f)), bg);   /* first: its square is opaque */
            if (f < 1) {
                SelectObject(dc, g_fontSmall);
                SetTextColor(dc, Mix(bg, fg, (int)(255 * (1 - f))));
                DrawTextW(dc, times[b], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        } else if (b == 1) {            /* archive: an envelope drops into the box */
            wchar_t box = 0xE7B8, env = 0xE715;
            int lid = (int)cy - DP(4), sv;   /* lid of the box glyph sits above this line */
            double la = el < 250 ? 1 : el < 400 ? 1 - (el - 250) / 150 : el < 800 ? 0 : el < 900 ? (el - 800) / 100 : 1;
            q = el < 350 ? 0 : el > 800 ? 1 : (el - 350) / 450;   /* drops once the lid is fading */
            sv = SaveDC(dc);
            IntersectClipRect(dc, r.left, lid, r.right, r.bottom);
            GlyphAt(dc, g_fontIcon, box, cx, cy, fg);   /* body */
            RestoreDC(dc, sv);
            sv = SaveDC(dc);
            IntersectClipRect(dc, r.left, r.top, r.right, lid);   /* above the body: gone once inside */
            if (q > 0)
                GlyphAt(dc, g_fontIconS, env, cx, cy - DP(16) + DP(22) * q * q,   /* falls in from above, all the way below the lid */
                        Mix(bg, fg, (int)(255 * (q < 0.25 ? q * 4 : 1))));
            if (la > 0) GlyphAt(dc, g_fontIcon, box, cx, cy, Mix(bg, fg, (int)(255 * la)));   /* lid fades out, then back */
            RestoreDC(dc, sv);
        } else if (b == 2) {            /* star: fills in from the click on */
            wchar_t solid = 0xE735, line = 0xE734;
            GlyphAt(dc, g_fontIcon, solid, cx, cy, Mix(bg, fg, (int)(255 * p)));
            GlyphAt(dc, g_fontIcon, line, cx, cy, fg);
        } else {                        /* read: envelope opens, the letter slides out to the right */
            wchar_t g = 0xE8C3, page = 0xE7C3;
            double m = el > 650 ? 1 : el / 650;   /* starts on the click */
            m = 1 - (1 - m) * (1 - m) * (1 - m);   /* moves at once, settles */
            GlyphAt(dc, g_fontIconS, page, cx + DP(9) * m, cy, Mix(bg, fg, (int)(255 * m)));   /* letter appears right */
            GlyphAt(dc, g_fontIcon, g, cx - DP(8) * m, cy, fg);   /* envelope steps left: pair centered */
        }
        SelectObject(dc, of);
        return;
    }
    if (HASBAR) for (i = 0; i < NBTN; i++) SmoothRound(dc, BtnRect(h, i), DP(8), BarFade(i == g_hot ? Gr(48) : Gr(22)));
    of = SelectObject(dc, g_fontIcon);
    SetBkMode(dc, TRANSPARENT);
    static const wchar_t *times[NBTN] = {L"10 min", L"1 hour", L"3 hours", L"8 hours"};
    if (g_snzMode) SelectObject(dc, g_fontSmall);
    for (i = 0; i < (HASBAR ? NBTN : 0); i++) {
        RECT r = BtnRect(h, i);
        SetTextColor(dc, BarFade(i == g_hot ? Gr(255) : Gr(160)));
        if (g_snzMode) DrawTextW(dc, times[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        else DrawTextW(dc, &glyph[i], 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    {
        RECT r = CloseRect(h);
        static const wchar_t x = 0xE711;    /* ChromeClose */
        COLORREF bg = g_hot == NBTN ? Gr(60) : BG, fg = g_hot == NBTN ? Gr(255) : Gr(140);
        SmoothRound(dc, r, DP(6), HASBAR ? BarFade(bg) : bg);   /* no bar without mail: the cross shows at once */
        SelectObject(dc, g_fontIconS);
        SetTextColor(dc, HASBAR ? BarFade(fg) : fg);
        DrawTextW(dc, &x, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
}

/* Grows the popup by the button bar while hovered; bottom corners grow upward */
static void HoverSize(HWND h, int on)
{
    g_barOn = on;
    SetTimer(h, TIMER_BAR, 10, NULL);   /* the timer slides it */
}

/* Top of the visible part: the window is always tall enough for the bar, a region shows only g_ph + g_bar.
   Resizing a window flashes what is behind it for a frame; changing a region does not. */
static int VisTop(void) { return (g_corner & 2) ? BARH - g_bar : 0; }

static HWND g_shadow;            /* soft shadow under the popup: own per-pixel-alpha window, since region windows get none from DWM */

/* Redraws the shadow for the popup's visible part, at the popup's fade alpha; hides it with the popup */
static void UpdShadow(void)
{
    int S = DP(14), R = DP(8), off = DP(4), w, hh, x, y;
    RECT wr;
    BITMAPINFO bi;
    DWORD *px;
    HDC sdc, mdc;
    HBITMAP bmp;
    HGDIOBJ ob;
    POINT src = {0, 0}, dst;
    SIZE sz;
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 0, AC_SRC_ALPHA};
    if (!g_shadow) return;
    if (!IsWindowVisible(g_popup) || g_alpha <= 0) { ShowWindow(g_shadow, SW_HIDE); return; }
    GetWindowRect(g_popup, &wr);
    w = g_pw + 2 * S;
    hh = g_ph + g_bar + 2 * S;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    sdc = GetDC(NULL);
    mdc = CreateCompatibleDC(sdc);
    bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp) { DeleteDC(mdc); ReleaseDC(NULL, sdc); return; }
    for (y = 0; y < hh; y++)
        for (x = 0; x < w; x++) {
            /* distance from the rounded rect (shifted down by off), fading out over S */
            double qx = fabs(x + 0.5 - w / 2.0) - (g_pw / 2.0 - R), qy = fabs(y + 0.5 - off - hh / 2.0) - ((g_ph + g_bar) / 2.0 - R);
            double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, d = sqrt(ox * ox + oy * oy) - R, t;
            int a;
            t = d <= 0 ? 1 : d >= S ? 0 : 1 - d / S;
            a = (int)(110 * t * t);
            px[y * w + x] = (DWORD)a << 24;     /* black, premultiplied */
        }
    ob = SelectObject(mdc, bmp);
    dst.x = wr.left - S;
    dst.y = wr.top + VisTop() - S;
    sz.cx = w;
    sz.cy = hh;
    bf.SourceConstantAlpha = (BYTE)g_alpha;
    UpdateLayeredWindow(g_shadow, sdc, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(g_shadow, g_popup, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);   /* just under the popup */
    SelectObject(mdc, ob);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

static void ApplyBar(HWND h)
{
    int y0 = VisTop();
    SetWindowRgn(h, CreateRoundRectRgn(0, y0, g_pw + 1, y0 + g_ph + g_bar + 1, DP(16), DP(16)), TRUE);
    UpdShadow();
}

/* R mark read, E or A archive, S star, Z snooze (then 1-4 pick the time), Esc close: only while the mouse is on the popup (it never takes focus, so hotkeys) */
/* Tooltip over the popup's bar buttons; b = button under the mouse, -1 hides it */
static void BtnTip(HWND h, int b)
{
    static const wchar_t *act[NBTN] = {L"Mark as read (R)", L"Archive (A)", L"Star (S)", L"Snooze (Z)"};
    static const wchar_t *snz[NBTN] = {L"10 minutes (1)", L"1 hour (2)", L"3 hours (3)", L"8 hours (4)"};
    static HWND tip;
    static TTTOOLINFOW ti;
    RECT r;
    if (!tip) {
        int round = 2 /* DWMWCP_ROUND */;
        COLORREF edge = Gr(75);
        RECT m = {DP(6), DP(3), DP(6), DP(3)};
        tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              0, 0, 0, 0, h, NULL, g_inst, NULL);
        SetWindowTheme(tip, L"", L"");         /* unthemed, so our colors apply */
        DwmSetWindowAttribute(tip, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
        DwmSetWindowAttribute(tip, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof edge);
        SendMessageW(tip, WM_SETFONT, (WPARAM)g_fontSmall, FALSE);
        SendMessageW(tip, TTM_SETMARGIN, 0, (LPARAM)&m);
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.hwnd = h;
        ti.lpszText = L"";
        SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    if (b < 0 || b >= NBTN) { SendMessageW(tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); return; }
    SendMessageW(tip, TTM_SETTIPBKCOLOR, Gr(43), 0);    /* every time: light/dark may have switched */
    SendMessageW(tip, TTM_SETTIPTEXTCOLOR, Gr(225), 0);
    ti.lpszText = (wchar_t *)(g_snzMode ? snz[b] : act[b]);
    SendMessageW(tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    r = BtnRect(h, b);
    OffsetRect(&r, 0, VisTop());
    MapWindowPoints(h, NULL, (POINT *)&r, 2);
    SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(r.left, r.bottom + DP(4)));
    SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

static void Keys(HWND h, int on)
{
    static const UINT vk[] = {'E', 'S', 'Z', VK_ESCAPE, '1', '2', '3', '4', 'R', 'A'};
    int i;
    for (i = 0; i < 10; i++) {
        UnregisterHotKey(h, i + 1);
        if (on) RegisterHotKey(h, i + 1, MOD_NOREPEAT, vk[i]);   /* taken by another app: that key just does nothing here */
    }
}

static void HidePopup(HWND h)
{
    BtnTip(h, -1);
    KillTimer(h, TIMER_HIDE);
    KillTimer(h, TIMER_FADE);
    KillTimer(h, TIMER_FADEIN);
    KillTimer(h, TIMER_BAR);
    KillTimer(h, TIMER_ANIM);
    g_anim = 0;
    g_bar = 0;
    g_alpha = 255;
    ShowWindow(h, SW_HIDE);
    UpdShadow();
    Keys(h, 0);
    PostMessageW(g_main, WM_NEXTMAIL, 0, 0);
}

LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc, tr;
        HBRUSH b;
        HPEN pen;
        HGDIOBJ ob, op, of;
        HDC pdc = BeginPaint(h, &ps), dc;
        HBITMAP buf;
        HGDIOBJ obuf;
        GetClientRect(h, &rc);
        rc.bottom = g_ph + g_bar;               /* visible part only */
        dc = CreateCompatibleDC(pdc);           /* draw off screen, then copy once: no flicker */
        buf = CreateCompatibleBitmap(pdc, rc.right, rc.bottom);
        obuf = SelectObject(dc, buf);
        b = CreateSolidBrush(BG);
        pen = g_dwm ? CreatePen(PS_NULL, 0, 0) : CreatePen(PS_INSIDEFRAME, 1, Gr(80));
        ob = SelectObject(dc, b);
        op = SelectObject(dc, pen);
        RoundRect(dc, 0, 0, rc.right + (g_dwm ? 1 : 0), rc.bottom + (g_dwm ? 1 : 0), DP(16), DP(16));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(pen);

        {
            int pad = DP(PAD), av = DP(AV), ch = g_ph;
            int tb = g_h1 + g_h2 + (g_to[0] ? g_h3 : 0) + (g_prev[0] ? g_h3 : 0), ty = (ch - tb) / 2;
            DrawAvatar(dc, pad, (ch - av) / 2, av, g_text, Prov(g_cur.acc ? g_cur.acc->email : ""));
            SetBkMode(dc, TRANSPARENT);
            tr.left = pad + av + DP(GAP); tr.right = rc.right - pad;
            of = SelectObject(dc, g_font);
            SetTextColor(dc, Gr(190));
            tr.top = ty; tr.bottom = ty + g_h1;
            {
                wchar_t tm[48] = L"";
                RECT nr = tr;
                int tw = 0;
                if (g_cur.when) {               /* " · 15:33", or " · Nov 12, 15:33" if not today */
                    FILETIME u = {(DWORD)g_cur.when, (DWORD)(g_cur.when >> 32)}, lf;
                    SYSTEMTIME lt, now;
                    wchar_t dt[24] = L"", hm[16];
                    FileTimeToLocalFileTime(&u, &lf);
                    FileTimeToSystemTime(&lf, &lt);
                    GetLocalTime(&now);
                    if (lt.wYear != now.wYear || lt.wMonth != now.wMonth || lt.wDay != now.wDay)
                        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &lt, L"MMM d, ", dt, 24, NULL);
                    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &lt, NULL, hm, 16);
                    swprintf_s(tm, 48, L"  \x00b7  %s%s", dt, hm);
                    tw = TextW(g_font, tm);
                }
                nr.right -= tw + DP(20);        /* room for the time and the close cross */
                DrawTextW(dc, g_text, -1, &nr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_CALCRECT);
                if (nr.right > tr.right - tw - DP(20)) nr.right = tr.right - tw - DP(20);
                DrawTextW(dc, g_text, -1, &nr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (tw) {
                    RECT r2 = {nr.right, tr.top, nr.right + tw, tr.bottom};
                    SetTextColor(dc, Gr(130));
                    DrawTextW(dc, tm, -1, &r2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
            SelectObject(dc, g_subjHot ? g_fontBigU : g_fontBig);
            SetTextColor(dc, Gr(255));
            tr.top = tr.bottom; tr.bottom = tr.top + g_h2;
            DrawTextW(dc, g_subj, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            g_subjRect = tr;            /* hit area: only as wide as the text */
            DrawTextW(dc, g_subj, -1, &g_subjRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_CALCRECT);
            if (g_subjRect.right > tr.right) g_subjRect.right = tr.right;
            if (g_prev[0]) {
                SelectObject(dc, g_fontSmall);
                SetTextColor(dc, Gr(165));
                int more = g_bar < g_more ? g_bar : g_more;   /* opens and closes with the bar, a pixel at a time */
                int k = g_more ? (g_bar - g_more) * 255 / DP(BAR) : 0;   /* the long text fades while the buttons slide, so it is gone before the space shrinks */
                if (k < 0) k = 0;
                tr.top = tr.bottom; tr.bottom = tr.top + g_h3 + g_more;
                SaveDC(dc);
                IntersectClipRect(dc, tr.left, tr.top, tr.right, tr.top + g_h3 + more);
                if (k < 255) {                  /* one line with "...", crossfaded with the long text */
                    RECT one = tr;
                    one.bottom = one.top + g_h3;
                    SetTextColor(dc, Mix(BG, Gr(165), 255 - k));
                    DrawTextW(dc, g_prev, -1, &one, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX | DT_SINGLELINE);
                }
                if (k > 0) {
                    SetTextColor(dc, Mix(BG, Gr(165), k));
                    DrawTextW(dc, g_prev, -1, &tr, DT_LEFT | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL);
                }
                RestoreDC(dc, -1);
                tr.bottom = tr.top + g_h3 + more;   /* the address line follows the edge */
            }
            if (g_to[0]) {                      /* faint: which address got it */
                SelectObject(dc, g_fontSmall);
                SetTextColor(dc, Gr(125));
                tr.top = tr.bottom; tr.bottom = tr.top + g_h3;
                DrawTextW(dc, g_to, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        SelectObject(dc, of);
        if (g_bar || (g_hover && !HASBAR)) DrawButtons(h, dc);
        BitBlt(pdc, 0, VisTop(), rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, obuf);
        DeleteObject(buf);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC) ImapKick();
        break;
    case WM_LBUTTONDOWN: {
        int b = BtnAt(h, (short)LOWORD(l), (short)HIWORD(l) - VisTop());
        POINT pt = {(short)LOWORD(l), (short)HIWORD(l) - VisTop()};
        if (b >= 0) { PopupAction(b); return 0; }
        if (!PtInRect(&g_subjRect, pt)) return 0;   /* only the subject opens the mail */
        ShellExecuteW(NULL, L"open", g_mailUrl, NULL, NULL, SW_SHOWNORMAL);
        HidePopup(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int b = BtnAt(h, (short)LOWORD(l), (short)HIWORD(l) - VisTop());
        POINT pt = {(short)LOWORD(l), (short)HIWORD(l) - VisTop()};
        int sh = PtInRect(&g_subjRect, pt);
        if (b != g_hot || sh != g_subjHot) { g_hot = b; g_subjHot = sh; BtnTip(h, -1); if (b >= 0 && !g_anim && g_alpha == 255) SetTimer(h, TIMER_TIP, 1000, NULL); else KillTimer(h, TIMER_TIP); InvalidateRect(h, NULL, FALSE); }
        if (!g_hover) {                         /* hold the popup while the mouse is on it */
            TRACKMOUSEEVENT te;
            te.cbSize = sizeof te;
            te.dwFlags = TME_LEAVE;
            te.hwndTrack = h;
            te.dwHoverTime = 0;
            TrackMouseEvent(&te);
            g_hover = 1;
            Keys(h, 1);
            KillTimer(h, TIMER_HIDE);
            KillTimer(h, TIMER_FADE);
            KillTimer(h, TIMER_FADEIN);
            g_alpha = 255;
            SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
            UpdShadow();
            if (HASBAR) HoverSize(h, 1);     /* quiet-hours summary: nothing to act on */
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, g_subjHot || g_hot >= 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_RBUTTONUP:
        HidePopup(h);                           /* right-click anywhere closes it */
        return 0;
    case WM_ERASEBKGND:
        return 1;                               /* WM_PAINT covers every pixel */
    case WM_HOTKEY:
        if (w == 4) HidePopup(h);
        else if (w >= 5 && w <= 8) { if (g_snzMode) PopupAction((int)w - 5); }   /* 1-4: snooze times */
        else if ((w <= 3 || w >= 9) && HASBAR) {
            int a = w == 9 ? 0 : w == 10 ? 1 : (int)w;   /* 0 read, 1 archive, 2 star, 3 snooze */
            if (a != 3) g_snzMode = 0;  /* R, E, A and S act even while the times show */
            PopupAction(a);
        }
        return 0;
    case WM_MOUSELEAVE:
        Keys(h, 0);
        g_hover = 0;
        g_hot = -1;
        BtnTip(h, -1);
        g_snzMode = 0;
        g_subjHot = 0;
        if (IsWindowVisible(h) && !g_anim) HoverSize(h, 0);   /* keep the bar for the animation */
        InvalidateRect(h, NULL, FALSE);
        if (IsWindowVisible(h)) SetTimer(h, TIMER_HIDE, 2000, NULL);
        return 0;
    case WM_TIMER:
        if (w == TIMER_TIP) { KillTimer(h, TIMER_TIP); if (g_hover && !g_anim && g_alpha == 255) BtnTip(h, g_hot); return 0; }
        if (w == TIMER_BAR) {               /* ease out: a third of the way each tick, about 0.15 s */
            int t = g_barOn ? BARH : 0, d = (t - g_bar) / 3;
            if (!d) d = t > g_bar ? 1 : -1;
            g_bar = t == g_bar ? t : g_bar + d;
            if (g_bar == t) KillTimer(h, TIMER_BAR);
            ApplyBar(h);
            InvalidateRect(h, NULL, FALSE);
        } else if (w == TIMER_HIDE) {
            KillTimer(h, TIMER_HIDE);
            SetTimer(h, TIMER_FADE, 30, NULL);
        } else if (w == TIMER_ANIM) {
            if (GetTickCount64() - g_animStart >= ANIM_MS) { KillTimer(h, TIMER_ANIM); SetTimer(h, TIMER_FADE, 30, NULL); }
            InvalidateRect(h, NULL, FALSE);
        } else if (w == TIMER_FADEIN) {
            g_alpha += 32;                      /* about 0.13 s */
            if (g_alpha >= 255) { g_alpha = 255; KillTimer(h, TIMER_FADEIN); }
            SetLayeredWindowAttributes(h, 0, (BYTE)g_alpha, LWA_ALPHA);
            SetWindowPos(h, HWND_TOPMOST, g_px, g_py - ((g_corner & 2) ? BARH : 0) + ((g_corner & 2) ? 1 : -1) * (255 - g_alpha) * DP(10) / 255, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
            UpdShadow();
        } else if (w == TIMER_FADE) {
            g_alpha -= 15;                      /* about 0.45 s fade */
            if (g_alpha <= 0) {
                HidePopup(h);
            } else {
                SetLayeredWindowAttributes(h, 0, (BYTE)g_alpha, LWA_ALPHA);
                UpdShadow();
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* "Segoe UI Variable" on Windows 11, "Segoe UI" where it is missing */
static HFONT MakeFont(int h, int weight)
{
    wchar_t face[64];
    HDC dc = GetDC(NULL);
    HFONT f = CreateFontW(h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    HGDIOBJ old = SelectObject(dc, f);
    GetTextFaceW(dc, 64, face);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    if (wcsncmp(face, L"Segoe UI Variable", 17) == 0) return f;
    DeleteObject(f);
    return CreateFontW(h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static int LineH(HFONT f)
{
    TEXTMETRICW tm;
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, f);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return tm.tmHeight;
}

static int TextW(HFONT f, const wchar_t *t)
{
    RECT r = {0, 0, 0, 0};
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return r.right - r.left;
}

static void ShowPopup(const wchar_t *from, const wchar_t *subj, const wchar_t *to, const wchar_t *prev)
{
    RECT wa;
    int w, x, y, ph;
    HRGN rgn;

    wcsncpy_s(g_text, 512, from, _TRUNCATE);
    wcsncpy_s(g_subj, 512, subj, _TRUNCATE);
    g_to[0] = 0;
    if (to) wcsncpy_s(g_to, 300, to, _TRUNCATE);
    g_prev[0] = 0;
    if (prev) wcsncpy_s(g_prev, 300, prev, _TRUNCATE);

    w = DP(380);                /* fixed: same size for every mail; long text gets "..." */
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (w > (wa.right - wa.left) / 2) w = (wa.right - wa.left) / 2;
    ph = g_h1 + g_h2 + (g_to[0] ? g_h3 : 0) + (g_prev[0] ? g_h3 : 0);
    if (ph < DP(AV)) ph = DP(AV);
    g_more = 0;
    if (g_prev[0]) {            /* hovered, the preview opens up to 4 lines */
        RECT r = {0, 0, w - 2 * DP(PAD) - DP(AV) - DP(GAP), 0}, one = r;
        HDC dc = GetDC(NULL);
        HGDIOBJ o = SelectObject(dc, g_fontSmall);
        DrawTextW(dc, g_prev, -1, &r, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(dc, L"X", 1, &one, DT_NOPREFIX | DT_CALCRECT);
        SelectObject(dc, o);
        ReleaseDC(NULL, dc);
        if (one.bottom > 0) g_more = (min(r.bottom / one.bottom, 4) - 1) * g_h3;
        if (g_more < 0) g_more = 0;
    }
    ph += 2 * DP(PAD);

    x = (g_corner & 1) ? wa.left + DP(16) : wa.right - w - DP(16);
    y = (g_corner & 2) ? wa.bottom - ph - DP(16) : wa.top + DP(16);

    g_px = x; g_py = y;
    g_pw = w; g_ph = ph;
    if (g_corner & 2) y -= BARH;        /* room for the bar; ApplyBar's region hides it until hovered */
    ph += BARH;
    (void)rgn;
    KillTimer(g_popup, TIMER_FADE);
    g_alpha = 0;
    SetLayeredWindowAttributes(g_popup, 0, 0, LWA_ALPHA);
    ApplyBar(g_popup);
    SetWindowPos(g_popup, HWND_TOPMOST, x, y + ((g_corner & 2) ? DP(10) : -DP(10)), w, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_popup, NULL, TRUE);
    SetTimer(g_popup, TIMER_FADEIN, 15, NULL);
    if (!g_hover) SetTimer(g_popup, TIMER_HIDE, g_popupMs, NULL);
}



/* ---------- IMAP IDLE (push) ---------- */


typedef struct {
    SOCKET s;
    CredHandle cred;
    CtxtHandle ctx;
    int haveCred, haveCtx;
    SecPkgContext_StreamSizes sz;
    char *sendBuf;
    char in[32768];                 /* encrypted bytes from the socket */
    int inLen;
    char plain[32768];              /* decrypted bytes not yet read */
    int plainLen, plainPos;
    int waitMs;                     /* read timeout */
    Account *acc;
} Tls;


static void ImapKick(void)
{
    int i;
    for (i = 0; i < g_nacc; i++) {
        SOCKET s = g_acc[i].sock;
        if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);   /* the IMAP thread sees the error and reconnects */
    }
}

static void SetTip(const wchar_t *t)
{
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_TIP;
    wcscpy_s(n.szTip, 128, t);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static int SendAll(SOCKET s, const char *d, int n)
{
    while (n > 0) {
        int r = send(s, d, n, 0);
        if (r <= 0) return 0;
        d += r;
        n -= r;
    }
    return 1;
}

static void TlsClose(Tls *t)
{
    if (t->haveCtx) DeleteSecurityContext(&t->ctx);
    if (t->haveCred) FreeCredentialsHandle(&t->cred);
    t->haveCtx = t->haveCred = 0;
    t->acc->sock = INVALID_SOCKET;
    if (t->s != INVALID_SOCKET) closesocket(t->s);
    t->s = INVALID_SOCKET;
}

/* Tray hover card: our own tooltip, drawn like the popup. Total unread, then one row per enabled account. */
static HWND g_card;
static POINT g_cardAt;              /* the tray icon, from NIN_POPUPOPEN */
#define CARD_W 260
#define CARD_ROW 22

static int CardRows(void)
{
    int i, n = 0;
    for (i = 0; i < g_nacc; i++) n += g_acc[i].enabled;
    return n;
}

static void CardPaint(HWND h, HDC dc)
{
    static const wchar_t *names[] = {L"connecting", L"", L"offline", L"sign-in failed"};
    static HFONT big, row;
    static int bigDpi;
    RECT rc, t;
    HBRUSH br;
    HPEN pen;
    HGDIOBJ op, ob, of;
    wchar_t s[300];
    int i, y, unread = 0;
    if (!big || bigDpi != g_dpi) { if (big) DeleteObject(big), DeleteObject(row);
        big = MakeFont(DP(-20), FW_SEMIBOLD); row = MakeFont(DP(-12), FW_NORMAL); bigDpi = g_dpi; }
    GetClientRect(h, &rc);
    br = CreateSolidBrush(BG); FillRect(dc, &rc, br); DeleteObject(br);
    pen = CreatePen(PS_SOLID, 1, Gr(60));   /* hairline edge, like the popup */
    op = SelectObject(dc, pen); ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 0, 0, rc.right, rc.bottom, DP(16), DP(16));
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
    for (i = 0; i < g_nacc; i++) if (g_acc[i].enabled && g_acc[i].state == 1) unread += g_acc[i].unread;
    SetBkMode(dc, TRANSPARENT);
    of = SelectObject(dc, big);
    SetTextColor(dc, Gr(255));
    swprintf_s(s, 300, unread ? L"%d unread" : L"No unread mail", unread);
    SetRect(&t, DP(16), DP(10), rc.right - DP(16), DP(40));
    DrawTextW(dc, s, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, row);
    for (i = 0, y = DP(44); i < g_nacc; i++) {
        Account *a = &g_acc[i];
        COLORREF dot = a->state == 1 ? RGB(76, 194, 108) : a->state == 3 ? RGB(232, 72, 72) : Gr(110);
        HBRUSH db;
        RECT d;
        if (!a->enabled) continue;
        SetRect(&d, DP(16), y + DP(CARD_ROW) / 2 - DP(4), DP(16) + DP(8), y + DP(CARD_ROW) / 2 + DP(4));
        db = CreateSolidBrush(dot);
        pen = CreatePen(PS_SOLID, 1, dot);
        op = SelectObject(dc, pen); ob = SelectObject(dc, db);
        Ellipse(dc, d.left, d.top, d.right, d.bottom);   /* green connected, gray offline or connecting, red sign-in failed */
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen); DeleteObject(db);
        if (a->state == 1) swprintf_s(s, 300, L"%d", a->unread);
        else wcscpy_s(s, 300, names[a->state & 3]);
        SetRect(&t, DP(32), y, rc.right - DP(16), y + DP(CARD_ROW));
        SetTextColor(dc, a->state == 1 && a->unread ? Gr(255) : Gr(150));
        DrawTextW(dc, s, -1, &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        t.right -= DP(60);              /* room for the count */
        swprintf_s(s, 300, L"%hs", a->nick[0] ? a->nick : a->email);
        SetTextColor(dc, Gr(210));
        DrawTextW(dc, s, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        y += DP(CARD_ROW);
    }
    SelectObject(dc, of);
}

static LRESULT CALLBACK CardProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        CardPaint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

/* Show (or refit) the card next to the tray icon at pt, inside that screen's work area. */
static void CardShow(POINT pt)
{
    MONITORINFO mi = {sizeof mi};
    int w = DP(CARD_W), hh = DP(52) + DP(CARD_ROW) * CardRows(), x, y;
    g_cardAt = pt;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    x = pt.x - w / 2;
    if (x + w > mi.rcWork.right - DP(8)) x = mi.rcWork.right - DP(8) - w;
    if (x < mi.rcWork.left + DP(8)) x = mi.rcWork.left + DP(8);
    y = pt.y > (mi.rcWork.top + mi.rcWork.bottom) / 2 ? mi.rcWork.bottom - DP(8) - hh : mi.rcWork.top + DP(8);   /* taskbar below or above */
    if (pt.y > mi.rcWork.top && pt.y < mi.rcWork.bottom) {   /* in the hidden-icons flyout: go above the icon, not behind the flyout */
        y = pt.y - DP(24) - hh;
        if (y < mi.rcWork.top + DP(8)) y = pt.y + DP(24);
    }
    if (!g_card) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpfnWndProc = CardProc;
        wc.lpszClassName = L"GNCard";
        RegisterClassExW(&wc);
        g_card = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"GNCard", L"", WS_POPUP,
                                 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    }
    SetWindowRgn(g_card, CreateRoundRectRgn(0, 0, w + 1, hh + 1, DP(16), DP(16)), FALSE);
    SetWindowPos(g_card, HWND_TOPMOST, x, y, w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_card, NULL, TRUE);
}

/* Tray tooltip: one line per enabled account (screen readers; the card is what's shown). */
static void UpdateTip(void)
{
    static const wchar_t *names[] = {L"connecting", L"connected", L"offline, retrying", L"login failed"};
    wchar_t tip[128] = L"pigeon", line[128];
    int i, unread = 0, off = 0, bad = 0;
    for (i = 0; i < g_nacc; i++) {
        if (!g_acc[i].enabled) continue;
        unread += g_acc[i].state == 1 ? g_acc[i].unread : 0;
        off += g_acc[i].state == 2;
        bad += g_acc[i].state == 3;
    }
    swprintf_s(tip, 128, L"pigeon \x00b7 %d unread", unread);
    if (off) { swprintf_s(line, 128, L" \x00b7 %d offline", off); wcsncat_s(tip, 128, line, _TRUNCATE); }
    if (bad) { swprintf_s(line, 128, L" \x00b7 %d sign-in failed", bad); wcsncat_s(tip, 128, line, _TRUNCATE); }
    for (i = 0; i < g_nacc; i++) {
        if (!g_acc[i].enabled) continue;
        if (g_acc[i].state == 1) swprintf_s(line, 128, L"\n%hs: %d unread", g_acc[i].email, g_acc[i].unread);
        else swprintf_s(line, 128, L"\n%hs: %s", g_acc[i].email, names[g_acc[i].state & 3]);
        wcsncat_s(tip, 128, line, _TRUNCATE);
    }
    SetTip(tip);
    if (g_card && IsWindowVisible(g_card)) CardShow(g_cardAt);   /* open: follow the change */
}

static void SetState(Account *a, int st)
{
    a->state = st;
    UpdateTip();
    PostMessageW(g_main, WM_UNREAD, 0, 0);
}

static void SetBuf(SecBuffer *b, unsigned long type, void *p, unsigned long n)
{
    b->BufferType = type;
    b->pvBuffer = p;
    b->cbBuffer = n;
}

/* TCP connect + TLS handshake; Windows checks the certificate against host. */
static int TlsConnect(Tls *t, const char *host, const char *port)
{
    struct addrinfo hints, *ai = NULL, *a;
    SCHANNEL_CRED sc;
    SecBuffer ob[1], ib[2];
    SecBufferDesc od, idd;
    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                  ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM, outFlags;
    SECURITY_STATUS st;
    TimeStamp ts;
    wchar_t whost[256];
    DWORD tmo = 30000;
    int ok;

    t->s = INVALID_SOCKET;
    t->inLen = t->plainLen = t->plainPos = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0) return 0;
    for (a = ai; a; a = a->ai_next) {
        t->s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (t->s == INVALID_SOCKET) continue;
        if (connect(t->s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        closesocket(t->s);
        t->s = INVALID_SOCKET;
    }
    freeaddrinfo(ai);
    if (t->s == INVALID_SOCKET) return 0;
    t->acc->sock = t->s;
    setsockopt(t->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);

    memset(&sc, 0, sizeof sc);
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    /* shortcut: TLS 1.2 only, since 1.3 post-handshake messages need extra handling; upgrade if Gmail drops 1.2 */
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_DEFAULT_CREDS;
    if (AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &t->cred, &ts) != SEC_E_OK)
        return 0;
    t->haveCred = 1;
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);

    SetBuf(&ob[0], SECBUFFER_TOKEN, NULL, 0);
    od.ulVersion = SECBUFFER_VERSION; od.cBuffers = 1; od.pBuffers = ob;
    st = InitializeSecurityContextW(&t->cred, NULL, whost, flags, 0, 0, NULL, 0, &t->ctx, &od, &outFlags, NULL);
    if (st != SEC_I_CONTINUE_NEEDED) return 0;
    t->haveCtx = 1;
    ok = SendAll(t->s, ob[0].pvBuffer, ob[0].cbBuffer);
    FreeContextBuffer(ob[0].pvBuffer);
    if (!ok) return 0;

    while (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_INCOMPLETE_MESSAGE) {
        if (t->inLen == 0 || st == SEC_E_INCOMPLETE_MESSAGE) {
            int r;
            if (t->inLen >= (int)sizeof t->in) return 0;
            r = recv(t->s, t->in + t->inLen, (int)sizeof t->in - t->inLen, 0);
            if (r <= 0) return 0;
            t->inLen += r;
        }
        SetBuf(&ib[0], SECBUFFER_TOKEN, t->in, t->inLen);
        SetBuf(&ib[1], SECBUFFER_EMPTY, NULL, 0);
        idd.ulVersion = SECBUFFER_VERSION; idd.cBuffers = 2; idd.pBuffers = ib;
        SetBuf(&ob[0], SECBUFFER_TOKEN, NULL, 0);
        st = InitializeSecurityContextW(&t->cred, &t->ctx, whost, flags, 0, 0, &idd, 0, NULL, &od, &outFlags, NULL);
        if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
        if (ob[0].cbBuffer && ob[0].pvBuffer) {
            ok = SendAll(t->s, ob[0].pvBuffer, ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (!ok) return 0;
        }
        if (FAILED(st)) return 0;
        if (ib[1].BufferType == SECBUFFER_EXTRA) {
            memmove(t->in, t->in + t->inLen - ib[1].cbBuffer, ib[1].cbBuffer);
            t->inLen = ib[1].cbBuffer;
        } else {
            t->inLen = 0;
        }
    }
    if (st != SEC_E_OK) return 0;
    if (QueryContextAttributesW(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sz) != SEC_E_OK) return 0;
    free(t->sendBuf);
    t->sendBuf = malloc(t->sz.cbHeader + t->sz.cbMaximumMessage + t->sz.cbTrailer);
    return t->sendBuf != NULL;
}

static int TlsSend(Tls *t, const char *d, int n)
{
    while (n > 0) {
        int c = n < (int)t->sz.cbMaximumMessage ? n : (int)t->sz.cbMaximumMessage;
        char *buf = t->sendBuf;
        SecBuffer b[4];
        SecBufferDesc desc;
        memcpy(buf + t->sz.cbHeader, d, c);
        SetBuf(&b[0], SECBUFFER_STREAM_HEADER, buf, t->sz.cbHeader);
        SetBuf(&b[1], SECBUFFER_DATA, buf + t->sz.cbHeader, c);
        SetBuf(&b[2], SECBUFFER_STREAM_TRAILER, buf + t->sz.cbHeader + c, t->sz.cbTrailer);
        SetBuf(&b[3], SECBUFFER_EMPTY, NULL, 0);
        desc.ulVersion = SECBUFFER_VERSION; desc.cBuffers = 4; desc.pBuffers = b;
        if (EncryptMessage(&t->ctx, 0, &desc, 0) != SEC_E_OK) return 0;
        if (!SendAll(t->s, buf, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return 0;
        d += c;
        n -= c;
    }
    return 1;
}

/* Refills t->plain. 1 = data, -1 = error/closed, -2 = timeout (t->waitMs). */
static int TlsFill(Tls *t)
{
    for (;;) {
        fd_set fs;
        struct timeval tv;
        int r, i;
        if (t->inLen > 0) {
            SecBuffer b[4];
            SecBufferDesc d;
            SECURITY_STATUS st;
            int extra = 0;
            SetBuf(&b[0], SECBUFFER_DATA, t->in, t->inLen);
            for (i = 1; i < 4; i++) SetBuf(&b[i], SECBUFFER_EMPTY, NULL, 0);
            d.ulVersion = SECBUFFER_VERSION; d.cBuffers = 4; d.pBuffers = b;
            st = DecryptMessage(&t->ctx, &d, 0, NULL);
            if (st == SEC_E_OK) {
                t->plainLen = t->plainPos = 0;
                for (i = 0; i < 4; i++) {
                    if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer <= sizeof t->plain) {
                        memcpy(t->plain, b[i].pvBuffer, b[i].cbBuffer);
                        t->plainLen = b[i].cbBuffer;
                    }
                    if (b[i].BufferType == SECBUFFER_EXTRA) extra = b[i].cbBuffer;
                }
                if (extra) memmove(t->in, t->in + t->inLen - extra, extra);
                t->inLen = extra;
                if (t->plainLen > 0) return 1;
                continue;
            }
            if (st != SEC_E_INCOMPLETE_MESSAGE) return -1;
        }
        FD_ZERO(&fs);
        FD_SET(t->s, &fs);
        tv.tv_sec = t->waitMs / 1000;
        tv.tv_usec = (t->waitMs % 1000) * 1000;
        r = select(0, &fs, NULL, NULL, &tv);
        if (r == 0) return -2;
        if (r < 0 || t->inLen >= (int)sizeof t->in) return -1;
        r = recv(t->s, t->in + t->inLen, (int)sizeof t->in - t->inLen, 0);
        if (r <= 0) return -1;
        t->inLen += r;
    }
}

static int TlsGetc(Tls *t)
{
    if (t->plainPos >= t->plainLen) {
        int r = TlsFill(t);
        if (r <= 0) return r == -2 ? -2 : -1;
    }
    return (unsigned char)t->plain[t->plainPos++];
}

/* One line without CRLF. Returns length, -1 error, -2 timeout. */
static int ReadLine(Tls *t, char *buf, int cap)
{
    int n = 0, c;
    for (;;) {
        c = TlsGetc(t);
        if (c < 0) return c;
        if (c == '\n') break;
        if (n < cap - 1) buf[n++] = (char)c;
    }
    if (n && buf[n - 1] == '\r') n--;
    buf[n] = 0;
    return n;
}

/* Reads exactly n bytes (an IMAP literal), keeping the first cap-1. */
static int ReadBytes(Tls *t, char *buf, int n, int cap)
{
    int i, c, k = 0;
    for (i = 0; i < n; i++) {
        c = TlsGetc(t);
        if (c < 0) return -1;
        if (k < cap - 1) buf[k++] = (char)c;
    }
    buf[k] = 0;
    return k;
}

static int Cmd(Tls *t, const char *cmd, char *tag)
{
    char buf[1024];
    sprintf_s(tag, 16, "a%d", ++t->acc->tag);
    int r;
    sprintf_s(buf, sizeof buf, "%s %s\r\n", tag, cmd);
    r = TlsSend(t, buf, (int)strlen(buf));
    SecureZeroMemory(buf, sizeof buf);   /* may hold LOGIN */
    return r;
}

/* Reads up to the tagged reply. 1 = OK, 0 = NO/BAD, -1 = connection error. Picks up [UIDNEXT n] if asked. */
static int WaitOk(Tls *t, const char *tag, unsigned long *uidnext)
{
    char line[2048];
    size_t tl = strlen(tag);
    for (;;) {
        const char *p;
        if (ReadLine(t, line, sizeof line) < 0) return -1;
        if (uidnext && (p = strstr(line, "[UIDNEXT ")) != NULL) *uidnext = strtoul(p + 9, NULL, 10);
        if (strncmp(line, tag, tl) == 0 && line[tl] == ' ') return strncmp(line + tl + 1, "OK", 2) == 0;
    }
}

static int B64(const char *s, int len, char *o, int cap)
{
    int i, n = 0, bits = 0;
    unsigned v = 0;
    for (i = 0; i < len; i++) {
        char c = s[i];
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else continue;
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) o[n++] = (char)((v >> bits) & 0xFF);
        }
    }
    return n;
}

static int QDec(const char *s, int len, char *o, int cap)
{
    int i, n = 0;
    for (i = 0; i < len && n < cap; i++) {
        if (s[i] == '_') o[n++] = ' ';
        else if (s[i] == '=' && i + 2 < len) {
            unsigned v = 0;
            sscanf_s(s + i + 1, "%2x", &v);
            o[n++] = (char)v;
            i += 2;
        } else o[n++] = s[i];
    }
    return n;
}

/* Header value to text, decoding =?charset?B|Q?...?= words (RFC 2047). */
static void DecodeMime(const char *p, wchar_t *out, int cap)
{
    int n = 0, lastEnc = 0;
    while (*p && n < cap - 1) {
        if (p[0] == '=' && p[1] == '?') {
            const char *cs = p + 2, *q = strchr(cs, '?');
            if (q && q[1] && q[2] == '?') {
                char enc = (char)(q[1] | 0x20);
                const char *txt = q + 3, *end = strstr(txt, "?=");
                if (end && (enc == 'b' || enc == 'q')) {
                    char raw[1024];
                    UINT cp = CP_UTF8;
                    int rn;
                    if (!_strnicmp(cs, "iso-8859-1", 10) || !_strnicmp(cs, "windows-1252", 12) || !_strnicmp(cs, "us-ascii", 8)) cp = 1252;
                    rn = enc == 'b' ? B64(txt, (int)(end - txt), raw, sizeof raw) : QDec(txt, (int)(end - txt), raw, sizeof raw);
                    n += MultiByteToWideChar(cp, 0, raw, rn, out + n, cap - n - 1);
                    p = end + 2;
                    lastEnc = 1;
                    continue;
                }
            }
        }
        if (lastEnc) {                      /* blanks between two encoded words are dropped */
            const char *w = p;
            while (*w == ' ' || *w == '\t') w++;
            if (w != p && w[0] == '=' && w[1] == '?') { p = w; continue; }
        }
        lastEnc = 0;
        {
            const char *st = p++;
            while (*p && !(p[0] == '=' && p[1] == '?')) p++;
            n += MultiByteToWideChar(CP_UTF8, 0, st, (int)(p - st), out + n, cap - n - 1);
        }
    }
    out[n] = 0;
}

/* Value of "key <number>" in an IMAP FETCH line, e.g. "UID 42"; key must follow '(' or ' '. */
static unsigned long long MetaNum(const char *s, const char *key)
{
    const char *p = s;
    size_t kl = strlen(key);
    while ((p = strstr(p, key)) != NULL) {
        if ((p == s || p[-1] == '(' || p[-1] == ' ') && p[kl] == ' ') return _strtoui64(p + kl + 1, NULL, 10);
        p += kl;
    }
    return 0;
}

/* strstr ignoring case */
static const char *IStr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++) if (!_strnicmp(h, n, nl)) return h;
    return NULL;
}

/* Does sender s ("Name <address>") contain any comma/semicolon separated entry of list? Case ignored. */
static int ListHit(const char *list, const char *s)
{
    char buf[512], *tok, *ctx = NULL;
    strcpy_s(buf, sizeof buf, list);
    for (tok = strtok_s(buf, ",;", &ctx); tok; tok = strtok_s(NULL, ",;", &ctx)) {
        size_t n;
        while (*tok == ' ') tok++;
        n = strlen(tok);
        while (n && tok[n - 1] == ' ') tok[--n] = 0;
        if (n && IStr(s, tok)) return 1;
    }
    return 0;
}

/* VIP list set: only those senders; mute list: never those */
static int SenderOk(const wchar_t *from)
{
    char u8[512];
    WideCharToMultiByte(CP_UTF8, 0, from, -1, u8, sizeof u8, NULL, NULL);
    if (g_vip[0] && !ListHit(g_vip, u8)) return 0;
    return !ListHit(g_mute, u8);
}

/* Quoted-printable body text: =XX and soft line breaks */
static int QpDec(const char *s, int len, char *o, int cap)
{
    int i, n = 0;
    for (i = 0; i < len && n < cap; i++) {
        if (s[i] == '=' && i + 1 < len && (s[i + 1] == '\r' || s[i + 1] == '\n')) {
            i += s[i + 1] == '\r' ? 2 : 1;
        } else if (s[i] == '=' && i + 2 < len && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            unsigned v = 0;
            sscanf_s(s + i + 1, "%2x", &v);
            o[n++] = (char)v;
            i += 2;
        } else o[n++] = s[i];
    }
    return n;
}

/* First words of a mail. hdr: top header with Content-Type and Content-Transfer-Encoding; body: start of BODY[TEXT].
   shortcut: picks the first text/plain (else text/html) part by searching, no full MIME tree; fine for real-world mail. */
static void Snippet(const char *hdr, const char *body, int blen, wchar_t *out, int cap)
{
    static const char *ents[][2] = {{"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&zwnj;", ""}};
    const char *ph = hdr, *start = body, *end = body + blen, *ct = IStr(hdr, "\nContent-Type:");
    char *raw, *txt;
    int n, html = 0, k, i, sp = 1;
    wchar_t *w;
    out[0] = 0;
    if (ct && IStr(ct, "multipart/") && IStr(ct, "multipart/") < (strstr(ct + 1, "\n") ? strstr(ct + 1, "\n") + 200 : ct + 200)) {
        const char *part = IStr(body, "Content-Type: text/plain");
        if (!part) { part = IStr(body, "Content-Type: text/html"); html = 1; }
        if (!part) return;
        ph = part;
        start = strstr(part, "\r\n\r\n");
        if (!start) return;
        start += 4;
        {
            const char *b = strstr(start, "\r\n--");
            if (b) end = b;
        }
    } else if (ct && IStr(ct, "text/html") == ct + 15 + strspn(ct + 15, " ")) {
        html = 1;
    }
    {   /* encoding of the chosen part only: look no further than its own header */
        const char *enc = IStr(ph, "Content-Transfer-Encoding:");
        const char *hend = ph == hdr ? hdr + strlen(hdr) : start;
        raw = malloc(blen + 1);
        if (!raw) return;
        n = (int)(end - start);
        if (enc && enc < hend && IStr(enc, "base64") && IStr(enc, "base64") < hend) n = B64(start, n, raw, blen);
        else if (enc && enc < hend && IStr(enc, "quoted-printable") && IStr(enc, "quoted-printable") < hend) n = QpDec(start, n, raw, blen);
        else memcpy(raw, start, n);
        raw[n] = 0;
    }
    txt = malloc(n + 1);
    if (!txt) { free(raw); return; }
    for (i = k = 0; i < n; i++) {               /* strip tags, entities and runs of blanks */
        char c = raw[i];
        if (html && c == '<') {
            if (!_strnicmp(raw + i, "<style", 6) || !_strnicmp(raw + i, "<script", 7) || !_strnicmp(raw + i, "<head", 5)) {
                const char *cl = IStr(raw + i, !_strnicmp(raw + i, "<style", 6) ? "</style" : !_strnicmp(raw + i, "<head", 5) ? "</head" : "</script");
                i = cl ? (int)(cl - raw) : n;
            }
            while (i < n && raw[i] != '>') i++;
            c = ' ';
        } else if (html && c == '&') {
            int e, hit = 0;
            for (e = 0; e < (int)(sizeof ents / sizeof ents[0]); e++) {
                size_t el = strlen(ents[e][0]);
                if (!_strnicmp(raw + i, ents[e][0], el)) {
                    i += (int)el - 1;
                    hit = 1;
                    c = ents[e][1][0];          /* one char each; a space joins the blank collapsing below */
                    break;
                }
            }
            if (hit && !c) continue;
        }
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ' || (unsigned char)c == 0xA0) {
            if (!sp) txt[k++] = ' ';
            sp = 1;
            continue;
        }
        txt[k++] = c;
        sp = 0;
    }
    while (k && txt[k - 1] == ' ') k--;
    txt[k] = 0;
    w = out;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, txt, -1, w, cap)) {
        /* cut in the middle of a character or not UTF-8: drop a broken tail, else read it as Windows-1252 */
        int t = k, ok = 0;
        while (t > 0 && k - t < 4 && !ok) { txt[--t] = 0; ok = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, txt, -1, w, cap) != 0; }
        if (!ok) { txt[k] = 0; MultiByteToWideChar(1252, 0, txt, -1, w, cap); }
    }
    out[cap - 1] = 0;
    free(raw);
    free(txt);
}

/* "From"/"Subject" out of a raw header block (folded lines joined). */
/* INTERNALDATE "14-Nov-2026 15:33:01 +0100" -> FILETIME UTC, 0 if missing */
static ULONGLONG ImapDate(const char *s)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    SYSTEMTIME st = {0};
    FILETIME ft;
    char m[4], sign;
    int d, y, hh, mm, ss, oh, om;
    const char *p = s ? strstr(s, "INTERNALDATE \"") : NULL, *f;
    if (!p || sscanf_s(p + 14, "%d-%3c-%d %d:%d:%d %c%2d%2d", &d, m, 3, &y, &hh, &mm, &ss, &sign, 1, &oh, &om) != 9) return 0;
    m[3] = 0;
    if (!(f = strstr(mon, m))) return 0;
    st.wYear = (WORD)y; st.wMonth = (WORD)((f - mon) / 3 + 1); st.wDay = (WORD)d;
    st.wHour = (WORD)hh; st.wMinute = (WORD)mm; st.wSecond = (WORD)ss;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime)
           - (sign == '-' ? -1LL : 1LL) * (oh * 60 + om) * 600000000LL;
}

static void HeaderField(char *hdr, const char *name, wchar_t *out, int cap)
{
    char val[1024];
    size_t nl = strlen(name);
    char *line = hdr;
    out[0] = 0;
    while (*line) {
        char *e = strstr(line, "\r\n");
        if (!_strnicmp(line, name, nl) && line[nl] == ':') {
            size_t k = 0;
            char *v = line + nl + 1;
            for (;;) {
                while (*v && *v != '\r' && k < sizeof val - 1) val[k++] = *v++;
                if (v[0] == '\r' && v[1] == '\n' && (v[2] == ' ' || v[2] == '\t')) v += 2;   /* folded */
                else break;
            }
            val[k] = 0;
            DecodeMime(Trim(val), out, cap);
            return;
        }
        if (!e) break;
        line = e + 2;
    }
}

/* Fetches mail with UID >= t->acc->uidnext and posts the newest unread one to the UI. 0 = connection error. */
static int FetchNew(Tls *t)
{
    char line[2048], rest[512], cmd[200], tag[16];
    char hdr[8192], bestHdr[8192] = "";
    NewMail *best = NULL;
    int count = 0, nmuted = 0;
    unsigned long maxUid = t->acc->uidnext - 1, muted[256];
    size_t tl;

    int gm = Prov(t->acc->email)->kind == 0;
    if (g_muteCat && gm) {              /* Gmail's tabs are not IMAP labels; its own search finds them */
        sprintf_s(cmd, sizeof cmd, "UID SEARCH UID %lu:* X-GM-RAW \"category:promotions OR category:social\"", t->acc->uidnext);
        if (!Cmd(t, cmd, tag)) return 0;
        tl = strlen(tag);
        for (;;) {
            if (ReadLine(t, line, sizeof line) < 0) return 0;
            if (strncmp(line, tag, tl) == 0 && line[tl] == ' ') break;
            if (!strncmp(line, "* SEARCH", 8)) {
                char *c = line + 8;
                while (*c && nmuted < 256) {       /* shortcut: 256 muted per batch, enough for any normal burst */
                    unsigned long u = strtoul(c, &c, 10);
                    if (!u) break;
                    muted[nmuted++] = u;
                }
            }
        }
    }

    sprintf_s(cmd, sizeof cmd, "UID FETCH %lu:* (UID FLAGS INTERNALDATE%s BODY.PEEK[HEADER.FIELDS (FROM SUBJECT CONTENT-TYPE CONTENT-TRANSFER-ENCODING)])", t->acc->uidnext, gm ? " X-GM-THRID" : "");
    if (!Cmd(t, cmd, tag)) return 0;
    tl = strlen(tag);
    for (;;) {
        int r = ReadLine(t, line, sizeof line);
        if (r < 0) { free(best); return 0; }
        if (strncmp(line, tag, tl) == 0 && line[tl] == ' ') break;
        if (line[0] == '*' && strstr(line, "FETCH (")) {
            unsigned long uid;
            unsigned long long thr;
            char *lb = strrchr(line, '{');
            hdr[0] = rest[0] = 0;
            if (lb && r > 0 && line[r - 1] == '}') {
                if (ReadBytes(t, hdr, atoi(lb + 1), sizeof hdr) < 0 || ReadLine(t, rest, sizeof rest) < 0) {
                    free(best);
                    return 0;
                }
            }
            uid = (unsigned long)(MetaNum(line, "UID") ? MetaNum(line, "UID") : MetaNum(rest, "UID"));
            thr = MetaNum(line, "X-GM-THRID") ? MetaNum(line, "X-GM-THRID") : MetaNum(rest, "X-GM-THRID");
            if (uid < t->acc->uidnext) continue;              /* "n:*" always returns the last mail, even if old */
            if (uid > maxUid) maxUid = uid;
            if (strstr(line, "\\Seen") || strstr(rest, "\\Seen")) continue;
            {
                wchar_t fr[256];
                int k, skip = 0;
                for (k = 0; k < nmuted; k++) if (muted[k] == uid) skip = 1;
                HeaderField(hdr, "From", fr, 256);
                if (skip || !SenderOk(fr)) continue;
            }
            if (!best && !(best = calloc(1, sizeof *best))) continue;
            count++;                                    /* newest wins; the rest become "+N more" */
            best->acc = t->acc;
            best->uid = uid;
            bestHdr[0] = '\n';                       /* so "\nContent-Type:" also matches the first line */
            strcpy_s(bestHdr + 1, sizeof bestHdr - 1, hdr);
            HeaderField(hdr, "From", best->from, 256);
            HeaderField(hdr, "Subject", best->subj, 256);
            best->when = ImapDate(strstr(line, "INTERNALDATE \"") ? line : rest);
            NameOnly(best->from);
            if (thr) swprintf_s(best->url, 512, L"https://mail.google.com/mail/?authuser=%hs#all/%llx", t->acc->email, thr);
            else InboxUrl(t->acc->email, best->url);
        }
    }
    t->acc->uidnext = maxUid + 1;
    if (best) {                         /* preview: the first 6000 bytes of the newest mail's body, left unread (PEEK) */
        char body[6200];
        int blen = -1;
        sprintf_s(cmd, sizeof cmd, "UID FETCH %lu (BODY.PEEK[TEXT]<0.6000>)", best->uid);
        if (!Cmd(t, cmd, tag)) { free(best); return 0; }
        for (;;) {
            int r = ReadLine(t, line, sizeof line);
            char *lb = strrchr(line, '{');
            if (r < 0) { free(best); return 0; }
            if (strncmp(line, tag, tl) == 0 && line[tl] == ' ') break;
            if (line[0] == '*' && lb && r > 0 && line[r - 1] == '}') {
                blen = ReadBytes(t, body, atoi(lb + 1), sizeof body);
                if (blen < 0 || ReadLine(t, rest, sizeof rest) < 0) { free(best); return 0; }
            }
        }
        if (blen > 0) Snippet(bestHdr, body, blen, best->prev, 300);
    }
    if (best) {
        AccLabel(t->acc, best->to);
        if (count > 1) {
            wchar_t more[32];
            if (!best->from[0]) wcscpy_s(best->from, 256, L"New mail");
            swprintf_s(more, 32, L"  +%d more", count - 1);
            wcsncat_s(best->from, 256, more, _TRUNCATE);
        }
    }
    if (best && !PostMessageW(g_main, WM_NEWMAIL, 0, (LPARAM)best)) free(best);
    return 1;
}

/* state.ini next to config.ini: last UID seen per account, so mail that arrives while pigeon is off still shows */
static void StatePath(wchar_t *p)
{
    wcscpy_s(p, MAX_PATH, g_cfgPath);
    wcscpy_s(wcsrchr(p, L'\\') + 1, 16, L"state.ini");
}

static unsigned long UidLoad(Account *a)
{
    wchar_t p[MAX_PATH], e[256];
    StatePath(p);
    MultiByteToWideChar(CP_UTF8, 0, a->email, -1, e, 256);
    return GetPrivateProfileIntW(L"uid", e, 0, p);
}

static void UidSave(Account *a)
{
    wchar_t p[MAX_PATH], e[256], v[16];
    StatePath(p);
    MultiByteToWideChar(CP_UTF8, 0, a->email, -1, e, 256);
    swprintf_s(v, 16, L"%lu", a->uidnext);
    WritePrivateProfileStringW(L"uid", e, v, p);
}

/* Asks for the inbox's unread count. 0 = connection broke. */
static int Unseen(Tls *t)
{
    char line[2048], tag[16];
    size_t tl;
    int n = -1;
    if (Prov(t->acc->email)->kind == 0) {   /* Gmail: Primary tab only, like Gmail's own count; on NO, STATUS below */
        if (!Cmd(t, "SEARCH X-GM-RAW \"in:inbox is:unread category:primary\"", tag)) return 0;
        tl = strlen(tag);
        for (;;) {
            int c, k = 0, cnt = 0, dig = 0;
            for (;;) {                      /* the result line can be huge: count numbers as they stream */
                if ((c = TlsGetc(t)) < 0) return 0;
                if (c == '\n') break;
                if (k < (int)sizeof line - 1) line[k++] = (char)c;
                if (c >= '0' && c <= '9') { cnt += !dig; dig = 1; } else dig = 0;
            }
            line[k] = 0;
            if (!strncmp(line, "* SEARCH", 8)) n = cnt;
            if (strncmp(line, tag, tl) == 0 && line[tl] == ' ') break;
        }
    }
    if (n < 0) {
    if (!Cmd(t, "STATUS INBOX (UNSEEN)", tag)) return 0;
    tl = strlen(tag);
    for (;;) {
        const char *u;
        if (ReadLine(t, line, sizeof line) < 0) return 0;
        if ((u = strstr(line, "(UNSEEN ")) != NULL) n = atoi(u + 8);
        if (strncmp(line, tag, tl) == 0 && line[tl] == ' ') break;
    }
    }
    if (n >= 0 && n != t->acc->unread) {
        t->acc->unread = n;
        UpdateTip();
        PostMessageW(g_main, WM_UNREAD, 0, 0);
    }
    return 1;
}

/* LOGIN with the app password, or XOAUTH2 for Microsoft. keep: store a rotated refresh token.
   1 = in, 0 = refused, -1 = connection error. */
static int Login(Tls *t, int keep)
{
    char line[2048], tag[16];
    int r;
    if (Prov(t->acc->email)->kind != 2) {
        char cmd[512];
        LoginCmd(t->acc, cmd, sizeof cmd);
        r = Cmd(t, cmd, tag);
        SecureZeroMemory(cmd, sizeof cmd);
        return r ? WaitOk(t, tag, NULL) : -1;
    } else {
        static char tok[PASSMAX], b64[PASSMAX * 2];   /* only ImapSession threads and one action thread at a time */
        static SRWLOCK lk = SRWLOCK_INIT;
        size_t tl;
        AcquireSRWLockExclusive(&lk);
        r = MsToken(t->acc, keep, tok, sizeof tok);
        if (r == 1) r = XoauthB64(t->acc->email, tok, b64, sizeof b64) ? 1 : -1;
        SecureZeroMemory(tok, sizeof tok);
        if (r == 1) {
            if (!Cmd(t, "AUTHENTICATE XOAUTH2", tag) || ReadLine(t, line, sizeof line) < 0 || line[0] != '+') r = -1;
            else r = TlsSend(t, b64, (int)strlen(b64)) && TlsSend(t, "\r\n", 2) ? 1 : -1;
        }
        SecureZeroMemory(b64, sizeof b64);
        ReleaseSRWLockExclusive(&lk);
        if (r != 1) return r;
        tl = strlen(tag);
        for (;;) {
            if (ReadLine(t, line, sizeof line) < 0) return -1;
            if (line[0] == '+') { if (!TlsSend(t, "\r\n", 2)) return -1; continue; }   /* error details: answer empty, then NO follows */
            if (strncmp(line, tag, tl) == 0 && line[tl] == ' ') return strncmp(line + tl + 1, "OK", 2) == 0;
        }
    }
}

/* One connection: login, select, then IDLE until something breaks. -1 = login refused, 0 = never got going, 1 = was connected. */
static int ImapSession(Tls *t)
{
    char line[2048], tag[16];
    unsigned long uidnext = 0;
    int r;

    t->waitMs = 30000;
    if (!TlsConnect(t, Prov(t->acc->email)->host, "993")) return 0;
    if (ReadLine(t, line, sizeof line) < 0 || strncmp(line, "* OK", 4) != 0) return 0;
    r = Login(t, 1);
    if (r == 0) return -1;
    if (r < 0) return 0;
    if (!Cmd(t, "SELECT INBOX", tag) || WaitOk(t, tag, &uidnext) != 1 || !uidnext) return 0;
    if (!t->acc->uidnext) {             /* first connect since start: pick up mail that came while pigeon was off */
        unsigned long saved = UidLoad(t->acc);
        t->acc->uidnext = (saved && saved <= uidnext) ? (saved + 200 < uidnext ? uidnext - 200 : saved) : uidnext;
    }
    if (uidnext < t->acc->uidnext) t->acc->uidnext = uidnext;   /* keep the old mark so mail during a reconnect still shows */
    else if (uidnext > t->acc->uidnext && !FetchNew(t)) return 1;
    UidSave(t->acc);
    SetState(t->acc, 1);
    if (!Unseen(t)) return 1;

    for (;;) {
        int got = 0, changed = 0;
        if (!Cmd(t, "IDLE", tag)) return 1;
        if (ReadLine(t, line, sizeof line) < 0 || line[0] != '+') return 1;
        t->waitMs = 10 * 60 * 1000;     /* renew IDLE every 10 min; Gmail ends it after about 30 */
        for (;;) {
            r = ReadLine(t, line, sizeof line);
            if (r == -2) break;
            if (r < 0) return 1;
            if (strstr(line, " EXISTS")) { got = 1; break; }
            if (strstr(line, " EXPUNGE") || strstr(line, " FETCH ")) { changed = 1; break; }   /* read, deleted or archived elsewhere */
        }
        t->waitMs = 30000;
        if (!TlsSend(t, "DONE\r\n", 6) || WaitOk(t, tag, NULL) != 1) return 1;
        if (got && !FetchNew(t)) return 1;
        if (got) UidSave(t->acc);
        if ((got || changed) && !Unseen(t)) return 1;
    }
}

/* One per enabled account; reconnects forever. */
static DWORD WINAPI ImapThread(LPVOID arg)
{
    Tls *t = calloc(1, sizeof *t);
    int wait = 5;
    if (!t) return 0;
    t->s = INVALID_SOCKET;
    t->acc = (Account *)arg;
    for (;;) {
        int r;
        while (!t->acc->enabled || t->acc->removed) WaitForSingleObject(t->acc->wake, INFINITE);
        r = ImapSession(t);
        TlsClose(t);
        if (!t->acc->enabled || t->acc->removed) continue;
        if (r < 0) {
            SetState(t->acc, 3);
            WaitForSingleObject(t->acc->wake, 5 * 60 * 1000);   /* "Fix login" wakes it at once */
            continue;
        }
        SetState(t->acc, 2);
        if (r == 1) wait = 5;
        WaitForSingleObject(t->acc->wake, wait * 1000);
        if (wait < 60) wait *= 2;
    }
}

static void StartAccount(Account *a)
{
    a->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, ImapThread, a, 0, NULL));
}

/* Drops the account's connection and wakes its thread, which then reconnects or idles. */
static void Kick(Account *a)
{
    SOCKET s = a->sock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    if (a->wake) SetEvent(a->wake);
}

static const wchar_t *TestTo(const Account *a)
{
    static wchar_t w[256];
    AccLabel(a, w);
    return w;
}

/* ---------- Settings window ---------- */

#define IDD_SETTINGS 101
#define IDD_ADD 102
#define IDC_LIST 1001
#define IDC_ADD 1002
#define IDC_REMOVE 1003
#define IDC_POPUP 1004
#define IDC_SOUND 1005
#define IDC_SOUNDFILE 1006
#define IDC_BROWSE 1007
#define IDC_TESTPOP 1008
#define IDC_CORNER0 1020          /* 1020-1023: corner radios, same order as g_corner */
#define IDC_AUTOSTART 1010
#define IDC_FULLHOLD 1011
#define IDC_QUIET 1012
#define IDC_QFROM 1013
#define IDC_QTO 1014
#define IDC_QFROMSPIN 1040       /* up-down arrows inside the quiet-hours time fields */
#define IDC_QTOSPIN 1041
#define IDC_VIP 1015
#define IDC_MUTE 1016
#define IDC_MUTECAT 1017
#define IDC_FIX 1018
#define IDC_EMAIL 2001
#define IDC_PASS 2002
#define IDC_HINT 2004
#define IDC_PASSLBL 2005

#define WM_LISTSTATE (WM_APP + 7)  /* to Settings: an account's state changed */
static HWND g_settings;
static int g_testMode;          /* test buttons and menus: hidden until the code is typed in Settings */
static int g_editOk;            /* rename asked by F2 or the menu; a slow second click doesn't count */

static int RowAcc(HWND lv, int row)
{
    LVITEMW it;
    memset(&it, 0, sizeof it);
    it.mask = LVIF_PARAM;
    it.iItem = row;
    return ListView_GetItem(lv, &it) ? (int)it.lParam : -1;
}

static void ListStatus(HWND lv)
{
    static const wchar_t *names[] = {L"connecting", L"connected", L"offline", L"sign-in failed"};
    int row, n = ListView_GetItemCount(lv);
    for (row = 0; row < n; row++) {
        Account *a = &g_acc[RowAcc(lv, row)];
        wchar_t t[40];
        swprintf_s(t, 40, L"%s%s", a->enabled ? names[a->state & 3] : L"off", a->muted ? L", muted" : L"");
        ListView_SetItemText(lv, row, 1, t);
    }
}

/* List's Name column: "Work (me@x.com)", or just the address */
static void ListName(int i, wchar_t *w)
{
    wchar_t n[64], e[256];
    MultiByteToWideChar(CP_UTF8, 0, g_acc[i].email, -1, e, 256);
    MultiByteToWideChar(CP_UTF8, 0, g_acc[i].nick, -1, n, 64);
    if (n[0]) swprintf_s(w, 256, L"%s (%s)", n, e);
    else wcscpy_s(w, 256, e);
}

static void ListAdd(HWND lv, int i)
{
    wchar_t w[256];
    LVITEMW it;
    ListName(i, w);
    memset(&it, 0, sizeof it);
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = w;
    it.lParam = i;
    it.iItem = ListView_InsertItem(lv, &it);
    ListView_SetCheckState(lv, it.iItem, g_acc[i].enabled);
    {
        const Provider *pv = Prov(g_acc[i].email);
        const char *at = strrchr(g_acc[i].email, '@');
        if (pv->kind == 0) wcscpy_s(w, 256, at && (_stricmp(at, "@gmail.com") == 0 || _stricmp(at, "@googlemail.com") == 0) ? L"Gmail" : L"Workspace");
        else wcscpy_s(w, 256, pv->name);
        ListView_SetItemText(lv, it.iItem, 2, w);
    }
}

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static int AutoStartGet(void)
{
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"pigeon", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

/* Windows starts pigeon at login via HKCU Run (the per-user startup list in Task Manager). */
static void AutoStartSet(int on)
{
    if (on) {
        wchar_t self[MAX_PATH], val[MAX_PATH + 4];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        swprintf_s(val, MAX_PATH + 4, L"\"%s\"", self);
        RegSetKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"pigeon", REG_SZ, val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"pigeon");
    }
}

/* ---------- Dark dialogs in the popup's look ---------- */

#define FIELD (g_light ? RGB(236, 236, 236) : RGB(22, 22, 22))   /* edits, list, buttons: same as the popup's bar buttons */
#define INK Gr(225)
#define CARD (g_light ? RGB(255, 255, 255) : RGB(43, 43, 43))     /* section cards: hidden statics 4000-4099 mark where */
#define IDC_XCLOSE 999              /* our own close button in the header */
static HBRUSH g_bgBrush, g_fieldBrush, g_cardBrush;
static HFONT g_hdrFont;

static int IsCard(HWND c) { int id = GetDlgCtrlID(c); return id >= 4000 && id < 4100; }

/* 1 if control c sits on a section card */
static int OnCard(HWND d, HWND c)
{
    RECT r, k;
    HWND x;
    GetWindowRect(c, &r);
    for (x = GetWindow(d, GW_CHILD); x; x = GetWindow(x, GW_HWNDNEXT))
        if (IsCard(x) && GetWindowRect(x, &k) && r.left >= k.left && r.top >= k.top && r.right <= k.right && r.bottom <= k.bottom) return 1;
    return 0;
}

/* Dark title bar, dark scrollbars/combos/list, flat list without a header */
static LRESULT CALLBACK LvProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static void DarkDlg(HWND d)
{
    BOOL on = TRUE;
    COLORREF cap = BG;
    HWND c;
    RECT wr, cr;
    POINT o = {0, 0};
    int dx, dy, round = 2 /* DWMWCP_ROUND */;
    COLORREF edge = Gr(58);
    static int brushLight = -1;
    if (brushLight != g_light) {        /* first dialog, or Windows switched light/dark since */
        if (g_bgBrush) { DeleteObject(g_bgBrush); DeleteObject(g_fieldBrush); DeleteObject(g_cardBrush); }
        g_bgBrush = CreateSolidBrush(BG);
        g_fieldBrush = CreateSolidBrush(FIELD);
        g_cardBrush = CreateSolidBrush(CARD);
        brushLight = g_light;
    }
    if (!g_hdrFont) g_hdrFont = MakeFont(-DP(15), FW_SEMIBOLD);
    on = !g_light;
    DwmSetWindowAttribute(d, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on);
    DwmSetWindowAttribute(d, 35 /* DWMWA_CAPTION_COLOR, Windows 11 */, &cap, sizeof cap);
    DwmSetWindowAttribute(d, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    DwmSetWindowAttribute(d, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof edge);
    /* No Windows title bar: the client takes the whole window (WM_NCCALCSIZE), controls move down under our header. */
    GetWindowRect(d, &wr);
    ClientToScreen(d, &o);
    dx = o.x - wr.left;
    dy = o.y - wr.top + DP(10);
    SetPropW(d, L"pgnh", (HANDLE)(INT_PTR)dy);
    SetWindowPos(d, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top + DP(10), SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    for (c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        RECT r;
        GetWindowRect(c, &r);
        MapWindowPoints(NULL, d, (POINT *)&r, 1);
        SetWindowPos(c, NULL, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    GetClientRect(d, &cr);
    CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, cr.right - DP(44), (dy - DP(30)) / 2, DP(36), DP(30), d, (HMENU)IDC_XCLOSE, g_inst, NULL);
    SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_nid.hIcon);
    for (c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[32];
        GetClassNameW(c, cls, 32);
        int lv = _wcsicmp(cls, WC_LISTVIEWW) == 0;
        SetWindowTheme(c, g_light ? L"Explorer" : _wcsicmp(cls, L"ComboBox") == 0 ? L"DarkMode_CFD" : lv ? L"DarkMode_ItemsView" : L"DarkMode_Explorer", NULL);
        if (lv || _wcsicmp(cls, L"Edit") == 0) {   /* borderless: WM_PAINT draws a rounded field behind it */
            SetWindowLongW(c, GWL_STYLE, GetWindowLongW(c, GWL_STYLE) & ~WS_BORDER);
            SetWindowLongW(c, GWL_EXSTYLE, GetWindowLongW(c, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
            SetWindowPos(c, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            if (!lv) {   /* single-line edits draw text at the top: shrink to text height, centered in the old box */
                HDC dc = GetDC(c);
                HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(c, WM_GETFONT, 0, 0));
                TEXTMETRICW tm;
                RECT r;
                int th, pad;
                GetTextMetricsW(dc, &tm);
                SelectObject(dc, of);
                ReleaseDC(c, dc);
                GetWindowRect(c, &r);
                MapWindowPoints(NULL, d, (POINT *)&r, 2);
                th = tm.tmHeight;
                pad = (r.bottom - r.top - th) / 2;
                if (pad > 0) {
                    SetWindowPos(c, NULL, r.left, r.top + pad, r.right - r.left, th, SWP_NOZORDER);
                    SetPropW(c, L"pgpad", (HANDLE)(INT_PTR)pad);
                }
            }
        }
        if (lv) {
            ListView_SetBkColor(c, FIELD);
            ListView_SetTextBkColor(c, FIELD);
            ListView_SetTextColor(c, INK);
            SetWindowTheme(ListView_GetHeader(c), g_light ? L"ItemsView" : L"DarkMode_ItemsView", NULL);
            SetWindowSubclass(c, LvProc, 1, 0);
        }
    }
}

/* Push buttons as the popup's rounded buttons, checkboxes as rounded boxes. Returns CDRF_* */
static LRESULT DrawBtn(NMCUSTOMDRAW *cd)
{
    HWND h = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    int hot = (cd->uItemState & CDIS_HOT) != 0, down = (cd->uItemState & CDIS_SELECTED) != 0;
    int focus = (cd->uItemState & CDIS_FOCUS) && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    wchar_t text[128];
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    GetWindowTextW(h, text, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, (cd->uItemState & CDIS_DISABLED) ? Gr(110) : INK);
    FillRect(dc, &r, OnCard(GetParent(h), h) ? g_cardBrush : g_bgBrush);
    if (GetDlgCtrlID(h) == IDC_XCLOSE) {    /* header close: an X, a soft square on hover */
        /* X from square dots on both diagonals: mirror-exact */
        int n = DP(10), t = DP(1) + 1, x0 = (r.left + r.right - n - t) / 2, y0 = (r.top + r.bottom - n - t) / 2, i;
        HBRUSH ink = CreateSolidBrush(INK);
        if (hot || down) SmoothRound(dc, r, DP(6), down ? Gr(70) : Gr(55));
        for (i = 0; i <= n; i++) {
            RECT p1 = {x0 + i, y0 + i, x0 + i + t, y0 + i + t}, p2 = {x0 + n - i, y0 + i, x0 + n - i + t, y0 + i + t};
            FillRect(dc, &p1, ink);
            FillRect(dc, &p2, ink);
        }
        DeleteObject(ink);
    } else if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX) {   /* a switch */
        int sw = DP(32), sh = DP(18), on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED, kd = sh - DP(6), kx;
        RECT b = {r.left, (r.top + r.bottom - sh) / 2, r.left + sw, (r.top + r.bottom - sh) / 2 + sh}, k;
        if (focus) { RECT f = b; InflateRect(&f, DP(2), DP(2)); SmoothRound(dc, f, (sh + DP(4)) / 2, Gr(150)); }
        SmoothRound(dc, b, sh / 2, on ? Gr(235) : hot ? Gr(95) : Gr(75));
        kx = on ? b.right - DP(3) - kd : b.left + DP(3);
        SetRect(&k, kx, b.top + DP(3), kx + kd, b.top + DP(3) + kd);
        SmoothRound(dc, k, kd / 2, on ? BG : g_light ? RGB(255, 255, 255) : Gr(210));   /* off knob: always the light one */
        r.left += sw + DP(8);
        DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);
    } else if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON) {   /* corner picker: a little screen, dot in the corner */
        int on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED, k = GetDlgCtrlID(h) - IDC_CORNER0;
        int sw = DP(20), sh = DP(14), dd = DP(5), x0 = (r.left + r.right - sw) / 2, y0 = (r.top + r.bottom - sh) / 2;
        RECT in = r, sc = {x0, y0, x0 + sw, y0 + sh}, sci = sc, dot;
        if (focus) { SmoothRound(dc, r, DP(8), Gr(150)); InflateRect(&in, -DP(1), -DP(1)); }
        SmoothRound(dc, in, DP(7), on ? Gr(80) : hot ? Gr(48) : FIELD);
        SmoothRound(dc, sc, DP(3), on ? INK : Gr(130));
        InflateRect(&sci, -DP(1) - 1, -DP(1) - 1);
        SmoothRound(dc, sci, DP(2), on ? Gr(80) : hot ? Gr(48) : FIELD);
        dot.left = (k == 0 || k == 2) ? sci.right - DP(2) - dd : sci.left + DP(2);
        dot.top = k >= 2 ? sci.bottom - DP(2) - dd : sci.top + DP(2);
        dot.right = dot.left + dd;
        dot.bottom = dot.top + dd;
        SmoothRound(dc, dot, DP(1), on ? INK : Gr(150));
    } else {
        int def = type == BS_DEFPUSHBUTTON;
        RECT in = r;
        if (focus || def) {
            SmoothRound(dc, r, DP(8), focus ? Gr(150) : Gr(90));
            InflateRect(&in, -DP(1), -DP(1));
        }
        SmoothRound(dc, in, DP(7), down ? Gr(64) : hot ? Gr(48) : FIELD);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);
    }
    SelectObject(dc, of);
    return CDRF_SKIPDEFAULT;
}

/* List header cell: white text on the field color, thin separator, sort arrow. */
static LRESULT DrawHdr(NMCUSTOMDRAW *cd)
{
    HDC dc = cd->hdc;
    RECT r = cd->rc;
    wchar_t text[64];
    HDITEMW hi;
    HPEN pen = CreatePen(PS_SOLID, 1, Gr(60)), op;
    memset(&hi, 0, sizeof hi);
    hi.mask = HDI_TEXT | HDI_FORMAT;
    hi.pszText = text;
    hi.cchTextMax = 64;
    Header_GetItem(cd->hdr.hwndFrom, (int)cd->dwItemSpec, &hi);
    FillRect(dc, &r, g_fieldBrush);
    op = SelectObject(dc, pen);
    MoveToEx(dc, r.right - 2, r.top + DP(4), NULL);   /* -2: lines up with the list's own column lines */
    LineTo(dc, r.right - 2, r.bottom - DP(4));
    SelectObject(dc, op);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, (cd->uItemState & CDIS_HOT) ? Gr(255) : Gr(235));
    r.left += DP(6);
    r.right -= DP(16);
    DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (hi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
        int x = r.right + DP(5), y = (r.top + r.bottom) / 2, a = DP(3), up = (hi.fmt & HDF_SORTUP) != 0;
        POINT t[3] = {{x - a, up ? y + a / 2 : y - a / 2}, {x + a, up ? y + a / 2 : y - a / 2}, {x, up ? y - a : y + a}};
        HBRUSH b = CreateSolidBrush(INK);
        HGDIOBJ ob = SelectObject(dc, b), opn = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, t, 3);
        SelectObject(dc, ob);
        SelectObject(dc, opn);
        DeleteObject(b);
    }
    return CDRF_SKIPDEFAULT;
}

#define WM_SAVECOLS (WM_APP + 20)

/* Account list layout into the config, right away */
static void SaveCols(HWND lv)
{
    int o[3], k;
    if (Header_GetItemCount(ListView_GetHeader(lv)) != 3 || !ListView_GetColumnOrderArray(lv, 3, o)) return;
    for (k = 0; k < 3; k++) {
        g_colOrder[k] = o[k];
        g_colW[k] = MulDiv(ListView_GetColumnWidth(lv, k), 96, g_dpi);
    }
    SaveConfig();
}

/* Header notifications go to the ListView, not the dialog, so catch them here. */
static LRESULT CALLBACK LvProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    if (m == WM_NOTIFY) {
        NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)l;
        if (cd->hdr.hwndFrom == ListView_GetHeader(h)) {
            if (cd->hdr.code == NM_CUSTOMDRAW)
                return cd->dwDrawStage == CDDS_PREPAINT ? CDRF_NOTIFYITEMDRAW : cd->dwDrawStage == CDDS_ITEMPREPAINT ? DrawHdr(cd) : CDRF_DODEFAULT;
            if (cd->hdr.code == HDN_ITEMCHANGINGW && (((NMHEADERW *)l)->pitem->mask & HDI_WIDTH)) {
                /* keep all columns inside the list: never a horizontal scrollbar */
                NMHEADERW *nh = (NMHEADERW *)l;
                RECT rc;
                int i, others = 0, n = Header_GetItemCount(cd->hdr.hwndFrom), lim;
                GetClientRect(h, &rc);
                for (i = 0; i < n; i++) if (i != nh->iItem) others += ListView_GetColumnWidth(h, i);
                lim = rc.right - others;
                if (nh->pitem->cxy > lim) nh->pitem->cxy = lim;
                if (nh->pitem->cxy < DP(40)) nh->pitem->cxy = DP(40);
                if (nh->pitem->cxy > lim) return TRUE;
            }
            if (cd->hdr.code == HDN_ITEMCHANGEDW || cd->hdr.code == HDN_ENDDRAG) {   /* resized/moved: no leftover lines */
                LRESULT r = DefSubclassProc(h, m, w, l);
                PostMessageW(h, WM_SAVECOLS, 0, 0);   /* after the move lands: order isn't updated yet on HDN_ENDDRAG */
                RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                return r;
            }
        }
    }
    if (m == WM_SAVECOLS) { SaveCols(h); return 0; }
    if (m == WM_PAINT && ListView_GetItemCount(h) == 0) {   /* empty: say what to do */
        LRESULT r = DefSubclassProc(h, m, w, l);
        HDC dc = GetDC(h);
        RECT rc, hr;
        GetClientRect(h, &rc);
        GetWindowRect(ListView_GetHeader(h), &hr);
        rc.top += hr.bottom - hr.top;
        SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, Gr(150));
        DrawTextW(dc, L"No accounts yet. Click Add to start.", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ReleaseDC(h, dc);
        return r;
    }
    if (m == WM_CTLCOLOREDIT) {         /* the rename box */
        SetTextColor((HDC)w, INK);
        SetBkColor((HDC)w, FIELD);
        return (LRESULT)g_fieldBrush;
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, LvProc, id);
    return DefSubclassProc(h, m, w, l);
}

/* Shared by the three dialogs: colors and button drawing. 1 = handled, *ret is the dialog result. */
static int DarkMsg(HWND d, UINT m, WPARAM w, LPARAM l, INT_PTR *ret)
{
    switch (m) {
    case WM_PAINT: {                    /* background, plus a rounded field under each edit and the list */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(d, &ps);
        HWND c;
        int hh = (int)(INT_PTR)GetPropW(d, L"pgnh");
        wchar_t title[64];
        RECT tr;
        FillRect(dc, &ps.rcPaint, g_bgBrush);
        if (hh) {                       /* header: logo and title where Windows had its title bar */
            DrawIconEx(dc, DP(14), (hh - DP(18)) / 2, g_nid.hIcon, DP(18), DP(18), 0, NULL, DI_NORMAL);
            GetWindowTextW(d, title, 64);
            GetClientRect(d, &tr);
            tr.left = DP(42);
            tr.bottom = hh;
            SelectObject(dc, g_hdrFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, Gr(245));
            DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        for (c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {   /* cards first, fields go on top */
            RECT r;
            if (!IsCard(c)) continue;
            GetWindowRect(c, &r);
            MapWindowPoints(NULL, d, (POINT *)&r, 2);
            SmoothRound(dc, r, DP(10), CARD);
        }
        for (c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            wchar_t cls[32];
            RECT r;
            GetClassNameW(c, cls, 32);
            if (!IsWindowVisible(c) || (_wcsicmp(cls, L"Edit") != 0 && _wcsicmp(cls, WC_LISTVIEWW) != 0)) continue;
            GetWindowRect(c, &r);
            MapWindowPoints(NULL, d, (POINT *)&r, 2);
            InflateRect(&r, DP(5), DP(3) + (int)(INT_PTR)GetPropW(c, L"pgpad"));
            if (GetDlgCtrlID(c) == IDC_QFROM || GetDlgCtrlID(c) == IDC_QTO) {   /* field reaches over its arrows */
                RECT sr;
                GetWindowRect(GetDlgItem(d, GetDlgCtrlID(c) == IDC_QFROM ? IDC_QFROMSPIN : IDC_QTOSPIN), &sr);
                MapWindowPoints(NULL, d, (POINT *)&sr, 2);
                r.right = sr.right + DP(3);
            }
            SmoothRound(dc, r, DP(8), FIELD);
        }
        EndPaint(d, &ps);
        *ret = TRUE;
        return g_bgBrush != NULL;
    }
    case WM_CTLCOLORDLG:
        *ret = (INT_PTR)g_bgBrush;
        return g_bgBrush != NULL;
    case WM_CTLCOLORSTATIC: {
        int card = OnCard(d, (HWND)l), id = GetDlgCtrlID((HWND)l);
        if (id == IDC_QFROM || id == IDC_QTO) {   /* read-only time boxes: same field look as the edits */
            SetTextColor((HDC)w, Gr(255));
            SetBkColor((HDC)w, FIELD);
            *ret = (INT_PTR)g_fieldBrush;
            return g_fieldBrush != NULL;
        }
        SetTextColor((HDC)w, id >= 4100 && id < 4200 ? Gr(150) : INK);   /* 4100-4199: section titles */
        SetBkColor((HDC)w, card ? CARD : BG);
        *ret = (INT_PTR)(card ? g_cardBrush : g_bgBrush);
        return g_bgBrush != NULL;
    }
    case WM_NCCALCSIZE:                 /* frameless once DarkDlg ran: client = whole window */
        if (!w || !GetPropW(d, L"pgnh")) return 0;
        SetWindowLongPtrW(d, DWLP_MSGRESULT, 0);
        *ret = TRUE;
        return 1;
    case WM_NCHITTEST: {                /* drag by the header */
        POINT pt = {(short)LOWORD(l), (short)HIWORD(l)};
        int hh = (int)(INT_PTR)GetPropW(d, L"pgnh");
        if (!hh) return 0;
        ScreenToClient(d, &pt);
        SetWindowLongPtrW(d, DWLP_MSGRESULT, pt.y >= 0 && pt.y < hh ? HTCAPTION : HTCLIENT);
        *ret = TRUE;
        return 1;
    }
    case WM_COMMAND:
        if (LOWORD(w) != IDC_XCLOSE) return 0;
        SendMessageW(d, WM_COMMAND, IDCANCEL, 0);
        *ret = TRUE;
        return 1;
    case WM_DESTROY:
        RemovePropW(d, L"pgnh");
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, Gr(255));
        SetBkColor((HDC)w, FIELD);
        *ret = (INT_PTR)g_fieldBrush;
        return g_fieldBrush != NULL;
    case WM_NOTIFY: {
        NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)l;
        wchar_t cls[16];
        if (!GetClassNameW(cd->hdr.hwndFrom, cls, 16)) return 0;
        if (cd->hdr.code != NM_CUSTOMDRAW || _wcsicmp(cls, L"Button") != 0) return 0;
        SetWindowLongPtrW(d, DWLP_MSGRESULT, cd->dwDrawStage == CDDS_PREPAINT ? DrawBtn(cd) : CDRF_DODEFAULT);
        *ret = TRUE;
        return 1;
    }
    }
    return 0;
}

/* Under the address: which provider, and what it needs. The password row hides for Microsoft. */
static void AddHint(HWND d)
{
    wchar_t we[256], hint[200] = L"";
    char e8[256];
    const char *at;
    int ms;
    GetDlgItemTextW(d, IDC_EMAIL, we, 256);
    WideCharToMultiByte(CP_UTF8, 0, we, -1, e8, sizeof e8, NULL, NULL);
    at = strrchr(e8, '@');
    ms = at && Prov(e8)->kind == 2;
    if (!at || !at[1]) ;
    else if (ms) wcscpy_s(hint, 200, L"Outlook / Hotmail: click Add, then sign in with Microsoft in your browser. No password needed.");
    else if (Prov(e8)->kind == 1) swprintf_s(hint, 200, L"%hs detected. Needs an app password, not your normal password.", at + 1);
    else if (_stricmp(at + 1, "gmail.com") == 0 || _stricmp(at + 1, "googlemail.com") == 0)
        wcscpy_s(hint, 200, L"Gmail detected. Needs an app password (2-Step Verification must be on).");
    else wcscpy_s(hint, 200, L"Custom domain: treated as Google Workspace. Needs an app password.");
    wchar_t old[200];
    int flip = !ms != !!IsWindowVisible(GetDlgItem(d, IDC_PASS));
    GetDlgItemTextW(d, IDC_HINT, old, 200);
    if (!flip && wcscmp(old, hint) == 0 && GetPropW(d, L"pgfit")) return;   /* nothing changed: no repaint, no flicker while typing */
    SetPropW(d, L"pgfit", (HANDLE)1);
    SetDlgItemTextW(d, IDC_HINT, hint);
    if (flip) {
        ShowWindow(GetDlgItem(d, IDC_PASSLBL), ms ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(d, IDC_PASS), ms ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(d, 2003), ms ? SW_HIDE : SW_SHOW);
    }
    {   /* stack: last field, hint sized to its text, buttons; window shrinks to fit */
        HWND hh = GetDlgItem(d, IDC_HINT), last = GetDlgItem(d, ms ? IDC_EMAIL : IDC_PASS);
        int ids[3] = {IDOK, IDCANCEL, 2003}, i, y, th = 0;
        RECT lr, hr, br, wr;
        GetWindowRect(last, &lr);
        GetWindowRect(hh, &hr);
        MapWindowPoints(NULL, d, (POINT *)&lr, 2);
        MapWindowPoints(NULL, d, (POINT *)&hr, 2);
        y = lr.bottom + DP(16);   /* clear of the rounded field drawn around the edit */
        if (hint[0]) {
            HDC dc = GetDC(hh);
            HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(hh, WM_GETFONT, 0, 0));
            RECT t = {0, 0, hr.right - hr.left, 0};
            DrawTextW(dc, hint, -1, &t, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, of);
            ReleaseDC(hh, dc);
            th = t.bottom;
        }
        SetWindowPos(hh, NULL, hr.left, y, hr.right - hr.left, th, SWP_NOZORDER);
        if (th) y += th + DP(6);
        y += DP(6);
        for (i = 0; i < 3; i++) {
            HWND b = GetDlgItem(d, ids[i]);
            GetWindowRect(b, &br);
            MapWindowPoints(NULL, d, (POINT *)&br, 2);
            SetWindowPos(b, NULL, br.left, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        GetWindowRect(d, &wr);
        SetWindowPos(d, NULL, 0, 0, wr.right - wr.left, y + (br.bottom - br.top) + DP(12), SWP_NOMOVE | SWP_NOZORDER);
    }
    InvalidateRect(d, NULL, TRUE);      /* repaint the rounded fields after the move */
}

static INT_PTR CALLBACK AddProc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    INT_PTR dr;
    if (DarkMsg(d, m, w, l, &dr)) return dr;
    if (m == WM_INITDIALOG) {
        Account *a = (Account *)l;
        SetWindowLongPtrW(d, DWLP_USER, l);
        DarkDlg(d);
        if (a->email[0]) {              /* fix mode: same account, new password or sign-in */
            wchar_t we[256];
            MultiByteToWideChar(CP_UTF8, 0, a->email, -1, we, 256);
            SetDlgItemTextW(d, IDC_EMAIL, we);
            SendDlgItemMessageW(d, IDC_EMAIL, EM_SETREADONLY, TRUE, 0);
            SetWindowTextW(d, L"Fix sign-in");
            SetDlgItemTextW(d, IDOK, L"Save");
            SetFocus(GetDlgItem(d, Prov(a->email)->kind == 2 ? IDOK : IDC_PASS));
            return FALSE;
        }
        AddHint(d);
        return TRUE;
    }
    if (m == WM_COMMAND && LOWORD(w) == IDC_EMAIL && HIWORD(w) == EN_CHANGE) { AddHint(d); return TRUE; }
    if (m == WM_COMMAND && LOWORD(w) == 2003) {
        wchar_t we[256], wu[256];
        char e8[256];
        const Provider *pv;
        GetDlgItemTextW(d, IDC_EMAIL, we, 256);
        WideCharToMultiByte(CP_UTF8, 0, we, -1, e8, sizeof e8, NULL, NULL);
        pv = Prov(e8);
        if (pv->kind == 2) MessageBoxW(d, L"Outlook and Hotmail need no app password: leave it empty and click Add to sign in with Microsoft.", L"pigeon", MB_ICONINFORMATION);
        else {
            MultiByteToWideChar(CP_UTF8, 0, pv->help, -1, wu, 256);
            ShellExecuteW(d, L"open", wu, NULL, NULL, SW_SHOWNORMAL);
        }
        return TRUE;
    }
    if (m == WM_COMMAND && LOWORD(w) == IDOK) {
        wchar_t we[256], wp[128];
        Account *dst = (Account *)GetWindowLongPtrW(d, DWLP_USER), tmp, *a = &tmp;
        int fix = dst->email[0] != 0;
        size_t i, o = 0;
        int k;
        GetDlgItemTextW(d, IDC_EMAIL, we, 256);
        GetDlgItemTextW(d, IDC_PASS, wp, 128);
        memset(a, 0, sizeof *a);
        WideCharToMultiByte(CP_UTF8, 0, we, -1, a->email, sizeof a->email, NULL, NULL);
        for (i = 0; wp[i] && o < sizeof a->pass - 1; i++) if (wp[i] != L' ') a->pass[o++] = (char)wp[i];   /* app passwords: a-z only */
        a->pass[o] = 0;
        SecureZeroMemory(wp, sizeof wp);
        PassSeal(a->pass);
        Trim(a->email);
        if (!strchr(a->email, '@') || (o == 0 && Prov(a->email)->kind != 2)) {
            MessageBoxW(d, L"Enter the email address and its app password.", L"pigeon", MB_ICONWARNING);
            return TRUE;
        }
        for (k = 0; k < g_nacc && !fix; k++)
            if (!g_acc[k].removed && _stricmp(g_acc[k].email, a->email) == 0) {
                MessageBoxW(d, L"That account is already in the list.", L"pigeon", MB_ICONWARNING);
                return TRUE;
            }
        if (Prov(a->email)->kind == 2 && !MsSignIn(d, a)) { SecureZeroMemory(&tmp, sizeof tmp); return TRUE; }   /* after the duplicate check: no sign-in for nothing */
        if (fix) {                      /* thread is idle or will fail once on the old one; then wake it */
            memcpy(dst->pass, a->pass, PASSMAX);
            dst->state = 0;
            Kick(dst);
        } else {                        /* new slot, no thread on it yet */
            memcpy(dst, a, sizeof *dst);
            dst->sock = INVALID_SOCKET;
            dst->enabled = 1;
        }
        SecureZeroMemory(&tmp, sizeof tmp);
        EndDialog(d, IDOK);
        return TRUE;
    }
    if (m == WM_COMMAND && LOWORD(w) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
    return FALSE;
}

static HWND g_sortLv;

static int CALLBACK SortCmp(LPARAM a, LPARAM b, LPARAM col)
{
    wchar_t x[256], y[256];
    ListView_GetItemText(g_sortLv, (int)a, (int)col, x, 256);
    ListView_GetItemText(g_sortLv, (int)b, (int)col, y, 256);
    return _wcsicmp(x, y) * g_sortDir;
}

/* Header click: sort by that column; a second click flips the order. The arrow shows which. */
static void SortList(HWND lv, int col)
{
    HWND hd = ListView_GetHeader(lv);
    HDITEMW hi;
    int i;
    g_sortDir = col == g_sortCol ? -g_sortDir : 1;
    g_sortCol = col;
    g_sortLv = lv;
    ListView_SortItemsEx(lv, SortCmp, col);
    for (i = 0; i < Header_GetItemCount(hd); i++) {
        hi.mask = HDI_FORMAT;
        Header_GetItem(hd, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == col) hi.fmt |= g_sortDir > 0 ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hd, i, &hi);
    }
    SaveConfig();
}

/* Adds an account and starts watching it at once. Returns 1 if added. */
static int AddAccount(HWND owner)
{
    Account *a = &g_acc[g_nacc];
    /* shortcut: removed accounts keep their slot until pigeon restarts, so at most 8 adds per run; compact on load if that bites */
    if (g_nacc >= MAX_ACC) { MessageBoxW(owner, L"Up to 8 accounts. Restart pigeon to free removed ones.", L"pigeon", MB_ICONINFORMATION); return 0; }
    memset(a, 0, sizeof *a);
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ADD), owner, AddProc, (LPARAM)a) != IDOK) return 0;
    StartAccount(a);
    g_nacc++;
    g_imap = 1;
    SaveConfig();
    UpdateTip();
    return 1;
}

/* Settings take effect as they change; Close saves them to disk. */
/* Quiet-hours time box: holds minutes after midnight, shows them in the Windows clock format */
static void QSet(HWND e, int t)
{
    SYSTEMTIME st = {0};
    wchar_t s[32];
    t = (t % 1440 + 1440) % 1440;
    SetWindowLongPtrW(e, GWLP_USERDATA, t);
    st.wHour = (WORD)(t / 60); st.wMinute = (WORD)(t % 60);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, s, 32);
    SetWindowTextW(e, s);               /* EN_CHANGE -> ApplySettings */
}

/* one 15-minute step up (dir 1) or down (-1); off-grid times snap to the grid */
static void QStep(HWND e, int dir)
{
    int t = (int)GetWindowLongPtrW(e, GWLP_USERDATA);
    QSet(e, dir > 0 ? (t / 15 + 1) * 15 : ((t + 14) / 15 - 1) * 15);
}

/* time box: mouse wheel and Up/Down keys step it */
static LRESULT CALLBACK QProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    if (m == WM_MOUSEWHEEL) { QStep(h, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1); return 0; }
    if (m == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN)) { QStep(h, w == VK_UP ? 1 : -1); return 0; }
    return DefSubclassProc(h, m, w, l);
}

static void ApplySettings(HWND d)
{
    int secs = (int)GetDlgItemInt(d, IDC_POPUP, NULL, FALSE), c;
    wchar_t w[512];
    g_popupMs = (secs < 1 ? 1 : secs > 60 ? 60 : secs) * 1000;
    g_sound = IsDlgButtonChecked(d, IDC_SOUND) == BST_CHECKED;
    for (c = 0; c < 4; c++) if (IsDlgButtonChecked(d, IDC_CORNER0 + c) == BST_CHECKED) g_corner = c;
    GetDlgItemTextW(d, IDC_SOUNDFILE, g_soundFile, MAX_PATH);
    g_fullHold = IsDlgButtonChecked(d, IDC_FULLHOLD) == BST_CHECKED;
    g_quiet = IsDlgButtonChecked(d, IDC_QUIET) == BST_CHECKED;
    g_quietFrom = (int)GetWindowLongPtrW(GetDlgItem(d, IDC_QFROM), GWLP_USERDATA);   /* minutes, kept by QSet */
    g_quietTo = (int)GetWindowLongPtrW(GetDlgItem(d, IDC_QTO), GWLP_USERDATA);
    GetDlgItemTextW(d, IDC_VIP, w, 512);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, g_vip, sizeof g_vip, NULL, NULL);
    GetDlgItemTextW(d, IDC_MUTE, w, 512);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, g_mute, sizeof g_mute, NULL, NULL);
    g_muteCat = IsDlgButtonChecked(d, IDC_MUTECAT) == BST_CHECKED;
}

#ifndef PIGEON_VERSION
#define PIGEON_VERSION "v1.0"     /* the GitHub release tag this build is: bump it with every release */
#endif
#define IDC_UPDATE 1042
#define UPCAP (16 << 20)

/* GET an https URL into out; follows GitHub's redirect to its download host. -1 on error, non-200 or too big. */
static int HttpGet(const char *url, char *out, int cap)
{
    wchar_t w[2048], host[256], path[2048];
    URL_COMPONENTS u;
    HINTERNET s, c = NULL, r = NULL;
    DWORD got = 0, st = 0, sl = sizeof st;
    int len = -1;
    memset(&u, 0, sizeof u);
    u.dwStructSize = sizeof u;
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path;  u.dwUrlPathLength = 2048;
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 2048);
    if (!WinHttpCrackUrl(w, 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS) return -1;
    s = WinHttpOpen(L"pigeon", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, 15000, 15000, 15000, 30000);
    c = WinHttpConnect(s, host, u.nPort, 0);
    if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)
          && WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &st, &sl, NULL) && st == 200) {
        len = 0;
        while (len < cap - 1 && WinHttpReadData(r, out + len, cap - 1 - len, &got) && got) len += got;
        out[len] = 0;
        if (len == cap - 1) len = -1;   /* cut off: never install half a file */
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return len;
}

/* Next string value after "key" in GitHub's JSON whose text ends with suf. */
static int JsonStr(const char *j, const char *key, const char *suf, char *out, int cap)
{
    char k[64];
    const char *p = j, *v, *e;
    sprintf_s(k, sizeof k, "\"%s\"", key);
    while ((p = strstr(p, k)) != NULL) {
        p += strlen(k);
        while (*p == ' ' || *p == ':') p++;
        if (*p++ != '"' || !(e = strchr(p, '"'))) continue;
        v = p; p = e;
        if ((int)(e - v) >= cap || (int)(e - v) < (int)strlen(suf) || strncmp(e - strlen(suf), suf, strlen(suf))) continue;
        memcpy(out, v, e - v); out[e - v] = 0;
        return 1;
    }
    return 0;
}

static void SelfPath(wchar_t *p, const wchar_t *ext)   /* this exe's path, plus ext (L"" for none) */
{
    GetModuleFileNameW(NULL, p, MAX_PATH);
    wcscat_s(p, MAX_PATH, ext);
}

/* Public half of the release signing key (BCRYPT_ECCPUBLIC_BLOB, P-256). The private half is only in the
   PIGEON_SIGNING_KEY secret of the GitHub build, which signs pigeon.exe into pigeon.exe.sig on release. */
static const UCHAR g_relKey[72] = {0x45,0x43,0x53,0x31,0x20,0x00,0x00,0x00,0x7c,0xfb,0xa0,0x48,0x2d,0x05,0xac,0xa6,0x0c,0x40,0x1b,0x5b,0xce,0xdf,0xc6,0xf7,0xa8,0x9a,0x33,0x7e,0xe9,0x3f,0x93,0xba,0x58,0x99,0x9f,0x55,0x34,0xe2,0x71,0xfe,0x1e,0xed,0x8b,0x25,0x46,0x57,0x62,0x3b,0x04,0xe3,0xb7,0xbe,0x51,0x31,0x6c,0x79,0xa7,0x12,0xac,0xe9,0x10,0xfc,0x94,0x3c,0xef,0x3d,0x91,0x90,0x31,0x15,0x7d,0xe7};

/* 1 if sig (64 bytes, r then s) is the release key's signature over the SHA-256 digest dig */
/* shortcut: the signature covers the exe, not its version, so an older signed release served as "latest" would install; sign the tag too if releases ever move off GitHub */
static int SigOk(const UCHAR *dig, const UCHAR *sig, int n)
{
    BCRYPT_ALG_HANDLE a;
    BCRYPT_KEY_HANDLE k;
    int ok = 0;
    if (n != 64 || BCryptOpenAlgorithmProvider(&a, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) return 0;
    if (!BCryptImportKeyPair(a, NULL, BCRYPT_ECCPUBLIC_BLOB, &k, (PUCHAR)g_relKey, sizeof g_relKey, 0)) {
        ok = BCryptVerifySignature(k, NULL, (PUCHAR)dig, 32, (PUCHAR)sig, 64, 0) == 0;
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

/* Settings "Check for updates" (or a click on the new-version balloon, d = NULL): newest GitHub release,
   checked against its .sig, swapped in for this exe, restarted. Config lives in %APPDATA%, so nothing to carry over. */
static void Update(HWND d)
{
    char *buf = malloc(UPCAP), tag[64], exe[1024], sigUrl[1024];
    UCHAR sig[64];
    wchar_t me[MAX_PATH], nw[MAX_PATH], old[MAX_PATH], msg[256];
    UCHAR dig[32];
    int n, i;
    HANDLE f;
    DWORD wr;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HCURSOR cur = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (!buf) return;
    if (HttpGet("https://api.github.com/repos/beeprat/pigeon/releases/latest", buf, UPCAP) <= 0
          || !JsonStr(buf, "tag_name", "", tag, sizeof tag)) {
        SetCursor(cur); free(buf);
        MessageBoxW(d, L"Couldn't reach GitHub. Try again later.", L"pigeon", MB_ICONWARNING);
        return;
    }
    SetCursor(cur);
    if (!strcmp(tag, PIGEON_VERSION)) {
        free(buf);
        MessageBoxW(d, L"pigeon is up to date (" TEXT(PIGEON_VERSION) L").", L"pigeon", MB_ICONINFORMATION);
        return;
    }
    if (!JsonStr(buf, "browser_download_url", ".exe", exe, sizeof exe) || !JsonStr(buf, "browser_download_url", ".exe.sig", sigUrl, sizeof sigUrl)) {
        free(buf);
        MessageBoxW(d, L"The new release has no download yet. Try again later.", L"pigeon", MB_ICONWARNING);
        return;
    }
    swprintf_s(msg, 256, L"pigeon %hs is out (you have " TEXT(PIGEON_VERSION) L").\n\nDownload it and restart? Your settings and accounts are kept.", tag);
    if (MessageBoxW(d, msg, L"pigeon update", MB_YESNO | MB_ICONQUESTION) != IDYES) { free(buf); return; }
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (HttpGet(sigUrl, buf, UPCAP) != 64) n = -1;
    else {
        memcpy(sig, buf, 64);
        n = HttpGet(exe, buf, UPCAP);
    }
    i = n > 0 && BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)buf, n, dig, 32) == 0 && SigOk(dig, sig, 64);
    SetCursor(cur);
    if (!i) {                           /* download failed or isn't signed by the release key: keep this exe */
        free(buf);
        MessageBoxW(d, L"The download failed or didn't check out. Nothing was changed.", L"pigeon", MB_ICONWARNING);
        return;
    }
    SelfPath(me, L""); SelfPath(nw, L".new"); SelfPath(old, L".old");
    f = CreateFileW(nw, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    i = f != INVALID_HANDLE_VALUE && WriteFile(f, buf, n, &wr, NULL) && wr == (DWORD)n;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(buf);
    /* a running exe can't be overwritten but can be renamed: move it aside, put the new one in its place */
    if (!i || !MoveFileExW(me, old, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(nw); i = 0; }
    else if (!MoveFileExW(nw, me, 0)) { MoveFileExW(old, me, 0); DeleteFileW(nw); i = 0; }
    if (!i) {
        MessageBoxW(d, L"Couldn't replace pigeon.exe (is its folder read-only?). Nothing was changed.", L"pigeon", MB_ICONWARNING);
        return;
    }
    SendMessageW(d, WM_COMMAND, IDCANCEL, 0);   /* close Settings the usual way: saves */
    SaveConfig();
    CloseHandle(g_mutex); g_mutex = NULL;       /* let the new one start */
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    swprintf_s(nw, MAX_PATH, L"\"%s\" /after %lu", me, GetCurrentProcessId());   /* it waits for us to exit */
    if (CreateProcessW(me, nw, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    PostMessageW(g_main, WM_COMMAND, IDM_EXIT, 0);
}

static char g_newTag[64];        /* newest release tag the daily check told about */
#define WM_NEWVER (WM_APP + 6)    /* from UpdCheck: g_newTag differs from this build */

/* Daily, on its own thread: is there a release other than this build? Tells the main window; never downloads. */
static DWORD WINAPI UpdCheck(LPVOID p)
{
    char *buf = malloc(UPCAP), tag[64];
    (void)p;
    if (buf && HttpGet("https://api.github.com/repos/beeprat/pigeon/releases/latest", buf, UPCAP) > 0
          && JsonStr(buf, "tag_name", "", tag, sizeof tag) && strcmp(tag, PIGEON_VERSION) && strcmp(tag, g_newTag)) {
        strcpy_s(g_newTag, sizeof g_newTag, tag);
        PostMessageW(g_main, WM_NEWVER, 0, 0);
    }
    free(buf);
    return 0;
}

static INT_PTR CALLBACK SettingsProc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    HWND lv = GetDlgItem(d, IDC_LIST);
    INT_PTR dr;
    static int init;                    /* filling controls: their change notifications are not edits */
    if (DarkMsg(d, m, w, l, &dr)) return dr;
    switch (m) {
    case WM_LISTSTATE:
        ListStatus(lv);
        return TRUE;
    case WM_NOTIFY: {
        NMLISTVIEW *nm = (NMLISTVIEW *)l;
        int i;
        if (!init && nm->hdr.code == UDN_DELTAPOS && (nm->hdr.idFrom == IDC_QFROMSPIN || nm->hdr.idFrom == IDC_QTOSPIN)) {
            QStep(GetDlgItem(d, nm->hdr.idFrom == IDC_QFROMSPIN ? IDC_QFROM : IDC_QTO), ((NMUPDOWN *)l)->iDelta < 0 ? 1 : -1);   /* up arrow: iDelta -1 */
            return TRUE;
        }
        if (init || nm->hdr.idFrom != IDC_LIST) break;
        if (nm->hdr.code == NM_DBLCLK) { SendMessageW(d, WM_COMMAND, IDC_FIX, 0); return TRUE; }
        if (nm->hdr.code == LVN_COLUMNCLICK) { SortList(lv, nm->iSubItem); return TRUE; }
        if (nm->hdr.code == LVN_KEYDOWN && ((NMLVKEYDOWN *)l)->wVKey == VK_F2) {   /* ListView has no F2 of its own */
            if ((i = ListView_GetNextItem(lv, -1, LVNI_SELECTED)) >= 0) { g_editOk = 1; ListView_EditLabel(lv, i); }
            return TRUE;
        }
        if (nm->hdr.code == NM_RCLICK && ((NMITEMACTIVATE *)l)->iItem >= 0) {   /* right-click menu on an account */
            HMENU pm = CreatePopupMenu();
            POINT pt;
            int row = ((NMITEMACTIVATE *)l)->iItem, cmd;
            ListView_SetItemState(lv, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            AppendMenuW(pm, MF_STRING, 1, L"Set nickname...	F2");
            AppendMenuW(pm, MF_STRING | (g_acc[RowAcc(lv, row)].muted ? MF_CHECKED : 0), 4, L"Mute popups");
            AppendMenuW(pm, MF_STRING, 2, L"Fix sign-in");
            AppendMenuW(pm, MF_SEPARATOR, 0, NULL);
            AppendMenuW(pm, MF_STRING, 3, L"Remove");
            GetCursorPos(&pt);
            cmd = TrackPopupMenu(pm, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, d, NULL);
            DestroyMenu(pm);
            if (cmd == 1) { SetFocus(lv); g_editOk = 1; ListView_EditLabel(lv, row); }
            if (cmd == 4 && (i = RowAcc(lv, row)) >= 0) { g_acc[i].muted = !g_acc[i].muted; ListStatus(lv); SaveConfig(); }
            if (cmd == 2) SendMessageW(d, WM_COMMAND, IDC_FIX, 0);
            if (cmd == 3) SendMessageW(d, WM_COMMAND, IDC_REMOVE, 0);
            return TRUE;
        }
        if (nm->hdr.code == LVN_BEGINLABELEDITW) {   /* rename: edit just the nickname */
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)l;
            HWND ed = ListView_GetEditControl(lv);
            wchar_t w[64];
            if (!g_editOk || (i = RowAcc(lv, di->item.iItem)) < 0) { g_editOk = 0; SetWindowLongPtrW(d, DWLP_MSGRESULT, TRUE); return TRUE; }
            g_editOk = 0;
            MultiByteToWideChar(CP_UTF8, 0, g_acc[i].nick, -1, w, 64);
            SetWindowTextW(ed, w);
            SendMessageW(ed, EM_SETLIMITTEXT, 40, 0);
            SendMessageW(ed, EM_SETCUEBANNER, TRUE, (LPARAM)L"Nickname, e.g. Work");
            SetWindowLongPtrW(d, DWLP_MSGRESULT, FALSE);
            return TRUE;
        }
        if (nm->hdr.code == LVN_ENDLABELEDITW) {
            NMLVDISPINFOW *di = (NMLVDISPINFOW *)l;
            wchar_t w[256], *c;
            if (di->item.pszText && (i = RowAcc(lv, di->item.iItem)) >= 0) {
                char *t;
                for (c = di->item.pszText; *c; c++) if (*c == L'|' || *c < 32) *c = L' ';   /* | splits the config line */
                WideCharToMultiByte(CP_UTF8, 0, di->item.pszText, -1, g_acc[i].nick, sizeof g_acc[i].nick, NULL, NULL);
                t = Trim(g_acc[i].nick);
                memmove(g_acc[i].nick, t, strlen(t) + 1);
                ListName(i, w);
                ListView_SetItemText(lv, di->item.iItem, 0, w);
                SaveConfig();
            }
            SetWindowLongPtrW(d, DWLP_MSGRESULT, FALSE);   /* we set the text ourselves */
            return TRUE;
        }
        if (nm->hdr.code != LVN_ITEMCHANGED || !(nm->uChanged & LVIF_STATE) || !((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) break;
        if ((i = RowAcc(lv, nm->iItem)) < 0) break;
        g_acc[i].enabled = ListView_GetCheckState(lv, nm->iItem) ? 1 : 0;
        g_acc[i].state = 0;
        Kick(&g_acc[i]);
        g_imap = AnyEnabled();
        SaveConfig();
        UpdateTip();
        ListStatus(lv);
        return TRUE;
    }
    case WM_INITDIALOG: {
        LVCOLUMNW c;
        RECT rc;
        int i;
        g_settings = d;
        init = 1;
        ShowWindow(GetDlgItem(d, IDC_TESTPOP), g_testMode ? SW_SHOW : SW_HIDE);
        DarkDlg(d);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP | LVS_EX_DOUBLEBUFFER);   /* drag headers to reorder */
        GetClientRect(lv, &rc);
        memset(&c, 0, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = L"Name";
        c.cx = rc.right * 50 / 100;     /* matches the NAME/STATUS/PROVIDER labels in pigeon.rc */
        ListView_InsertColumn(lv, 0, &c);
        c.pszText = L"Status";
        c.cx = rc.right * 27 / 100;
        ListView_InsertColumn(lv, 1, &c);
        c.pszText = L"Provider";
        c.cx = rc.right * 23 / 100 - GetSystemMetrics(SM_CXVSCROLL);
        ListView_InsertColumn(lv, 2, &c);
        for (i = 0; i < g_nacc; i++) if (!g_acc[i].removed) ListAdd(lv, i);
        if (g_colW[0] && g_colW[1] && g_colW[2]) for (i = 0; i < 3; i++) ListView_SetColumnWidth(lv, i, DP(g_colW[i]));
        ListView_SetColumnOrderArray(lv, 3, g_colOrder);
        if (g_sortCol >= 0) { g_sortDir = -g_sortDir; SortList(lv, g_sortCol); }   /* SortList flips it back */
        {   /* checkboxes in the theme: grey outline off, white with a black tick on */
            int sz = DP(16), k;
            HIMAGELIST il = ImageList_Create(sz, sz, ILC_COLOR24, 2, 0);
            HDC sdc = GetDC(NULL), mdc = CreateCompatibleDC(sdc);
            for (k = 0; k < 2; k++) {
                HBITMAP bm = CreateCompatibleBitmap(sdc, sz, sz);
                HGDIOBJ ob = SelectObject(mdc, bm);
                RECT b = {0, 0, sz, sz}, in;
                FillRect(mdc, &b, g_fieldBrush);
                InflateRect(&b, -DP(2), -DP(2));
                SmoothRound(mdc, b, DP(3), k ? Gr(240) : Gr(140));
                in = b;
                InflateRect(&in, -DP(1) - 1, -DP(1) - 1);
                if (!k) SmoothRound(mdc, in, DP(2), FIELD);
                else {
                    HPEN pen = CreatePen(PS_SOLID, DP(2), Gr(20));
                    HGDIOBJ op = SelectObject(mdc, pen);
                    MoveToEx(mdc, b.left + DP(3), b.top + DP(6), NULL);
                    LineTo(mdc, b.left + DP(5), b.top + DP(8));
                    LineTo(mdc, b.right - DP(3), b.top + DP(3));
                    SelectObject(mdc, op);
                    DeleteObject(pen);
                }
                SelectObject(mdc, ob);
                ImageList_Add(il, bm, NULL);
                DeleteObject(bm);
            }
            DeleteDC(mdc);
            ReleaseDC(NULL, sdc);
            il = ListView_SetImageList(lv, il, LVSIL_STATE);
            if (il) ImageList_Destroy(il);
        }
        ListStatus(lv);
        SetDlgItemInt(d, IDC_POPUP, g_popupMs / 1000, FALSE);
        CheckDlgButton(d, IDC_SOUND, g_sound ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_AUTOSTART, AutoStartGet() ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(d, IDC_SOUNDFILE, g_soundFile);
        CheckDlgButton(d, IDC_FULLHOLD, g_fullHold ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_QUIET, g_quiet ? BST_CHECKED : BST_UNCHECKED);
        QSet(GetDlgItem(d, IDC_QFROM), g_quietFrom);
        QSet(GetDlgItem(d, IDC_QTO), g_quietTo);
        SetWindowSubclass(GetDlgItem(d, IDC_QFROM), QProc, 1, 0);
        SetWindowSubclass(GetDlgItem(d, IDC_QTO), QProc, 1, 0);
        {
            wchar_t w[512];
            MultiByteToWideChar(CP_UTF8, 0, g_vip, -1, w, 512);
            SetDlgItemTextW(d, IDC_VIP, w);
            MultiByteToWideChar(CP_UTF8, 0, g_mute, -1, w, 512);
            SetDlgItemTextW(d, IDC_MUTE, w);
        }
        CheckDlgButton(d, IDC_MUTECAT, g_muteCat ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(d, IDC_CORNER0 + g_corner, BST_CHECKED);
        SendDlgItemMessageW(d, IDC_SOUNDFILE, EM_SETCUEBANNER, TRUE, (LPARAM)L"Windows default sound");
        SendDlgItemMessageW(d, IDC_VIP, EM_SETCUEBANNER, TRUE, (LPARAM)L"Everyone");
        SendDlgItemMessageW(d, IDC_MUTE, EM_SETCUEBANNER, TRUE, (LPARAM)L"Nobody");
        SetForegroundWindow(d);
        init = 0;
        return TRUE;
    }
    case WM_COMMAND:
        if (!init && (HIWORD(w) == EN_CHANGE || HIWORD(w) == BN_CLICKED || HIWORD(w) == CBN_SELCHANGE)) ApplySettings(d);
        switch (LOWORD(w)) {
        case IDC_ADD:
            if (AddAccount(d)) { init = 1; ListAdd(lv, g_nacc - 1); init = 0; ListStatus(lv); }
            break;
        case IDC_REMOVE: {
            int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED), i = row < 0 ? -1 : RowAcc(lv, row);
            if (i < 0) break;
            init = 1;                   /* deleting the row is not an uncheck */
            ListView_DeleteItem(lv, row);
            init = 0;
            g_acc[i].removed = 1;
            g_acc[i].enabled = 0;
            Kick(&g_acc[i]);
            g_imap = AnyEnabled();
            SaveConfig();
            UpdateTip();
            break;
        }
        case IDC_FIX: {
            int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED), i = row < 0 ? -1 : RowAcc(lv, row);
            if (i < 0) { MessageBoxW(d, L"Select an account first.", L"pigeon", MB_ICONINFORMATION); break; }
            if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ADD), d, AddProc, (LPARAM)&g_acc[i]) == IDOK) {
                SaveConfig();
                ListStatus(lv);
            }
            break;
        }
        case IDC_BROWSE: {
            OPENFILENAMEW of;
            wchar_t file[MAX_PATH] = L"";
            memset(&of, 0, sizeof of);
            of.lStructSize = sizeof of;
            of.hwndOwner = d;
            of.lpstrFilter = L"WAV sound (*.wav)\0*.wav\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH;
            of.lpstrInitialDir = L"C:\\Windows\\Media";
            of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&of)) SetDlgItemTextW(d, IDC_SOUNDFILE, file);
            break;
        }
        case IDC_TESTPOP: {
            int keep = g_sound, keepCorner = g_corner;
            wchar_t keepFile[MAX_PATH];
            wcscpy_s(keepFile, MAX_PATH, g_soundFile);
            g_sound = IsDlgButtonChecked(d, IDC_SOUND) == BST_CHECKED;     /* preview what is on screen */
            GetDlgItemTextW(d, IDC_SOUNDFILE, g_soundFile, MAX_PATH);
            ShowPopup(L"Name of sender", L"Subject of the mail", g_nacc ? TestTo(&g_acc[0]) : NULL, L"Hi! Just checking in about tomorrow, are we still on for lunch?");
            PlayNotify();
            g_sound = keep;
            g_corner = keepCorner;
            wcscpy_s(g_soundFile, MAX_PATH, keepFile);
            break;
        }
        case IDC_UPDATE:
            Update(d);
            return TRUE;
        case IDCANCEL:                  /* Close, Esc and the X: write it all down */
            ApplySettings(d);
            AutoStartSet(IsDlgButtonChecked(d, IDC_AUTOSTART) == BST_CHECKED);
            if (!SaveConfig()) MessageBoxW(d, L"Could not save settings.", L"pigeon", MB_ICONERROR);
            EndDialog(d, IDCANCEL);
            break;
        }
        return TRUE;
    case WM_DESTROY:
        g_settings = NULL;
        break;
    }
    return FALSE;
}

/* ---------- Popup buttons ---------- */

#define QMAX 16
#define TIMER_SNOOZE 100        /* 100..100+QMAX-1: one per snoozed mail */
static NewMail *g_snz[QMAX];
static ULONGLONG g_snzDue[QMAX];        /* when it comes back, ms since 1601 (UTC) */

static ULONGLONG NowMs(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000;
}

static void SnzPath(wchar_t *p)
{
    wcscpy_s(p, MAX_PATH, g_cfgPath);
    wcscpy_s(wcsrchr(p, L'\\') + 1, 16, L"snooze.txt");
}

/* Field for snooze.txt: UTF-8 with tabs and line breaks blanked, since they separate fields and entries */
static void PutField(FILE *f, const wchar_t *w, int last)
{
    char u8[2048], *c;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof u8, NULL, NULL);
    for (c = u8; *c; c++) if (*c == '\t' || *c == '\r' || *c == '\n') *c = ' ';
    fputs(u8, f);
    fputc(last ? '\n' : '\t', f);
}

/* One line per snoozed mail: due, uid, account, from, subject, url, to, preview */
static void SnzSave(void)
{
    wchar_t p[MAX_PATH];
    FILE *f;
    int i;
    SnzPath(p);
    f = _wfopen(p, L"wb");
    if (!f) return;
    for (i = 0; i < QMAX; i++) {
        NewMail *n = g_snz[i];
        if (!n) continue;
        fprintf(f, "%llu:%llu\t%lu\t%s\t", g_snzDue[i], n->when, n->uid, n->acc ? n->acc->email : "");   /* due:received */
        PutField(f, n->from, 0);
        PutField(f, n->subj, 0);
        PutField(f, n->url, 0);
        PutField(f, n->to, 0);
        PutField(f, n->prev, 1);
    }
    fclose(f);
}

static void SnzArm(int i)
{
    ULONGLONG now = NowMs(), left = g_snzDue[i] > now ? g_snzDue[i] - now : 1000;
    if (left > 0x7FFFFFFF) left = 0x7FFFFFFF;
    SetTimer(g_main, TIMER_SNOOZE + i, (UINT)left, NULL);
}

/* At startup: snoozed mail from before a restart comes back on time (or right away if its time passed) */
static void SnzLoad(void)
{
    wchar_t p[MAX_PATH];
    char line[8192];
    FILE *f;
    int i = 0;
    SnzPath(p);
    f = _wfopen(p, L"rb");
    if (!f) return;
    while (i < QMAX && fgets(line, sizeof line, f)) {
        char *fld[8], *c = line, *ctx = NULL;
        int k, a;
        NewMail *n;
        for (k = 0; k < 8; k++) {           /* strtok would merge empty fields */
            fld[k] = c;
            c = strchr(c, k < 7 ? '\t' : '\n');
            if (!c) break;
            *c++ = 0;
        }
        (void)ctx;
        if (k < 7) continue;
        if (k == 8 || !strchr(fld[7], '\n')) { char *e = strpbrk(fld[7], "\r\n"); if (e) *e = 0; }
        if (!(n = calloc(1, sizeof *n))) break;
        g_snzDue[i] = _strtoui64(fld[0], &c, 10);
        if (*c == ':') n->when = _strtoui64(c + 1, NULL, 10);   /* older files have no received time */
        n->uid = strtoul(fld[1], NULL, 10);
        for (a = 0; a < g_nacc; a++) if (!strcmp(g_acc[a].email, fld[2])) n->acc = &g_acc[a];
        MultiByteToWideChar(CP_UTF8, 0, fld[3], -1, n->from, 256);
        MultiByteToWideChar(CP_UTF8, 0, fld[4], -1, n->subj, 256);
        MultiByteToWideChar(CP_UTF8, 0, fld[5], -1, n->url, 512);
        MultiByteToWideChar(CP_UTF8, 0, fld[6], -1, n->to, 256);
        MultiByteToWideChar(CP_UTF8, 0, fld[7], -1, n->prev, 300);
        g_snz[i] = n;
        SnzArm(i++);
    }
    fclose(f);
}

typedef struct { Account acc; unsigned long uid; int act; } Act;

static void Notice(const wchar_t *subj)
{
    NewMail *nm = calloc(1, sizeof *nm);
    if (!nm) return;
    wcscpy_s(nm->from, 256, L"pigeon");
    wcscpy_s(nm->subj, 256, subj);
    wcscpy_s(nm->url, 512, L"https://mail.google.com/mail/#inbox");
    PostMessageW(g_main, WM_NEWMAIL, 0, (LPARAM)nm);
}

/* Logs in on a separate connection (the main one is busy in IDLE), changes one mail, logs out. */
static DWORD WINAPI ActThread(LPVOID arg)
{
    static const char *store[3] = {"+FLAGS.SILENT (\\Seen)", "-X-GM-LABELS (\\Inbox)", "+FLAGS.SILENT (\\Flagged)"};
    static const wchar_t *fail[3] = {L"Could not mark the mail as seen", L"Could not archive the mail", L"Could not star the mail"};
    Act *a = arg;
    Tls *t = calloc(1, sizeof *t);
    char line[2048], cmd[600], tag[16];
    int ok = 0;
    if (t) {
        t->s = INVALID_SOCKET;
        t->acc = &a->acc;               /* private copy: leaves the IDLE connection's socket and tags alone */
        t->waitMs = 30000;
        if (TlsConnect(t, Prov(a->acc.email)->host, "993") && ReadLine(t, line, sizeof line) >= 0 && strncmp(line, "* OK", 4) == 0) {
            if (Login(t, 0) == 1 &&
                Cmd(t, "SELECT INBOX", tag) && WaitOk(t, tag, NULL) == 1) {
                if (a->act == 1 && Prov(a->acc.email)->kind != 0) {   /* no Gmail labels: move to the folder marked \Archive */
                    char box[256] = "Archive";
                    size_t tl;
                    if (Cmd(t, "LIST (SPECIAL-USE) \"\" \"*\"", tag)) {
                        tl = strlen(tag);
                        while (ReadLine(t, line, sizeof line) >= 0 && !(strncmp(line, tag, tl) == 0 && line[tl] == ' ')) {
                            char *q;
                            if (!strstr(line, "\\Archive")) continue;
                            q = line + strlen(line);
                            while (q > line && (q[-1] == '"' || q[-1] == ' ')) *--q = 0;
                            while (q > line && q[-1] != ' ' && q[-1] != '"') q--;
                            if (*q) strcpy_s(box, sizeof box, q);
                        }
                    }
                    strcpy_s(cmd, sizeof cmd, "UID MOVE ");
                    sprintf_s(cmd + strlen(cmd), sizeof cmd - strlen(cmd), "%lu ", a->uid);
                    QuoteTo(cmd, sizeof cmd, box);
                } else sprintf_s(cmd, sizeof cmd, "UID STORE %lu %s", a->uid, store[a->act]);
                ok = Cmd(t, cmd, tag) && WaitOk(t, tag, NULL) == 1;
                if (Cmd(t, "LOGOUT", tag)) WaitOk(t, tag, NULL);
            }
        }
        TlsClose(t);
        free(t);
    }
    if (!ok) Notice(fail[a->act]);
    SecureZeroMemory(a, sizeof *a);
    free(a);
    return 0;
}

static void Snooze(int minutes)
{
    int i;
    for (i = 0; i < QMAX && g_snz[i]; i++);
    if (i == QMAX || !(g_snz[i] = malloc(sizeof(NewMail)))) return;
    *g_snz[i] = g_cur;
    g_snzDue[i] = NowMs() + (ULONGLONG)minutes * 60 * 1000;
    SnzArm(i);
    SnzSave();
}

/* b: 0 seen, 1 archive, 2 star, 3 snooze, NBTN close */
static void PopupAction(int b)
{
    int snz = g_snzMode;
    if (g_alpha < 255 || g_anim) return;   /* already going: a second click must not act twice */
    if (b == NBTN) {                    /* close cross: same fade as timing out */
        KillTimer(g_popup, TIMER_HIDE);
        SetTimer(g_popup, TIMER_FADE, 30, NULL);
        return;
    }
    if (g_snzMode) {                    /* second click: one of the snooze times */
        static const int mins[NBTN] = {10, 60, 180, 480};
        g_snzMode = 0;
        Snooze(mins[b]);
    } else if (b == 3) {                /* bar turns into the snooze times; leaving the popup turns it back */
        g_snzMode = 1;
        BtnTip(g_popup, -1);
        SetTimer(g_popup, TIMER_TIP, 1000, NULL);
        InvalidateRect(g_popup, NULL, FALSE);
        return;
    } else if (g_cur.acc && g_cur.uid) {
        Act *a = malloc(sizeof *a);
        if (a) {
            a->acc = *g_cur.acc;
            a->acc.sock = INVALID_SOCKET;
            a->acc.tag = 0;
            a->uid = g_cur.uid;
            a->act = b;
            CloseHandle(CreateThread(NULL, 0, ActThread, a, 0, NULL));
        }
    }
    KillTimer(g_popup, TIMER_HIDE);
    KillTimer(g_popup, TIMER_TIP);      /* clicked before the tooltip came up: it never does */
    BtnTip(g_popup, -1);
    if (b >= 0 && b < NBTN && g_bar) {  /* animation first; TIMER_ANIM starts the fade */
        g_anim = b + 1;
        g_animSnz = snz;
        g_animStart = GetTickCount64();
        SetTimer(g_popup, TIMER_ANIM, 10, NULL);
        InvalidateRect(g_popup, NULL, FALSE);
        return;
    }
    SetTimer(g_popup, TIMER_FADE, 30, NULL);   /* fade out, like the close cross */
}

/* ---------- Test menu: fake mail through the real queue and popup ---------- */

#define IDD_TEST 103
#define IDT_SHORT 3001
#define IDT_LONG 3002
#define IDT_MORE 3003
#define IDT_QUEUE 3004
#define IDT_ACCOUNTS 3005
#define IDT_SOUND 3006
#define IDT_RECHECK 3007
#define IDT_STARTUP 3008
#define IDT_PROVIDERS 3009
#define IDT_LONGTEXT 3010

static int g_noBatch;               /* test menu's provider popups still queued: shown one by one, not batched */

static void FakeMailAcc(const wchar_t *from, const wchar_t *subj, const wchar_t *to, Account *acc, const wchar_t *prev)
{
    NewMail *nm = calloc(1, sizeof *nm);
    if (!nm) return;
    nm->acc = acc;
    wcscpy_s(nm->from, 256, from);
    wcscpy_s(nm->subj, 256, subj);
    if (to) wcscpy_s(nm->to, 256, to);
    wcsncpy_s(nm->prev, 300, prev ? prev : L"Hi! Just checking in about tomorrow, are we still on for lunch?", _TRUNCATE);
    wcscpy_s(nm->url, 512, L"https://mail.google.com/mail/#inbox");
    GetSystemTimeAsFileTime((FILETIME *)&nm->when);
    PostMessageW(g_main, WM_NEWMAIL, 0, (LPARAM)nm);
}

static void FakeMail(const wchar_t *from, const wchar_t *subj, const wchar_t *to) { FakeMailAcc(from, subj, to, NULL, NULL); }

/* One popup per provider pigeon knows, each from a made-up address on its domain, so every corner logo shows. */
static void FakeProviders(void)
{
    static Account fake[sizeof g_prov / sizeof g_prov[0]];
    size_t i, j, n = 0;
    for (i = 0; i < sizeof g_prov / sizeof g_prov[0]; i++) {
        Account *a = &fake[n];
        char *star;
        wchar_t subj[64];
        for (j = 0; j < i && wcscmp(g_prov[j].name, g_prov[i].name); j++) ;
        if (j < i) continue;            /* provider already shown */
        memset(a, 0, sizeof *a);
        a->sock = INVALID_SOCKET;       /* never connected: actions on it go nowhere */
        sprintf_s(a->email, sizeof a->email, "test@%s", g_prov[i].dom);
        if ((star = strchr(a->email, '*')) != NULL) strcpy_s(star, 4, "com");
        swprintf_s(subj, 64, L"%s test", g_prov[i].name);
        FakeMailAcc(L"Name of sender", subj, TestTo(a), a, NULL);
        n++;
    }
    g_noBatch = (int)n;
}

static INT_PTR CALLBACK TestProc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    const wchar_t *to = g_nacc ? TestTo(&g_acc[0]) : NULL;
    int i;
    INT_PTR dr;
    if (DarkMsg(d, m, w, l, &dr)) return dr;
    if (m == WM_INITDIALOG) {
        DarkDlg(d);
        SetForegroundWindow(d);
        return TRUE;
    }
    if (m != WM_COMMAND) return FALSE;
    switch (LOWORD(w)) {
    case IDT_SHORT:  FakeMail(L"Name of sender", L"Subject of the mail", to); break;
    case IDT_LONGTEXT:                  /* everything long: name and subject get "...", the hovered preview stops at 4 lines */
        FakeMailAcc(L"A sender with a rather long display name that keeps on going",
                    L"This is a very long subject line to check how wide the popup gets before it is cut off with dots", to, NULL,
                    L"Hi! Just checking in about tomorrow, are we still on for lunch? I was thinking of the new place on Main Street "
                    L"around 12:30, they have a patio and the menu looks great. If that doesn't work, Thursday or Friday are open too, "
                    L"and I could also do dinner instead. Let me know what suits you best and I'll book a table for us.");
        break;
    case IDT_LONG:   FakeMail(L"A sender with a rather long display name",
                              L"This is a very long subject line to check how wide the popup gets before it is cut off with dots", to); break;
    case IDT_MORE:   FakeMail(L"Name of sender  +3 more", L"Newest of four mails at once", to); break;
    case IDT_QUEUE:
        FakeMail(L"First sender", L"Mail 1 of 3", to);
        FakeMail(L"Second sender", L"Mail 2 of 3", to);
        FakeMail(L"Third sender", L"Mail 3 of 3", to);
        break;
    case IDT_ACCOUNTS:
        for (i = 0; i < g_nacc; i++) FakeMail(L"Name of sender", L"One popup per account", TestTo(&g_acc[i]));
        break;
    case IDT_PROVIDERS: FakeProviders(); break;
    case IDT_SOUND:  PlayNotify(); break;
    case IDT_RECHECK:   /* real mail: step back and reconnect; only unread ones pop up */
    case IDT_STARTUP: { /* same window startup scans: the last 200 mails per account */
        unsigned long back = LOWORD(w) == IDT_STARTUP ? 200 : 5;
        for (i = 0; i < g_nacc; i++) g_acc[i].uidnext = g_acc[i].uidnext > back ? g_acc[i].uidnext - back : 1;
        ImapKick();
        break;
    }
    case IDCANCEL:   EndDialog(d, 0); break;
    }
    return TRUE;
}

static int g_reopen;             /* theme changed while Settings was open: open it again */
static void OpenSettings(void)
{
    if (g_settings) { SetForegroundWindow(g_settings); return; }
    do {
        g_reopen = 0;
        DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, SettingsProc, 0);
    } while (g_reopen);
}

/* ---------- Tray badge ---------- */

static HICON g_baseIcon, g_badgeIcon;

/* Straight-alpha "over": paints color c with coverage a (0..255) onto pixel p (BGRA) */
static void Over(BYTE *p, COLORREF c, int a)
{
    int pa = p[3], oa = a + pa * (255 - a) / 255;
    if (!oa) return;
    p[0] = (BYTE)((GetBValue(c) * a + p[0] * pa * (255 - a) / 255) / oa);
    p[1] = (BYTE)((GetGValue(c) * a + p[1] * pa * (255 - a) / 255) / oa);
    p[2] = (BYTE)((GetRValue(c) * a + p[2] * pa * (255 - a) / 255) / oa);
    p[3] = (BYTE)oa;
}

/* The app icon with a red pill holding n at the bottom right */
static HICON BadgeIcon(HICON base, int n)
{
    int sz = GetSystemMetrics(SM_CXSMICON), x, y, sx, sy, i, ph = sz * 5 / 8, pw, px, py, tw, th;
    wchar_t num[8];
    ICONINFO ii;
    BITMAPINFO bi;
    BYTE *px32, *tx;
    HDC dc = GetDC(NULL), m = CreateCompatibleDC(dc);
    HBITMAP tb, mask;
    HFONT f;
    HGDIOBJ of, ob;
    SIZE ts;
    HICON out = NULL;

    swprintf_s(num, 8, n > 99 ? L"99+" : L"%d", n);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;            /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    px32 = calloc((size_t)sz * sz, 4);
    if (!px32 || !GetIconInfo(base, &ii)) goto done;
    GetDIBits(dc, ii.hbmColor, 0, sz, px32, &bi, DIB_RGB_COLORS);
    for (i = 0; i < sz * sz && !px32[i * 4 + 3]; i++);
    if (i == sz * sz) {                     /* icon without alpha: take it from the mask */
        BYTE *mk = calloc((size_t)sz * sz, 4);
        if (mk) {
            GetDIBits(dc, ii.hbmMask, 0, sz, mk, &bi, DIB_RGB_COLORS);
            for (i = 0; i < sz * sz; i++) px32[i * 4 + 3] = mk[i * 4] ? 0 : 255;
            free(mk);
        }
    }
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);

    /* number: white on black in a scratch bitmap, its brightness is the text coverage */
    f = CreateFontW(-(ph - 1), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    of = SelectObject(m, f);
    GetTextExtentPoint32W(m, num, (int)wcslen(num), &ts);
    tw = ts.cx;
    th = ts.cy;
    pw = tw + ph / 2 > ph ? tw + ph / 2 : ph;
    if (pw > sz) pw = sz;
    px = sz - pw;
    py = sz - ph;
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;
    tb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&tx, NULL, 0);
    if (!tb) { SelectObject(m, of); DeleteObject(f); goto done; }
    ob = SelectObject(m, tb);
    SetBkColor(m, RGB(0, 0, 0));
    SetTextColor(m, RGB(255, 255, 255));
    {
        RECT r = {0, 0, pw, ph};
        r.top = (ph - th) / 2;
        r.bottom = r.top + th;
        ExtTextOutW(m, 0, 0, ETO_OPAQUE, &(RECT){0, 0, pw, ph}, NULL, 0, NULL);
        DrawTextW(m, num, -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    GdiFlush();
    /* pill: 4x4 samples per pixel for smooth edges, then the number on top */
    for (y = 0; y < ph; y++)
        for (x = 0; x < pw; x++) {
            int cov = 0;
            for (sy = 0; sy < 4; sy++)
                for (sx = 0; sx < 4; sx++) {
                    double fx = x + (sx + 0.5) / 4, fy = y + (sy + 0.5) / 4, r = ph / 2.0;
                    double cx = fx < r ? r : fx > pw - r ? pw - r : fx, dx = fx - cx, dy = fy - r;
                    if (dx * dx + dy * dy <= r * r) cov++;
                }
            if (cov) {
                BYTE *p = px32 + ((py + y) * sz + px + x) * 4;
                Over(p, RGB(234, 67, 53), cov * 255 / 16);
                Over(p, RGB(255, 255, 255), tx[(y * pw + x) * 4 + 1] * cov / 16);
            }
        }
    SelectObject(m, ob);
    DeleteObject(tb);
    SelectObject(m, of);
    DeleteObject(f);
    {
        HBITMAP col;
        BYTE *cb;
        bi.bmiHeader.biWidth = sz;
        bi.bmiHeader.biHeight = -sz;
        col = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&cb, NULL, 0);
        mask = CreateBitmap(sz, sz, 1, 1, NULL);
        if (col && mask) {
            BYTE *zero = calloc(((sz + 15) / 16) * 2, sz);
            memcpy(cb, px32, (size_t)sz * sz * 4);
            if (zero) { SetBitmapBits(mask, ((sz + 15) / 16) * 2 * sz, zero); free(zero); }
            ii.fIcon = TRUE;
            ii.xHotspot = ii.yHotspot = 0;
            ii.hbmColor = col;
            ii.hbmMask = mask;
            out = CreateIconIndirect(&ii);
        }
        if (col) DeleteObject(col);
        if (mask) DeleteObject(mask);
    }
done:
    free(px32);
    DeleteDC(m);
    ReleaseDC(NULL, dc);
    return out;
}

/* Tray icon shows the total unread count of the enabled accounts */
static void UpdateBadge(void)
{
    NOTIFYICONDATAW n = g_nid;
    HICON old = g_badgeIcon;
    int i, total = 0;
    for (i = 0; i < g_nacc; i++) if (g_acc[i].enabled && g_acc[i].state == 1) total += g_acc[i].unread;
    g_badgeIcon = total ? BadgeIcon(g_baseIcon, total) : NULL;
    n.uFlags = NIF_ICON;
    n.hIcon = g_badgeIcon ? g_badgeIcon : g_baseIcon;
    Shell_NotifyIconW(NIM_MODIFY, &n);
    if (old) DestroyIcon(old);
}

/* ---------- Do not disturb ---------- */

static int InQuietHours(void)
{
    SYSTEMTIME st;
    int now;
    if (NowMs() < g_pauseUntil) return 1;
    if (!g_quiet || g_quietFrom == g_quietTo) return 0;
    GetLocalTime(&st);
    now = st.wHour * 60 + st.wMinute;
    return g_quietFrom < g_quietTo ? now >= g_quietFrom && now < g_quietTo
                                   : now >= g_quietFrom || now < g_quietTo;
}

/* A fullscreen game, video or presentation is in front */
static int FullscreenBusy(void)
{
    QUERY_USER_NOTIFICATION_STATE st;
    if (!g_fullHold || FAILED(SHQueryUserNotificationState(&st))) return 0;
    return st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
}

static NewMail *g_q[QMAX];       /* mail waiting for the popup to close */
#define BATCH 5                  /* this many waiting: shown as one summary popup */
static int g_qn;
static int g_held;               /* mails that came during quiet hours */
static char g_heldAcc[128];      /* account of the last one, for the summary's link */

#define NHIST 10
static NewMail g_hist[NHIST];    /* last popups, newest first, for the tray's "Recent" menu */
static int g_nhist;

static void HistAdd(const NewMail *nm)
{
    int i;
    for (i = 0; i < g_nhist; i++)       /* shown again: move it to the top */
        if (!wcscmp(g_hist[i].url, nm->url) && !wcscmp(g_hist[i].subj, nm->subj)) {
            memmove(g_hist + i, g_hist + i + 1, (g_nhist - i - 1) * sizeof *g_hist);
            g_nhist--;
            break;
        }
    memmove(g_hist + 1, g_hist, (NHIST - 1) * sizeof *g_hist);
    g_hist[0] = *nm;
    if (g_nhist < NHIST) g_nhist++;
}

static void ShowNext(void)
{
    NewMail *nm;
    if (!g_qn) return;
    if (FullscreenBusy()) {             /* hold the queue; TIMER_DND tries again */
        SetTimer(g_main, TIMER_DND, 5000, NULL);
        return;
    }
    if (g_qn >= BATCH && !g_noBatch) {  /* a pile-up: one "N new mails" popup instead of a long chain */
        int i, total = 0;
        wchar_t who[256] = L"";
        nm = calloc(1, sizeof *nm);
        if (!nm) return;
        nm->acc = g_q[0]->acc;          /* badge and inbox link: the first one's account */
        InboxUrl(nm->acc ? nm->acc->email : "", nm->url);
        for (i = 0; i < g_qn; i++) {
            NewMail *q = g_q[i];
            wchar_t *more = wcsstr(q->from, L"  +"), name[256];
            total += 1 + (more ? _wtoi(more + 3) : 0);   /* "Anna  +2 more" is 3 mails */
            wcsncpy_s(name, 256, q->from, more ? (size_t)(more - q->from) : _TRUNCATE);
            if (name[0] && !wcsstr(who, name) && wcslen(who) + wcslen(name) + 2 < 256) {
                if (who[0]) wcscat_s(who, 256, L", ");
                wcscat_s(who, 256, name);
            }
            HistAdd(q);
            free(q);
        }
        swprintf_s(nm->from, 256, L"%d new mails", total);
        wcscpy_s(nm->subj, 256, who[0] ? who : L"New mail");
        g_qn = 0;
        nm->uid = NOMAIL;               /* nothing to act on: no button bar */
    } else {
        if (g_noBatch) g_noBatch--;
        nm = g_q[0];
        memmove(g_q, g_q + 1, (--g_qn) * sizeof *g_q);
    }
    g_cur = *nm;
    HistAdd(nm);
    wcscpy_s(g_mailUrl, 512, nm->url);
    ShowPopup(nm->from[0] ? nm->from : L"New mail", nm->subj[0] ? nm->subj : L"(no subject)", nm->to, nm->prev);
    PlayNotify();
    free(nm);
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NEWVER:                     /* balloon: pigeon vX is out; a click runs the usual update, which asks first */
        g_nid.uFlags = NIF_INFO;
        swprintf_s(g_nid.szInfoTitle, 64, L"pigeon %hs is out", g_newTag);
        wcscpy_s(g_nid.szInfo, 256, L"Click to update.");
        g_nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        return 0;
    case WM_TIMER:
        if (w == TIMER_UPD) {
            HANDLE t;
            SetTimer(h, TIMER_UPD, 24 * 3600000, NULL);   /* first one a minute after start, then daily */
            if ((t = CreateThread(NULL, 0, UpdCheck, NULL, 0, NULL)) != NULL) CloseHandle(t);
            return 0;
        }
        if (w == TIMER_QUIET && g_held && !InQuietHours()) {
            NewMail *nm = calloc(1, sizeof *nm);
            if (nm) {
                wcscpy_s(nm->from, 256, L"While quiet hours");
                swprintf_s(nm->subj, 256, g_held == 1 ? L"1 new mail" : L"%d new mails", g_held);
                InboxUrl(g_heldAcc, nm->url);
                nm->uid = NOMAIL;
                g_held = 0;
                PostMessageW(h, WM_NEWMAIL, 0, (LPARAM)nm);
            }
        }
        if (w == TIMER_DND) {
            KillTimer(h, TIMER_DND);
            if (!IsWindowVisible(g_popup)) ShowNext();   /* sets the timer again if still fullscreen */
        }
        if (w >= TIMER_SNOOZE && w < TIMER_SNOOZE + QMAX) {   /* snoozed mail comes back */
            KillTimer(h, w);
            if (g_snz[w - TIMER_SNOOZE]) PostMessageW(h, WM_NEWMAIL, 0, (LPARAM)g_snz[w - TIMER_SNOOZE]);
            g_snz[w - TIMER_SNOOZE] = NULL;
            SnzSave();
        }
        return 0;
    case WM_UNREAD:
        UpdateBadge();
        if (g_settings) PostMessageW(g_settings, WM_LISTSTATE, 0, 0);
        return 0;
    case WM_NEWMAIL:
        if (((NewMail *)l)->acc && ((NewMail *)l)->acc->muted) { free((NewMail *)l); return 0; }   /* muted account */
        if (InQuietHours()) {           /* no popup now; one summary when quiet hours end (TIMER_QUIET) */
            NewMail *nm = (NewMail *)l;
            g_held++;
            if (nm->acc) strcpy_s(g_heldAcc, sizeof g_heldAcc, nm->acc->email);
            free(nm);
            return 0;
        }
        if (g_qn == QMAX) { free(g_q[0]); memmove(g_q, g_q + 1, (QMAX - 1) * sizeof *g_q); g_qn--; }   /* full: drop oldest */
        g_q[g_qn++] = (NewMail *)l;
        if (!IsWindowVisible(g_popup)) ShowNext();   /* otherwise it waits for WM_NEXTMAIL */
        return 0;
    case WM_NEXTMAIL:
        if (g_qn && !IsWindowVisible(g_popup)) ShowNext();
        return 0;
    case WM_SAVECFG:
        SaveConfig();
        return 0;
    default:
        if (m == g_taskbarMsg && m) { Shell_NotifyIconW(NIM_ADD, &g_nid); Shell_NotifyIconW(NIM_SETVERSION, &g_nid); UpdateBadge(); return 0; }   /* Explorer restarted */
        break;
    case WM_TRAY:
        if (LOWORD(l) == NIN_POPUPOPEN) { POINT pt = {GET_X_LPARAM(w), GET_Y_LPARAM(w)}; CardShow(pt); return 0; }
        if (LOWORD(l) == NIN_POPUPCLOSE) { if (g_card) ShowWindow(g_card, SW_HIDE); return 0; }
        if (LOWORD(l) == NIN_BALLOONUSERCLICK) { Update(NULL); return 0; }
        l = LOWORD(l);                  /* NOTIFYICON_VERSION_4: event in the low word */
        if (g_card && l != WM_MOUSEMOVE) ShowWindow(g_card, SW_HIDE);   /* clicks hide it; hovering sends WM_MOUSEMOVE */
        if (l == WM_LBUTTONUP) {        /* inbox of the account with the most unread; Settings is in the right-click menu */
            wchar_t url[512] = L"https://mail.google.com/mail/#inbox";
            int i, best = -1;
            for (i = 0; i < g_nacc; i++)
                if (g_acc[i].enabled && (best < 0 || g_acc[i].unread > g_acc[best].unread)) best = i;
            if (best >= 0) InboxUrl(g_acc[best].email, url);
            ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        }
        if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
            POINT pt;
            HMENU menu = CreatePopupMenu();
            GetCursorPos(&pt);
            if (g_nhist) {
                HMENU sub = CreatePopupMenu();
                int i;
                for (i = 0; i < g_nhist; i++) {
                    wchar_t t[160], *c;
                    swprintf_s(t, 160, L"%.40s  \x2014  %.80s", g_hist[i].from, g_hist[i].subj);
                    for (c = t; *c; c++) if (*c == L'&') *c = L'+';   /* & would underline */
                    AppendMenuW(sub, MF_STRING, IDM_HIST + i, t);
                }
                AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"Recent");   /* destroyed with menu */
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            }
            AppendMenuW(menu, MF_STRING, IDM_PAUSE, NowMs() < g_pauseUntil ? L"Resume popups" : L"Pause popups for 1 hour");
            if (NowMs() >= g_pauseUntil) AppendMenuW(menu, MF_STRING, IDM_PAUSEDAY, L"Pause popups until 8:00");
            AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");
            if (g_testMode) {
                AppendMenuW(menu, MF_STRING, IDM_TEST, L"Test");
                AppendMenuW(menu, MF_STRING, IDM_TESTMENU, L"Test menu...");
            }
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
            SetForegroundWindow(h);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            PostMessageW(h, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) >= IDM_HIST && LOWORD(w) < IDM_HIST + g_nhist && g_qn < QMAX) {   /* reopen a recent popup */
            NewMail *nm = malloc(sizeof *nm);
            if (nm) {
                *nm = g_hist[LOWORD(w) - IDM_HIST];
                memmove(g_q + 1, g_q, g_qn * sizeof *g_q);   /* front of the line */
                g_q[0] = nm;
                g_qn++;
                if (IsWindowVisible(g_popup)) HidePopup(g_popup);   /* its WM_NEXTMAIL shows this one */
                else ShowNext();
            }
        }
        if (LOWORD(w) == IDM_PAUSE) {
            g_pauseUntil = NowMs() < g_pauseUntil ? 0 : NowMs() + 3600000;
            SaveConfig();
            if (!g_pauseUntil) SendMessageW(h, WM_TIMER, TIMER_QUIET, 0);   /* resumed: held summary now, not in a minute */
        }
        if (LOWORD(w) == IDM_PAUSEDAY) {        /* the next 8:00 local time */
            SYSTEMTIME lt, ut;
            FILETIME f;
            ULARGE_INTEGER u;
            GetLocalTime(&lt);
            if (lt.wHour >= 8) {                /* tomorrow: step a day on the local calendar */
                SystemTimeToFileTime(&lt, &f);
                u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime;
                u.QuadPart += 864000000000ULL;
                f.dwLowDateTime = u.LowPart; f.dwHighDateTime = u.HighPart;
                FileTimeToSystemTime(&f, &lt);
            }
            lt.wHour = 8; lt.wMinute = lt.wSecond = lt.wMilliseconds = 0;
            if (TzSpecificLocalTimeToSystemTime(NULL, &lt, &ut) && SystemTimeToFileTime(&ut, &f)) {
                g_pauseUntil = (((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10000;
                SaveConfig();
            }
        }
        if (LOWORD(w) == IDM_SETTINGS) OpenSettings();
        if (LOWORD(w) == IDM_TESTMENU) {
            static int open;
            if (!open) { open = 1; DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_TEST), NULL, TestProc, 0); open = 0; }
        }
        if (LOWORD(w) == IDM_TEST) { memset(&g_cur, 0, sizeof g_cur); ShowPopup(L"Name of sender", L"Subject of the mail", g_nacc ? TestTo(&g_acc[0]) : NULL, L"Hi! Just checking in about tomorrow, are we still on for lunch? I was thinking of the new place on Main Street around 12:30, they have a patio and the menu looks great. Let me know if that works or if another day is better."); PlayNotify(); }
        if (LOWORD(w) == IDM_EXIT) DestroyWindow(h);
        return 0;
    case WM_SETTINGCHANGE:              /* light/dark switched: windows opened from now on follow */
        if (l && wcscmp((const wchar_t *)l, L"ImmersiveColorSet") == 0 && ReadLight() != g_light) {
            g_light = !g_light;
            if (g_settings && IsWindowEnabled(g_settings)) {   /* not while Add is open over it */
                g_reopen = 1;
                PostMessageW(g_settings, WM_COMMAND, IDCANCEL, 0);
            }
            if (g_popup) InvalidateRect(g_popup, NULL, TRUE);
        }
        break;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_badgeIcon) DestroyIcon(g_badgeIcon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Typing the test code anywhere in Settings turns on test mode for this run */
static LRESULT CALLBACK CodeHook(int code, WPARAM w, LPARAM l)
{
    static const char want[] = "0692005420";
    static char got[sizeof want];
    MSG *m = (MSG *)l;
    if (code >= 0 && w == PM_REMOVE && m->message == WM_CHAR && g_settings && !g_testMode
        && (m->hwnd == g_settings || IsChild(g_settings, m->hwnd))) {
        memmove(got, got + 1, sizeof want - 2);
        got[sizeof want - 2] = (char)m->wParam;
        if (memcmp(got, want, sizeof want - 1) == 0) {
            g_testMode = 1;
            ShowWindow(GetDlgItem(g_settings, IDC_TESTPOP), SW_SHOW);
            MessageBeep(MB_OK);
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;

    (void)hPrev; (void)show;

    INITCOMMONCONTROLSEX icc;
    int configured;

    if (wcsstr(cmd, L"/after ")) {      /* started by an update: let the old one finish quitting */
        HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(wcsstr(cmd, L"/after ") + 7, NULL, 10));
        if (old) { WaitForSingleObject(old, 10000); CloseHandle(old); }
    }
    g_mutex = CreateMutexW(NULL, FALSE, L"pigeon-singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   /* already running */
    g_inst = hInst;
    {
        wchar_t old[MAX_PATH];
        SelfPath(old, L".old");
        DeleteFileW(old);               /* left behind by an update */
    }
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;   /* BAR: tooltips */
    InitCommonControlsEx(&icc);
    configured = LoadConfig();
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"grimnotif", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, L"grimnotif");   /* old name's autostart: point it at this exe */
        AutoStartSet(1);
    }

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpfnWndProc = PopupProc;
    wc.lpszClassName = L"GNPopup";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"GNMain";
    RegisterClassExW(&wc);

    g_dpi = GetDpiForSystem();          /* manifest says dpiAware, so draw at real size */
    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    SetWindowsHookExW(WH_GETMESSAGE, CodeHook, NULL, GetCurrentThreadId());
    g_light = wcsstr(cmd, L"--light") ? 1 : wcsstr(cmd, L"--dark") ? 0 : ReadLight();   /* flags: preview either look */
    g_fontBig = MakeFont(DP(-16), FW_NORMAL);
    {
        LOGFONTW lf;
        GetObjectW(g_fontBig, sizeof lf, &lf);
        lf.lfUnderline = TRUE;
        g_fontBigU = CreateFontIndirectW(&lf);
    }
    g_font = MakeFont(DP(-12), FW_SEMIBOLD);
    g_fontSmall = MakeFont(DP(-11), FW_NORMAL);
    g_fontIcon = CreateFontW(DP(-16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe Fluent Icons");
    {   /* Windows 10 has the same glyphs in Segoe MDL2 Assets */
        wchar_t face[64];
        HDC dc = GetDC(NULL);
        HGDIOBJ old = SelectObject(dc, g_fontIcon);
        GetTextFaceW(dc, 64, face);
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
        if (wcscmp(face, L"Segoe Fluent Icons") != 0) {
            DeleteObject(g_fontIcon);
            g_fontIcon = CreateFontW(DP(-16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
        }
    }
    {
        LOGFONTW lf;
        GetObjectW(g_fontIcon, sizeof lf, &lf);
        lf.lfHeight = DP(-10);
        g_fontIconS = CreateFontIndirectW(&lf);
    }
    g_h1 = LineH(g_font);
    g_h2 = LineH(g_fontBig);
    g_h3 = LineH(g_fontSmall);

    g_popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"GNPopup", L"",
                              WS_POPUP, 0, 0, 10, 10, NULL, NULL, hInst, NULL);
    {
        DWORD corner = 2;                   /* DWMWCP_ROUND */
        COLORREF border = Gr(75);
        g_dwm = SUCCEEDED(DwmSetWindowAttribute(g_popup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner));
        if (g_dwm) DwmSetWindowAttribute(g_popup, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
        if (g_dwm) {    /* the popup always uses a region (see VisTop): turn off DWM's corners and border, which would outline the hidden bar area */
            corner = 1;                     /* DWMWCP_DONOTROUND */
            border = 0xFFFFFFFE;            /* DWMWA_COLOR_NONE */
            DwmSetWindowAttribute(g_popup, 33, &corner, sizeof corner);
            DwmSetWindowAttribute(g_popup, 34, &border, sizeof border);
        }
        g_dwm = 0;
    }
    g_shadow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"STATIC", L"",
                               WS_POPUP, 0, 0, 1, 1, NULL, NULL, hInst, NULL);
    g_main = CreateWindowExW(0, L"GNMain", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    SetTimer(g_main, TIMER_QUIET, 60000, NULL);
    SetTimer(g_main, TIMER_UPD, 60000, NULL);

    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_baseIcon = LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wcscpy_s(g_nid.szTip, 128, L"pigeon");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;   /* no NIF_SHOWTIP: the shell sends NIN_POPUPOPEN/CLOSE and we draw the card */
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

    {
        WSADATA wsa;
        int i;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        for (i = 0; i < g_nacc; i++) StartAccount(&g_acc[i]);   /* disabled ones idle until checked */
        UpdateTip();
        SnzLoad();
    }
    if (wcsstr(cmd, L"--test")) ShowPopup(L"Name of sender", L"Subject of the mail", g_nacc ? TestTo(&g_acc[0]) : NULL, L"Hi! Just checking in about tomorrow, are we still on for lunch?");   /* preview */
    if (!g_nacc) {                            /* first run: straight to adding an account */
        if (!AddAccount(NULL)) DestroyWindow(g_main);
    } else if (!configured) {                 /* all accounts off: ask */
        OpenSettings();
        if (!AnyEnabled()) DestroyWindow(g_main);
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
