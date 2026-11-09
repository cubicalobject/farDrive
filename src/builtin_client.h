// builtin_client.h - optional OAuth client shipped inside the plugin.
//
// When both values are non-empty, users can sign in without creating their own
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
