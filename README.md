# pigeon

Tiny Windows tray app that pops up new mail instantly: Gmail, Outlook/Hotmail, Yahoo, AOL, iCloud, GMX, Web.de, Yandex, Fastmail, Zoho. One ~340 KB exe, ~5 MB RAM, no browser tab needed. Installer or portable exe, no admin rights either way.

<!-- add screenshot.gif of the popup here -->

## Features

- Instant push via IMAP IDLE, several accounts
- Popup with sender, subject and preview; archive, star, snooze, open
- Keys while hovering: `R` mark as read, `A` or `E` archive, `S` star, `Z` snooze (`1`–`4` pick time), `Esc` close
- Tray badge with unread count, "Recent" history menu
- Do-not-disturb: holds popups during fullscreen apps and quiet hours, then shows a summary
- Sender filters: VIP list, mute list, hide Promotions/Social (Gmail)
- Mail stays unread until you open it

## Setup

1. Download `pigeon-setup.exe` from [Releases](../../releases) and run it (Start menu entry, uninstall from Settings > Apps). Or take `pigeon.exe` and run it from any folder.
2. Click **Add...** and enter your email address.
   - **Gmail, Yahoo, AOL, iCloud, Yandex, Fastmail, Zoho:** create an app password (the **Get app password...** button opens the right page; 2-step verification must be on) and paste it.
   - **Outlook, Hotmail, Live:** leave the password empty and click **Add**; sign in on the Microsoft page that opens.
   - **GMX, Web.de:** turn on IMAP in the webmail settings, then use your normal password (or an app password if 2-step is on).
   - Zoho's free plan has no IMAP; paid plans work.

Other domains are treated as Google Workspace (Gmail).

To start with Windows: put a shortcut to `pigeon.exe` in `shell:startup`.

## Security

Passwords and tokens are encrypted with Windows DPAPI; pigeon only talks to your mail provider over TLS. Details and how to verify downloads: [SECURITY.md](SECURITY.md).

## Outlook sign-in (for builders)

Outlook needs a Microsoft app ID. Register a free app at https://entra.microsoft.com: *App registrations* → *New*, account type **Personal Microsoft accounts only**, platform **Mobile and desktop applications**, then *Authentication* → **Allow public client flows: Yes**. Put the Application (client) ID in `MS_CLIENT_ID` in `pigeon.c`. It is not a secret.

## Build

From an x64 Native Tools prompt (Visual Studio Build Tools):

```
rc /nologo /fo pigeon.res pigeon.rc
cl /nologo /MT /O1 /W3 /sdl /guard:cf /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE pigeon.c pigeon.res /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT /guard:cf
```

Tests: `cl /utf-8 /DUNICODE /D_UNICODE test_imap.c && test_imap.exe`. Fuzzing: see `fuzz.c`.

## License

MIT
