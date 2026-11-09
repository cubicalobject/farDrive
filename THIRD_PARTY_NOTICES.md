# Third-party components

## Far Manager Plugin SDK headers

`third_party/plugin.hpp`, `third_party/DlgBuilder.hpp`

Copyright (c) 1996 Eugene Roshal, Copyright (c) 2000 Far Group.
BSD 3-Clause License; the full text is at the top of each header.
Source: https://github.com/FarGroup/FarManager (plugin/common/unicode).

## nlohmann/json

`third_party/nlohmann/json.hpp`

Copyright (c) 2013-2025 Niels Lohmann. MIT License.
Source: https://github.com/nlohmann/json

## Runtime

The plugin links only against Windows system libraries (WinHTTP, Winsock,
BCrypt, Shell32) and the MinGW-w64 / libstdc++ runtime, which is statically
linked under the GCC Runtime Library Exception.
