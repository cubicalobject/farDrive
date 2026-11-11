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
static const char* const kFileFields = "id,name,mimeType,size,modifiedTime,createdTime,md5Checksum,trashed,explicitlyTrashed,shortcutDetails,webViewLink";

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
	bool explicitlyTrashed = false;   // trashed by itself, not just because a parent folder was
	bool isShortcut = false;
	std::string targetId;      // for shortcuts
	std::string targetMime;
	std::string webViewLink;   // opens the item in the browser (Google Docs etc.)

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

// Export formats Drive supports per Google document kind ("doc", "sheet", "slides", "drawing").
inline const char* allowed_export_exts(const char* kind)
{
	std::string k = kind;
	if (k == "doc") return "docx odt rtf pdf txt html epub md";
	if (k == "sheet") return "xlsx ods csv tsv pdf";
	if (k == "slides") return "pptx odp pdf txt";
	if (k == "drawing") return "png jpg svg pdf";
	return "";
}

inline bool valid_export_ext(const char* kind, const std::string& ext)
{
	std::string allowed = std::string(" ") + allowed_export_exts(kind) + " ";
	return !ext.empty() && allowed.find(" " + ext + " ") != std::string::npos;
}

// Lower-case hex MD5 of a file (BCrypt). Empty string on failure.
inline std::string md5_file(const std::wstring& path)
{
	HANDLE h = CreateFileW(long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return {};
	struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{ h };
	BCRYPT_ALG_HANDLE alg = nullptr;
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) return {};
	BCRYPT_HASH_HANDLE hh = nullptr;
	std::string out;
	if (BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0)
	{
		std::vector<unsigned char> buf(1 << 20);
		DWORD got = 0;
		bool ok = true;
		while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
			if (BCryptHashData(hh, buf.data(), got, 0) != 0) { ok = false; break; }
		unsigned char hash[16];
		if (ok && BCryptFinishHash(hh, hash, sizeof hash, 0) == 0)
		{
			static const char* hex = "0123456789abcdef";
			for (unsigned char b : hash) { out += hex[b >> 4]; out += hex[b & 15]; }
		}
		BCryptDestroyHash(hh);
	}
	BCryptCloseAlgorithmProvider(alg, 0);
	return out;
}

class Drive
{
public:
	Drive(Config& cfg, IHttp& http) : m_cfg(cfg), m_http(http) {}

