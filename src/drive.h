// drive.h - Google Drive REST API v3 client used by the plugin
#pragma once
#include "util.h"
#include "http.h"
#include "oauth.h"
#include <algorithm>
#include <thread>
#include <chrono>

static const char* const kMimeFolder = "application/vnd.google-apps.folder";
static const char* const kMimeShortcut = "application/vnd.google-apps.shortcut";
static const char* const kApiBase = "https://www.googleapis.com/drive/v3/";
static const char* const kUploadBase = "https://www.googleapis.com/upload/drive/v3/";
static const char* const kFileFields = "id,name,mimeType,size,modifiedTime,createdTime,md5Checksum,trashed,shortcutDetails";

struct DriveFile
{
	std::string id;
	std::string name;
	std::string mimeType;
	std::string md5;
	uint64_t size = 0;
	FILETIME modified{};
	FILETIME created{};
	bool trashed = false;
	bool isShortcut = false;
	std::string targetId;      // for shortcuts
	std::string targetMime;

	bool isFolder() const { return mimeType == kMimeFolder || (isShortcut && targetMime == kMimeFolder); }
	bool isGoogleDoc() const { return starts_with(effectiveMime(), "application/vnd.google-apps.") && !isFolder(); }
	const std::string& effectiveMime() const { return isShortcut ? targetMime : mimeType; }
	const std::string& effectiveId() const { return isShortcut ? targetId : id; }
};

struct DriveAbout
{
	std::string email;
	uint64_t usage = 0;
	uint64_t limit = 0;   // 0 = unlimited
};

