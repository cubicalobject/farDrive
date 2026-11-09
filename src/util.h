// util.h - small helpers shared by the Google Drive plugin
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <cctype>

// Error coming from HTTP / Drive / local I/O. `status` holds HTTP status when relevant.
struct GdError : std::runtime_error
{
	int status;
	explicit GdError(const std::string& m, int s = 0) : std::runtime_error(m), status(s) {}
};

// Thrown when the user cancels an operation (Esc).
struct GdCancel {};

inline std::wstring utf8_to_wide(std::string_view s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

inline std::string wide_to_utf8(std::wstring_view w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

// RFC 3986 percent-encoding (everything except unreserved characters).
inline std::string url_encode(std::string_view s)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(s.size() * 3);
	for (unsigned char c : s)
	{
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

inline std::string url_decode(std::string_view s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '%' && i + 2 < s.size())
		{
			out += (char)strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16);
			i += 2;
		}
		else if (s[i] == '+')
			out += ' ';
		else
			out += s[i];
	}
	return out;
}

// Escape a string literal for use inside a Drive API `q` query ('...').
inline std::string q_escape(std::string_view s)
{
	std::string out;
	for (char c : s)
	{
		if (c == '\\' || c == '\'') out += '\\';
		out += c;
	}
	return out;
}

// "2024-01-02T03:04:05.678Z" -> FILETIME (UTC). Returns false on parse failure.
inline bool parse_rfc3339(const std::string& s, FILETIME& ft)
{
	int Y, M, D, h, m;
	float sec = 0;
	if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%f", &Y, &M, &D, &h, &m, &sec) < 6)
		return false;
	SYSTEMTIME st{};
	st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
	st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wSecond = (WORD)sec;
	st.wMilliseconds = (WORD)((sec - (int)sec) * 1000);
	return SystemTimeToFileTime(&st, &ft) != 0;
}

inline std::string filetime_to_rfc3339(const FILETIME& ft)
{
	SYSTEMTIME st{};
	if (!FileTimeToSystemTime(&ft, &st)) return {};
	char buf[64];
	snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

inline int64_t unix_now()
{
	return (int64_t)time(nullptr);
}

// Make a Drive file name safe for the local file system.
inline std::wstring sanitize_filename(std::wstring name)
{
	for (auto& c : name)
	{
		if (c < 32 || wcschr(L"\\/:*?\"<>|", c))
			c = L'_';
	}
	while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
		name.pop_back();
	if (name.empty() || name == L"." || name == L"..")
		name = L"_";
	return name;
}

inline std::wstring format_size(uint64_t bytes)
{
	const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024 && u < 4) { v /= 1024; ++u; }
	wchar_t buf[64];
	if (u == 0) swprintf(buf, 64, L"%llu %s", (unsigned long long)bytes, units[u]);
	else swprintf(buf, 64, L"%.1f %s", v, units[u]);
	return buf;
}

inline std::wstring win_error_text(DWORD err, HMODULE mod = nullptr)
{
	wchar_t* buf = nullptr;
	DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	if (mod) flags |= FORMAT_MESSAGE_FROM_HMODULE;
	DWORD n = FormatMessageW(flags, mod, err, 0, (LPWSTR)&buf, 0, nullptr);
	std::wstring s = n ? std::wstring(buf, n) : L"";
	if (buf) LocalFree(buf);
	while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
	if (s.empty())
	{
		wchar_t tmp[32];
		swprintf(tmp, 32, L"error %lu", (unsigned long)err);
		s = tmp;
	}
	return s;
}

inline bool starts_with(std::wstring_view s, std::wstring_view p)
{
	return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
inline bool starts_with(std::string_view s, std::string_view p)
{
	return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

inline std::string to_lower(std::string s)
{
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}
