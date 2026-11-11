// oauth.h - plugin configuration + Google OAuth 2.0 (loopback / PKCE) for installed apps
#pragma once
#include "util.h"
#include "http.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <functional>
#include "builtin_client.h"

using json = nlohmann::json;

static const char* const kScope = "https://www.googleapis.com/auth/drive";
static const char* const kAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
static const char* const kTokenEndpoint = "https://oauth2.googleapis.com/token";
static const char* const kRevokeEndpoint = "https://oauth2.googleapis.com/revoke";

// Secrets in the config file are protected with DPAPI (current Windows user) and stored as "dpapi:<base64>".
namespace dpapi
{
	inline std::string protect(const std::string& plain)
	{
		if (plain.empty()) return {};
		DATA_BLOB in{ (DWORD)plain.size(), (BYTE*)plain.data() }, out{};
		if (!CryptProtectData(&in, L"FarGoogleDrive", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
			throw GdError("CryptProtectData failed: " + wide_to_utf8(win_error_text(GetLastError())));
		std::string s = "dpapi:" + base64_encode(out.pbData, out.cbData);
		LocalFree(out.pbData);
		return s;
	}

	// Returns the plaintext; legacy unprotected values pass through unchanged (legacy=true).
	inline std::string unprotect(const std::string& stored, bool* legacy = nullptr)
	{
		if (!starts_with(stored, "dpapi:"))
		{
			if (legacy && !stored.empty()) *legacy = true;
			return stored;
		}
		std::string raw = base64_decode(std::string_view(stored).substr(6));
		DATA_BLOB in{ (DWORD)raw.size(), (BYTE*)raw.data() }, out{};
		if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
			return {};
		std::string s((const char*)out.pbData, out.cbData);
		LocalFree(out.pbData);
		return s;
	}
}

struct Config
{
	std::string client_id;
	std::string client_secret;
	std::string refresh_token;
	std::string access_token;
	int64_t expires_at = 0;      // unix time when access_token expires
	std::string account_email;
	bool trash_on_delete = true;
	bool add_to_disks_menu = true;
	std::string export_doc = "docx";
	std::string export_sheet = "xlsx";
	std::string export_slides = "pptx";
	std::string export_drawing = "png";

	std::wstring file;           // where it is stored

	bool signed_in() const { return !refresh_token.empty(); }

	// Client shipped with the plugin: either read from GoogleDrive.client.json next to the DLL
	// (see load_client_file) or compiled in (see builtin_client.h). The file wins.
	std::string shipped_client_id = BUILTIN_CLIENT_ID;
	std::string shipped_client_secret = BUILTIN_CLIENT_SECRET;

	bool has_builtin_client() const { return !shipped_client_id.empty(); }
	// Fields from the settings dialog win; otherwise fall back to the shipped client.
	std::string effective_client_id() const { return client_id.empty() ? shipped_client_id : client_id; }
	std::string effective_client_secret() const { return client_id.empty() ? shipped_client_secret : client_secret; }
	bool has_client() const { return !effective_client_id().empty(); }

	// Reads {"client_id": "...", "client_secret": "..."} from `path` if it exists.
	// Returns true when a client id was found there.
	bool load_client_file(const std::wstring& path)
	{
		std::ifstream f(long_path(path).c_str(), std::ios::binary);
		if (!f) return false;
		try
		{
			json j = json::parse(f);
			std::string id = j.value("client_id", "");
			if (id.empty()) return false;
			shipped_client_id = id;
			shipped_client_secret = j.value("client_secret", "");
			return true;
		}
		catch (...) { return false; }
	}

	// Loads the file if present. A corrupt file is moved aside (config.json.bad) instead of being
	// silently treated as empty. Returns true when something was loaded.
	bool load()
	{
		json j;
		{
			std::ifstream f(long_path(file).c_str(), std::ios::binary);
			if (!f) return false;
			try { j = json::parse(f); }
			catch (...)
			{
				f.close();
				MoveFileExW(long_path(file).c_str(), long_path(file + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
				return false;
			}
		}   // the stream must be closed before save() replaces the file below
		bool legacy = false;
		client_id = j.value("client_id", "");
		client_secret = dpapi::unprotect(j.value("client_secret", ""), &legacy);
		refresh_token = dpapi::unprotect(j.value("refresh_token", ""), &legacy);
		access_token = dpapi::unprotect(j.value("access_token", ""), &legacy);
		expires_at = j.value("expires_at", (int64_t)0);
		account_email = j.value("account_email", "");
		trash_on_delete = j.value("trash_on_delete", true);
		add_to_disks_menu = j.value("add_to_disks_menu", true);
		export_doc = j.value("export_doc", "docx");
		export_sheet = j.value("export_sheet", "xlsx");
		export_slides = j.value("export_slides", "pptx");
		export_drawing = j.value("export_drawing", "png");
		if (legacy)
		{
			// re-save so plaintext secrets written by an older build get protected
			try { save(); } catch (...) {}
		}
		return true;
	}

	// Atomic save (write temp file, then replace). Throws GdError on failure.
	void save() const
	{
		json j{
			{"client_id", client_id}, {"client_secret", dpapi::protect(client_secret)},
			{"refresh_token", dpapi::protect(refresh_token)}, {"access_token", dpapi::protect(access_token)},
			{"expires_at", expires_at}, {"account_email", account_email},
			{"trash_on_delete", trash_on_delete}, {"add_to_disks_menu", add_to_disks_menu},
			{"export_doc", export_doc}, {"export_sheet", export_sheet},
			{"export_slides", export_slides}, {"export_drawing", export_drawing},
		};
		size_t slash = file.find_last_of(L'\\');
		if (slash != std::wstring::npos)
		{
			std::wstring dir = file.substr(0, slash);
			for (size_t p = dir.find(L'\\', 3); p != std::wstring::npos; p = dir.find(L'\\', p + 1))
				CreateDirectoryW(long_path(dir.substr(0, p)).c_str(), nullptr);
			CreateDirectoryW(long_path(dir).c_str(), nullptr);
		}
		std::wstring tmp = file + L".tmp";
		{
			std::ofstream f(long_path(tmp).c_str(), std::ios::binary | std::ios::trunc);
			f << j.dump(2);
			f.flush();
			if (!f.good())
				throw GdError("Cannot write settings file " + wide_to_utf8(tmp));
		}
		if (!MoveFileExW(long_path(tmp).c_str(), long_path(file).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DWORD err = GetLastError();
			DeleteFileW(long_path(tmp).c_str());
			throw GdError("Cannot replace settings file " + wide_to_utf8(file) + ": " + wide_to_utf8(win_error_text(err)));
		}
	}
};

namespace oauth
{
	inline std::string base64url(const unsigned char* data, size_t n)
	{
		std::string s = base64_encode(data, n);
		for (auto& c : s) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
		while (!s.empty() && s.back() == '=') s.pop_back();
		return s;
	}

	inline std::string random_verifier()
	{
		unsigned char buf[32];
		if (BCryptGenRandom(nullptr, buf, sizeof buf, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
			throw GdError("BCryptGenRandom failed - cannot create a secure PKCE verifier");
		return base64url(buf, sizeof buf);
	}

	inline std::string sha256_b64url(const std::string& s)
	{
		BCRYPT_ALG_HANDLE alg = nullptr;
		unsigned char hash[32] = {};
		bool ok = false;
		if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
		{
			BCRYPT_HASH_HANDLE hh = nullptr;
			if (BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0)
			{
				ok = BCryptHashData(hh, (PUCHAR)s.data(), (ULONG)s.size(), 0) == 0
					&& BCryptFinishHash(hh, hash, sizeof hash, 0) == 0;
				BCryptDestroyHash(hh);
			}
			BCryptCloseAlgorithmProvider(alg, 0);
		}
		if (!ok) throw GdError("SHA-256 failed");
		return base64url(hash, sizeof hash);
	}

	inline std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv)
	{
		std::string out;
		for (auto& [k, v] : kv)
		{
			if (!out.empty()) out += '&';
			out += url_encode(k) + "=" + url_encode(v);
		}
		return out;
	}

	inline json post_form(IHttp& http, const char* url,
		const std::vector<std::pair<std::string, std::string>>& kv)
	{
		auto resp = http.request(L"POST", utf8_to_wide(url),
			{ L"Content-Type: application/x-www-form-urlencoded" }, form_encode(kv));
		json j;
		try { j = json::parse(resp.body); } catch (...) { j = json::object(); }
		if (!resp.ok())
		{
			std::string msg = j.value("error", "") + ": " + j.value("error_description", "");
			if (msg == ": ") msg = resp.body.substr(0, 300);
			throw GdError("Token endpoint error (" + std::to_string(resp.status) + ") " + msg, (int)resp.status);
		}
		return j;
	}

	// Makes sure cfg.access_token is valid; refreshes it when needed. When Google reports that
	// the stored refresh token is no longer usable (revoked, expired in testing mode, or the
	// OAuth client changed), the tokens are cleared and GdAuthError is thrown so the UI can
	// offer a fresh sign-in instead of repeating a raw error forever.
	inline void ensure_token(Config& cfg, IHttp& http)
	{
		if (!cfg.access_token.empty() && cfg.expires_at - 60 > unix_now())
			return;
		if (cfg.refresh_token.empty())
			throw GdAuthError("Not signed in to Google Drive");
		auto resp = http.request(L"POST", utf8_to_wide(kTokenEndpoint),
			{ L"Content-Type: application/x-www-form-urlencoded" }, form_encode({
				{"client_id", cfg.effective_client_id()}, {"client_secret", cfg.effective_client_secret()},
				{"refresh_token", cfg.refresh_token}, {"grant_type", "refresh_token"} }));
		json j;
		try { j = json::parse(resp.body); } catch (...) { j = json::object(); }
		if (!resp.ok())
		{
			std::string code = j.is_object() ? j.value("error", "") : "";
			std::string desc = j.is_object() ? j.value("error_description", "") : "";
			if (code == "invalid_grant" || code == "invalid_client" || code == "unauthorized_client")
			{
				cfg.refresh_token.clear();
				cfg.access_token.clear();
				cfg.expires_at = 0;
				try { cfg.save(); } catch (...) {}
				throw GdAuthError("Your Google sign-in is no longer valid (" + code + (desc.empty() ? "" : ": " + desc)
					+ "). Please sign in again.", (int)resp.status);
			}
			std::string msg = code + ": " + desc;
			if (msg == ": ") msg = resp.body.substr(0, 300);
			throw GdError("Token endpoint error (" + std::to_string(resp.status) + ") " + msg, (int)resp.status);
		}
		cfg.access_token = j.value("access_token", "");
		cfg.expires_at = unix_now() + j.value("expires_in", 3600);
		try { cfg.save(); } catch (...) {}   // a failed save must not break the current operation
	}

	inline void force_refresh(Config& cfg, IHttp& http)
	{
		cfg.access_token.clear();
		ensure_token(cfg, http);
	}

	struct WsaInit
	{
		bool ok;
		WsaInit() { WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
		~WsaInit() { if (ok) WSACleanup(); }
	};

	// Splits the query string of an HTTP request line ("GET /?a=b&c=d HTTP/1.1").
	// Returns false when the line is not a GET of the callback path.
	inline bool parse_callback_line(const std::string& line, std::vector<std::pair<std::string, std::string>>& params)
	{
		if (!starts_with(line, "GET /")) return false;
		size_t sp = line.find(' ', 4);
		std::string target = line.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
		size_t q = target.find('?');
		if (q == std::string::npos || target.compare(0, q, "/") != 0) return false;
		std::string qs = target.substr(q + 1);
		size_t p = 0;
		while (p <= qs.size())
		{
			size_t amp = qs.find('&', p);
			if (amp == std::string::npos) amp = qs.size();
			std::string kv = qs.substr(p, amp - p);
			if (!kv.empty())
			{
				size_t eq = kv.find('=');
				params.emplace_back(url_decode(kv.substr(0, eq)), eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1)));
			}
			p = amp + 1;
		}
		return true;
	}

	inline void send_http(SOCKET s, const char* status, const std::string& html)
	{
		std::string resp = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: text/html; charset=utf-8\r\n"
			"Connection: close\r\nContent-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
		send(s, resp.data(), (int)resp.size(), 0);
		shutdown(s, SD_SEND);
	}

	inline std::string page(const std::string& heading, const std::string& text)
	{
		return "<html><body style='font-family:sans-serif'><h2>" + html_escape(heading) + "</h2><p>" + html_escape(text) + "</p></body></html>";
	}

	// Full interactive sign-in. `wait` is polled every 200 ms while we wait for the browser;
	// returning false from it aborts (GdCancel). Fills refresh/access token in cfg.
	inline void sign_in(Config& cfg, IHttp& http, const std::function<bool()>& wait)
	{
		if (!cfg.has_client())
			throw GdError("Client ID is empty. Create an OAuth 'Desktop app' client in Google Cloud Console first.");
		const std::string clientId = cfg.effective_client_id();
		const std::string clientSecret = cfg.effective_client_secret();

		WsaInit wsa;
		if (!wsa.ok) throw GdError("WSAStartup failed");

		SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (ls == INVALID_SOCKET) throw GdError("socket() failed");
		struct SockGuard { SOCKET s; ~SockGuard() { if (s != INVALID_SOCKET) closesocket(s); } } lg{ ls };

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(ls, (sockaddr*)&addr, sizeof addr) != 0) throw GdError("bind() failed");
		int alen = sizeof addr;
		getsockname(ls, (sockaddr*)&addr, &alen);
		int port = ntohs(addr.sin_port);
		if (listen(ls, 4) != 0) throw GdError("listen() failed");

		std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/";
		std::string verifier = random_verifier();
		std::string challenge = sha256_b64url(verifier);
		std::string state = random_verifier();

		std::string url = std::string(kAuthEndpoint) + "?" + form_encode({
			{"client_id", clientId}, {"redirect_uri", redirect}, {"response_type", "code"},
			{"scope", kScope}, {"access_type", "offline"}, {"prompt", "consent"},
			{"code_challenge", challenge}, {"code_challenge_method", "S256"}, {"state", state} });

		ShellExecuteW(nullptr, L"open", utf8_to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

		// Wait for the browser to hit the loopback listener. Connections that are not the OAuth
		// callback (favicon requests, port probes) get a 404 and we keep waiting.
		std::string code, err, gotState;
		bool got = false;
		ULONGLONG deadline = GetTickCount64() + 5ull * 60 * 1000;
		while (!got && GetTickCount64() < deadline)
		{
			fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
			timeval tv{ 0, 200000 };
			int r = select(0, &rd, nullptr, nullptr, &tv);
			if (r <= 0)
			{
				if (wait && !wait()) throw GdCancel{};
				continue;
			}
			SOCKET cs = accept(ls, nullptr, nullptr);
			if (cs == INVALID_SOCKET) continue;
			SockGuard cg{ cs };

			std::string reqText;
			char buf[4096];
			DWORD tmo = 3000;
			setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
			while (reqText.find("\r\n\r\n") == std::string::npos && reqText.size() < 65536)
			{
				int n = recv(cs, buf, sizeof buf, 0);
				if (n <= 0) break;
				reqText.append(buf, n);
			}
			std::string line = reqText.substr(0, reqText.find("\r\n"));
			std::vector<std::pair<std::string, std::string>> params;
			if (!parse_callback_line(line, params))
			{
				send_http(cs, "404 Not Found", page("Not found", ""));
				continue;
			}
			for (auto& [k, v] : params)
			{
				if (k == "code") code = v;
				else if (k == "error") err = v;
				else if (k == "state") gotState = v;
			}
			if (code.empty() && err.empty())
			{
				send_http(cs, "404 Not Found", page("Not found", ""));
				continue;
			}
			got = true;
			if (gotState != state)
			{
				send_http(cs, "400 Bad Request", page("Sign-in rejected", "State mismatch."));
				throw GdError("OAuth state mismatch - sign-in rejected");
			}
			if (code.empty())
			{
				send_http(cs, "200 OK", page("Sign-in failed", err));
				throw GdError("Authorization denied: " + err);
			}
			send_http(cs, "200 OK", page("Far Manager - Google Drive", "Signed in. You can close this window and return to Far Manager."));
		}
		if (!got) throw GdError("Timed out waiting for the browser sign-in");

		json j = post_form(http, kTokenEndpoint, {
			{"code", code}, {"client_id", clientId}, {"client_secret", clientSecret},
			{"redirect_uri", redirect}, {"grant_type", "authorization_code"}, {"code_verifier", verifier} });

		cfg.access_token = j.value("access_token", "");
		cfg.expires_at = unix_now() + j.value("expires_in", 3600);
		std::string rt = j.value("refresh_token", "");
		if (rt.empty() && cfg.refresh_token.empty())
			throw GdError("Google did not return a refresh token. Remove the app's access at myaccount.google.com/permissions and sign in again.");
		if (!rt.empty()) cfg.refresh_token = rt;
		cfg.save();
	}

	inline void sign_out(Config& cfg, IHttp& http)
	{
		std::string tok = cfg.refresh_token.empty() ? cfg.access_token : cfg.refresh_token;
		if (!tok.empty())
		{
			try
			{
				http.request(L"POST", utf8_to_wide(kRevokeEndpoint),
					{ L"Content-Type: application/x-www-form-urlencoded" }, "token=" + url_encode(tok));
			}
			catch (...) {}
		}
		cfg.refresh_token.clear();
		cfg.access_token.clear();
		cfg.expires_at = 0;
		cfg.account_email.clear();
		cfg.save();
	}
}
