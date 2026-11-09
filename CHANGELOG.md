# Changelog

## 1.0.0.1 - 2026-11-08

First release.

- Google Drive panel for Far Manager 3 x64 (build 6000 or later).
- Copy/move Drive <-> local (F5/F6), view/edit (F3/F4), new folder (F7),
  trash/delete (F8), rename (Shift+F6), search (Alt+F7), folder shortcuts.
- `Shared with me`, `Shared drives` and `Trash` pseudo-folders.
- Google Docs/Sheets/Slides exported as docx/xlsx/pptx (configurable);
  text/CSV export for viewing.
- Resumable chunked uploads with retry; streaming downloads with progress
  and Esc to cancel.
- OAuth 2.0 sign-in (loopback + PKCE). Optional built-in client; users can
  also enter their own client ID/secret.
