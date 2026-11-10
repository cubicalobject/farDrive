// util.h - small helpers shared by the Google Drive plugin
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <set>
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

inline std::string html_escape(std::string_view s)
{
	std::string out;
	for (char c : s)
	{
		switch (c)
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		case '\'': out += "&#39;"; break;
		default: out += c;
		}
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

// "2024-01-02T03:04:05.678Z" / "...05+02:00" -> FILETIME (UTC). Returns false on parse failure.
inline bool parse_rfc3339(const std::string& s, FILETIME& ft)
{
	int Y, M, D, h, m, sec, consumed = 0;
	if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &Y, &M, &D, &h, &m, &sec, &consumed) < 6 || consumed == 0)
		return false;
	const char* p = s.c_str() + consumed;
	int ms = 0;
	if (*p == '.')
	{
		++p;
		int digits = 0;
		while (isdigit((unsigned char)*p))
		{
			if (digits < 3) ms = ms * 10 + (*p - '0');
			++digits; ++p;
		}
		while (digits < 3) { ms *= 10; ++digits; }
	}
	long offsetMin = 0;
	if (*p == '+' || *p == '-')
	{
		int oh = 0, om = 0;
		if (sscanf(p + 1, "%2d:%2d", &oh, &om) >= 1)
			offsetMin = (oh * 60 + om) * (*p == '-' ? -1 : 1);
	}
	SYSTEMTIME st{};
	st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
	st.wHour = (WORD)h; st.wMinute = (WORD)m; st.wSecond = (WORD)sec; st.wMilliseconds = (WORD)ms;
	if (!SystemTimeToFileTime(&st, &ft)) return false;
	if (offsetMin)
	{
		ULARGE_INTEGER u{ ft.dwLowDateTime, ft.dwHighDateTime };
		u.QuadPart -= (long long)offsetMin * 60ll * 10000000ll;   // local = UTC + offset  =>  UTC = local - offset
		ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
	}
	return true;
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

// Make a Drive file name safe for the local file system (invalid chars, trailing dots,
// Windows reserved device names).
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
	// CON, PRN, AUX, NUL, COM1-9, LPT1-9 (with or without extension) are not creatable on Windows
	std::wstring stem = name.substr(0, name.find(L'.'));
	for (auto& c : stem) c = (wchar_t)towupper(c);
	bool reserved = stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL"
		|| (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'1' && stem[3] <= L'9');
	if (reserved) name.insert(0, L"_");
	return name;
}

// Assign unique panel names: duplicates become "name (2).ext", and the suffix is only used
// when that name is not taken by another real item. Case-insensitive, like the panel.
inline std::vector<std::wstring> unique_display_names(const std::vector<std::pair<std::wstring, bool>>& items /* name, isFolder */)
{
	auto lower = [](std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; };
	std::set<std::wstring> all, assigned;
	for (auto& it : items) all.insert(lower(it.first));
	std::vector<std::wstring> out;
	out.reserve(items.size());
	for (auto& [name, isFolder] : items)
	{
		std::wstring key = lower(name);
		if (assigned.insert(key).second) { out.push_back(name); continue; }
		size_t dot = name.find_last_of(L'.');
		bool splitExt = dot != std::wstring::npos && dot > 0 && !isFolder;
		for (int n = 2;; ++n)
		{
			std::wstring suffix = L" (" + std::to_wstring(n) + L")";
			std::wstring cand = splitExt ? name.substr(0, dot) + suffix + name.substr(dot) : name + suffix;
			std::wstring ck = lower(cand);
			if (!all.count(ck) && assigned.insert(ck).second) { out.push_back(cand); break; }
		}
	}
	return out;
}

// \\?\ form of an absolute path so deep trees are not limited to MAX_PATH.
inline std::wstring long_path(const std::wstring& p)
{
	if (p.size() < 3 || p.compare(0, 4, L"\\\\?\\") == 0) return p;
	if (p[0] == L'\\' && p[1] == L'\\') return L"\\\\?\\UNC\\" + p.substr(2);
	if (iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return L"\\\\?\\" + p;
	return p;
}

inline std::wstring format_size(uint64_t bytes)
{
	const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024 && u < 4) { v /= 1024; ++u; }
	wchar_t buf[64];
	if (u == 0) swprintf(buf, 64, L"%llu %ls", (unsigned long long)bytes, units[u]);
	else swprintf(buf, 64, L"%.1f %ls", v, units[u]);
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

// Standard base64 (with padding), used for DPAPI blobs in the config file.
inline std::string base64_encode(const void* data, size_t n)
{
	static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char* p = (const unsigned char*)data;
	std::string out;
	size_t i = 0;
	for (; i + 2 < n; i += 3)
	{
		uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
		out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
	}
	if (i + 1 == n)
	{
		uint32_t v = p[i] << 16;
		out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
	}
	else if (i + 2 == n)
	{
		uint32_t v = (p[i] << 16) | (p[i + 1] << 8);
		out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
	}
	return out;
}

inline std::string base64_decode(std::string_view s)
{
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	std::string out;
	uint32_t acc = 0;
	int bits = 0;
	for (char c : s)
	{
		if (c == '=') break;
		int v = val(c);
		if (v < 0) continue;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
	}
	return out;
}

// MIME type for an upload, from a small table and then the registry; octet-stream otherwise.
inline std::string guess_mime(const std::wstring& fileName)
{
	size_t dot = fileName.find_last_of(L'.');
	if (dot == std::wstring::npos) return "application/octet-stream";
	std::wstring ext = fileName.substr(dot);
	for (auto& c : ext) c = (wchar_t)towlower(c);
	static const std::pair<const wchar_t*, const char*> tbl[] = {
		{L".jpg", "image/jpeg"}, {L".jpeg", "image/jpeg"}, {L".png", "image/png"}, {L".gif", "image/gif"},
		{L".webp", "image/webp"}, {L".heic", "image/heic"}, {L".svg", "image/svg+xml"}, {L".bmp", "image/bmp"},
		{L".pdf", "application/pdf"}, {L".txt", "text/plain"}, {L".md", "text/markdown"}, {L".csv", "text/csv"},
		{L".json", "application/json"}, {L".html", "text/html"}, {L".htm", "text/html"}, {L".xml", "application/xml"},
		{L".zip", "application/zip"}, {L".7z", "application/x-7z-compressed"}, {L".rar", "application/vnd.rar"},
		{L".mp3", "audio/mpeg"}, {L".flac", "audio/flac"}, {L".wav", "audio/wav"}, {L".m4a", "audio/mp4"},
		{L".mp4", "video/mp4"}, {L".mov", "video/quicktime"}, {L".mkv", "video/x-matroska"}, {L".avi", "video/x-msvideo"},
		{L".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
		{L".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
		{L".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
		{L".doc", "application/msword"}, {L".xls", "application/vnd.ms-excel"}, {L".ppt", "application/vnd.ms-powerpoint"},
		{L".odt", "application/vnd.oasis.opendocument.text"}, {L".ods", "application/vnd.oasis.opendocument.spreadsheet"},
		{L".epub", "application/epub+zip"}, {L".rtf", "application/rtf"},
	};
	for (auto& [e, m] : tbl) if (ext == e) return m;
	wchar_t buf[256];
	DWORD cb = sizeof buf;
	if (RegGetValueW(HKEY_CLASSES_ROOT, ext.c_str(), L"Content Type", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS && buf[0])
		return wide_to_utf8(buf);
	return "application/octet-stream";
}
