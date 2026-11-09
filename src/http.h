// http.h - minimal WinHTTP client: memory or file bodies, file sink, progress + cancel
#pragma once
#include "util.h"
#include <winhttp.h>
#include <functional>
#include <map>
#include <algorithm>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

// (bytes done, bytes total) -> keep going?
using ProgressFn = std::function<bool(uint64_t, uint64_t)>;

struct HttpResponse
{
	DWORD status = 0;
	std::string body;                            // response body (empty when streamed to a file)
	std::map<std::string, std::string> headers;  // lower-case names

	std::string header(const char* name) const
	{
		auto it = headers.find(to_lower(name));
		return it == headers.end() ? std::string() : it->second;
	}
	bool ok() const { return status >= 200 && status < 300; }
};

// A slice of an open file to send as the request body.
struct FileSlice
{
	HANDLE file = INVALID_HANDLE_VALUE;
	uint64_t offset = 0;
	uint64_t length = 0;
};

class HttpClient
{
public:
	HttpClient()
	{
		m_session = WinHttpOpen(L"FarGoogleDrive/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!m_session)
			m_session = WinHttpOpen(L"FarGoogleDrive/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!m_session)
			throw GdError("WinHttpOpen failed: " + err_text(GetLastError()));
		WinHttpSetTimeouts(m_session, 20000, 30000, 120000, 120000);
		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
		if (!WinHttpSetOption(m_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols))
		{
			protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
			WinHttpSetOption(m_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
		}
	}
	~HttpClient() { if (m_session) WinHttpCloseHandle(m_session); }
	HttpClient(const HttpClient&) = delete;
	HttpClient& operator=(const HttpClient&) = delete;

	// Perform a request.
	//  headers : "Name: value" strings
	//  body    : in-memory request body (ignored when `slice` is given)
	//  slice   : optional file slice to stream as the body
	//  sink    : optional file handle; a 2xx response body is written there instead of `body`
	//  progress: called with (done,total) while sending `slice` or receiving into `sink`
	HttpResponse request(const wchar_t* method, const std::wstring& url,
		const std::vector<std::wstring>& headers, const std::string& body,
		const FileSlice* slice = nullptr, HANDLE sink = INVALID_HANDLE_VALUE,
		const ProgressFn& progress = nullptr)
	{
		URL_COMPONENTS uc{};
		uc.dwStructSize = sizeof uc;
		uc.dwSchemeLength = (DWORD)-1;
		uc.dwHostNameLength = (DWORD)-1;
		uc.dwUrlPathLength = (DWORD)-1;
		uc.dwExtraInfoLength = (DWORD)-1;
		if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
			throw GdError("Bad URL: " + wide_to_utf8(url));
		std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
		std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
		path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
		bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

		Handle conn(WinHttpConnect(m_session, host.c_str(), uc.nPort, 0));
		if (!conn) throw GdError("WinHttpConnect failed: " + err_text(GetLastError()));
		Handle req(WinHttpOpenRequest(conn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
		if (!req) throw GdError("WinHttpOpenRequest failed: " + err_text(GetLastError()));

		for (const auto& h : headers)
			WinHttpAddRequestHeaders(req, h.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

		if (slice)
		{
			if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
				(DWORD)slice->length, 0))
				throw GdError("WinHttpSendRequest failed: " + err_text(GetLastError()));
			LARGE_INTEGER li; li.QuadPart = (LONGLONG)slice->offset;
			if (!SetFilePointerEx(slice->file, li, nullptr, FILE_BEGIN))
				throw GdError("Seek failed: " + wide_to_utf8(win_error_text(GetLastError())));
			std::vector<char> buf(256 * 1024);
			uint64_t sent = 0;
			while (sent < slice->length)
			{
				DWORD want = (DWORD)std::min<uint64_t>(buf.size(), slice->length - sent);
				DWORD got = 0;
				if (!ReadFile(slice->file, buf.data(), want, &got, nullptr) || got == 0)
					throw GdError("Read failed: " + wide_to_utf8(win_error_text(GetLastError())));
				DWORD written = 0;
				if (!WinHttpWriteData(req, buf.data(), got, &written))
					throw GdError("WinHttpWriteData failed: " + err_text(GetLastError()));
				sent += written;
				if (progress && !progress(sent, slice->length))
					throw GdCancel{};
			}
		}
		else
		{
			if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
				body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
				(DWORD)body.size(), (DWORD)body.size(), 0))
				throw GdError("WinHttpSendRequest failed: " + err_text(GetLastError()));
		}

		if (!WinHttpReceiveResponse(req, nullptr))
			throw GdError("WinHttpReceiveResponse failed: " + err_text(GetLastError()));

		HttpResponse resp;
		DWORD status = 0, len = sizeof status;
		WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
		resp.status = status;

		DWORD rawLen = 0;
		WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
			nullptr, &rawLen, WINHTTP_NO_HEADER_INDEX);
		if (rawLen)
		{
			std::wstring raw(rawLen / sizeof(wchar_t), L'\0');
			if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
				raw.data(), &rawLen, WINHTTP_NO_HEADER_INDEX))
				parse_headers(wide_to_utf8(raw.c_str()), resp.headers);
		}

		uint64_t total = 0;
		{
			std::string cl = resp.header("Content-Length");
			if (!cl.empty()) total = strtoull(cl.c_str(), nullptr, 10);
		}
		bool toFile = sink != INVALID_HANDLE_VALUE && resp.ok();
		uint64_t done = 0;
		std::vector<char> buf(256 * 1024);
		for (;;)
		{
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(req, &avail))
				throw GdError("WinHttpQueryDataAvailable failed: " + err_text(GetLastError()));
			if (avail == 0) break;
			if (avail > buf.size()) avail = (DWORD)buf.size();
			DWORD got = 0;
			if (!WinHttpReadData(req, buf.data(), avail, &got))
				throw GdError("WinHttpReadData failed: " + err_text(GetLastError()));
			if (got == 0) break;
			if (toFile)
			{
				DWORD written = 0;
				if (!WriteFile(sink, buf.data(), got, &written, nullptr) || written != got)
					throw GdError("Write failed: " + wide_to_utf8(win_error_text(GetLastError())));
			}
			else
				resp.body.append(buf.data(), got);
			done += got;
			if (toFile && progress && !progress(done, total))
				throw GdCancel{};
		}
		return resp;
	}

private:
	struct Handle
	{
		HINTERNET h;
		explicit Handle(HINTERNET x) : h(x) {}
		~Handle() { if (h) WinHttpCloseHandle(h); }
		operator HINTERNET() const { return h; }
		explicit operator bool() const { return h != nullptr; }
	};

	static std::string err_text(DWORD err)
	{
		HMODULE mod = GetModuleHandleW(L"winhttp.dll");
		return wide_to_utf8(win_error_text(err, mod)) + " (" + std::to_string(err) + ")";
	}

	static void parse_headers(const std::string& raw, std::map<std::string, std::string>& out)
	{
		size_t pos = 0;
		while (pos < raw.size())
		{
			size_t eol = raw.find("\r\n", pos);
			if (eol == std::string::npos) eol = raw.size();
			std::string line = raw.substr(pos, eol - pos);
			pos = eol + 2;
			size_t colon = line.find(':');
			if (colon == std::string::npos) continue;
			std::string name = to_lower(line.substr(0, colon));
			size_t v = colon + 1;
			while (v < line.size() && line[v] == ' ') ++v;
			out[name] = line.substr(v);
		}
	}

	HINTERNET m_session = nullptr;
};
