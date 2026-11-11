// test_host.cpp - loads GoogleDrive.dll like Far would, smoke-tests the live HTTP layer, and
// unit-tests the Drive/OAuth/config logic against a scripted fake transport.
#include "../src/util.h"
#include "../src/http.h"
#include "../src/oauth.h"
#include "../src/drive.h"
#include <plugin.hpp>
#include <cstdio>
#include <deque>
#include <functional>

static int g_fails = 0;
static void check(bool ok, const char* what)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) ++g_fails;
}

static const wchar_t* WINAPI FakeGetMsg(const UUID*, intptr_t id) { static wchar_t b[32]; swprintf(b, 32, L"msg%d", (int)id); return b; }

// ---------------------------------------------------------------------------
// scripted transport
// ---------------------------------------------------------------------------
struct FakeHttp : IHttp
{
	struct Req
	{
		std::wstring method, url;
		std::vector<std::wstring> headers;
		std::string body;
		uint64_t sliceOff = 0, sliceLen = 0;
		bool hasSlice = false;
		std::wstring header(const wchar_t* name) const
		{
			size_t n = wcslen(name);
			for (auto& h : headers) if (_wcsnicmp(h.c_str(), name, n) == 0) return h;
			return L"";
		}
	};
	std::vector<Req> log;
	std::deque<std::function<HttpResponse(const Req&)>> script;

	HttpResponse request(const wchar_t* method, const std::wstring& url, const std::vector<std::wstring>& headers,
		const std::string& body, const FileSlice* slice, HANDLE sink, const ProgressFn& progress) override
	{
		Req r{ method, url, headers, body };
		if (slice) { r.hasSlice = true; r.sliceOff = slice->offset; r.sliceLen = slice->length; }
		log.push_back(r);
		if (script.empty()) throw GdError("FakeHttp: no scripted response for " + wide_to_utf8(url));
		auto fn = script.front();
		script.pop_front();
		HttpResponse resp = fn(r);
		if (sink != INVALID_HANDLE_VALUE && resp.ok())
		{
			DWORD w = 0;
			WriteFile(sink, resp.body.data(), (DWORD)resp.body.size(), &w, nullptr);
			if (progress && !progress(resp.body.size(), resp.body.size())) throw GdCancel{};
			resp.body.clear();
		}
		return resp;
	}
};

static HttpResponse Resp(DWORD status, std::string body = "{}", std::map<std::string, std::string> headers = {})
{
	HttpResponse r;
	r.status = status;
	r.body = std::move(body);
	for (auto& [k, v] : headers) r.headers[to_lower(k)] = v;
	return r;
}
static auto Const(DWORD status, std::string body = "{}", std::map<std::string, std::string> headers = {})
{
	return [=](const FakeHttp::Req&) { return Resp(status, body, headers); };
}

static std::wstring TempDir()
{
	wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
	std::wstring d = std::wstring(tmp) + L"gdrive_tests";
	CreateDirectoryW(d.c_str(), nullptr);
	return d;
}
static void WriteText(const std::wstring& path, const std::string& text)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
	DWORD w = 0; WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr); CloseHandle(h);
}
static std::string ReadText(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return "<missing>";
	std::string s(1 << 20, '\0'); DWORD n = 0; ReadFile(h, s.data(), (DWORD)s.size(), &n, nullptr); CloseHandle(h);
	s.resize(n); return s;
}
static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

