# Security

## Reporting a vulnerability

Please do not open a public issue. Use GitHub's **Report a vulnerability** button on the Security tab of this repository (private advisory). Expect a reply within a week.

## How grimnotif handles your account

- Uses a Gmail **app password**, never your Google password. Revoke it any time at https://myaccount.google.com/apppasswords.
- On disk: the app password is encrypted with Windows DPAPI in `%APPDATA%\grimnotif\config.ini`; only your Windows user on that PC can decrypt it.
- In memory: kept encrypted (`CryptProtectMemory`) and decrypted only while sending the IMAP login, then wiped.
- Network: TLS 1.2 to `imap.gmail.com:993` only, with certificate and host-name checks. Nothing listens for connections; no telemetry.
- Mail is read with `BODY.PEEK`, so it stays unread.

## Verifying a download

Each release lists the SHA-256 of `grimnotif.exe` and carries a GitHub build attestation:

```
gh attestation verify grimnotif.exe --repo beeprat/grimnotif
```
