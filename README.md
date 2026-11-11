# Google Drive plugin for Far Manager 3 𓆈

Mounts your Google Drive as a Far Manager panel. Native C++ (x64), talks to the
Google Drive REST API v3 directly through WinHTTP, signs in with OAuth 2.0
(loopback + PKCE). No external tools or runtimes needed.

## Features   𓆟

| Key / action              | function                                                            |
|---------------------------|---------------------------------------------------------------------|
| F11, `gdrive:`, Alt+F1/F2 | Open the Drive panel (`gdrive:Folder\Sub` jumps to a folder)        |
| F5 / F6                   | Copy / move Drive -> local and local -> Drive; F6 in Trash restores |
| Enter                     | Enter a folder; open a Google Doc/Sheet/Slides in the browser       |
| F3 / F4                   | View / edit a Drive file (Google Docs are exported as text/CSV)     |
| F7                        | Create a folder on Drive                                            |
| F8                        | Move to Trash (or delete permanently, configurable)                 |
| Shift+F6                  | Rename                                                              |
| Alt+F7                    | Far's file search works inside Drive                                |
| Ctrl+Shift+0..9           | Folder shortcuts to Drive folders                                   |
| Ctrl+L                    | Account e-mail, storage used / limit, current folder id             |
| Esc                       | Cancels a running transfer (asks first)                             |

Root folder shows `Shared with me`, `Shared drives` and `Trash` as pseudo-folders.
Google Docs / Sheets / Slides are exported on download (default docx / xlsx / pptx,
configurable). Uploads use multipart (< 5 MB) or resumable chunked upload with
retry. Large files stream to/from disk; file modification times are preserved.

## Installation (users)

Requirements: Far Manager 3 x64, build 6000 or newer (2022+).

