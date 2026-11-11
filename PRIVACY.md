# Privacy policy

*Google Drive plugin for Far Manager* ("the plugin") is an open-source
extension for the Far Manager file manager. This page describes what the
plugin does with your data.

## What the plugin does

The plugin runs entirely on your own computer, inside Far Manager. It lets you
browse your Google Drive and copy files between Google Drive and your local
disks. To do that it talks directly to Google's Drive API over HTTPS.

## Data the plugin accesses

When you sign in, Google asks you to grant the plugin access to your Google
Drive files (`https://www.googleapis.com/auth/drive`). The plugin uses this
access only to perform the actions you request in Far Manager: listing
folders, downloading, uploading, renaming, creating folders and moving files
to the trash.

The plugin also reads your account e-mail address and storage quota from the
Drive API so it can show them in the panel information (Ctrl+L).

## Data stored on your computer

- The sign-in tokens Google issues (refresh token and access token) are stored
  in your Far Manager profile folder, encrypted with Windows Data Protection
  (DPAPI) so only your Windows user account can read them.
- Your settings (export formats, trash option) and your account e-mail address
  are stored in the same file.
- Files you copy are written where you tell Far Manager to put them.

You can remove all stored tokens at any time with **Sign out** in the
plugin's settings dialog, which also revokes them at Google, or by removing
the plugin's access at https://myaccount.google.com/permissions.

## Data sent anywhere else

None. The plugin communicates only with Google's servers
(`accounts.google.com`, `oauth2.googleapis.com`, `www.googleapis.com`). It
has no server of its own, collects no analytics or telemetry, and never
transmits your files, tokens or account information to the plugin's author or
to any third party.

## Google API Services User Data Policy

The plugin's use of information received from Google APIs adheres to the
[Google API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy),
including the Limited Use requirements.

## Changes and contact

This policy may be updated when the plugin's behaviour changes; the history is
visible in the repository. Questions can be raised as an issue at
https://github.com/cubicalobject/farDrive/issues.
