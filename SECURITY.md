# security

## reporting a vulnerability

please do not open a public issue. use GitHub's **Report a vulnerability** button on the Security tab of this repository (private advisory).
### expect a reply within a week.

## how pigeon handles your account

- uses an **app password**, never your main password (revoke it any time in your provider's security settings). Outlook/Hotmail use Microsoft sign-in instead: pigeon stores only a refresh token limited to mail (IMAP) access, revocable at https://account.live.com/consent/Manage.
- on disk: the app password or refresh token is encrypted with Windows DPAPI in `%APPDATA%\pigeon\config.ini`; only your Windows user on that PC can decrypt it.
- in memory: kept encrypted (`CryptProtectMemory`) and decrypted only while sending the IMAP login, then wiped.
- network: TLS 1.2 to your provider's IMAP server on port 993 (and HTTPS to `login.microsoftonline.com` for Outlook), with certificate and host-name checks. Nothing listens for connections; no telemetry.
- mail is read with `BODY.PEEK`, so it stays unread.

## verifying a download

each release lists the SHA-256 of `pigeon.exe` and carries a GitHub build attestation:

```
gh attestation verify pigeon.exe --repo beeprat/pigeon
```