// Export table: extension -> MIME type that Drive can export Google Docs to.
inline const char* export_mime_for_ext(const std::string& ext)
{
	static const std::pair<const char*, const char*> tbl[] = {
		{"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
		{"odt", "application/vnd.oasis.opendocument.text"},
		{"rtf", "application/rtf"}, {"pdf", "application/pdf"}, {"txt", "text/plain"},
		{"html", "text/html"}, {"epub", "application/epub+zip"}, {"md", "text/markdown"},
		{"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
		{"ods", "application/vnd.oasis.opendocument.spreadsheet"}, {"csv", "text/csv"}, {"tsv", "text/tab-separated-values"},
		{"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
		{"odp", "application/vnd.oasis.opendocument.presentation"},
		{"png", "image/png"}, {"jpg", "image/jpeg"}, {"svg", "image/svg+xml"},
		{"json", "application/vnd.google-apps.script+json"},
	};
	for (auto& [e, m] : tbl) if (ext == e) return m;
	return nullptr;
}

class Drive
{
public:
	Drive(Config& cfg, HttpClient& http) : m_cfg(cfg), m_http(http) {}

	// --- metadata -----------------------------------------------------------

	std::vector<DriveFile> list(const std::string& q, const std::string& orderBy = "folder,name")
	{
		std::vector<DriveFile> out;
		std::string pageToken;
		do
		{
			std::string url = std::string(kApiBase) + "files?q=" + url_encode(q)
				+ "&fields=" + url_encode(std::string("nextPageToken,files(") + kFileFields + ")")
				+ "&pageSize=1000&supportsAllDrives=true&includeItemsFromAllDrives=true"
				+ "&orderBy=" + url_encode(orderBy);
			if (!pageToken.empty()) url += "&pageToken=" + url_encode(pageToken);
			json j = call(L"GET", url);
			for (auto& f : j["files"]) out.push_back(parse(f));
			pageToken = j.value("nextPageToken", "");
		} while (!pageToken.empty());
		return out;
	}

	std::vector<DriveFile> listChildren(const std::string& folderId)
	{
		return list("'" + q_escape(folderId) + "' in parents and trashed = false");
	}
	std::vector<DriveFile> listSharedWithMe()
	{
		return list("sharedWithMe = true and trashed = false", "folder,name");
	}
	std::vector<DriveFile> listTrash()
	{
		return list("trashed = true and 'me' in owners", "folder,name");
	}

	// Shared drives, presented as folders whose id is the drive id.
	std::vector<DriveFile> listSharedDrives()
	{
		std::vector<DriveFile> out;
		std::string pageToken;
		do
		{
			std::string url = std::string(kApiBase) + "drives?pageSize=100&fields=" + url_encode("nextPageToken,drives(id,name,createdTime)");
			if (!pageToken.empty()) url += "&pageToken=" + url_encode(pageToken);
			json j = call(L"GET", url);
			if (j.contains("drives"))
			{
				for (auto& d : j["drives"])
				{
					DriveFile f;
					f.id = d.value("id", "");
					f.name = d.value("name", "");
					f.mimeType = kMimeFolder;
					parse_rfc3339(d.value("createdTime", ""), f.created);
					f.modified = f.created;
					out.push_back(f);
				}
			}
			pageToken = j.value("nextPageToken", "");
		} while (!pageToken.empty());
		return out;
	}

	DriveFile get(const std::string& id)
	{
		return parse(call(L"GET", std::string(kApiBase) + "files/" + url_encode(id)
			+ "?fields=" + url_encode(kFileFields) + "&supportsAllDrives=true"));
	}

	DriveAbout about()
	{
		json j = call(L"GET", std::string(kApiBase) + "about?fields=" + url_encode("user(emailAddress),storageQuota(limit,usage)"));
		DriveAbout a;
		if (j.contains("user")) a.email = j["user"].value("emailAddress", "");
		if (j.contains("storageQuota"))
		{
			a.usage = to_u64(j["storageQuota"].value("usage", "0"));
			a.limit = to_u64(j["storageQuota"].value("limit", "0"));
		}
		return a;
	}

	DriveFile createFolder(const std::string& parentId, const std::string& name)
	{
		json body{ {"name", name}, {"mimeType", kMimeFolder}, {"parents", json::array({parentId})} };
		return parse(call(L"POST", std::string(kApiBase) + "files?fields=" + url_encode(kFileFields) + "&supportsAllDrives=true",
			body.dump(), "application/json; charset=UTF-8"));
	}

	void trash(const std::string& id)
	{
		call(L"PATCH", std::string(kApiBase) + "files/" + url_encode(id) + "?supportsAllDrives=true",
			json{ {"trashed", true} }.dump(), "application/json; charset=UTF-8");
	}
	void untrash(const std::string& id)
	{
		call(L"PATCH", std::string(kApiBase) + "files/" + url_encode(id) + "?supportsAllDrives=true",
			json{ {"trashed", false} }.dump(), "application/json; charset=UTF-8");
	}
	void remove(const std::string& id)
	{
		call(L"DELETE", std::string(kApiBase) + "files/" + url_encode(id) + "?supportsAllDrives=true");
	}
	void rename(const std::string& id, const std::string& newName)
	{
		call(L"PATCH", std::string(kApiBase) + "files/" + url_encode(id) + "?supportsAllDrives=true",
			json{ {"name", newName} }.dump(), "application/json; charset=UTF-8");
	}

	// --- content -----------------------------------------------------------

	// Chooses the local extension for a Google-native document ("" if not exportable).
	std::string exportExtFor(const std::string& mime) const
	{
		if (mime == "application/vnd.google-apps.document") return m_cfg.export_doc;
		if (mime == "application/vnd.google-apps.spreadsheet") return m_cfg.export_sheet;
		if (mime == "application/vnd.google-apps.presentation") return m_cfg.export_slides;
		if (mime == "application/vnd.google-apps.drawing") return m_cfg.export_drawing;
		if (mime == "application/vnd.google-apps.script") return "json";
		if (mime == "application/vnd.google-apps.jam") return "pdf";
		return "";
	}

	// Downloads (or exports) a file to localPath. Throws GdError / GdCancel.
	// exportExtOverride forces a particular export format for Google-native documents.
	void download(const DriveFile& f, const std::wstring& localPath, const ProgressFn& progress,
		const char* exportExtOverride = nullptr)
	{
		std::string id = f.effectiveId();
		std::string url;
		if (f.isGoogleDoc())
		{
			std::string ext = exportExtOverride ? exportExtOverride : exportExtFor(f.effectiveMime());
			const char* mime = ext.empty() ? nullptr : export_mime_for_ext(ext);
			if (!mime) throw GdError("This Google file type cannot be exported: " + f.effectiveMime());
			url = std::string(kApiBase) + "files/" + url_encode(id) + "/export?mimeType=" + url_encode(mime);
		}
		else
			url = std::string(kApiBase) + "files/" + url_encode(id) + "?alt=media&supportsAllDrives=true";

		for (int attempt = 0;; ++attempt)
		{
			oauth::ensure_token(m_cfg, m_http);
			HANDLE h = CreateFileW(localPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				throw GdError("Cannot create " + wide_to_utf8(localPath) + ": " + wide_to_utf8(win_error_text(GetLastError())));
			HttpResponse resp;
			try
			{
				resp = m_http.request(L"GET", utf8_to_wide(url), { authHeader() }, "", nullptr, h, progress);
			}
			catch (...)
			{
				CloseHandle(h);
				DeleteFileW(localPath.c_str());
				throw;
			}
			if (resp.ok())
			{
				// preserve modification time
				if (f.modified.dwHighDateTime || f.modified.dwLowDateTime)
					SetFileTime(h, nullptr, nullptr, &f.modified);
				CloseHandle(h);
				return;
			}
			CloseHandle(h);
			DeleteFileW(localPath.c_str());
			if (resp.status == 401 && attempt == 0) { oauth::force_refresh(m_cfg, m_http); continue; }
			if ((resp.status == 429 || resp.status >= 500) && attempt < 3) { backoff(attempt); continue; }
			throw GdError(apiError(resp), (int)resp.status);
		}
	}

	// Uploads localPath as a new file in parentId, or replaces the content of existingId.
	DriveFile upload(const std::string& parentId, const std::string& name, const std::wstring& localPath,
		const std::string& existingId, const ProgressFn& progress)
	{
		HANDLE h = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			throw GdError("Cannot open " + wide_to_utf8(localPath) + ": " + wide_to_utf8(win_error_text(GetLastError())));
		struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{ h };

		LARGE_INTEGER sz{};
		GetFileSizeEx(h, &sz);
		uint64_t total = (uint64_t)sz.QuadPart;
		FILETIME mtime{};
		GetFileTime(h, nullptr, nullptr, &mtime);

		json meta = json::object();
		if (existingId.empty()) { meta["name"] = name; meta["parents"] = json::array({ parentId }); }
		std::string m = filetime_to_rfc3339(mtime);
		if (!m.empty()) meta["modifiedTime"] = m;

		if (total < kMultipartLimit)
			return uploadMultipart(h, total, meta, existingId, progress);
		return uploadResumable(h, total, meta, existingId, progress);
	}

private:
	static constexpr uint64_t kMultipartLimit = 5ull * 1024 * 1024;
	static constexpr uint64_t kChunk = 8ull * 1024 * 1024;   // multiple of 256 KiB

	Config& m_cfg;
	HttpClient& m_http;

	std::wstring authHeader() { return L"Authorization: Bearer " + utf8_to_wide(m_cfg.access_token); }

	static uint64_t to_u64(const std::string& s) { return strtoull(s.c_str(), nullptr, 10); }

	static void backoff(int attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(500 * (1 << attempt)));
	}

	static std::string apiError(const HttpResponse& r)
	{
		try
		{
			json j = json::parse(r.body);
			if (j.contains("error"))
			{
				auto& e = j["error"];
				if (e.is_object())
				{
					std::string msg = e.value("message", "");
					std::string reason;
					if (e.contains("errors") && !e["errors"].empty())
						reason = e["errors"][0].value("reason", "");
					return "Google Drive error " + std::to_string(r.status) + ": " + msg + (reason.empty() ? "" : " [" + reason + "]");
				}
				return "Google Drive error " + std::to_string(r.status) + ": " + e.dump();
			}
		}
		catch (...) {}
		return "Google Drive error " + std::to_string(r.status) + (r.body.empty() ? "" : ": " + r.body.substr(0, 300));
	}

	static DriveFile parse(const json& f)
	{
		DriveFile d;
		d.id = f.value("id", "");
		d.name = f.value("name", "");
		d.mimeType = f.value("mimeType", "");
		d.md5 = f.value("md5Checksum", "");
		d.size = to_u64(f.value("size", "0"));
		d.trashed = f.value("trashed", false);
		parse_rfc3339(f.value("modifiedTime", ""), d.modified);
		parse_rfc3339(f.value("createdTime", ""), d.created);
		if (d.mimeType == kMimeShortcut && f.contains("shortcutDetails"))
		{
			d.isShortcut = true;
			d.targetId = f["shortcutDetails"].value("targetId", "");
			d.targetMime = f["shortcutDetails"].value("targetMimeType", "");
		}
		return d;
	}

	// JSON API call with auth, one 401 retry and backoff on 429/5xx.
	json call(const wchar_t* method, const std::string& url, const std::string& body = "", const char* contentType = nullptr)
	{
		for (int attempt = 0;; ++attempt)
		{
			oauth::ensure_token(m_cfg, m_http);
			std::vector<std::wstring> hdrs{ authHeader() };
			if (contentType) hdrs.push_back(L"Content-Type: " + utf8_to_wide(contentType));
			HttpResponse r = m_http.request(method, utf8_to_wide(url), hdrs, body);
			if (r.ok())
			{
				if (r.body.empty()) return json::object();
				try { return json::parse(r.body); }
				catch (const std::exception& e) { throw GdError(std::string("Bad JSON from Drive: ") + e.what()); }
			}
			if (r.status == 401 && attempt == 0) { oauth::force_refresh(m_cfg, m_http); continue; }
			if ((r.status == 429 || r.status >= 500) && attempt < 3) { backoff(attempt); continue; }
			throw GdError(apiError(r), (int)r.status);
		}
	}

	DriveFile uploadMultipart(HANDLE h, uint64_t total, const json& meta, const std::string& existingId, const ProgressFn& progress)
	{
		std::string data(total, '\0');
		uint64_t off = 0;
		while (off < total)
		{
			DWORD got = 0;
			if (!ReadFile(h, data.data() + off, (DWORD)std::min<uint64_t>(1 << 20, total - off), &got, nullptr) || got == 0)
				throw GdError("Read failed: " + wide_to_utf8(win_error_text(GetLastError())));
			off += got;
		}
		if (progress && !progress(0, total)) throw GdCancel{};

		std::string boundary = "far_gdrive_" + oauth::random_verifier();
		std::string body = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" + meta.dump()
			+ "\r\n--" + boundary + "\r\nContent-Type: application/octet-stream\r\n\r\n" + data + "\r\n--" + boundary + "--";
		std::string url = std::string(kUploadBase) + "files" + (existingId.empty() ? "" : "/" + url_encode(existingId))
			+ "?uploadType=multipart&supportsAllDrives=true&fields=" + url_encode(kFileFields);
		json j = call(existingId.empty() ? L"POST" : L"PATCH", url, body, ("multipart/related; boundary=" + boundary).c_str());
		if (progress && !progress(total, total)) throw GdCancel{};
		return parse(j);
	}

	DriveFile uploadResumable(HANDLE h, uint64_t total, const json& meta, const std::string& existingId, const ProgressFn& progress)
	{
		// 1. start a session
		std::string sessionUri;
		for (int attempt = 0;; ++attempt)
		{
			oauth::ensure_token(m_cfg, m_http);
			std::string url = std::string(kUploadBase) + "files" + (existingId.empty() ? "" : "/" + url_encode(existingId))
				+ "?uploadType=resumable&supportsAllDrives=true&fields=" + url_encode(kFileFields);
			HttpResponse r = m_http.request(existingId.empty() ? L"POST" : L"PATCH", utf8_to_wide(url),
				{ authHeader(), L"Content-Type: application/json; charset=UTF-8",
				  L"X-Upload-Content-Type: application/octet-stream",
				  L"X-Upload-Content-Length: " + std::to_wstring(total) }, meta.dump());
			if (r.ok())
			{
				sessionUri = r.header("Location");
				if (sessionUri.empty()) throw GdError("Resumable upload: no session URI");
				break;
			}
			if (r.status == 401 && attempt == 0) { oauth::force_refresh(m_cfg, m_http); continue; }
			if ((r.status == 429 || r.status >= 500) && attempt < 3) { backoff(attempt); continue; }
			throw GdError(apiError(r), (int)r.status);
		}

		// 2. send chunks
		uint64_t pos = 0;
		int failures = 0;
		while (pos < total)
		{
			uint64_t len = std::min<uint64_t>(kChunk, total - pos);
			FileSlice slice{ h, pos, len };
			std::wstring range = L"Content-Range: bytes " + std::to_wstring(pos) + L"-" + std::to_wstring(pos + len - 1) + L"/" + std::to_wstring(total);
			HttpResponse r;
			try
			{
				r = m_http.request(L"PUT", utf8_to_wide(sessionUri), { range }, "", &slice, INVALID_HANDLE_VALUE,
					[&](uint64_t done, uint64_t) { return !progress || progress(pos + done, total); });
			}
			catch (const GdError&)
			{
				if (++failures > 5) throw;
				backoff(std::min(failures, 4));
				pos = queryResumePosition(sessionUri, total);
				continue;
			}
			if (r.status == 308)
			{
				std::string rng = r.header("Range");   // "bytes=0-12345"
				size_t dash = rng.find('-');
				pos = dash == std::string::npos ? pos + len : to_u64(rng.substr(dash + 1)) + 1;
				failures = 0;
				continue;
			}
			if (r.ok())
			{
				if (progress) progress(total, total);
				try { return parse(json::parse(r.body)); }
				catch (...) { throw GdError("Bad JSON after upload"); }
			}
			if ((r.status == 429 || r.status >= 500) && ++failures <= 5)
			{
				backoff(std::min(failures, 4));
				pos = queryResumePosition(sessionUri, total);
				continue;
			}
			throw GdError(apiError(r), (int)r.status);
		}
		throw GdError("Upload ended without a final response");
	}

	uint64_t queryResumePosition(const std::string& sessionUri, uint64_t total)
	{
		HttpResponse r = m_http.request(L"PUT", utf8_to_wide(sessionUri),
			{ L"Content-Range: bytes */" + std::to_wstring(total) }, "");
		if (r.status == 308)
		{
			std::string rng = r.header("Range");
			size_t dash = rng.find('-');
			return dash == std::string::npos ? 0 : to_u64(rng.substr(dash + 1)) + 1;
		}
		if (r.ok()) return total;
		throw GdError(apiError(r), (int)r.status);
	}
};
