# grimnotif

Tiny Windows tray app that pops up new Gmail instantly. One ~260 KB exe, ~5 MB RAM, no install, no browser tab needed.

<!-- add screenshot.gif of the popup here -->

## Features

- Instant push via IMAP IDLE, several accounts
- Popup with sender, subject and preview; archive, star, snooze, open
- Keys while hovering: `E` archive, `S` star, `Z` snooze (`1`–`4` pick time), `Esc` close
- Tray badge with unread count, "Recent" history menu
- Do-not-disturb: holds popups during fullscreen apps and quiet hours, then shows a summary
- Sender filters: VIP list, mute list, hide Promotions/Social
- Mail stays unread until you open it

## Setup

1. Turn on 2-Step Verification for your Google account.
2. Create an app password: https://myaccount.google.com/apppasswords (name it "grimnotif").
3. Download `grimnotif.exe` from [Releases](../../releases), run it, click **Add...**, enter your Gmail address and the 16-letter app password.

To start with Windows: put a shortcut to `grimnotif.exe` in `shell:startup`.

## Security

App passwords are encrypted with Windows DPAPI and only talk to `imap.gmail.com` over TLS. Details and how to verify downloads: [SECURITY.md](SECURITY.md).

## Build

From an x64 Native Tools prompt (Visual Studio Build Tools):

```
rc /nologo /fo grimnotif.res grimnotif.rc
cl /nologo /MT /O1 /W3 /sdl /guard:cf /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE grimnotif.c grimnotif.res /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT /guard:cf
```

Tests: `cl /utf-8 /DUNICODE /D_UNICODE test_imap.c && test_imap.exe`. Fuzzing: see `fuzz.c`.

## License

MIT