1. Download `GoogleDrive-<version>-x64.zip` from the [Releases page](https://github.com/cubicalobject/farDrive/releases).
2. Extract it so that you get `<Far folder>\Plugins\GoogleDrive\GoogleDrive.dll`
   (the zip contains the `GoogleDrive` folder; drop it into `Plugins`).
3. Restart Far.

## Opening the panel

Any of these opens Google Drive in the active panel:

- **F11** (plugins menu) -> **Google Drive**. Most reliable.
- Type `gdrive:` on the command line and press Enter. `gdrive:Photos\2024`
  opens that folder directly.
- **F9 -> Left / Right -> Change drive -> Google Drive**.
- **Alt+F1 / Alt+F2** (drive menu). On many laptops the F-keys are media keys
  unless **Fn** is held, so use **Fn+Alt+F1** or toggle Fn Lock (often Fn+Esc).
  Windows Terminal or GPU overlay hotkeys can also swallow Alt+F-keys. 𓆌

## Signing in  𓄿

The first time you open the panel the plugin asks to sign in. Your browser
opens the Google login. Approve the Drive permission and return to Far.

If the release you installed was built with a built-in Google client (official
releases are), you are done. Because the plugin is not verified by Google you
will see a **"Google hasn't verified this app"** page: click **Advanced**, then
**Go to <app name> (unsafe)**, then allow. The wording is Google's; the plugin
only ever talks to Google's own servers from your PC.

To use your own Google client instead (or if the build has no built-in one),
see [Bring your own OAuth client](#bring-your-own-oauth-client) below.

Tokens are stored in `%FARPROFILE%\PluginsData\GoogleDrive\config.json`
(normally `%APPDATA%\Far Manager\Profile\...`). Treat that file like a password;
**Sign out** in the settings dialog revokes and deletes the tokens.

## Everyday use

- Put a local folder in the other panel. **F5** copies the selected Drive items
  there (a dialog lets you change the destination); **F5** from the local panel
  uploads into the current Drive folder. **F6** does the same and removes the
  source afterwards. Google Docs/Sheets/Slides are exported by F6 but never
  deleted. If one item fails you get Retry / Skip / Skip all / Cancel.
- Folders are copied recursively. Existing files prompt Overwrite / All /
  Skip / Skip all unless the operation is silent.
- **F3 / F4** open a file in Far's viewer/editor. Google Docs show as text and
  ask for confirmation before a save replaces the document; Sheets and Slides
  open read-only because only the first sheet / plain text is exported. Press
  **Enter** on a Google file to open it in the browser instead.
- **F7** creates a folder, **Shift+F6** renames, **F8** moves to Trash (or
  deletes permanently if that option is off in settings; inside `Trash` F8
  always deletes permanently). Inside `Trash`, **F6** restores the selected
  items to their original folder.
- Press **Esc** during a transfer to cancel it.
- **Ctrl+L** shows account e-mail, used / total storage and the current
  folder id. **Ctrl+Shift+0..9** stores the current Drive folder as a Far
  folder shortcut; **Right Ctrl+0..9** returns to it.
- Root pseudo-folders: `Shared with me`, `Shared drives`, `Trash`. Nothing can
  be created or uploaded directly into those lists, only into folders below them.
- Google Docs/Sheets/Slides download as docx/xlsx/pptx by default; change the
  extensions in **F9 -> Options -> Plugins configuration -> Google Drive
  settings** (docx/odt/rtf/pdf/txt/html/epub/md, xlsx/ods/csv/tsv/pdf,
  pptx/odp/pdf).

## Bring your own OAuth client  𓆤

Google requires every app that uses the Drive API to identify itself with an
OAuth client. Official releases carry one, but you can use your own: it removes
the "unverified app" warning for you and does not count against the shared
client's user limit. About five minutes in the (free) Google Cloud console.
The direct links open the right page once a project is selected in the
dropdown at the top.

1. **Project and API.** Go to <https://console.cloud.google.com/>, create a
   project (any name), then open                       
   <https://console.cloud.google.com/apis/library/drive.googleapis.com> and
   click **Enable**. (The "APIs & services" overview only lists APIs that are
   already enabled, so Drive is not visible there until this step is done.)
2. **Consent screen.** Open <https://console.cloud.google.com/auth/overview>,
   **Get started**: any app name, your Gmail as support e-mail, Audience
   **External**, your Gmail as contact, tick the policy box, **Create**.
3. **Test user.** Open <https://console.cloud.google.com/auth/audience>, under
   **Test users** click **Add users**, enter your Gmail, **Save**.
4. **Client.** Open <https://console.cloud.google.com/auth/clients>, **Create
   client**, type **Desktop app**, any name, **Create**. Copy the **Client ID**
   (ends in `.apps.googleusercontent.com`) and the **Client secret** now; the
   secret is only shown in full at this moment.
5. **In Far.** **F9 -> Options -> Plugins configuration -> Google Drive
   settings**, paste the ID and secret (**Shift+Ins** pastes), press
   **Sign in...**, approve in the browser (click **Continue** on the testing-mode
   warning), **OK**.

While your consent screen is in *Testing* status Google expires the sign-in
after 7 days. Click **Publish app** on the Audience page to avoid that; the
app keeps working, you just see the unverified warning at each sign-in.

## Building 𓃥

Requires the MSYS2 UCRT64 toolchain (`C:\msys64\ucrt64\bin\g++.exe`, GCC 14).

```
build.cmd            # builds out\GoogleDrive\GoogleDrive.dll (+ lng/hlf/license/changelog)
build.cmd install    # also copies it to %FARHOME%\Plugins\GoogleDrive (default: %ProgramFiles%\Far Manager)
build.cmd package    # also zips it to out\GoogleDrive-<version>-x64.zip
```

`build.sh` does the same from an MSYS2 shell and is what CI uses.
The DLL is statically linked (no MinGW runtime DLLs needed) and only imports
system libraries: winhttp, ws2_32, bcrypt, shell32, kernel32, ucrt.

To compile a built-in OAuth client into the plugin, set `BUILTIN_CLIENT_ID`
and `BUILTIN_CLIENT_SECRET` in the environment before running `build.cmd`.
Keep them out of the source tree; see `src/builtin_client.h`.

Smoke tests (load the DLL like Far does, hit Google's public discovery
endpoint, check the OAuth error path and the helper functions):

```
g++ -std=c++20 -O1 -DUNICODE -D_UNICODE -static -I third_party -o test\test_host.exe test\test_host.cpp -lwinhttp -lws2_32 -lshell32 -lbcrypt -lcrypt32 -ladvapi32
test\test_host.exe out\GoogleDrive\GoogleDrive.dll
```

### Releasing  𓆚  𓆚  𓆚

The GitHub Actions workflow in `.github/workflows/build.yml` builds and tests
every push. Pushing a tag `vX.Y.Z.W` additionally creates a GitHub release
with the zip attached. Add the repository secrets `GDRIVE_CLIENT_ID` and
`GDRIVE_CLIENT_SECRET` to have the built-in client compiled into releases.
Bump the version in `src/version.h` and add a `CHANGELOG.md` entry first.

## Layout  𓃹

```
src/GoogleDrive.cpp   Far plugin: panel, dialogs, file operations
src/drive.h           Drive API v3 client (list/get/about/mkdir/trash/rename/download/upload)
src/oauth.h           Config file + OAuth sign-in (loopback listener, PKCE), token refresh
src/http.h            WinHTTP wrapper (memory/file bodies, streaming download, progress)
src/util.h            UTF-8/UTF-16, URL encoding, RFC 3339 time, file-name sanitising
src/builtin_client.h  optional compiled-in OAuth client (empty in the repository)
src/version.h         plugin version and author
src/GoogleDrive.def   exported Far entry points
GoogleDriveEng.lng    UI strings (order must match the enum in GoogleDrive.cpp)
GoogleDriveEng.hlf    F1 help
test/test_host.cpp    smoke and unit tests (fake HTTP transport)
third_party/          Far PluginSDK headers (plugin.hpp, DlgBuilder.hpp), nlohmann/json
```

## Known limitations  𓆑

- Drive allows several files with the same name in one folder; the panel shows
  the duplicates as `name (2).ext`, `name (3).ext`.
- Google-native files (Docs/Sheets/Slides/Drawings) can only be exported, and
  only up to 10 MB (drive API limit). Forms, Sites and Maps cannot be exported. 
- Saving a Google Doc from F4 replaces the whole document with plain text
  (after a confirmation); Sheets and Slides cannot be saved back at all.
- Copying into another plugin panel such as an archive goes through a temporary
  local copy (that is how Far handles plugin-to-plugin transfers). Between two
  Google Drive panels, F5/F6 copy and move on Google's side instead.
- Only English UI strings are included. 𓅣

## License 𓆏

MIT, see `LICENSE`. Third-party components are listed in `THIRD_PARTY_NOTICES.md`.
