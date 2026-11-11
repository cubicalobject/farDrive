// builtin_client.h - optional OAuth client compiled into the plugin.
//
// The simpler alternative is a file: put GoogleDrive.client.json next to GoogleDrive.dll
// (see GoogleDrive.client.json.example); it takes precedence over these macros.
//
// When a shipped client is present, users can sign in without creating their own
// Google Cloud project; the settings dialog fields then act as an override.
// Keep them EMPTY in the repository. Inject them at build time instead, e.g.
//   g++ ... -DBUILTIN_CLIENT_ID="\"123-abc.apps.googleusercontent.com\"" -DBUILTIN_CLIENT_SECRET="\"GOCSPX-...\""
// (the GitHub Actions workflow does this from repository secrets).
//
// Google treats the secret of a "Desktop app" OAuth client as non-confidential,
// so shipping it in a binary is allowed; it only identifies the app.
#pragma once

#ifndef BUILTIN_CLIENT_ID
#define BUILTIN_CLIENT_ID ""
#endif
#ifndef BUILTIN_CLIENT_SECRET
#define BUILTIN_CLIENT_SECRET ""
#endif
