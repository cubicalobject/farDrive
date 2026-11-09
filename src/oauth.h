// oauth.h - plugin configuration + Google OAuth 2.0 (loopback / PKCE) for installed apps
#pragma once
#include "util.h"
#include "http.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include "builtin_client.h"

using json = nlohmann::json;

static const char* const kScope = "https://www.googleapis.com/auth/drive";
static const char* const kAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
static const char* const kTokenEndpoint = "https://oauth2.googleapis.com/token";
static const char* const kRevokeEndpoint = "https://oauth2.googleapis.com/revoke";

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

	// A client compiled into the plugin (see builtin_client.h), if any.
	static bool has_builtin_client() { return BUILTIN_CLIENT_ID[0] != 0; }
	// Fields from the settings dialog win; otherwise fall back to the built-in client.
	std::string effective_client_id() const { return client_id.empty() ? BUILTIN_CLIENT_ID : client_id; }
	std::string effective_client_secret() const { return client_id.empty() ? BUILTIN_CLIENT_SECRET : client_secret; }
	bool has_client() const { return !effective_client_id().empty(); }

	void load()
	{
		std::ifstream f(file.c_str(), std::ios::binary);
		if (!f) return;
		try
		{
			json j = json::parse(f);
			client_id = j.value("client_id", "");
			client_secret = j.value("client_secret", "");
			refresh_token = j.value("refresh_token", "");
			access_token = j.value("access_token", "");
			expires_at = j.value("expires_at", (int64_t)0);
			account_email = j.value("account_email", "");
			trash_on_delete = j.value("trash_on_delete", true);
			add_to_disks_menu = j.value("add_to_disks_menu", true);
			export_doc = j.value("export_doc", "docx");
			export_sheet = j.value("export_sheet", "xlsx");
			export_slides = j.value("export_slides", "pptx");
			export_drawing = j.value("export_drawing", "png");
		}
		catch (...) {}
	}

	void save() const
	{
		json j{
			{"client_id", client_id}, {"client_secret", client_secret},
			{"refresh_token", refresh_token}, {"access_token", access_token},
			{"expires_at", expires_at}, {"account_email", account_email},
			{"trash_on_delete", trash_on_delete}, {"add_to_disks_menu", add_to_disks_menu},
			{"export_doc", export_doc}, {"export_sheet", export_sheet},
			{"export_slides", export_slides}, {"export_drawing", export_drawing},
		};
		// make sure the directory exists
		size_t slash = file.find_last_of(L'\\');
		if (slash != std::wstring::npos)
		{
			std::wstring dir = file.substr(0, slash);
			for (size_t p = dir.find(L'\\', 3); p != std::wstring::npos; p = dir.find(L'\\', p + 1))
				CreateDirectoryW(dir.substr(0, p).c_str(), nullptr);
			CreateDirectoryW(dir.c_str(), nullptr);
		}
		std::ofstream f(file.c_str(), std::ios::binary | std::ios::trunc);
		f << j.dump(2);
	}
};