static Config TestConfig(const std::wstring& dir)
{
	Config cfg;
	cfg.client_id = "cid"; cfg.client_secret = "csec";
	cfg.refresh_token = "rt"; cfg.access_token = "at"; cfg.expires_at = unix_now() + 3600;
	cfg.file = dir + L"\\cfg.json";
	return cfg;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::wstring dllArg = argc > 1 ? utf8_to_wide(argv[1]) : L"";
	std::wstring dir = TempDir();

	// --- 1. plugin DLL loads and exports what Far needs
	const wchar_t* dll = argc > 1 ? dllArg.c_str() : L"out\\GoogleDrive\\GoogleDrive.dll";
	HMODULE h = LoadLibraryW(dll);
	check(h != nullptr, "LoadLibrary GoogleDrive.dll");
	if (!h) { printf("  error %lu\n", GetLastError()); return 1; }
	const char* names[] = { "GetGlobalInfoW","SetStartupInfoW","GetPluginInfoW","ConfigureW","OpenW","ClosePanelW",
		"GetOpenPanelInfoW","GetFindDataW","FreeFindDataW","SetDirectoryW","GetFilesW","PutFilesW","DeleteFilesW",
		"MakeDirectoryW","ProcessPanelInputW","ExitFARW" };
	bool allExports = true;
	for (auto n : names) if (!GetProcAddress(h, n)) { allExports = false; printf("  missing export %s\n", n); }
	check(allExports, "all Far exports present");

	auto GetGlobalInfo = (void(WINAPI*)(GlobalInfo*))GetProcAddress(h, "GetGlobalInfoW");
	GlobalInfo gi{};
	GetGlobalInfo(&gi);
	check(gi.StructSize == sizeof(GlobalInfo) && gi.Title && wcscmp(gi.Title, L"Google Drive") == 0, "GetGlobalInfoW");
	check(gi.MinFarVersion.Build <= FARMANAGERVERSION_BUILD, "MinFarVersion <= SDK build");

	auto SetStartupInfo = (void(WINAPI*)(const PluginStartupInfo*))GetProcAddress(h, "SetStartupInfoW");
	PluginStartupInfo psi{};
	FarStandardFunctions fsf{};
	fsf.StructSize = sizeof fsf;
	psi.StructSize = sizeof psi;
	psi.ModuleName = dll;
	psi.FSF = &fsf;
	psi.GetMsg = FakeGetMsg;
	SetEnvironmentVariableW(L"FARPROFILE", (dir + L"\\profile").c_str());
	SetStartupInfo(&psi);

	auto GetPluginInfo = (void(WINAPI*)(PluginInfo*))GetProcAddress(h, "GetPluginInfoW");
	PluginInfo pi{};
	GetPluginInfo(&pi);
	check(pi.StructSize == sizeof(PluginInfo) && pi.PluginMenu.Count == 1 && pi.CommandPrefix && wcscmp(pi.CommandPrefix, L"gdrive") == 0, "GetPluginInfoW");

	// --- 2. live HTTP layer against Google endpoints (no credentials needed)
	try
	{
		HttpClient http;
		auto r = http.request(L"GET", L"https://www.googleapis.com/discovery/v1/apis?name=drive&preferred=true", {}, "");
		check(r.status == 200, "HTTPS GET discovery -> 200");
		json j = json::parse(r.body);
		check(j.contains("items"), "discovery JSON parsed");

		Config cfg = TestConfig(dir);
		cfg.client_id = "bogus"; cfg.client_secret = "bogus"; cfg.refresh_token = "bogus"; cfg.access_token.clear();
		bool threw = false;
		try { oauth::ensure_token(cfg, http); }
		catch (const GdError& e) { threw = true; printf("  token error as expected: %s\n", e.what()); }
		check(threw, "refresh with bogus creds -> GdError");

		std::wstring path = dir + L"\\download.json";
		HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		uint64_t seen = 0;
		auto r2 = http.request(L"GET", L"https://www.googleapis.com/discovery/v1/apis", {}, "", nullptr, f,
			[&](uint64_t d, uint64_t) { seen = d; return true; });
		LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz); CloseHandle(f);
		check(r2.status == 200 && sz.QuadPart > 0 && (uint64_t)sz.QuadPart == seen, "download to file with progress");
		DeleteFileW(path.c_str());
	}
	catch (const std::exception& e) { printf("FAIL live HTTP exception: %s\n", e.what()); ++g_fails; }

	// --- 3. utilities
	{
		FILETIME ft{};
		bool allMs = true;
		for (int ms = 0; ms < 1000; ++ms)
		{
			char s[64]; snprintf(s, sizeof s, "2024-03-05T06:07:08.%03dZ", ms);
			SYSTEMTIME st{};
			if (!parse_rfc3339(s, ft) || !FileTimeToSystemTime(&ft, &st) || st.wMilliseconds != ms || st.wSecond != 8)
			{
				if (allMs) printf("  first wrong ms: %d -> %d\n", ms, st.wMilliseconds);
				allMs = false;
			}
		}
		check(allMs, "parse_rfc3339 exact for all 1000 millisecond values");
		check(parse_rfc3339("2024-03-05T06:07:08Z", ft) && filetime_to_rfc3339(ft) == "2024-03-05T06:07:08.000Z", "parse_rfc3339 without fraction");
		check(parse_rfc3339("2024-03-05T06:07:08.5Z", ft) && filetime_to_rfc3339(ft) == "2024-03-05T06:07:08.500Z", "parse_rfc3339 short fraction");
		check(parse_rfc3339("2024-03-05T06:07:08.123456Z", ft) && filetime_to_rfc3339(ft) == "2024-03-05T06:07:08.123Z", "parse_rfc3339 long fraction");
		check(parse_rfc3339("2024-03-05T08:07:08+02:00", ft) && filetime_to_rfc3339(ft) == "2024-03-05T06:07:08.000Z", "parse_rfc3339 positive offset");
		check(parse_rfc3339("2024-03-05T01:07:08.250-05:00", ft) && filetime_to_rfc3339(ft) == "2024-03-05T06:07:08.250Z", "parse_rfc3339 negative offset");
		check(!parse_rfc3339("garbage", ft), "parse_rfc3339 rejects garbage");

		check(sanitize_filename(L"a/b:c?.txt.") == L"a_b_c_.txt", "sanitize_filename invalid chars");
		check(sanitize_filename(L"CON") == L"_CON" && sanitize_filename(L"con.txt") == L"_con.txt"
			&& sanitize_filename(L"COM1.log") == L"_COM1.log" && sanitize_filename(L"console.txt") == L"console.txt"
			&& sanitize_filename(L"LPT0") == L"LPT0", "sanitize_filename reserved device names");

		auto u = unique_display_names({ {L"a.txt", false}, {L"a.txt", false}, {L"a (2).txt", false} });
		check(u.size() == 3 && u[0] == L"a.txt" && u[1] == L"a (3).txt" && u[2] == L"a (2).txt", "unique_display_names skips taken suffix");
		u = unique_display_names({ {L"X", true}, {L"x", true}, {L"X", true} });
		check(u[0] == L"X" && u[1] == L"x (2)" && u[2] == L"X (3)", "unique_display_names folders, case-insensitive");
		u = unique_display_names({ {L"README", false}, {L"readme", false} });
		check(u[1] == L"readme (2)", "unique_display_names no extension");

		check(format_size(1536) == L"1.5 KB" && format_size(10) == L"10 B", "format_size prints units");
		check(url_encode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9", "url_encode");
		check(q_escape("it's \\ ok") == "it\\'s \\\\ ok", "q_escape");
		check(html_escape("<b>&'\"") == "&lt;b&gt;&amp;&#39;&quot;", "html_escape");
		check(oauth::sha256_b64url("abc") == "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0", "sha256 base64url");
		check(base64_decode(base64_encode("hello, world!", 13)) == "hello, world!" && base64_encode("ab", 2) == "YWI=", "base64 round trip");
		check(export_mime_for_ext("docx") != nullptr && export_mime_for_ext("zzz") == nullptr, "export table");
		check(valid_export_ext("doc", "docx") && valid_export_ext("sheet", "csv") && valid_export_ext("slides", "txt")
			&& valid_export_ext("drawing", "svg") && !valid_export_ext("doc", "doc") && !valid_export_ext("sheet", "docx")
			&& !valid_export_ext("doc", ""), "valid_export_ext per document kind");
		check(guess_mime(L"photo.JPG") == "image/jpeg" && guess_mime(L"noext") == "application/octet-stream", "guess_mime");
		check(long_path(L"C:\\a\\b") == L"\\\\?\\C:\\a\\b" && long_path(L"\\\\srv\\share\\x") == L"\\\\?\\UNC\\srv\\share\\x"
			&& long_path(L"\\\\?\\C:\\x") == L"\\\\?\\C:\\x", "long_path");

		std::vector<std::pair<std::string, std::string>> params;
		check(oauth::parse_callback_line("GET /?state=xyz&code=4%2Fabc HTTP/1.1", params) && params.size() == 2
			&& params[1].first == "code" && params[1].second == "4/abc", "parse_callback_line decodes query");
		params.clear();
		check(!oauth::parse_callback_line("GET /favicon.ico HTTP/1.1", params), "parse_callback_line ignores other paths");
		check(!oauth::parse_callback_line("POST /?code=1 HTTP/1.1", params), "parse_callback_line ignores non-GET");
	}

	// --- 4. config file: DPAPI protection, atomic save, corrupt file handling
	{
		Config cfg = TestConfig(dir);
		cfg.refresh_token = "secret-refresh-token";
		cfg.client_secret = "secret-client";
		cfg.save();
		std::string raw = ReadText(cfg.file);
		check(raw.find("secret-refresh-token") == std::string::npos && raw.find("secret-client") == std::string::npos
			&& raw.find("dpapi:") != std::string::npos, "config secrets are DPAPI-protected on disk");
		check(!Exists(cfg.file + L".tmp"), "config save leaves no temp file");
		Config back; back.file = cfg.file;
		check(back.load() && back.refresh_token == "secret-refresh-token" && back.client_secret == "secret-client"
			&& back.client_id == "cid", "config DPAPI round trip");

		WriteText(cfg.file, "{\"client_id\":\"old\",\"refresh_token\":\"plain-token\"}");
		Config legacy; legacy.file = cfg.file;
		check(legacy.load() && legacy.refresh_token == "plain-token", "legacy plaintext config still loads");
		check(ReadText(cfg.file).find("plain-token") == std::string::npos, "legacy config is re-saved protected");

		WriteText(cfg.file, "{{{ not json");
		Config broken; broken.file = cfg.file;
		check(!broken.load() && !Exists(cfg.file) && Exists(cfg.file + L".bad"), "corrupt config moved to .bad");
		DeleteFileW((cfg.file + L".bad").c_str());

		Config unwritable = TestConfig(dir);
		unwritable.file = L"Z:\\no\\such\\drive\\cfg.json";
		bool threw = false;
		try { unwritable.save(); } catch (const GdError&) { threw = true; }
		check(threw, "config save reports failure");

		// shipped client file next to the DLL: used when the dialog fields are empty
		std::wstring clientFile = dir + L"\\GoogleDrive.client.json";
		WriteText(clientFile, R"({"client_id":"ship-id.apps.googleusercontent.com","client_secret":"ship-secret"})");
		Config shipped; shipped.file = dir + L"\\none.json";
		check(shipped.load_client_file(clientFile) && shipped.has_builtin_client()
			&& shipped.effective_client_id() == "ship-id.apps.googleusercontent.com"
			&& shipped.effective_client_secret() == "ship-secret", "client file provides the shipped client");
		shipped.client_id = "own-id"; shipped.client_secret = "own-secret";
		check(shipped.effective_client_id() == "own-id" && shipped.effective_client_secret() == "own-secret", "dialog fields override the shipped client");
		WriteText(clientFile, "{ not json");
		Config broken2;
		check(!broken2.load_client_file(clientFile) && !broken2.load_client_file(dir + L"\\missing.json"), "bad or missing client file is ignored");
		DeleteFileW(clientFile.c_str());
	}

	// --- 5. Drive logic against the fake transport
	{
		FakeHttp fake;
		Config cfg = TestConfig(dir);
		Drive drive(cfg, fake);
		drive.retryBaseMs = 0;

		// 403 rate limit is retried, other 403s are not
		fake.script.push_back(Const(403, R"({"error":{"errors":[{"reason":"userRateLimitExceeded"}],"message":"Rate limit"}})"));
		fake.script.push_back(Const(200, R"({"files":[{"id":"a","name":"A"}]})"));
		auto files = drive.listChildren("root");
		check(files.size() == 1 && fake.log.size() == 2, "403 rateLimitExceeded is retried");
		fake.log.clear();
		fake.script.push_back(Const(403, R"({"error":{"errors":[{"reason":"forbidden"}],"message":"no"}})"));
		bool threw = false;
		try { drive.listChildren("root"); } catch (const GdError& e) { threw = e.status == 403; }
		check(threw && fake.log.size() == 1, "403 forbidden is not retried");

		// Trash: top level shows only explicitly trashed items; children below keep the filter off
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"files":[
			{"id":"t1","name":"Trashed folder","mimeType":"application/vnd.google-apps.folder","trashed":true,"explicitlyTrashed":true},
			{"id":"t2","name":"child of it","trashed":true,"explicitlyTrashed":false},
			{"id":"t3","name":"Trashed file","trashed":true,"explicitlyTrashed":true}]})"));
		auto trash = drive.listTrash();
		check(trash.size() == 2 && trash[0].id == "t1" && trash[1].id == "t3", "listTrash hides items trashed via their parent");
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"files":[]})"));
		drive.listChildren("t1", true);
		fake.script.push_back(Const(200, R"({"files":[]})"));
		drive.listChildren("t1", false);
		auto qParam = [](const std::wstring& url) {
			size_t a = url.find(L"?q="), b = url.find(L'&', a == std::wstring::npos ? 0 : a);
			return a == std::wstring::npos ? std::wstring() : url.substr(a + 3, b == std::wstring::npos ? std::wstring::npos : b - a - 3);
		};
		check(qParam(fake.log[0].url).find(L"trashed") == std::wstring::npos
			&& qParam(fake.log[1].url).find(L"trashed%20%3D%20false") != std::wstring::npos,
			"listChildren drops the trashed filter inside the Trash");

		// server-side move / copy / parents / ancestor check
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"id":"f1"})"));
		drive.move("f1", "newP", "oldP");
		check(fake.log[0].method == L"PATCH" && fake.log[0].url.find(L"addParents=newP") != std::wstring::npos
			&& fake.log[0].url.find(L"removeParents=oldP") != std::wstring::npos, "move uses addParents/removeParents");
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"id":"c1","name":"Doc","mimeType":"application/vnd.google-apps.document"})"));
		DriveFile cp = drive.copyFile("f1", "dest", "Doc");
		check(cp.id == "c1" && fake.log[0].method == L"POST" && fake.log[0].url.find(L"files/f1/copy") != std::wstring::npos
			&& fake.log[0].body.find("\"parents\":[\"dest\"]") != std::string::npos, "copyFile posts to files/{id}/copy with parents");
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"parents":["p1"]})"));   // parents(x) -> p1
		fake.script.push_back(Const(200, R"({"parents":["root"]})")); // parents(p1) -> root
		fake.script.push_back(Const(200, R"({})"));                    // parents(root) -> none
		check(!drive.isSameOrAncestor("zzz", "x") && fake.log.size() == 3, "isSameOrAncestor walks up to the root");
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"parents":["target"]})"));
		check(drive.isSameOrAncestor("target", "x") && fake.log.size() == 1, "isSameOrAncestor finds a parent");
		check(drive.isSameOrAncestor("x", "x"), "isSameOrAncestor: same folder");

		// Starred pseudo-folder queries starred, non-trashed items
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"files":[{"id":"s1","name":"Fav"}]})"));
		auto starred = drive.listStarred();
		check(starred.size() == 1 && qParam(fake.log[0].url).find(L"starred%20%3D%20true") != std::wstring::npos
			&& qParam(fake.log[0].url).find(L"trashed%20%3D%20false") != std::wstring::npos, "listStarred query");

		// expired / revoked refresh token: tokens are cleared and GdAuthError is raised
		{
			Config expired = TestConfig(dir);
			expired.access_token.clear();
			Drive d2(expired, fake);
			fake.log.clear();
			fake.script.push_back(Const(400, R"({"error":"invalid_grant","error_description":"Token has been expired or revoked."})"));
			bool auth = false;
			try { d2.listChildren("root"); } catch (const GdAuthError&) { auth = true; } catch (...) {}
			check(auth && !expired.signed_in() && expired.access_token.empty() && expired.client_id == "cid",
				"invalid_grant clears the tokens, keeps the client, raises GdAuthError");
			Config other = TestConfig(dir);
			other.access_token.clear();
			Drive d3(other, fake);
			fake.script.push_back(Const(500, R"({"error":"server_error"})"));
			bool plain = false;
			try { d3.listChildren("root"); } catch (const GdAuthError&) {} catch (const GdError&) { plain = true; }
			check(plain && other.signed_in(), "other token endpoint errors keep the sign-in");
		}

		// network error on GET is retried
		fake.log.clear();
		fake.script.push_back([](const FakeHttp::Req&) -> HttpResponse { throw GdError("WinHttpSendRequest failed"); });
		fake.script.push_back(Const(200, R"({"files":[]})"));
		drive.listChildren("root");
		check(fake.log.size() == 2, "network error on GET is retried");

		// createFolder: after a failed POST, an existing folder is picked up instead of a duplicate
		fake.log.clear();
		fake.script.push_back(Const(500, R"({"error":{"message":"backend"}})"));
		fake.script.push_back(Const(200, R"({"files":[{"id":"fld","name":"New","mimeType":"application/vnd.google-apps.folder"}]})"));
		DriveFile created = drive.createFolder("root", "New");
		check(created.id == "fld" && fake.log.size() == 2 && fake.log[1].method == L"GET", "createFolder after 500 reuses the folder that got created");
		fake.log.clear();
		fake.script.push_back(Const(500));
		fake.script.push_back(Const(200, R"({"files":[]})"));
		fake.script.push_back(Const(200, R"({"id":"x","name":"New","mimeType":"application/vnd.google-apps.folder"})"));
		created = drive.createFolder("root", "New");
		check(created.id == "x" && fake.log.size() == 3, "createFolder retries when nothing was created");

		// download: failure keeps the existing local file, .part is cleaned up
		std::wstring local = dir + L"\\target.bin";
		WriteText(local, "old content");
		DriveFile f; f.id = "f1"; f.name = "target.bin"; f.mimeType = "application/octet-stream";
		fake.log.clear();
		for (int i = 0; i < 4; ++i) fake.script.push_back(Const(500, "oops"));
		threw = false;
		try { drive.download(f, local, nullptr); } catch (const GdError&) { threw = true; }
		check(threw && ReadText(local) == "old content" && !Exists(local + L".part"), "failed download leaves the local file untouched");

		// download: cancelled mid-transfer keeps the existing local file
		fake.script.push_back(Const(200, "new content"));
		threw = false;
		try { drive.download(f, local, [](uint64_t, uint64_t) { return false; }); } catch (const GdCancel&) { threw = true; }
		check(threw && ReadText(local) == "old content" && !Exists(local + L".part"), "cancelled download leaves the local file untouched");

		// download: checksum mismatch is rejected
		f.md5 = "00000000000000000000000000000000";
		fake.script.push_back(Const(200, "new content"));
		threw = false;
		try { drive.download(f, local, nullptr); } catch (const GdError& e) { threw = std::string(e.what()).find("Checksum") != std::string::npos; }
		check(threw && ReadText(local) == "old content" && !Exists(local + L".part"), "checksum mismatch keeps the old file");

		// download: matching checksum replaces the file
		WriteText(dir + L"\\expected.bin", "new content");
		f.md5 = md5_file(dir + L"\\expected.bin");
		fake.script.push_back(Const(200, "new content"));
		drive.download(f, local, nullptr);
		check(!f.md5.empty() && ReadText(local) == "new content" && !Exists(local + L".part"), "verified download replaces the file");

		// multipart upload carries the guessed MIME type; a failed create is not blindly retried
		std::wstring png = dir + L"\\pic.png";
		WriteText(png, "PNGDATA");
		fake.log.clear();
		fake.script.push_back(Const(200, R"({"id":"p","name":"pic.png"})"));
		DriveFile up = drive.upload("root", "pic.png", png, "", nullptr);
		check(up.id == "p" && fake.log.size() == 1 && fake.log[0].body.find("Content-Type: image/png") != std::string::npos
			&& fake.log[0].body.find("\"mimeType\":\"image/png\"") != std::string::npos, "multipart upload sends MIME type");
		fake.log.clear();
		fake.script.push_back(Const(500));
		threw = false;
		try { drive.upload("root", "pic.png", png, "", nullptr); } catch (const GdError&) { threw = true; }
		check(threw && fake.log.size() == 1, "multipart create is not auto-retried (no duplicates)");

		// resumable upload: 308 without Range means nothing was stored -> resend from 0
		std::wstring big = dir + L"\\big.bin";
		{
			HANDLE bh = CreateFileW(big.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
			LARGE_INTEGER sz; sz.QuadPart = 6ll * 1024 * 1024;
			SetFilePointerEx(bh, sz, nullptr, FILE_BEGIN); SetEndOfFile(bh); CloseHandle(bh);
		}
		fake.log.clear();
		fake.script.push_back(Const(200, "", { {"Location", "https://upload.test/session1"} }));
		fake.script.push_back(Const(308, ""));                                            // no Range header
		fake.script.push_back(Const(200, R"({"id":"big","name":"big.bin"})"));
		up = drive.upload("root", "big.bin", big, "", nullptr);
		check(up.id == "big" && fake.log.size() == 3 && fake.log[2].hasSlice && fake.log[2].sliceOff == 0
			&& starts_with(fake.log[2].header(L"Content-Range"), L"Content-Range: bytes 0-"), "308 without Range restarts the chunk from 0");
		check(starts_with(fake.log[0].header(L"X-Upload-Content-Type"), L"X-Upload-Content-Type: application/octet-stream"), "resumable start sends content type");

		// resumable upload: 308 with Range resumes after the stored bytes
		fake.log.clear();
		fake.script.push_back(Const(200, "", { {"Location", "https://upload.test/session2"} }));
		fake.script.push_back(Const(308, "", { {"Range", "bytes=0-1048575"} }));
		fake.script.push_back(Const(200, R"({"id":"big2"})"));
		up = drive.upload("root", "big.bin", big, "", nullptr);
		check(up.id == "big2" && fake.log[2].sliceOff == 1048576 && fake.log[2].sliceLen == 6ull * 1024 * 1024 - 1048576,
			"308 with Range resumes at the right offset");

		// resumable upload: 5xx on a chunk queries the resume position and continues
		fake.log.clear();
		fake.script.push_back(Const(200, "", { {"Location", "https://upload.test/session3"} }));
		fake.script.push_back(Const(503, ""));
		fake.script.push_back(Const(308, "", { {"Range", "bytes=0-262143"} }));   // answer to the "bytes */total" query
		fake.script.push_back(Const(200, R"({"id":"big3"})"));
		up = drive.upload("root", "big.bin", big, "", nullptr);
		check(up.id == "big3" && fake.log.size() == 4 && !fake.log[2].hasSlice && fake.log[3].sliceOff == 262144,
			"chunk failure resumes from the server's position");

		// resumable upload: the final chunk's reply is lost but the status query says "done" -> success, no re-upload
		fake.log.clear();
		fake.script.push_back(Const(200, "", { {"Location", "https://upload.test/session4"} }));
		fake.script.push_back([](const FakeHttp::Req&) -> HttpResponse { throw GdError("connection reset"); });
		fake.script.push_back(Const(200, R"({"id":"finished","name":"big.bin"})"));   // answer to "bytes */total"
		up = drive.upload("root", "big.bin", big, "", nullptr);
		check(up.id == "finished" && fake.log.size() == 3, "lost final reply + completed status query = success");

		// resumable upload: a server that never advances must not loop forever
		fake.log.clear();
		fake.script.push_back(Const(200, "", { {"Location", "https://upload.test/session5"} }));
		for (int i = 0; i < 10; ++i) fake.script.push_back(Const(308, ""));
		threw = false;
		try { drive.upload("root", "big.bin", big, "", nullptr); } catch (const GdError& e) { threw = std::string(e.what()).find("not progressing") != std::string::npos; }
		check(threw && fake.log.size() <= 7, "stalled 308 responses give up");
		fake.script.clear();

		DeleteFileW(big.c_str()); DeleteFileW(png.c_str()); DeleteFileW(local.c_str()); DeleteFileW((dir + L"\\expected.bin").c_str());
	}

	printf("\n%d failure(s)\n", g_fails);
	return g_fails ? 1 : 0;
}
