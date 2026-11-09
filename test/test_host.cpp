// test_host.cpp - loads GoogleDrive.dll like Far would and smoke-tests the HTTP layer.
#include "../src/util.h"
#include "../src/http.h"
#include "../src/oauth.h"
#include "../src/drive.h"
#include <plugin.hpp>
#include <cstdio>

static const wchar_t* WINAPI FakeGetMsg(const UUID*, intptr_t id) { static wchar_t b[32]; swprintf(b, 32, L"msg%d", (int)id); return b; }

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	int fails = 0;
	std::wstring dllArg = argc > 1 ? utf8_to_wide(argv[1]) : L"";
	auto check = [&](bool ok, const char* what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; };

	// --- 1. plugin DLL loads and exports what Far needs
	const wchar_t* dll = argc > 1 ? dllArg.c_str() : L"out\\GoogleDrive\\GoogleDrive.dll";
	HMODULE h = LoadLibraryW(dll);
	check(h != nullptr, "LoadLibrary GoogleDrive.dll");
	if (!h) { printf("  error %lu\n", GetLastError()); return 1; }
	const char* names[] = { "GetGlobalInfoW","SetStartupInfoW","GetPluginInfoW","ConfigureW","OpenW","ClosePanelW",
		"GetOpenPanelInfoW","GetFindDataW","FreeFindDataW","SetDirectoryW","GetFilesW","PutFilesW","DeleteFilesW",
		"MakeDirectoryW","ProcessPanelInputW","ExitFARW" };
	for (auto n : names) check(GetProcAddress(h, n) != nullptr, n);

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
	wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
	std::wstring profile = std::wstring(tmpDir) + L"gdrive_test_profile";
	SetEnvironmentVariableW(L"FARPROFILE", profile.c_str());
	SetStartupInfo(&psi);

	auto GetPluginInfo = (void(WINAPI*)(PluginInfo*))GetProcAddress(h, "GetPluginInfoW");
	PluginInfo pi{};
	GetPluginInfo(&pi);
	check(pi.StructSize == sizeof(PluginInfo) && pi.PluginMenu.Count == 1 && pi.CommandPrefix && wcscmp(pi.CommandPrefix, L"gdrive") == 0, "GetPluginInfoW");

	// --- 2. HTTP layer against Google endpoints (no credentials needed)
	try
	{
		HttpClient http;
		auto r = http.request(L"GET", L"https://www.googleapis.com/discovery/v1/apis?name=drive&preferred=true", {}, "");
		check(r.status == 200, "HTTPS GET discovery -> 200");
		json j = json::parse(r.body);
		check(j.contains("items"), "discovery JSON parsed");

		// token endpoint rejects bogus credentials with a JSON error
		Config cfg;
		cfg.client_id = "bogus"; cfg.client_secret = "bogus"; cfg.refresh_token = "bogus";
		cfg.file = profile + L"\\cfg.json";
		bool threw = false;
		try { oauth::ensure_token(cfg, http); }
		catch (const GdError& e) { threw = true; printf("  token error as expected: %s\n", e.what()); }
		check(threw, "refresh with bogus creds -> GdError");

		// file download sink + progress
		wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
		std::wstring path = std::wstring(tmp) + L"gdrive_test_download.json";
		HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		uint64_t seen = 0;
		auto r2 = http.request(L"GET", L"https://www.googleapis.com/discovery/v1/apis", {}, "", nullptr, f,
			[&](uint64_t d, uint64_t) { seen = d; return true; });
		LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz); CloseHandle(f);
		check(r2.status == 200 && sz.QuadPart > 0 && (uint64_t)sz.QuadPart == seen, "download to file with progress");
		DeleteFileW(path.c_str());

		// Drive API without a token -> clean error
		Drive drive(cfg, http);
		threw = false;
		try { drive.listChildren("root"); } catch (const GdError&) { threw = true; }
		check(threw, "Drive call without token -> GdError");
	}
	catch (const std::exception& e) { printf("FAIL exception: %s\n", e.what()); ++fails; }

	// --- 3. utilities
	FILETIME ft{};
	check(parse_rfc3339("2024-03-05T06:07:08.123Z", ft) && (ft.dwHighDateTime || ft.dwLowDateTime), "parse_rfc3339");
	check(filetime_to_rfc3339(ft) == "2024-03-05T06:07:08.123Z", "filetime round trip");
	check(sanitize_filename(L"a/b:c?.txt.") == L"a_b_c_.txt", "sanitize_filename");
	check(url_encode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9", "url_encode");
	check(q_escape("it's \\ ok") == "it\\'s \\\\ ok", "q_escape");
	check(oauth::sha256_b64url("abc") == "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0", "sha256 base64url");
	check(export_mime_for_ext("docx") != nullptr && export_mime_for_ext("zzz") == nullptr, "export table");

	printf("\n%d failure(s)\n", fails);
	return fails ? 1 : 0;
}
