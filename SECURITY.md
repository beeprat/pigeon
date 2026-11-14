# Security

## Reporting a vulnerability

Please do not open a public issue. Use GitHub's **Report a vulnerability** button on the Security tab of this repository (private advisory). Expect a reply within a week.

## How grimnotif handles your account

- Uses an **app password**, never your main password (revoke it any time in your provider's security settings). Outlook/Hotmail use Microsoft sign-in instead: grimnotif stores only a refresh token limited to mail (IMAP) access, revocable at https://account.live.com/consent/Manage.
- On disk: the app password or refresh token is encrypted with Windows DPAPI in `%APPDATA%\grimnotif\config.ini`; only your Windows user on that PC can decrypt it.
- In memory: kept encrypted (`CryptProtectMemory`) and decrypted only while sending the IMAP login, then wiped.
- Network: TLS 1.2 to your provider's IMAP server on port 993 (and HTTPS to `login.microsoftonline.com` for Outlook), with certificate and host-name checks. Nothing listens for connections; no telemetry.
- Mail is read with `BODY.PEEK`, so it stays unread.

## Verifying a download

Each release lists the SHA-256 of `grimnotif.exe` and carries a GitHub build attestation:

```
gh attestation verify grimnotif.exe --repo beeprat/grimnotif
```