	int retryBaseMs = 500;     // first back-off; doubles per attempt (tests set 0)
	int maxAttempts = 4;

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
			if (j.contains("files"))
				for (auto& f : j["files"]) out.push_back(parse(f));
			pageToken = j.value("nextPageToken", "");
		} while (!pageToken.empty());
		return out;
	}

	// Children of a folder. Inside the Trash the children of a trashed folder are themselves
	// flagged as trashed, so the caller passes includeTrashed=true there; otherwise trashed
	// items (which Drive still reports under their old parent) are filtered out.
	std::vector<DriveFile> listChildren(const std::string& folderId, bool includeTrashed = false)
	{
		return list("'" + q_escape(folderId) + "' in parents" + (includeTrashed ? "" : " and trashed = false"));
	}
	std::vector<DriveFile> listSharedWithMe()
	{
		return list("sharedWithMe = true and trashed = false", "folder,name");
	}
	std::vector<DriveFile> listStarred()
	{
		return list("starred = true and trashed = false", "folder,name");
	}
	// Top level of the Trash: only items the user trashed themselves. Items that are trashed
	// because a parent folder was trashed are reached by entering that folder.
	std::vector<DriveFile> listTrash()
	{
		std::vector<DriveFile> all = list("trashed = true and 'me' in owners", "folder,name");
		std::vector<DriveFile> out;
		for (auto& f : all) if (f.explicitlyTrashed) out.push_back(f);
		return out;
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

	// Creates a folder. A POST is not idempotent and Drive allows duplicate names, so after a
	// failed/uncertain attempt we look for a folder with that name before retrying.
	DriveFile createFolder(const std::string& parentId, const std::string& name)
	{
		json body{ {"name", name}, {"mimeType", kMimeFolder}, {"parents", json::array({parentId})} };
		std::string url = std::string(kApiBase) + "files?fields=" + url_encode(kFileFields) + "&supportsAllDrives=true";
		for (int attempt = 0;; ++attempt)
		{
			try
			{
				return parse(call(L"POST", url, body.dump(), "application/json; charset=UTF-8", /*retry*/false));
			}
			catch (const GdError& e)
			{
				bool uncertain = e.status == 0 || e.status == 429 || e.status >= 500 || e.status == 403;
				if (!uncertain || attempt + 1 >= maxAttempts) throw;
			}
			backoff(attempt);
			auto existing = list("'" + q_escape(parentId) + "' in parents and name = '" + q_escape(name)
				+ "' and mimeType = '" + kMimeFolder + "' and trashed = false");
			if (!existing.empty()) return existing.front();
		}
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

	// --- server-side copy / move ------------------------------------------------

	std::vector<std::string> parents(const std::string& id)
	{
		json j = call(L"GET", std::string(kApiBase) + "files/" + url_encode(id) + "?fields=parents&supportsAllDrives=true");
		std::vector<std::string> out;
		if (j.contains("parents") && j["parents"].is_array())
			for (auto& p : j["parents"]) out.push_back(p.get<std::string>());
		return out;
	}

	// Re-parents an item (file or whole folder subtree) in place: same id, sharing, comments,
	// revision history. oldParent may be empty when the item has no known parent.
	void move(const std::string& id, const std::string& newParent, const std::string& oldParent)
	{
		std::string url = std::string(kApiBase) + "files/" + url_encode(id) + "?supportsAllDrives=true&addParents=" + url_encode(newParent)
			+ (oldParent.empty() ? "" : "&removeParents=" + url_encode(oldParent)) + "&fields=id";
		call(L"PATCH", url, "{}", "application/json; charset=UTF-8");
	}

	// Server-side copy of one file (not folders) into destParent. Google-native documents stay
	// native; shortcuts are copied as shortcuts.
	DriveFile copyFile(const std::string& id, const std::string& destParent, const std::string& name)
	{
		json body{ {"parents", json::array({destParent})} };
		if (!name.empty()) body["name"] = name;
		return parse(call(L"POST", std::string(kApiBase) + "files/" + url_encode(id) + "/copy?supportsAllDrives=true&fields=" + url_encode(kFileFields),
			body.dump(), "application/json; charset=UTF-8", /*retry*/ false));
	}

	// True when `folderId` is `id` itself or one of its ancestors (walks parents, bounded depth).
	bool isSameOrAncestor(const std::string& folderId, const std::string& id)
	{
		std::string cur = id;
		for (int depth = 0; depth < 64 && !cur.empty(); ++depth)
		{
			if (cur == folderId) return true;
			auto ps = parents(cur);
			if (ps.empty()) return false;
			cur = ps.front();
		}
		return false;
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

	// Downloads (or exports) a file to localPath. The data goes to "<localPath>.part" first and
	// replaces the destination only after a complete, checksum-verified transfer, so a failed or
	// cancelled download never destroys an existing local file. Throws GdError / GdCancel.
	void download(const DriveFile& f, const std::wstring& localPath, const ProgressFn& progress,
		const char* exportExtOverride = nullptr)
	{
		std::string id = f.effectiveId();
		std::string url;
		bool isExport = f.isGoogleDoc();
		if (isExport)
		{
			std::string ext = exportExtOverride ? exportExtOverride : exportExtFor(f.effectiveMime());
			const char* mime = ext.empty() ? nullptr : export_mime_for_ext(ext);
			if (!mime) throw GdError("This Google file type cannot be exported: " + f.effectiveMime());
			url = std::string(kApiBase) + "files/" + url_encode(id) + "/export?mimeType=" + url_encode(mime);
		}
		else
			url = std::string(kApiBase) + "files/" + url_encode(id) + "?alt=media&supportsAllDrives=true";

		const std::wstring part = localPath + L".part";
		const std::wstring lpPart = long_path(part);
		for (int attempt = 0;; ++attempt)
		{
			oauth::ensure_token(m_cfg, m_http);
			HANDLE h = CreateFileW(lpPart.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				throw GdError("Cannot create " + wide_to_utf8(part) + ": " + wide_to_utf8(win_error_text(GetLastError())));
			HttpResponse resp;
			try
			{
				resp = m_http.request(L"GET", utf8_to_wide(url), { authHeader() }, "", nullptr, h, progress);
			}
			catch (...)
			{
				CloseHandle(h);
				DeleteFileW(lpPart.c_str());
				throw;
			}
			if (resp.ok())
			{
				if (f.modified.dwHighDateTime || f.modified.dwLowDateTime)
					SetFileTime(h, nullptr, nullptr, &f.modified);
				CloseHandle(h);
				if (!isExport && !f.md5.empty())
				{
					std::string got = md5_file(part);
					if (!got.empty() && got != to_lower(f.md5))
					{
						DeleteFileW(lpPart.c_str());
						throw GdError("Checksum mismatch after downloading " + f.name + " - the local file was left unchanged");
					}
				}
				if (!MoveFileExW(lpPart.c_str(), long_path(localPath).c_str(), MOVEFILE_REPLACE_EXISTING))
				{
					DWORD err = GetLastError();
					DeleteFileW(lpPart.c_str());
					throw GdError("Cannot replace " + wide_to_utf8(localPath) + ": " + wide_to_utf8(win_error_text(err)));
				}
				return;
			}
			CloseHandle(h);
			DeleteFileW(lpPart.c_str());
			if (resp.status == 401 && attempt == 0) { oauth::force_refresh(m_cfg, m_http); continue; }
			if (isRetryable(resp) && attempt + 1 < maxAttempts) { backoff(attempt); continue; }
			throw GdError(apiError(resp), (int)resp.status);
		}
	}

	// Uploads localPath as a new file in parentId, or replaces the content of existingId.
	// contentType: MIME type of the data; guessed from the name when empty.
	DriveFile upload(const std::string& parentId, const std::string& name, const std::wstring& localPath,
		const std::string& existingId, const ProgressFn& progress, const std::string& contentType = "")
	{
		HANDLE h = CreateFileW(long_path(localPath).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			throw GdError("Cannot open " + wide_to_utf8(localPath) + ": " + wide_to_utf8(win_error_text(GetLastError())));
		struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{ h };

		LARGE_INTEGER sz{};
		GetFileSizeEx(h, &sz);
		uint64_t total = (uint64_t)sz.QuadPart;
		FILETIME mtime{};
		GetFileTime(h, nullptr, nullptr, &mtime);

		std::string mime = contentType.empty() ? guess_mime(utf8_to_wide(name)) : contentType;
		json meta = json::object();
		if (existingId.empty())
		{
			meta["name"] = name;
			meta["parents"] = json::array({ parentId });
			meta["mimeType"] = mime;
		}
		std::string m = filetime_to_rfc3339(mtime);
		if (!m.empty()) meta["modifiedTime"] = m;

		if (total < kMultipartLimit)
			return uploadMultipart(h, total, meta, existingId, mime, progress);
		return uploadResumable(h, total, meta, existingId, mime, progress);
	}

	// True for responses worth retrying: 429, 5xx, and Drive's 403 rate-limit variants.
	static bool isRetryable(const HttpResponse& r)
	{
		if (r.status == 429 || r.status >= 500) return true;
		if (r.status == 403)
		{
			std::string reason = errorReason(r);
			return reason == "rateLimitExceeded" || reason == "userRateLimitExceeded" || reason == "sharingRateLimitExceeded";
		}
		return false;
	}

private:
	static constexpr uint64_t kMultipartLimit = 5ull * 1024 * 1024;
	static constexpr uint64_t kChunk = 8ull * 1024 * 1024;   // multiple of 256 KiB

	Config& m_cfg;
	IHttp& m_http;

	std::wstring authHeader() { return L"Authorization: Bearer " + utf8_to_wide(m_cfg.access_token); }

	static uint64_t to_u64(const std::string& s) { return strtoull(s.c_str(), nullptr, 10); }

	void backoff(int attempt) const
	{
		if (retryBaseMs > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(retryBaseMs * (1 << std::min(attempt, 4))));
	}

	static std::string errorReason(const HttpResponse& r)
	{
		try
		{
			json j = json::parse(r.body);
			if (j.contains("error") && j["error"].is_object())
			{
				auto& e = j["error"];
				if (e.contains("errors") && e["errors"].is_array() && !e["errors"].empty())
					return e["errors"][0].value("reason", "");
				if (e.contains("status")) return e.value("status", "");
			}
		}
		catch (...) {}
		return {};
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
					std::string reason = errorReason(r);
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
		d.explicitlyTrashed = f.value("explicitlyTrashed", false);
		d.webViewLink = f.value("webViewLink", "");
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

	// JSON API call with auth, one 401 refresh, and back-off on retryable responses.
	// Network-level failures are retried for GET only (idempotent). `retry=false` disables
	// status-based retries (used by createFolder, which handles them itself).
	json call(const wchar_t* method, const std::string& url, const std::string& body = "",
		const char* contentType = nullptr, bool retry = true)
	{
		bool isGet = wcscmp(method, L"GET") == 0;
		for (int attempt = 0;; ++attempt)
		{
			oauth::ensure_token(m_cfg, m_http);
			std::vector<std::wstring> hdrs{ authHeader() };
			if (contentType) hdrs.push_back(L"Content-Type: " + utf8_to_wide(contentType));
			HttpResponse r;
			try
			{
				r = m_http.request(method, utf8_to_wide(url), hdrs, body);
			}
			catch (const GdError&)
			{
				if (isGet && retry && attempt + 1 < maxAttempts) { backoff(attempt); continue; }
				throw;
			}
			if (r.ok())
			{
				if (r.body.empty()) return json::object();
				try { return json::parse(r.body); }
				catch (const std::exception& e) { throw GdError(std::string("Bad JSON from Drive: ") + e.what()); }
			}
			if (r.status == 401 && attempt == 0) { oauth::force_refresh(m_cfg, m_http); continue; }
			if (retry && isRetryable(r) && attempt + 1 < maxAttempts) { backoff(attempt); continue; }
			throw GdError(apiError(r), (int)r.status);
		}
	}

	DriveFile uploadMultipart(HANDLE h, uint64_t total, const json& meta, const std::string& existingId,
		const std::string& mime, const ProgressFn& progress)
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
			+ "\r\n--" + boundary + "\r\nContent-Type: " + mime + "\r\n\r\n" + data + "\r\n--" + boundary + "--";
		std::string url = std::string(kUploadBase) + "files" + (existingId.empty() ? "" : "/" + url_encode(existingId))
			+ "?uploadType=multipart&supportsAllDrives=true&fields=" + url_encode(kFileFields);
		// a POST may have been applied even if the response was lost, so do not auto-retry a create
		json j = call(existingId.empty() ? L"POST" : L"PATCH", url, body, ("multipart/related; boundary=" + boundary).c_str(),
			/*retry*/ !existingId.empty());
		if (progress && !progress(total, total)) throw GdCancel{};
		return parse(j);
	}

	DriveFile uploadResumable(HANDLE h, uint64_t total, const json& meta, const std::string& existingId,
		const std::string& mime, const ProgressFn& progress)
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
				  L"X-Upload-Content-Type: " + utf8_to_wide(mime),
				  L"X-Upload-Content-Length: " + std::to_wstring(total) }, meta.dump());
			if (r.ok())
			{
				sessionUri = r.header("Location");
				if (sessionUri.empty()) throw GdError("Resumable upload: no session URI");
				break;
			}
			if (r.status == 401 && attempt == 0) { oauth::force_refresh(m_cfg, m_http); continue; }
			if (isRetryable(r) && attempt + 1 < maxAttempts) { backoff(attempt); continue; }
			throw GdError(apiError(r), (int)r.status);
		}

		// 2. send chunks
		uint64_t pos = 0;
		int failures = 0;
		int stalls = 0;   // consecutive 308s that did not advance `pos`
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
				ResumeStatus rs = queryResumePosition(sessionUri, total);
				if (rs.complete) { if (progress) progress(total, total); return rs.file; }   // the lost reply was the final one
				pos = rs.pos;
				continue;
			}
			if (r.status == 308)
			{
				// "Range: bytes=0-N" tells how much the server holds; no Range header means nothing yet.
				uint64_t next = rangeEnd(r.header("Range"));
				stalls = next > pos ? 0 : stalls + 1;
				if (stalls > 3) throw GdError("Upload is not progressing (server keeps asking for the same bytes)");
				pos = next;
				failures = 0;
				continue;
			}
			if (r.ok())
			{
				if (progress) progress(total, total);
				try { return parse(json::parse(r.body)); }
				catch (...) { throw GdError("Bad JSON after upload"); }
			}
			if (isRetryable(r) && ++failures <= 5)
			{
				backoff(std::min(failures, 4));
				ResumeStatus rs = queryResumePosition(sessionUri, total);
				if (rs.complete) { if (progress) progress(total, total); return rs.file; }
				pos = rs.pos;
				continue;
			}
			throw GdError(apiError(r), (int)r.status);
		}
		throw GdError("Upload ended without a final response");
	}

	// Next byte to send according to a "Range: bytes=0-N" header (0 when absent).
	static uint64_t rangeEnd(const std::string& rng)
	{
		size_t dash = rng.find('-');
		return dash == std::string::npos ? 0 : to_u64(rng.substr(dash + 1)) + 1;
	}

	struct ResumeStatus
	{
		uint64_t pos = 0;        // next byte to send
		bool complete = false;   // the upload already finished; `file` is the result
		DriveFile file;
	};

	// Asks the session where it stands. A 2xx answer means the upload is finished (the reply
	// that got lost was the final one) and carries the file resource.
	ResumeStatus queryResumePosition(const std::string& sessionUri, uint64_t total)
	{
		HttpResponse r = m_http.request(L"PUT", utf8_to_wide(sessionUri),
			{ L"Content-Range: bytes */" + std::to_wstring(total) }, "");
		ResumeStatus rs;
		if (r.status == 308) { rs.pos = rangeEnd(r.header("Range")); return rs; }
		if (r.ok())
		{
			rs.complete = true;
			rs.pos = total;
			try { rs.file = parse(json::parse(r.body)); }
			catch (...) { throw GdError("Upload finished but Drive returned no file description"); }
			return rs;
		}
		throw GdError(apiError(r), (int)r.status);
	}
};