namespace oauth
{
	inline std::string base64url(const unsigned char* data, size_t n)
	{
		static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
		std::string out;
		size_t i = 0;
		while (i + 2 < n)
		{
			uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
			out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
			out += tbl[(v >> 6) & 63];  out += tbl[v & 63];
			i += 3;
		}
		if (i + 1 == n)
		{
			uint32_t v = data[i] << 16;
			out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
		}
		else if (i + 2 == n)
		{
			uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
			out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63];
		}
		return out;
	}

	inline std::string random_verifier()
	{
		unsigned char buf[32];
		if (BCryptGenRandom(nullptr, buf, sizeof buf, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		{
			for (auto& b : buf) b = (unsigned char)rand();
		}
		return base64url(buf, sizeof buf);
	}

	inline std::string sha256_b64url(const std::string& s)
	{
		BCRYPT_ALG_HANDLE alg = nullptr;
		unsigned char hash[32] = {};
		if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
		{
			BCRYPT_HASH_HANDLE hh = nullptr;
			if (BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0)
			{
				BCryptHashData(hh, (PUCHAR)s.data(), (ULONG)s.size(), 0);
				BCryptFinishHash(hh, hash, sizeof hash, 0);
				BCryptDestroyHash(hh);
			}
			BCryptCloseAlgorithmProvider(alg, 0);
		}
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

	inline json post_form(HttpClient& http, const char* url,
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

	// Makes sure cfg.access_token is valid; refreshes it when needed.
	inline void ensure_token(Config& cfg, HttpClient& http)
	{
		if (!cfg.access_token.empty() && cfg.expires_at - 60 > unix_now())
			return;
		if (cfg.refresh_token.empty())
			throw GdError("Not signed in to Google Drive");
		json j = post_form(http, kTokenEndpoint, {
			{"client_id", cfg.effective_client_id()}, {"client_secret", cfg.effective_client_secret()},
			{"refresh_token", cfg.refresh_token}, {"grant_type", "refresh_token"} });
		cfg.access_token = j.value("access_token", "");
		cfg.expires_at = unix_now() + j.value("expires_in", 3600);
		cfg.save();
	}

	inline void force_refresh(Config& cfg, HttpClient& http)
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

	// Full interactive sign-in. `wait` is polled every 200 ms while we wait for the browser;
	// returning false from it aborts (GdCancel). Fills refresh/access token in cfg.
	inline void sign_in(Config& cfg, HttpClient& http, const std::function<bool()>& wait)
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
		if (listen(ls, 1) != 0) throw GdError("listen() failed");

		std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/";
		std::string verifier = random_verifier();
		std::string challenge = sha256_b64url(verifier);
		std::string state = random_verifier();

		std::string url = std::string(kAuthEndpoint) + "?" + form_encode({
			{"client_id", clientId}, {"redirect_uri", redirect}, {"response_type", "code"},
			{"scope", kScope}, {"access_type", "offline"}, {"prompt", "consent"},
			{"code_challenge", challenge}, {"code_challenge_method", "S256"}, {"state", state} });

		ShellExecuteW(nullptr, L"open", utf8_to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

		// wait for the browser to hit our loopback listener
		SOCKET cs = INVALID_SOCKET;
		for (int ticks = 0; ticks < 5 * 60 * 5; ++ticks)   // 5 minutes
		{
			fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
			timeval tv{ 0, 200000 };
			int r = select(0, &rd, nullptr, nullptr, &tv);
			if (r > 0) { cs = accept(ls, nullptr, nullptr); break; }
			if (wait && !wait()) throw GdCancel{};
		}
		if (cs == INVALID_SOCKET) throw GdError("Timed out waiting for the browser sign-in");
		SockGuard cg{ cs };

		std::string reqText;
		char buf[4096];
		DWORD tmo = 5000;
		setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
		while (reqText.find("\r\n\r\n") == std::string::npos && reqText.size() < 65536)
		{
			int n = recv(cs, buf, sizeof buf, 0);
			if (n <= 0) break;
			reqText.append(buf, n);
		}

		// GET /?state=..&code=..&scope=.. HTTP/1.1
		std::string code, err, gotState;
		{
			size_t q = reqText.find('?');
			size_t sp = reqText.find(' ', q == std::string::npos ? 0 : q);
			if (q != std::string::npos && sp != std::string::npos)
			{
				std::string qs = reqText.substr(q + 1, sp - q - 1);
				size_t p = 0;
				while (p <= qs.size())
				{
					size_t amp = qs.find('&', p);
					if (amp == std::string::npos) amp = qs.size();
					std::string kv = qs.substr(p, amp - p);
					size_t eq = kv.find('=');
					std::string k = kv.substr(0, eq);
					std::string v = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
					if (k == "code") code = v;
					else if (k == "error") err = v;
					else if (k == "state") gotState = v;
					p = amp + 1;
				}
			}
		}

		std::string html = (code.empty())
			? "<html><body style='font-family:sans-serif'><h2>Sign-in failed</h2><p>" + err + "</p></body></html>"
			: "<html><body style='font-family:sans-serif'><h2>Far Manager - Google Drive</h2>"
			  "<p>Signed in. You can close this window and return to Far Manager.</p></body></html>";
		std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
			+ std::to_string(html.size()) + "\r\n\r\n" + html;
		send(cs, resp.data(), (int)resp.size(), 0);
		shutdown(cs, SD_SEND);

		if (code.empty())
			throw GdError(err.empty() ? "No authorization code received" : "Authorization denied: " + err);
		if (gotState != state)
			throw GdError("OAuth state mismatch - sign-in rejected");

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

	inline void sign_out(Config& cfg, HttpClient& http)
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
