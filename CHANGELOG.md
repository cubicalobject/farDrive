# Changelog

## 1.0.0.5 - 2026-11-11

Fixes from an independent code review.

- F4 save-back now updates exactly the file that was opened, even if the panel
  moved to another folder in the meantime (previously it could overwrite a
  same-named file in the current folder).
- A resumable upload whose final reply was lost is recognised as finished
  instead of being reported as an error (Retry would have uploaded a duplicate).
- Copying the `Trash` pseudo-folder, or folders below it, lists the trashed
  contents correctly; a server that never advances an upload no longer loops.
- Copy/Move dialog: a destination like `\folder` is taken relative to the
  drive root. Far's search thread no longer triggers quota refreshes.
- Clearer "not available here" message; sign-in errors inside a batch keep
  their type.

## 1.0.0.4 - 2026-11-11

- New `Starred` pseudo-folder at the root, listing everything you starred in
  Google Drive (suggested by a user).

## 1.0.0.3 - 2026-11-11

Second round of tester feedback.

- Moving into another plugin panel (archive, FTP, a second Drive panel) no
  longer deletes what was not copied: the silent delete Far issues afterwards
  skips Google-native documents, anything the preceding copy did not complete,
  and everything inside the Trash. In that mode exported Google documents keep
  their panel name so Far finds them.
- F5/F6 to a local panel show a Copy/Move dialog with an editable destination;
  a move states whether the originals go to the Trash or are deleted.
- F6 inside the Trash restores the selected items to their original folder.
- Enter on a Google Doc/Sheet/Slides opens it in the browser. F4 on a Sheet or
  Slides opens the export read-only and says so up front.
- Per-item Retry / Skip / Skip all errors / Cancel instead of aborting a whole
  batch on the first error.
- Esc asks for confirmation before cancelling a transfer.
- Folder shortcuts (Ctrl+Shift+0..9 / RightCtrl+0..9) land in the right
  folder; a failed multi-level path change keeps the previous folder.
- Alt+F7 search no longer enters the pseudo folders or shortcut folders.
- F7 refuses a name that already exists in the folder.
- Export formats are validated in the settings dialog; Drawings got a field.
- Storage quota in Ctrl+L is refreshed while browsing.
- Sign-in: the link is copied to the clipboard, and the wait box explains
  Google's "unverified app" page. Unique hotkeys in the settings dialog.
- build.cmd install reports a failed copy; CI builds releases for tags with
  or without a leading "v".
- Help updated (keys, special folders, built-in client, sign-in page).

## 1.0.0.2 - 2026-11-10

Fixes from the first round of tester feedback.

- Trash: the top level shows only items you trashed yourself, and folders
  inside the Trash now list their real contents. Previously a trashed folder
  looked empty, so F5 copied nothing and F6 permanently deleted the folder
  with everything in it.
- F4 on Google Sheets/Slides can no longer save back (only the first sheet /
  plain text had been exported, so saving wiped the rest). F4 on a Google Doc
  asks for confirmation before replacing the document with plain text.
- F6 (move) exports Google Docs/Sheets/Slides but leaves the originals on
  Drive instead of trashing or deleting them; a message says so.
- An expired or revoked sign-in (invalid_grant, or a changed OAuth client)
  clears the stored tokens and offers to sign in again, instead of showing a
  raw error on every open.
- F8 and Shift+F6 on an entry in the "Shared drives" list report that the
  operation is not available instead of failing with a 404.

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
- OAuth 2.0 sign-in (loopback + PKCE). The release can ship an OAuth client
  (`GoogleDrive.client.json` next to the DLL, or compiled in) so users only
  have to sign in; users can also enter their own client ID/secret.
- Downloads go to a `.part` file, are MD5-verified and only then replace the
  local file; a move (F6) never deletes items that were not fully copied.
- Tokens and the client secret are stored DPAPI-protected (current user).
- Rate-limit (403/429) and transient errors are retried with back-off;
  folder creation is duplicate-safe on retry.
