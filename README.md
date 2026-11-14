<img src="logo.png" width="96" alt="pigeon logo" align="right">

# pigeon

tiny Windows tray app that pops up new mail, supports 10 mail providers.\
one ~340 KB exe, ~5 MB RAM, no browser tab needed.\
installer or portable exe, no admin rights either way.

<!-- add screenshot.gif of the popup here -->

## features

- instant push via IMAP IDLE, limit of 8 accounts.
- popup with sender, subject and preview; archive, star, snooze, open
- keys while hovering: `R` mark as read, `A` or `E` archive, `S` star, `Z` snooze (`1`–`4` pick time), `Esc` close
- tray badge with unread count, "Recent" history menu
- do-not-disturb: holds popups during fullscreen apps and quiet hours, then shows a summary
- sender filters: VIP list, mute list, hide Promotions/Social (Gmail)
- mail stays unread until you open it

## setup

1. download `pigeon-setup.exe` from [Releases](../../releases) and run it (Start menu entry, uninstall from Settings > Apps).\
   or take `pigeon.exe` and run it from any folder.
3. click **Add...** and enter your email address.
   - **Gmail, Yahoo, AOL, iCloud, Yandex, Fastmail, Zoho:** create an app password (the **Get app password...** button opens the right page; 2-step verification must be on) and paste it.
   - **Outlook, Hotmail, Live:** leave the password empty and click **Add**; sign in on the Microsoft page that opens.
   - **GMX, Web.de:** turn on IMAP in the webmail settings, then use your normal password (or an app password if 2-step is on).
   - Zoho's free plan has no IMAP; paid plans work.

other domains are treated as Google Workspace (Gmail).

to start with Windows: put a shortcut to `pigeon.exe` in `shell:startup`.

## security

passwords and tokens are encrypted with Windows DPAPI; pigeon only talks to your mail provider over TLS. details and how to verify downloads: [SECURITY.md](SECURITY.md).

## Outlook sign-in (for builders)

Outlook needs a Microsoft app ID.\
register a free app at https://entra.microsoft.com: *App registrations* → *New*, account type **Personal Microsoft accounts only**, platform **Mobile and desktop applications**, then *Authentication* → **Allow public client flows: Yes**.\
put the Application (client) ID in `MS_CLIENT_ID` in `pigeon.c`. it is not a secret.

## build

from an x64 Native Tools prompt (Visual Studio Build Tools):

```
rc /nologo /fo pigeon.res pigeon.rc
cl /nologo /MT /O1 /W3 /sdl /guard:cf /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE pigeon.c pigeon.res /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT /guard:cf
```

tests: `cl /utf-8 /DUNICODE /D_UNICODE test_imap.c && test_imap.exe`. fuzzing: see `fuzz.c`.

## license

MIT
