// GoogleDrive.cpp - Far Manager 3 plugin: browse Google Drive as a panel.
//   F5/F6 copy/move Drive <-> local, F7 new folder, F8 trash/delete, Shift+F6 rename,
//   Alt+F7 search, Ctrl+Shift+0..9 folder shortcuts, "gdrive:Folder\Sub" command prefix.
#include "util.h"
#include "http.h"
#include "oauth.h"
#include "drive.h"
#include "version.h"

#include <plugin.hpp>
#include <DlgBuilder.hpp>

#include <memory>
#include <map>
#include <set>
#include <algorithm>
#include <cwchar>

// ---------------------------------------------------------------------------
// identifiers
// ---------------------------------------------------------------------------
static const GUID PluginGuid    = { 0xa1c0f6d2, 0x3b7e, 0x4f1a, { 0x9c, 0x58, 0x2d, 0x6e, 0x8f, 0x0b, 0x4a, 0x17 } };
static const GUID MenuGuid      = { 0xb2d1e7f3, 0x4c8f, 0x4a2b, { 0x8d, 0x69, 0x3e, 0x7f, 0x9a, 0x1c, 0x5b, 0x28 } };
static const GUID ConfigGuid    = { 0xc3e2f8a4, 0x5d9a, 0x4b3c, { 0x9e, 0x7a, 0x4f, 0x8a, 0x0b, 0x2d, 0x6c, 0x39 } };
static const GUID MsgGuid       = { 0xd4f3a9b5, 0x6eab, 0x4c4d, { 0xaf, 0x8b, 0x5a, 0x9b, 0x1c, 0x3e, 0x7d, 0x4a } };
static const GUID ProgressGuid  = { 0xe5a4bac6, 0x7fbc, 0x4d5e, { 0xb0, 0x9c, 0x6b, 0xac, 0x2d, 0x4f, 0x8e, 0x5b } };
static const GUID MkdirGuid     = { 0xf6b5cbd7, 0x80cd, 0x4e6f, { 0x81, 0xad, 0x7c, 0xbd, 0x3e, 0x5a, 0x9f, 0x6c } };
static const GUID RenameGuid    = { 0x07c6dce8, 0x91de, 0x4f70, { 0x92, 0xbe, 0x8d, 0xce, 0x4f, 0x6b, 0x0a, 0x7d } };
static const GUID OverwriteGuid = { 0x18d7edf9, 0xa2ef, 0x4081, { 0xa3, 0xcf, 0x9e, 0xdf, 0x50, 0x7c, 0x1b, 0x8e } };
static const GUID DeleteGuid    = { 0x29e8fe0a, 0xb3f0, 0x4192, { 0xb4, 0xd0, 0xaf, 0xe0, 0x61, 0x8d, 0x2c, 0x9f } };
static const GUID CopyGuid      = { 0x3af90f1b, 0xc401, 0x42a3, { 0x85, 0xe1, 0xb0, 0xf1, 0x72, 0xae, 0x3d, 0xa0 } };
static const GUID ItemErrorGuid = { 0x4b0a102c, 0xd512, 0x43b4, { 0x96, 0xf2, 0xc1, 0x02, 0x83, 0xbf, 0x4e, 0xb1 } };

// Message IDs - keep in sync with GoogleDrive*.lng
enum
{
	MTitle,
	MConfigTitle,
	MClientId,
	MClientSecret,
	MTrashOnDelete,
	MAddToDisks,
	MExportDoc,
	MExportSheet,
	MExportSlides,
	MSignIn,
	MSignOut,
	MOk,
	MCancel,
	MStatusNotSignedIn,
	MStatusSignedIn,
	MSignInHelp1,
	MSignInHelp2,
	MSignInWaiting,
	MSignInEsc,
	MSignInDone,
	MSignedOut,
	MError,
	MWarning,
	MReading,
	MDownloading,
	MUploading,
	MDeleting,
	MDeleteTitle,
	MTrashQuestion,
	MDeleteQuestion,
	MItems,
	MDelete,
	MOverwriteTitle,
	MOverwriteQ,
	MOverwrite,
	MOverwriteAll,
	MSkip,
	MSkipAll,
	MMkdirTitle,
	MMkdirPrompt,
	MRenameTitle,
	MRenamePrompt,
	MSharedWithMe,
	MTrashFolder,
	MNotAllowedHere,
	MInfoAccount,
	MInfoUsed,
	MInfoLimit,
	MInfoFolder,
	MNeedSignIn,
	MNotExportable,
	MSkipped,
	MSharedDrives,
	MBuiltinHint,
	MStatusBuiltin,
	MSignInNow,
	MFolderLoop,
	MNotMovedSkipped,
	MExportedNotMoved,
	MSaveDocTitle,
	MSaveDocWarn1,
	MSaveDocWarn2,
	MReplace,
	MSaveNotSupported,
	MAuthExpired,
	MCopyTitle,
	MMoveTitle,
	MCopyPrompt,
	MMovePrompt,
	MMovePromptPerm,
	MItemsSelected,
	MRestoreTitle,
	MRestoreQuestion,
	MRestore,
	MItemError,
	MRetry,
	MSkipAllErrors,
	MCancelOpQuestion,
	MFolderExists,
	MBadExportExt,
	MExportDrawing,
	MSignInHint,
	MSignInUrlCopied,
	MGoogleFileViewOnly,
	MStarred,
	MDriveCopyQ,
	MDriveMoveQ,
	MCopyingOnDrive,
	MMovingOnDrive,
	MMoveIntoItself,
	MDriveMoveHint,
};

// ---------------------------------------------------------------------------
// globals
// ---------------------------------------------------------------------------
static PluginStartupInfo PSI{};
static FarStandardFunctions FSF{};
static std::wstring g_pluginDir;
static Config g_cfg;
static std::unique_ptr<HttpClient> g_http;
static std::unique_ptr<Drive> g_drive;

static const char* const kSharedId = "@shared";
static const char* const kTrashId = "@trash";
static const char* const kDrivesId = "@drives";
static const char* const kStarredId = "@starred";

static const wchar_t* M(int id) { return PSI.GetMsg(&PluginGuid, id); }

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
static intptr_t Message(FARMESSAGEFLAGS flags, const GUID& id, const std::vector<std::wstring>& lines,
	const std::vector<std::wstring>& buttons = {})
{
	std::vector<const wchar_t*> items;
	for (auto& l : lines) items.push_back(l.c_str());
	for (auto& b : buttons) items.push_back(b.c_str());
	return PSI.Message(&PluginGuid, &id, flags, nullptr, items.data(), items.size(), (intptr_t)buttons.size());
}

static std::vector<std::wstring> WrapText(const std::wstring& text, size_t width = 70, size_t maxLines = 8)
{
	std::vector<std::wstring> out;
	std::wstring line;
	size_t pos = 0;
	while (pos < text.size() && out.size() < maxLines)
	{
		size_t sp = text.find_first_of(L" \n", pos);
		if (sp == std::wstring::npos) sp = text.size();
		std::wstring word = text.substr(pos, sp - pos);
		bool newline = sp < text.size() && text[sp] == L'\n';
		if (!line.empty() && line.size() + 1 + word.size() > width) { out.push_back(line); line.clear(); }
		while (word.size() > width) { out.push_back(word.substr(0, width)); word.erase(0, width); }
		if (!line.empty()) line += L' ';
		line += word;
		if (newline) { out.push_back(line); line.clear(); }
		pos = sp + 1;
	}
	if (!line.empty() && out.size() < maxLines) out.push_back(line);
	return out;
}

static void ShowError(const std::wstring& text)
{
	std::vector<std::wstring> lines{ M(MError) };
	for (auto& l : WrapText(text)) lines.push_back(l);
	Message(FMSG_WARNING | FMSG_MB_OK, MsgGuid, lines);
}
static void ShowError(const std::string& utf8) { ShowError(utf8_to_wide(utf8)); }

static void ShowInfo(const std::wstring& title, const std::wstring& text)
{
	std::vector<std::wstring> lines{ title };
	for (auto& l : WrapText(text)) lines.push_back(l);
	Message(FMSG_MB_OK, MsgGuid, lines);
}

// Drains console input and reports whether Esc was pressed. Draining (rather than peeking) is
// deliberate: keys typed during a long transfer must not be replayed into Far afterwards.
static bool CheckForEsc()
{
	HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
	INPUT_RECORD rec;
	DWORD n = 0;
	bool esc = false;
	while (PeekConsoleInputW(h, &rec, 1, &n) && n)
	{
		ReadConsoleInputW(h, &rec, 1, &n);
		if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown && rec.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE)
			esc = true;
	}
	return esc;
}

static std::wstring FitWidth(std::wstring s, size_t w)
{
	if (s.size() > w)
	{
		size_t keep = w - 3;
		s = s.substr(0, keep / 2) + L"..." + s.substr(s.size() - (keep - keep / 2));
	}
	if (s.size() < w) s.append(w - s.size(), L' ');
	return s;
}

// Progress box + taskbar progress. update() returns false when the user pressed Esc.
class Progress
{
public:
	Progress(const wchar_t* title, bool visible) : m_title(title), m_visible(visible)
	{
		if (m_visible) m_screen = PSI.SaveScreen(0, 0, -1, -1);
		PSI.AdvControl(&PluginGuid, ACTL_SETPROGRESSSTATE, TBPS_INDETERMINATE, nullptr);
	}
	~Progress()
	{
		PSI.AdvControl(&PluginGuid, ACTL_SETPROGRESSSTATE, TBPS_NOPROGRESS, nullptr);
		PSI.AdvControl(&PluginGuid, ACTL_PROGRESSNOTIFY, 0, nullptr);
		if (m_screen) PSI.RestoreScreen(m_screen);
	}
	void setTitle(const wchar_t* t) { m_title = t; }

	bool update(const std::wstring& line1, const std::wstring& line2, uint64_t done, uint64_t total, bool force = false)
	{
		if (CheckForEsc())
		{
			// like Far's own copy dialog: confirm before throwing away a running transfer
			if (!m_visible) return false;
			if (Message(FMSG_WARNING | FMSG_MB_YESNO, MsgGuid, { m_title, M(MCancelOpQuestion) }) == 0) return false;
			force = true;
		}
		ULONGLONG now = GetTickCount64();
		if (!force && now - m_last < 150) return true;
		m_last = now;
		if (total)
		{
			PSI.AdvControl(&PluginGuid, ACTL_SETPROGRESSSTATE, TBPS_NORMAL, nullptr);
			ProgressValue pv{ sizeof pv, done, total };
			PSI.AdvControl(&PluginGuid, ACTL_SETPROGRESSVALUE, 0, &pv);
		}
		if (!m_visible) return true;

		const size_t W = 60;
		std::wstring bar;
		if (total)
		{
			size_t filled = (size_t)std::min<uint64_t>(W, done * W / total);
			bar = std::wstring(filled, L'\x2588') + std::wstring(W - filled, L'\x2591');
		}
		else
			bar = std::wstring(W, L'\x2591');
		std::wstring bytes = format_size(done);
		if (total) bytes += L" / " + format_size(total) + L"  (" + std::to_wstring(done * 100 / total) + L"%)";

		std::vector<std::wstring> lines{ m_title, FitWidth(line1, W), FitWidth(line2, W), bar, FitWidth(bytes, W) };
		Message(FMSG_LEFTALIGN, ProgressGuid, lines);
		return true;
	}

private:
	std::wstring m_title;
	bool m_visible;
	HANDLE m_screen = nullptr;
	ULONGLONG m_last = 0;
};

static void ThrowIfCancelled(bool ok) { if (!ok) throw GdCancel{}; }

// Creates the HTTP client on demand (its constructor can fail); reports the error once asked.
static bool EnsureHttp()
{
	if (g_http) return true;
	try
	{
		g_http = std::make_unique<HttpClient>();
		g_drive = std::make_unique<Drive>(g_cfg, *g_http);
		return true;
	}
	catch (const std::exception& e)
	{
		ShowError(std::string("WinHTTP could not be initialised: ") + e.what());
		return false;
	}
}

static void SaveConfigReporting()
{
	try { g_cfg.save(); }
	catch (const std::exception& e) { ShowError(e.what()); }
}

// ---------------------------------------------------------------------------
// panel
// ---------------------------------------------------------------------------
struct Entry : DriveFile
{
	std::wstring display;   // name shown in the panel (unique within the folder, file-system safe)
	bool pseudo = false;    // "Shared with me" / "Shared drives" / "Trash"
};

struct Level
{
	std::string id;
	std::wstring name;
};

class Panel
{
public:
	Panel() { m_path.push_back({ "root", L"" }); updateStrings(); }

	const std::string& curId() const { return m_path.back().id; }
	const std::wstring& curDir() const { return m_curDir; }
	bool isRoot() const { return m_path.size() == 1; }
	bool inTrash() const { for (auto& l : m_path) if (l.id == kTrashId) return true; return false; }
	bool isPseudoRoot() const { return curId() == kSharedId || curId() == kTrashId || curId() == kDrivesId || curId() == kStarredId; }
	// writes are not allowed in Trash or directly in the "Shared with me" / "Shared drives" / "Starred" lists
	bool canWrite() const { return !inTrash() && !isPseudoRoot(); }

	void invalidate() { m_entriesFor = "\x01"; m_fresh = false; }

	// Listing for lookups (F5/F8/chdir): reuse what the panel currently shows.
	void ensureEntries()
	{
		if (m_entriesFor == curId()) return;
		m_entries = fetch(curId(), inTrash());
		m_entriesFor = curId();
		m_fresh = true;
	}

	// Listing for display (GetFindData): re-read unless the cache was filled since the last
	// display, so Ctrl+R and Far's automatic refresh pick up changes made elsewhere.
	// refreshAbout=false for Far's search thread (no extra network call / config save there).
	void refreshForDisplay(bool refreshAbout)
	{
		if (m_entriesFor == curId() && m_fresh) { m_fresh = false; return; }
		m_entries = fetch(curId(), inTrash());
		m_entriesFor = curId();
		m_fresh = false;
		if (refreshAbout && GetTickCount64() - m_aboutTick > 30000) loadAbout();   // keep quota / free space current
	}

	// Lists one folder. includeTrashed must be true for folders that live below the Trash
	// (everything there is flagged trashed); the caller knows that from the path it walked.
	std::vector<Entry> fetch(const std::string& id, bool includeTrashed)
	{
		std::vector<Entry> out;
		std::vector<DriveFile> files;
		bool drivesList = id == kDrivesId;
		if (id == kSharedId) files = g_drive->listSharedWithMe();
		else if (id == kStarredId) files = g_drive->listStarred();
		else if (id == kTrashId) files = g_drive->listTrash();
		else if (drivesList) files = g_drive->listSharedDrives();
		else
		{
			files = g_drive->listChildren(id, includeTrashed);
			if (id == "root")
			{
				auto pseudo = [&](const char* pid, int msg) {
					Entry e; e.id = pid; e.mimeType = kMimeFolder; e.display = M(msg); e.pseudo = true; out.push_back(e);
				};
				pseudo(kStarredId, MStarred);
				pseudo(kSharedId, MSharedWithMe);
				pseudo(kDrivesId, MSharedDrives);
				pseudo(kTrashId, MTrashFolder);
			}
		}
		// Panel names double as local file names (F3/F4 temp files), so they are sanitised, and
		// they must be unique because Far hands items back to us by name.
		std::vector<std::pair<std::wstring, bool>> names;
		for (auto& e : out) names.emplace_back(e.display, true);
		for (auto& f : files) names.emplace_back(sanitize_filename(utf8_to_wide(f.name)), f.isFolder());
		std::vector<std::wstring> uniq = unique_display_names(names);
		size_t i = 0;
		for (auto& e : out) e.display = uniq[i++];
		for (auto& f : files)
		{
			Entry e;
			static_cast<DriveFile&>(e) = f;
			e.display = uniq[i++];
			e.pseudo = drivesList;   // shared drives are containers, not files: no delete/rename
			out.push_back(std::move(e));
		}
		return out;
	}

	const Entry* find(const wchar_t* display)
	{
		ensureEntries();
		for (auto& e : m_entries)
			if (_wcsicmp(e.display.c_str(), display) == 0) return &e;
		return nullptr;
	}

	// Change directory: "..", "name", "a\b\c", "\" (root) or "\a\b". On failure the previous
	// location is kept, so the listing and the title never disagree.
	bool chdir(const std::wstring& dir)
	{
		std::wstring d = dir;
		if (d.empty() || d == L"\\" || d == L"/") { toRoot(); return true; }
		std::vector<Level> saved = m_path;
		if (d[0] == L'\\' || d[0] == L'/')
		{
			// Far re-sends our own CurDir after opening a folder shortcut: already there
			if (_wcsicmp(d.c_str(), m_curDir.c_str()) == 0) return true;
			toRoot();
			d.erase(0, 1);
		}
		bool ok = true;
		size_t pos = 0;
		while (ok && pos <= d.size())
		{
			size_t sep = d.find_first_of(L"\\/", pos);
			if (sep == std::wstring::npos) sep = d.size();
			std::wstring comp = d.substr(pos, sep - pos);
			pos = sep + 1;
			if (comp.empty() || comp == L".") continue;
			if (comp == L"..")
			{
				if (m_path.size() > 1) { m_path.pop_back(); invalidate(); }
				continue;
			}
			const Entry* e = find(comp.c_str());
			if (!e || !e->isFolder()) { ok = false; break; }
			m_path.push_back({ e->pseudo ? e->id : e->effectiveId(), e->display });
			invalidate();
		}
		if (!ok) { m_path = saved; invalidate(); }
		updateStrings();
		return ok;
	}

	void toRoot() { m_path.resize(1); invalidate(); updateStrings(); }

	void loadAbout()
	{
		m_aboutTick = GetTickCount64();
		try { m_about = g_drive->about(); m_aboutLoaded = true; }
		catch (...) { m_aboutLoaded = false; }
		if (m_aboutLoaded && !m_about.email.empty() && g_cfg.account_email != m_about.email)
		{
			g_cfg.account_email = m_about.email;
			try { g_cfg.save(); } catch (...) {}
		}
		updateStrings();
	}

	// --- shortcut (Ctrl+Shift+0..9) support -----------------------------------
	std::wstring serialize() const
	{
		std::wstring s;
		for (size_t i = 1; i < m_path.size(); ++i)
		{
			if (i > 1) s += L'\x02';
			s += utf8_to_wide(m_path[i].id) + L'\x01' + m_path[i].name;
		}
		return s;
	}
	void restore(const std::wstring& s)
	{
		m_path.resize(1);
		size_t pos = 0;
		while (pos < s.size())
		{
			size_t end = s.find(L'\x02', pos);
			if (end == std::wstring::npos) end = s.size();
			std::wstring part = s.substr(pos, end - pos);
			size_t sep = part.find(L'\x01');
			if (sep != std::wstring::npos)
				m_path.push_back({ wide_to_utf8(part.substr(0, sep)), part.substr(sep + 1) });
			pos = end + 1;
		}
		invalidate();
		updateStrings();
	}

	void fillOpenPanelInfo(OpenPanelInfo* info)
	{
		info->Flags = OPIF_ADDDOTS | OPIF_SHOWPRESERVECASE | OPIF_SHORTCUT;
		if (m_aboutLoaded && m_about.limit) info->Flags |= OPIF_USEFREESIZE;
		info->HostFile = nullptr;
		info->CurDir = m_curDir.c_str();
		info->Format = L"Google Drive";
		info->PanelTitle = m_title.c_str();
		info->InfoLines = m_info.data();
		info->InfoLinesNumber = m_info.size();
		info->ShortcutData = m_shortcut.c_str();
		info->FreeSize = (m_aboutLoaded && m_about.limit > m_about.usage) ? m_about.limit - m_about.usage : 0;
	}

	std::vector<Entry> m_entries;
	// Ids of entries the last silent (plugin-to-plugin) GetFiles did not copy completely.
	// A following silent DeleteFiles must leave them alone.
	std::set<std::string> m_notMovable;

	// Files handed to Far's viewer/editor (F3/F4), keyed by the lower-case temp path. A later
	// save-back (PutFiles with OPM_EDIT) must update exactly that file, even if the panel has
	// moved to another folder in the meantime.
	struct EditTarget
	{
		std::string fileId;
		std::string name;
		std::string mime;        // effective MIME type of the Drive file
		bool googleDoc = false;
	};
	std::map<std::wstring, EditTarget> m_editTargets;

private:
	void updateStrings()
	{
		// absolute, so that Far's SetDirectory(CurDir) after a folder shortcut is not taken as relative
		m_curDir = L"\\";
		for (size_t i = 1; i < m_path.size(); ++i)
		{
			if (i > 1) m_curDir += L'\\';
			m_curDir += m_path[i].name;
		}
		m_title = L" Google Drive:" + m_curDir + L" ";
		m_shortcut = serialize();

		m_infoAccount = utf8_to_wide(m_about.email.empty() ? g_cfg.account_email : m_about.email);
		m_infoUsed = m_aboutLoaded ? format_size(m_about.usage) : L"";
		m_infoLimit = m_aboutLoaded ? (m_about.limit ? format_size(m_about.limit) : L"-") : L"";
		m_infoFolder = utf8_to_wide(curId());
		m_info.clear();
		m_info.push_back({ M(MTitle), nullptr, IPLFLAGS_SEPARATOR });
		m_info.push_back({ M(MInfoAccount), m_infoAccount.c_str(), IPLFLAGS_NONE });
		m_info.push_back({ M(MInfoUsed), m_infoUsed.c_str(), IPLFLAGS_NONE });
		m_info.push_back({ M(MInfoLimit), m_infoLimit.c_str(), IPLFLAGS_NONE });
		m_info.push_back({ M(MInfoFolder), m_infoFolder.c_str(), IPLFLAGS_NONE });
	}

	std::vector<Level> m_path;
	std::string m_entriesFor = "\x01";
	bool m_fresh = false;
	std::wstring m_title, m_curDir, m_shortcut;
	std::wstring m_infoAccount, m_infoUsed, m_infoLimit, m_infoFolder;
	std::vector<InfoPanelLine> m_info;
	DriveAbout m_about;
	bool m_aboutLoaded = false;
	ULONGLONG m_aboutTick = 0;
};

// ---------------------------------------------------------------------------
// configuration dialog / sign-in
// ---------------------------------------------------------------------------
static bool DoSignIn()
{
	if (!EnsureHttp()) return false;
	std::vector<std::wstring> lines{ M(MTitle), M(MSignInWaiting), M(MSignInHint), M(MSignInUrlCopied), M(MSignInEsc) };
	HANDLE scr = PSI.SaveScreen(0, 0, -1, -1);
	Message(FMSG_LEFTALIGN, ProgressGuid, lines);
	bool ok = false;
	try
	{
		CheckForEsc();   // drain pending input
		oauth::sign_in(g_cfg, *g_http, [] { return !CheckForEsc(); },
			[](const std::string& url) {
				// if the browser does not open, the user can still paste the link
				if (FSF.CopyToClipboard) FSF.CopyToClipboard(FCT_STREAM, utf8_to_wide(url).c_str());
			});
		try { g_cfg.account_email = g_drive->about().email; g_cfg.save(); } catch (...) {}
		ok = true;
	}
	catch (const GdCancel&) {}
	catch (const std::exception& e) { PSI.RestoreScreen(scr); scr = nullptr; ShowError(e.what()); }
	if (scr) PSI.RestoreScreen(scr);
	if (ok) ShowInfo(M(MTitle), std::wstring(M(MSignInDone)) + L"\n" + utf8_to_wide(g_cfg.account_email));
	return ok;
}

static void ToBuf(wchar_t* buf, size_t n, const std::string& s)
{
	std::wstring w = utf8_to_wide(s);
	wcsncpy(buf, w.c_str(), n - 1);
	buf[n - 1] = 0;
}

static intptr_t ShowConfig()
{
	bool changed = false;
	for (;;)
	{
		wchar_t clientId[512], secret[256], expDoc[16], expSheet[16], expSlides[16], expDrawing[16];
		ToBuf(clientId, 512, g_cfg.client_id);
		ToBuf(secret, 256, g_cfg.client_secret);
		ToBuf(expDoc, 16, g_cfg.export_doc);
		ToBuf(expSheet, 16, g_cfg.export_sheet);
		ToBuf(expSlides, 16, g_cfg.export_slides);
		ToBuf(expDrawing, 16, g_cfg.export_drawing);
		int trash = g_cfg.trash_on_delete ? 1 : 0;
		int disks = g_cfg.add_to_disks_menu ? 1 : 0;

		std::wstring status = g_cfg.signed_in()
			? std::wstring(M(MStatusSignedIn)) + L" " + utf8_to_wide(g_cfg.account_email)
			: std::wstring(M(MStatusNotSignedIn));
		if (g_cfg.has_builtin_client() && g_cfg.client_id.empty())
			status += std::wstring(L"  (") + M(MStatusBuiltin) + L")";

		PluginDialogBuilder b(PSI, PluginGuid, ConfigGuid, MConfigTitle, L"Config");
		if (g_cfg.has_builtin_client())
			b.AddText(MBuiltinHint);
		else
		{
			b.AddText(MSignInHelp1);
			b.AddText(MSignInHelp2);
		}
		b.AddSeparator();
		b.AddText(MClientId);
		b.AddEditField(clientId, 512, 66);
		b.AddText(MClientSecret);
		b.AddPasswordField(secret, 256, 66);
		b.AddSeparator();
		b.AddCheckbox(MTrashOnDelete, &trash);
		b.AddCheckbox(MAddToDisks, &disks);
		b.AddSeparator();
		auto* e1 = b.AddEditField(expDoc, 16, 8);    b.AddTextBefore(e1, MExportDoc);
		auto* e2 = b.AddEditField(expSheet, 16, 8);  b.AddTextBefore(e2, MExportSheet);
		auto* e3 = b.AddEditField(expSlides, 16, 8); b.AddTextBefore(e3, MExportSlides);
		auto* e4 = b.AddEditField(expDrawing, 16, 8); b.AddTextBefore(e4, MExportDrawing);
		b.AddSeparator();
		b.AddText(status.c_str());
		const int buttons[] = { MSignIn, MSignOut, MOk, MCancel };
		b.AddButtons(4, buttons, 2, 3);

		intptr_t r = b.ShowDialogEx();
		if (r < 0 || r == 3) return changed ? 1 : 0;

		std::string newId = wide_to_utf8(clientId), newSecret = wide_to_utf8(secret);
		if (newId != g_cfg.client_id || newSecret != g_cfg.client_secret)
		{
			// credentials changed - old tokens are useless
			if (newId != g_cfg.client_id) { g_cfg.refresh_token.clear(); g_cfg.access_token.clear(); g_cfg.expires_at = 0; }
			g_cfg.client_id = newId;
			g_cfg.client_secret = newSecret;
		}
		g_cfg.trash_on_delete = trash != 0;
		g_cfg.add_to_disks_menu = disks != 0;
		auto ext = [](const wchar_t* w, const char* def) { std::string s = to_lower(wide_to_utf8(w)); if (!s.empty() && s[0] == '.') s.erase(0, 1); return s.empty() ? def : s; };
		const std::pair<const char*, std::string> wanted[] = {
			{ "doc", ext(expDoc, "docx") }, { "sheet", ext(expSheet, "xlsx") },
			{ "slides", ext(expSlides, "pptx") }, { "drawing", ext(expDrawing, "png") } };
		bool valid = true;
		for (auto& [kind, v] : wanted)
		{
			if (valid_export_ext(kind, v)) continue;
			ShowError(std::wstring(M(MBadExportExt)) + L" " + utf8_to_wide(allowed_export_exts(kind)) + L"  (" + utf8_to_wide(v) + L")");
			valid = false;
			break;
		}
		if (!valid) continue;   // re-show the dialog with the stored values
		g_cfg.export_doc = wanted[0].second;
		g_cfg.export_sheet = wanted[1].second;
		g_cfg.export_slides = wanted[2].second;
		g_cfg.export_drawing = wanted[3].second;
		SaveConfigReporting();
		changed = true;

		if (r == 2) return 1;
		if (r == 0) DoSignIn();
		else if (r == 1 && EnsureHttp())
		{
			try
			{
				oauth::sign_out(g_cfg, *g_http);
				ShowInfo(M(MTitle), M(MSignedOut));
			}
			catch (const std::exception& e) { ShowError(e.what()); }
		}
	}
}

// ---------------------------------------------------------------------------
// file operations
// ---------------------------------------------------------------------------
enum class Overwrite { Ask, All, SkipAll };

// returns: 1 overwrite, 0 skip, throws GdCancel on cancel
static int AskOverwrite(const std::wstring& name, Overwrite& mode)
{
	if (mode == Overwrite::All) return 1;
	if (mode == Overwrite::SkipAll) return 0;
	intptr_t r = Message(FMSG_WARNING, OverwriteGuid, { M(MOverwriteTitle), name, M(MOverwriteQ) },
		{ M(MOverwrite), M(MOverwriteAll), M(MSkip), M(MSkipAll), M(MCancel) });
	switch (r)
	{
	case 0: return 1;
	case 1: mode = Overwrite::All; return 1;
	case 2: return 0;
	case 3: mode = Overwrite::SkipAll; return 0;
	default: throw GdCancel{};
	}
}

struct OpStats
{
	int skipped = 0;
	std::wstring firstSkipReason;
	bool silent = false;          // no prompts allowed (OPM_SILENT)
	bool skipAllErrors = false;   // user chose "Skip all errors"
	void skip(const std::wstring& reason) { ++skipped; if (firstSkipReason.empty()) firstSkipReason = reason; }
};

// Per-item error handling: Retry / Skip / Skip all / Cancel. Returns true to retry the item,
// false to skip it (already counted). Sign-in problems are never per-item and propagate.
static bool HandleItemError(const std::wstring& name, const GdError& err, OpStats& st)
{
	if (dynamic_cast<const GdAuthError*>(&err)) throw GdAuthError(err.what(), err.status);   // keep the type (no slicing)
	std::wstring text = utf8_to_wide(err.what());
	if (st.silent || st.skipAllErrors)
	{
		st.skip(name + L": " + text);
		return false;
	}
	std::vector<std::wstring> lines{ M(MItemError), name };
	for (auto& l : WrapText(text, 70, 5)) lines.push_back(l);
	intptr_t r = Message(FMSG_WARNING, ItemErrorGuid, lines, { M(MRetry), M(MSkip), M(MSkipAllErrors), M(MCancel) });
	switch (r)
	{
	case 0: return true;
	case 1: st.skip(name + L": " + text); return false;
	case 2: st.skipAllErrors = true; st.skip(name + L": " + text); return false;
	default: throw GdCancel{};
	}
}

// Copy/Move dialog shown for F5/F6 from Drive to a local panel (Far leaves it to the plugin).
// Returns false when cancelled; `dest` receives the chosen folder.
static bool AskDestination(std::wstring& dest, size_t count, bool move, bool permanent)
{
	wchar_t buf[2048];
	wcsncpy(buf, dest.c_str(), 2047);
	buf[2047] = 0;
	std::wstring what = std::to_wstring(count) + L" " + M(MItemsSelected);
	PluginDialogBuilder b(PSI, PluginGuid, CopyGuid, move ? MMoveTitle : MCopyTitle, L"Contents");
	b.AddText(what.c_str());
	b.AddText(move ? (permanent ? MMovePromptPerm : MMovePrompt) : MCopyPrompt);
	b.AddEditField(buf, 2048, 60, L"GoogleDriveCopyTo");
	b.AddOKCancel(MOk, MCancel);
	if (!b.ShowDialog()) return false;
	std::wstring d = buf;
	while (!d.empty() && d.back() == L' ') d.pop_back();
	while (!d.empty() && d.front() == L' ') d.erase(0, 1);
	if (d.empty()) return false;
	if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') d = d.substr(1, d.size() - 2);
	bool absolute = (d.size() >= 2 && d[1] == L':') || (d.size() >= 2 && d[0] == L'\\' && d[1] == L'\\');
	if (!absolute)
	{
		if (d[0] == L'\\' || d[0] == L'/')
		{
			// "\sub": relative to the root of the destination's drive
			if (dest.size() >= 2 && dest[1] == L':') d = dest.substr(0, 2) + d;
			else d = dest + d;
		}
		else
			d = dest + L"\\" + d;
	}
	while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
	dest = d;
	return true;
}

// Creates every missing component of an absolute folder path.
static void CreateDirectoryTree(const std::wstring& path)
{
	std::wstring lp = long_path(path);
	size_t start = lp.find(L'\\', lp.compare(0, 4, L"\\\\?\\") == 0 ? 7 : 3);
	for (size_t p = start; p != std::wstring::npos; p = lp.find(L'\\', p + 1))
		CreateDirectoryW(lp.substr(0, p).c_str(), nullptr);
	CreateDirectoryW(lp.c_str(), nullptr);
}

// Plain-text-ish export used when a Google document is opened with F3/F4.
static const char* ViewExportExt(const std::string& mime)
{
	if (mime == "application/vnd.google-apps.document") return "txt";
	if (mime == "application/vnd.google-apps.spreadsheet") return "csv";
	if (mime == "application/vnd.google-apps.presentation") return "txt";
	return nullptr;
}

// Outcome of copying one entry. Only `Copied` items may be deleted by a move: an export of a
// Google document is a lossy conversion, not a copy, so the original must stay on Drive.
enum class CopyResult { Skipped, Copied, Exported };

// Downloads one panel entry (recursively for folders). A folder counts as Copied only when every
// child was Copied; it is Exported when nothing was skipped but some children were exports.
// keepName: the local name must equal the panel name (F3/F4/QuickView temp files, and Far's silent
//           plugin-to-plugin mode, where Far later looks the files up by their panel names).
// insideTrash: the entry lives below the Trash (its children are all flagged trashed).
// visited: folder ids already entered in this operation (shortcuts can form cycles).
static CopyResult DownloadEntry(Panel& p, const Entry& e, const std::wstring& destDir, Progress& prog, Overwrite& ow,
	OpStats& st, bool viewMode, bool keepName, bool insideTrash, std::set<std::string>& visited)
{
	std::wstring local = destDir + L"\\" + e.display;
	if (e.isFolder())
	{
		std::string fid = e.pseudo ? e.id : e.effectiveId();
		if (!visited.insert(fid).second)
		{
			st.skip(std::wstring(M(MFolderLoop)) + L" " + e.display);
			return CopyResult::Skipped;
		}
		if (!CreateDirectoryW(long_path(local).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
			throw GdError("Cannot create folder " + wide_to_utf8(local) + ": " + wide_to_utf8(win_error_text(GetLastError())));
		ThrowIfCancelled(prog.update(e.display, L"", 0, 0, true));
		bool childrenInTrash = insideTrash || (e.pseudo && e.id == kTrashId);
		std::vector<Entry> children;
		for (;;)
		{
			try { children = p.fetch(fid, childrenInTrash); break; }
			catch (const GdError& err) { if (!HandleItemError(e.display, err, st)) return CopyResult::Skipped; }
		}
		CopyResult result = CopyResult::Copied;
		for (auto& child : children)
		{
			CopyResult r = DownloadEntry(p, child, local, prog, ow, st, viewMode, keepName, childrenInTrash, visited);
			if (r == CopyResult::Skipped) result = CopyResult::Skipped;
			else if (r == CopyResult::Exported && result == CopyResult::Copied) result = CopyResult::Exported;
		}
		return result;
	}
	DriveFile f = e;
	if (f.isShortcut)
	{
		// shortcut to a file: fetch the real metadata (size, times, md5)
		try { DriveFile t = g_drive->get(f.targetId); t.name = f.name; f = t; } catch (...) {}
	}
	const char* exportOverride = nullptr;
	if (f.isGoogleDoc())
	{
		std::string ext = g_drive->exportExtFor(f.effectiveMime());
		if (viewMode && ViewExportExt(f.effectiveMime())) { exportOverride = ViewExportExt(f.effectiveMime()); ext = exportOverride; }
		if (ext.empty() || !export_mime_for_ext(ext))
		{
			st.skip(std::wstring(M(MNotExportable)) + L" " + e.display);
			return CopyResult::Skipped;
		}
		std::wstring wext = L"." + utf8_to_wide(ext);
		if (!keepName && (local.size() < wext.size() || _wcsicmp(local.c_str() + local.size() - wext.size(), wext.c_str()) != 0))
			local += wext;
	}
	if (!viewMode && GetFileAttributesW(long_path(local).c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		if (!AskOverwrite(local, ow)) { st.skip(L""); return CopyResult::Skipped; }
	}
	ThrowIfCancelled(prog.update(e.display, local, 0, f.size, true));
	for (;;)
	{
		try
		{
			g_drive->download(f, local, [&](uint64_t done, uint64_t total) {
				return prog.update(e.display, local, done, total ? total : f.size);
			}, exportOverride);
			break;
		}
		catch (const GdError& err) { if (!HandleItemError(e.display, err, st)) return CopyResult::Skipped; }
	}
	return f.isGoogleDoc() ? CopyResult::Exported : CopyResult::Copied;
}

static void DeleteEntry(Panel& p, const Entry& e)
{
	if (e.pseudo) return;
	if (p.inTrash() || !g_cfg.trash_on_delete) g_drive->remove(e.id);
	else g_drive->trash(e.id);
}

// Upload cache of folder listings so we can detect existing names.
using ChildrenCache = std::map<std::string, std::vector<DriveFile>>;

static const std::vector<DriveFile>& Children(ChildrenCache& cache, const std::string& folderId)
{
	auto it = cache.find(folderId);
	if (it == cache.end()) it = cache.emplace(folderId, g_drive->listChildren(folderId)).first;
	return it->second;
}

static const DriveFile* FindChild(const std::vector<DriveFile>& list, const std::string& name, bool folder)
{
	for (auto& f : list) if (f.isFolder() == folder && f.name == name) return &f;
	std::wstring wn = utf8_to_wide(name);
	for (auto& f : list) if (f.isFolder() == folder && _wcsicmp(utf8_to_wide(f.name).c_str(), wn.c_str()) == 0) return &f;
	return nullptr;
}

static void UploadPath(const std::wstring& localPath, const std::wstring& name, const std::string& parentId,
	bool isDir, bool move, ChildrenCache& cache, Progress& prog, Overwrite& ow, OpStats& st)
{
	std::string uname = wide_to_utf8(name);
	if (isDir)
	{
		std::string folderId;
		if (const DriveFile* existing = FindChild(Children(cache, parentId), uname, true)) folderId = existing->effectiveId();
		else
		{
			ThrowIfCancelled(prog.update(name, L"", 0, 0, true));
			for (;;)
			{
				try
				{
					DriveFile created = g_drive->createFolder(parentId, uname);
					cache[parentId].push_back(created);
					folderId = created.id;
					break;
				}
				catch (const GdError& err) { if (!HandleItemError(name, err, st)) return; }
			}
		}
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(long_path(localPath + L"\\*").c_str(), &fd);
		bool complete = true;
		if (h != INVALID_HANDLE_VALUE)
		{
			struct Closer { HANDLE h; ~Closer() { FindClose(h); } } c{ h };
			do
			{
				if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
				bool sub = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				if (sub && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				{
					// junctions / symlinked folders can loop; they are not followed
					st.skip(std::wstring(M(MFolderLoop)) + L" " + fd.cFileName);
					complete = false;
					continue;
				}
				int before = st.skipped;
				UploadPath(localPath + L"\\" + fd.cFileName, fd.cFileName, folderId, sub, move, cache, prog, ow, st);
				if (st.skipped != before) complete = false;
			} while (FindNextFileW(h, &fd));
		}
		if (move && complete) RemoveDirectoryW(long_path(localPath).c_str());
		return;
	}

	std::string existingId;
	if (const DriveFile* f = FindChild(Children(cache, parentId), uname, false))
	{
		if (!AskOverwrite(name, ow)) { st.skip(L""); return; }
		existingId = f->effectiveId();
	}
	ThrowIfCancelled(prog.update(name, localPath, 0, 0, true));
	DriveFile up;
	for (;;)
	{
		try
		{
			up = g_drive->upload(parentId, uname, localPath, existingId, [&](uint64_t done, uint64_t total) {
				return prog.update(name, localPath, done, total);
			});
			break;
		}
		catch (const GdError& err) { if (!HandleItemError(name, err, st)) return; }
	}
	if (existingId.empty()) cache[parentId].push_back(up);
	if (move) DeleteFileW(long_path(localPath).c_str());
}

static void ReportSkipped(const OpStats& st)
{
	if (st.skipped && !st.firstSkipReason.empty())
		ShowInfo(M(MWarning), std::wstring(M(MSkipped)) + L" " + std::to_wstring(st.skipped) + L"\n" + st.firstSkipReason);
}

// ---------------------------------------------------------------------------
// exported plugin API
// ---------------------------------------------------------------------------
extern "C"
{

void WINAPI GetGlobalInfoW(GlobalInfo* info)
{
	info->StructSize = sizeof(GlobalInfo);
	info->MinFarVersion = MAKEFARVERSION(3, 0, 0, 6000, VS_RELEASE);
	info->Version = MAKEFARVERSION(PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, PLUGIN_VERSION_REVISION, PLUGIN_VERSION_BUILD, VS_RELEASE);
	info->Guid = PluginGuid;
	info->Title = L"Google Drive";
	info->Description = L"Browse and copy files to/from Google Drive";
	info->Author = PLUGIN_AUTHOR;
}

void WINAPI SetStartupInfoW(const PluginStartupInfo* info)
{
	memcpy(&PSI, info, std::min<size_t>(info->StructSize, sizeof PSI));
	memcpy(&FSF, info->FSF, std::min<size_t>(info->FSF->StructSize, sizeof FSF));
	PSI.FSF = &FSF;

	g_pluginDir = PSI.ModuleName;
	size_t slash = g_pluginDir.find_last_of(L'\\');
	if (slash != std::wstring::npos) g_pluginDir.resize(slash);

	// Settings live in the Far profile; fall back to the roaming profile location, and only
	// as a last resort to the plugin folder (which may be read-only under Program Files).
	wchar_t buf[MAX_PATH] = {};
	if (GetEnvironmentVariableW(L"FARPROFILE", buf, MAX_PATH) && buf[0])
		g_cfg.file = std::wstring(buf) + L"\\PluginsData\\GoogleDrive\\config.json";
	else if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) && buf[0])
		g_cfg.file = std::wstring(buf) + L"\\Far Manager\\Profile\\PluginsData\\GoogleDrive\\config.json";
	else
		g_cfg.file = g_pluginDir + L"\\config.json";
	g_cfg.load();
	// Optional client shipped next to the DLL, so users need no Google Cloud project of their own.
	g_cfg.load_client_file(g_pluginDir + L"\\GoogleDrive.client.json");

	try
	{
		g_http = std::make_unique<HttpClient>();
		g_drive = std::make_unique<Drive>(g_cfg, *g_http);
	}
	catch (...) {}   // reported later by EnsureHttp() when the user actually needs it
}

void WINAPI GetPluginInfoW(PluginInfo* info)
{
	static const wchar_t* menu[1];
	static const wchar_t* cfg[1];
	menu[0] = M(MTitle);
	cfg[0] = M(MConfigTitle);

	info->StructSize = sizeof(PluginInfo);
	info->Flags = PF_NONE;
	if (g_cfg.add_to_disks_menu)
	{
		info->DiskMenu.Guids = &MenuGuid;
		info->DiskMenu.Strings = menu;
		info->DiskMenu.Count = 1;
	}
	info->PluginMenu.Guids = &MenuGuid;
	info->PluginMenu.Strings = menu;
	info->PluginMenu.Count = 1;
	info->PluginConfig.Guids = &ConfigGuid;
	info->PluginConfig.Strings = cfg;
	info->PluginConfig.Count = 1;
	info->CommandPrefix = L"gdrive";
}

intptr_t WINAPI ConfigureW(const ConfigureInfo*)
{
	return ShowConfig();
}

HANDLE WINAPI OpenW(const OpenInfo* info)
{
	if (!EnsureHttp()) return nullptr;
	if (!g_cfg.signed_in())
	{
		if (g_cfg.has_client())
		{
			// a client is available (built-in or configured): offer to sign in right away
			intptr_t r = Message(FMSG_MB_YESNO, MsgGuid, { M(MTitle), M(MSignInNow) });
			if (r != 0) return nullptr;
			DoSignIn();
		}
		else
		{
			intptr_t r = Message(FMSG_MB_YESNO, MsgGuid, { M(MTitle), M(MNeedSignIn) });
			if (r != 0) return nullptr;
			ShowConfig();
		}
		if (!g_cfg.signed_in()) return nullptr;
	}

	std::unique_ptr<Panel> p(new Panel);
	std::wstring target;
	if (info->OpenFrom == OPEN_SHORTCUT)
	{
		auto* si = (const OpenShortcutInfo*)info->Data;
		if (si && si->ShortcutData) p->restore(si->ShortcutData);
	}
	else if (info->OpenFrom == OPEN_COMMANDLINE)
	{
		auto* ci = (const OpenCommandLineInfo*)info->Data;
		if (ci && ci->CommandLine) target = ci->CommandLine;
		while (!target.empty() && target[0] == L' ') target.erase(0, 1);
		if (target.size() >= 2 && target.front() == L'"' && target.back() == L'"') target = target.substr(1, target.size() - 2);
	}

	auto openListing = [&]() {
		Progress prog(M(MReading), true);
		prog.update(L"", L"", 0, 0, true);
		if (!target.empty() && !p->chdir(target))
		{
			p->toRoot();
			ShowError(L"Folder not found: " + target);
		}
		try { p->ensureEntries(); }
		catch (const GdAuthError&) { throw; }
		catch (const GdError&)
		{
			if (info->OpenFrom != OPEN_SHORTCUT) throw;
			p->toRoot();          // stale shortcut: fall back to root
			p->ensureEntries();
		}
		p->loadAbout();
	};
	for (int attempt = 0;; ++attempt)
	{
		try
		{
			openListing();
			return p.release();
		}
		catch (const GdCancel&) { return nullptr; }
		catch (const GdAuthError& e)
		{
			// the stored sign-in was rejected (tokens already cleared): offer a fresh sign-in once
			if (attempt > 0 || Message(FMSG_WARNING | FMSG_MB_YESNO, MsgGuid, { M(MTitle), M(MAuthExpired) }) != 0)
			{
				if (attempt > 0) ShowError(e.what());
				return nullptr;
			}
			if (!DoSignIn()) return nullptr;
		}
		catch (const std::exception& e) { ShowError(e.what()); return nullptr; }
	}
}

void WINAPI ClosePanelW(const ClosePanelInfo* info)
{
	delete (Panel*)info->hPanel;
}

void WINAPI GetOpenPanelInfoW(OpenPanelInfo* info)
{
	info->StructSize = sizeof(OpenPanelInfo);
	((Panel*)info->hPanel)->fillOpenPanelInfo(info);
}

intptr_t WINAPI GetFindDataW(GetFindDataInfo* info)
{
	Panel* p = (Panel*)info->hPanel;
	try
	{
		std::unique_ptr<Progress> prog;
		if (!(info->OpMode & (OPM_FIND | OPM_SILENT)))
		{
			prog.reset(new Progress(M(MReading), true));
			prog->update(L"", L"", 0, 0, true);
		}
		p->refreshForDisplay(!(info->OpMode & OPM_FIND));
	}
	catch (const GdCancel&) { return FALSE; }
	catch (const std::exception& e)
	{
		if (!(info->OpMode & (OPM_FIND | OPM_SILENT))) ShowError(e.what());
		return FALSE;
	}

	// Far's file search (Alt+F7) enters every folder it is shown: keep it out of the pseudo
	// folders (Trash, every shared drive...) and of shortcut folders, which can form cycles.
	std::vector<const Entry*> shown;
	for (auto& e : p->m_entries)
	{
		if ((info->OpMode & OPM_FIND) && (e.pseudo || (e.isShortcut && e.isFolder()))) continue;
		shown.push_back(&e);
	}
	size_t n = shown.size();
	PluginPanelItem* items = n ? new PluginPanelItem[n]() : nullptr;
	for (size_t i = 0; i < n; ++i)
	{
		const Entry& e = *shown[i];
		PluginPanelItem& it = items[i];
		it.FileName = _wcsdup(e.display.c_str());
		it.AlternateFileName = nullptr;
		it.FileAttributes = e.isFolder() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
		if (e.isShortcut) it.FileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
		it.FileSize = e.isFolder() ? 0 : e.size;
		it.AllocationSize = it.FileSize;
		it.LastWriteTime = e.modified;
		it.ChangeTime = e.modified;
		it.CreationTime = e.created;
		it.LastAccessTime = e.modified;
	}
	info->PanelItem = items;
	info->ItemsNumber = n;
	return TRUE;
}

void WINAPI FreeFindDataW(const FreeFindDataInfo* info)
{
	for (size_t i = 0; i < info->ItemsNumber; ++i)
		free((void*)info->PanelItem[i].FileName);
	delete[] info->PanelItem;
}

intptr_t WINAPI SetDirectoryW(const SetDirectoryInfo* info)
{
	Panel* p = (Panel*)info->hPanel;
	try
	{
		return p->chdir(info->Dir ? info->Dir : L"") ? TRUE : FALSE;
	}
	catch (const GdCancel&) { return FALSE; }
	catch (const std::exception& e)
	{
		if (!(info->OpMode & (OPM_FIND | OPM_SILENT))) ShowError(e.what());
		return FALSE;
	}
}

intptr_t WINAPI GetFilesW(GetFilesInfo* info)
{
	Panel* p = (Panel*)info->hPanel;
	if (info->ItemsNumber == 0 || !info->DestPath) return 0;
	bool silent = (info->OpMode & OPM_SILENT) != 0;
	bool showUi = !(info->OpMode & OPM_FIND);
	bool viewMode = (info->OpMode & (OPM_VIEW | OPM_EDIT | OPM_QUICKVIEW)) != 0;
	bool interactive = !silent && !viewMode && showUi;   // F5/F6 from the user: show the Copy/Move dialog
	// Silent non-view mode is Far copying into a temp folder for another plugin panel; it will look
	// the files up by their panel names afterwards, so no export extension may be added.
	bool keepName = viewMode || silent;
	Overwrite ow = silent ? Overwrite::All : Overwrite::Ask;
	OpStats st;
	st.silent = silent;

	std::wstring dest = info->DestPath;
	while (!dest.empty() && (dest.back() == L'\\' || dest.back() == L'/')) dest.pop_back();
	if (dest.empty()) dest = L".";

	// resolve the selection first (pseudo folders are skipped in a multi-selection)
	std::vector<const Entry*> selection;
	try
	{
		size_t real = 0;
		for (size_t i = 0; i < info->ItemsNumber; ++i)
		{
			const wchar_t* name = info->PanelItem[i].FileName;
			if (!name || !wcscmp(name, L"..")) continue;
			if (const Entry* e = p->find(name)) selection.push_back(e);
		}
		for (auto* e : selection) if (!e->pseudo) ++real;
		if (real > 0)
			selection.erase(std::remove_if(selection.begin(), selection.end(), [](const Entry* e) { return e->pseudo; }), selection.end());
	}
	catch (const std::exception& e) { if (showUi) ShowError(e.what()); return 0; }
	if (selection.empty()) return 1;

	bool permanent = p->inTrash() || !g_cfg.trash_on_delete;
	if (interactive)
	{
		if (!AskDestination(dest, selection.size(), info->Move != 0, permanent)) return -1;
		CreateDirectoryTree(dest);
	}

	try
	{
		Progress prog(M(MDownloading), showUi);
		std::set<std::string> visited{ p->curId() };
		std::vector<const Entry*> copied;     // fully copied items - the only ones a move may delete
		bool anySkipped = false, anyExported = false;
		p->m_notMovable.clear();
		for (auto* e : selection)
		{
			CopyResult r = DownloadEntry(*p, *e, dest, prog, ow, st, viewMode, keepName, p->inTrash(), visited);
			switch (r)
			{
			case CopyResult::Copied: copied.push_back(e); break;
			case CopyResult::Exported: anyExported = true; break;
			case CopyResult::Skipped: anySkipped = true; break;
			}
			if (r != CopyResult::Copied) p->m_notMovable.insert(e->id);
			if (viewMode && !e->isFolder() && r != CopyResult::Skipped)
			{
				// remember which Drive file this temp copy belongs to, for a later F4 save-back
				std::wstring key = dest + L"\\" + e->display;
				for (auto& c : key) c = (wchar_t)towlower(c);
				p->m_editTargets[key] = Panel::EditTarget{ e->effectiveId(), e->name, e->effectiveMime(), e->isGoogleDoc() };
			}
			if ((info->OpMode & OPM_EDIT) && e->isGoogleDoc() && e->effectiveMime() != "application/vnd.google-apps.document")
			{
				// F4 on a Sheet / Slides: what the editor gets is a partial export that cannot go back
				std::wstring local = dest + L"\\" + e->display;
				SetFileAttributesW(long_path(local).c_str(), FILE_ATTRIBUTE_READONLY);
				ShowInfo(M(MWarning), M(MGoogleFileViewOnly));
			}
		}
		if (info->Move)
		{
			prog.setTitle(M(MDeleting));
			for (auto* e : copied)
			{
				ThrowIfCancelled(prog.update(e->display, L"", 0, 0, true));
				DeleteEntry(*p, *e);
			}
			p->invalidate();
			if (anyExported) st.skip(M(MExportedNotMoved));
			if (anySkipped) st.skip(M(MNotMovedSkipped));
		}
	}
	catch (const GdCancel&) { if (info->Move) p->invalidate(); return -1; }
	catch (const std::exception& e)
	{
		if (info->Move) p->invalidate();
		if (showUi) ShowError(e.what());
		return 0;
	}
	if (viewMode && st.skipped && !st.firstSkipReason.empty()) ShowError(st.firstSkipReason);   // e.g. F3 on a Form
	else if (showUi && !silent) ReportSkipped(st);
	return 1;
}

intptr_t WINAPI PutFilesW(const PutFilesInfo* info)
{
	Panel* p = (Panel*)info->hPanel;
	if (info->ItemsNumber == 0) return 0;
	bool silent = (info->OpMode & OPM_SILENT) != 0;
	bool editMode = (info->OpMode & OPM_EDIT) != 0;   // F4 save: the temp file carries the panel name

	std::wstring src = info->SrcPath ? info->SrcPath : L"";
	if (src.empty())
	{
		wchar_t cwd[MAX_PATH * 4];
		GetCurrentDirectoryW(MAX_PATH * 4, cwd);
		src = cwd;
	}
	while (!src.empty() && (src.back() == L'\\' || src.back() == L'/')) src.pop_back();

	Overwrite ow = silent ? Overwrite::All : Overwrite::Ask;
	OpStats st;
	ChildrenCache cache;
	try
	{
		p->ensureEntries();
		std::vector<DriveFile> cur;
		for (auto& e : p->m_entries) if (!e.pseudo) cur.push_back(e);
		cache[p->curId()] = std::move(cur);

		Progress prog(M(MUploading), true);
		for (size_t i = 0; i < info->ItemsNumber; ++i)
		{
			const wchar_t* name = info->PanelItem[i].FileName;
			if (!name || !*name || !wcscmp(name, L"..")) continue;
			std::wstring local = (name[1] == L':' || name[0] == L'\\') ? std::wstring(name) : src + L"\\" + name;
			std::wstring base = local.substr(local.find_last_of(L'\\') + 1);
			DWORD attr = info->PanelItem[i].FileAttributes;
			if (attr == INVALID_FILE_ATTRIBUTES || attr == 0) attr = GetFileAttributesW(long_path(local).c_str());
			bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

			if (editMode && !isDir)
			{
				// Map the edited temp file back to the Drive file it was opened from (recorded by
				// GetFiles under the temp path), so the right file is updated even if the panel
				// has moved to another folder since, and so a sanitised/de-duplicated panel name
				// or an exported Google Doc updates the original instead of creating a new file.
				Panel::EditTarget target;
				bool known = false;
				{
					std::wstring key = local;
					for (auto& c : key) c = (wchar_t)towlower(c);
					auto it = p->m_editTargets.find(key);
					if (it != p->m_editTargets.end()) { target = it->second; known = true; }
				}
				if (!known)
				{
					if (const Entry* e = p->find(base.c_str()); e && !e->isFolder() && !e->pseudo)
					{
						target = Panel::EditTarget{ e->effectiveId(), e->name, e->effectiveMime(), e->isGoogleDoc() };
						known = true;
					}
				}
				if (known)
				{
					std::string mime;
					if (target.googleDoc)
					{
						// The editor only ever saw an export. For Sheets/Slides that export is partial
						// (first sheet / plain text), so writing it back would destroy the rest: refuse.
						// For Docs it is complete but unformatted: ask first.
						const char* ext = ViewExportExt(target.mime);
						if (!ext || target.mime != "application/vnd.google-apps.document")
						{
							ShowError(M(MSaveNotSupported));
							return 0;
						}
						intptr_t r = Message(FMSG_WARNING, DeleteGuid,
							{ M(MSaveDocTitle), utf8_to_wide(target.name), M(MSaveDocWarn1), M(MSaveDocWarn2) }, { M(MReplace), M(MCancel) });
						if (r != 0) return -1;
						mime = export_mime_for_ext(ext);
					}
					std::wstring shown = utf8_to_wide(target.name);
					ThrowIfCancelled(prog.update(shown, local, 0, 0, true));
					g_drive->upload(p->curId(), target.name, local, target.fileId,
						[&](uint64_t done, uint64_t total) { return prog.update(shown, local, done, total); }, mime);
					continue;
				}
			}
			if (!p->canWrite())
			{
				if (!silent) ShowError(M(MNotAllowedHere));
				return 0;
			}
			UploadPath(local, base, p->curId(), isDir, info->Move != 0, cache, prog, ow, st);
		}
	}
	catch (const GdCancel&) { p->invalidate(); return -1; }
	catch (const std::exception& e)
	{
		p->invalidate();
		if (!silent) ShowError(e.what());
		return 0;
	}
	p->invalidate();
	if (!silent) ReportSkipped(st);
	return 1;
}

intptr_t WINAPI DeleteFilesW(const DeleteFilesInfo* info)
{
	Panel* p = (Panel*)info->hPanel;
	if (info->ItemsNumber == 0) return 0;
	bool silent = (info->OpMode & OPM_SILENT) != 0;

	std::vector<const Entry*> targets;
	bool refused = false;   // pseudo entries (Shared with me / Shared drives / Trash / a shared drive)
	try
	{
		for (size_t i = 0; i < info->ItemsNumber; ++i)
		{
			const wchar_t* name = info->PanelItem[i].FileName;
			if (!name || !wcscmp(name, L"..")) continue;
			const Entry* e = p->find(name);
			if (!e) continue;
			if (e->pseudo) { refused = true; continue; }
			if (silent)
			{
				// Silent deletes come from Far's plugin-to-plugin move (temp copy, then delete the
				// originals). Protect what that copy could not have carried: Google-native documents
				// (only lossy exports), anything the preceding GetFiles did not copy completely, and
				// everything inside the Trash, where a delete is permanent.
				if (e->isGoogleDoc() || p->m_notMovable.count(e->id) || p->inTrash()) continue;
			}
			targets.push_back(e);
		}
	}
	catch (const std::exception& e) { if (!silent) ShowError(e.what()); return 0; }
	if (targets.empty())
	{
		if (refused && !silent) ShowError(M(MNotAllowedHere));
		return refused ? 0 : 1;
	}

	bool permanent = p->inTrash() || !g_cfg.trash_on_delete;
	if (!silent)
	{
		std::wstring what = targets.size() == 1 ? targets[0]->display : std::to_wstring(targets.size()) + L" " + M(MItems);
		intptr_t r = Message(FMSG_WARNING, DeleteGuid, { M(MDeleteTitle), M(permanent ? MDeleteQuestion : MTrashQuestion), what },
			{ M(MDelete), M(MCancel) });
		if (r != 0) return -1;
	}
	try
	{
		Progress prog(M(MDeleting), !silent);
		size_t i = 0;
		for (auto* e : targets)
		{
			ThrowIfCancelled(prog.update(e->display, L"", i++, targets.size(), true));
			DeleteEntry(*p, *e);
		}
	}
	catch (const GdCancel&) { p->invalidate(); return -1; }
	catch (const std::exception& e) { p->invalidate(); if (!silent) ShowError(e.what()); return 0; }
	p->invalidate();
	return 1;
}

intptr_t WINAPI MakeDirectoryW(MakeDirectoryInfo* info)
{
	Panel* p = (Panel*)info->hPanel;
	bool silent = (info->OpMode & OPM_SILENT) != 0;
	if (!p->canWrite())
	{
		if (!silent) ShowError(M(MNotAllowedHere));
		return 0;
	}
	static std::wstring s_name;
	std::wstring name = info->Name ? info->Name : L"";
	if (name.empty())
	{
		if (silent) return 0;
		wchar_t buf[512] = {};
		if (!PSI.InputBox(&PluginGuid, &MkdirGuid, M(MMkdirTitle), M(MMkdirPrompt), L"GoogleDriveMkdir", L"",
			buf, 512, nullptr, FIB_BUTTONS))
			return -1;
		name = buf;
	}
	if (name.empty()) return -1;
	try
	{
		// Drive would happily create a second folder with the same name; Far users expect an error
		p->ensureEntries();
		for (auto& e : p->m_entries)
		{
			if (e.isFolder() && !e.pseudo && (_wcsicmp(e.display.c_str(), name.c_str()) == 0
				|| _wcsicmp(utf8_to_wide(e.name).c_str(), name.c_str()) == 0))
			{
				if (!silent) ShowError(M(MFolderExists));
				return 0;
			}
		}
		Progress prog(M(MMkdirTitle), !silent);
		prog.update(name, L"", 0, 0, true);
		g_drive->createFolder(p->curId(), wide_to_utf8(name));
	}
	catch (const GdCancel&) { return -1; }
	catch (const std::exception& e) { if (!silent) ShowError(e.what()); return 0; }
	p->invalidate();
	s_name = name;
	info->Name = s_name.c_str();
	return 1;
}

// Name of the panel item under the cursor ("" if none).
static std::wstring CurrentItemName(HANDLE hPanel)
{
	size_t sz = PSI.PanelControl(hPanel, FCTL_GETCURRENTPANELITEM, 0, nullptr);
	if (!sz) return L"";
	std::vector<char> buf(sz);
	FarGetPluginPanelItem gpi{ sizeof gpi, sz, (PluginPanelItem*)buf.data() };
	if (!PSI.PanelControl(hPanel, FCTL_GETCURRENTPANELITEM, 0, &gpi) || !gpi.Item->FileName) return L"";
	return gpi.Item->FileName;
}

// Names of the selected panel items, or the current item when nothing is selected.
static std::vector<std::wstring> SelectedItemNames(HANDLE hPanel)
{
	std::vector<std::wstring> names;
	PanelInfo pi{ sizeof pi };
	if (PSI.PanelControl(hPanel, FCTL_GETPANELINFO, 0, &pi))
	{
		for (size_t i = 0; i < pi.SelectedItemsNumber; ++i)
		{
			size_t sz = PSI.PanelControl(hPanel, FCTL_GETSELECTEDPANELITEM, i, nullptr);
			if (!sz) continue;
			std::vector<char> buf(sz);
			FarGetPluginPanelItem gpi{ sizeof gpi, sz, (PluginPanelItem*)buf.data() };
			if (PSI.PanelControl(hPanel, FCTL_GETSELECTEDPANELITEM, i, &gpi) && gpi.Item->FileName
				&& (gpi.Item->Flags & PPIF_SELECTED))
				names.push_back(gpi.Item->FileName);
		}
	}
	if (names.empty())
	{
		std::wstring cur = CurrentItemName(hPanel);
		if (!cur.empty()) names.push_back(cur);
	}
	names.erase(std::remove(names.begin(), names.end(), std::wstring(L"..")), names.end());
	return names;
}

static void RedrawPanel(HANDLE hPanel)
{
	PSI.PanelControl(hPanel, FCTL_UPDATEPANEL, 1, nullptr);
	PSI.PanelControl(hPanel, FCTL_REDRAWPANEL, 0, nullptr);
}

// The passive panel, if it is another Google Drive panel of this plugin.
static Panel* OtherDrivePanel()
{
	PanelInfo pi{ sizeof pi };
	if (!PSI.PanelControl(PANEL_PASSIVE, FCTL_GETPANELINFO, 0, &pi)) return nullptr;
	if (pi.PanelType != PTYPE_FILEPANEL || !pi.PluginHandle || !IsEqualGUID(pi.OwnerGuid, PluginGuid)) return nullptr;
	return (Panel*)pi.PluginHandle;
}

// Server-side copy of one entry into destId (recursive for folders). Returns false when skipped.
static bool CopyEntryOnDrive(Panel& src, const Entry& e, bool srcInTrash, const std::string& destId,
	ChildrenCache& cache, Progress& prog, Overwrite& ow, OpStats& st, std::set<std::string>& visited)
{
	ThrowIfCancelled(prog.update(e.display, L"", 0, 0, true));
	if (e.isFolder() && !e.isShortcut)
	{
		if (!visited.insert(e.id).second) { st.skip(std::wstring(M(MFolderLoop)) + L" " + e.display); return false; }
		std::string folderId;
		if (const DriveFile* existing = FindChild(Children(cache, destId), e.name, true)) folderId = existing->effectiveId();
		else
		{
			for (;;)
			{
				try { DriveFile f = g_drive->createFolder(destId, e.name); cache[destId].push_back(f); folderId = f.id; break; }
				catch (const GdError& err) { if (!HandleItemError(e.display, err, st)) return false; }
			}
		}
		std::vector<Entry> children;
		for (;;)
		{
			try { children = src.fetch(e.id, srcInTrash); break; }
			catch (const GdError& err) { if (!HandleItemError(e.display, err, st)) return false; }
		}
		bool all = true;
		for (auto& c : children)
			if (!CopyEntryOnDrive(src, c, srcInTrash, folderId, cache, prog, ow, st, visited)) all = false;
		return all;
	}
	// file or shortcut: Drive's copy keeps native formats and copies shortcuts as shortcuts
	const DriveFile* clash = FindChild(Children(cache, destId), e.name, false);
	if (clash && clash->id == e.id) clash = nullptr;   // copying into its own folder: just create alongside
	if (clash && !AskOverwrite(utf8_to_wide(e.name), ow)) { st.skip(L""); return false; }
	for (;;)
	{
		try
		{
			DriveFile made = g_drive->copyFile(e.id, destId, e.name);
			if (clash) g_drive->trash(clash->id);   // "overwrite": the old one goes to the Trash
			cache[destId].push_back(made);
			break;
		}
		catch (const GdError& err) { if (!HandleItemError(e.display, err, st)) return false; }
	}
	return true;
}

// F5/F6 between two Google Drive panels: do it on Google's side instead of Far's download +
// re-upload. Returns true when the key was handled.
static bool DriveToDriveTransfer(Panel* src, HANDLE hSrc, bool move, bool currentOnly)
{
	Panel* dest = OtherDrivePanel();
	if (!dest || dest == src) return false;
	if (move && src->inTrash()) return false;         // F6 in the Trash is "restore"
	if (!dest->canWrite()) { ShowError(M(MNotAllowedHere)); return true; }

	try
	{
		std::vector<const Entry*> items;
		std::vector<std::wstring> names;
		if (currentOnly) { std::wstring c = CurrentItemName(hSrc); if (!c.empty() && c != L"..") names.push_back(c); }
		else names = SelectedItemNames(hSrc);
		for (auto& n : names)
			if (const Entry* e = src->find(n.c_str()); e && !e->pseudo) items.push_back(e);
		if (items.empty()) return true;

		std::wstring what = std::to_wstring(items.size()) + L" " + M(MItemsSelected);
		std::wstring where = L"Google Drive:" + dest->curDir();
		if (Message(FMSG_NONE, CopyGuid, { M(move ? MMoveTitle : MCopyTitle), what, M(move ? MDriveMoveQ : MDriveCopyQ), where },
			{ M(MOk), M(MCancel) }) != 0)
			return true;

		OpStats st;
		Overwrite ow = Overwrite::Ask;
		ChildrenCache cache;
		std::set<std::string> visited{ dest->curId() };
		{
			Progress prog(M(move ? MMovingOnDrive : MCopyingOnDrive), true);
			// the destination listing, for name clashes
			dest->ensureEntries();
			std::vector<DriveFile> cur;
			for (auto& e : dest->m_entries) if (!e.pseudo) cur.push_back(e);
			cache[dest->curId()] = std::move(cur);

			size_t i = 0;
			for (auto* e : items)
			{
				ThrowIfCancelled(prog.update(e->display, where, i++, items.size(), true));
				if (!move)
				{
					CopyEntryOnDrive(*src, *e, src->inTrash(), dest->curId(), cache, prog, ow, st, visited);
					continue;
				}
				// move: re-parent in place
				try
				{
					std::string oldParent;
					if (src->isPseudoRoot())
					{
						// Starred / Shared with me: the real parent is not the list we are looking at
						auto ps = g_drive->parents(e->id);
						if (!ps.empty()) oldParent = ps.front();
					}
					else
						oldParent = src->curId();
					if (oldParent == dest->curId()) continue;   // already there
					if (e->isFolder() && !e->isShortcut && g_drive->isSameOrAncestor(e->id, dest->curId()))
					{
						st.skip(std::wstring(M(MMoveIntoItself)) + L" " + e->display);
						continue;
					}
					for (;;)
					{
						try { g_drive->move(e->id, dest->curId(), oldParent); break; }
						catch (const GdError& err)
						{
							GdError hinted(err.status == 403 ? err.what() + std::string("\n") + wide_to_utf8(M(MDriveMoveHint)) : err.what(), err.status);
							if (!HandleItemError(e->display, hinted, st)) break;
						}
					}
				}
				catch (const GdAuthError&) { throw; }
				catch (const GdError& err) { if (!HandleItemError(e->display, err, st)) continue; }
			}
		}
		ReportSkipped(st);
	}
	catch (const GdCancel&) {}
	catch (const std::exception& ex) { ShowError(ex.what()); }
	src->invalidate();
	dest->invalidate();
	RedrawPanel(hSrc);
	RedrawPanel(PANEL_PASSIVE);
	return true;
}

intptr_t WINAPI ProcessPanelInputW(const ProcessPanelInputInfo* info)
{
	const KEY_EVENT_RECORD& k = info->Rec.Event.KeyEvent;
	if (info->Rec.EventType != KEY_EVENT || !k.bKeyDown) return FALSE;
	DWORD cs = k.dwControlKeyState;
	bool shift = (cs & SHIFT_PRESSED) != 0;
	bool ctrlAlt = (cs & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
	Panel* p = (Panel*)info->hPanel;

	bool rename = k.wVirtualKeyCode == VK_F6 && shift && !ctrlAlt;
	bool restore = k.wVirtualKeyCode == VK_F6 && !shift && !ctrlAlt && p->inTrash();
	bool enter = k.wVirtualKeyCode == VK_RETURN && !shift && !ctrlAlt;

	// F5 / F6 (Shift+F5 for the current item only) towards another Google Drive panel:
	// copy or move on Google's side instead of Far's download + re-upload.
	if ((k.wVirtualKeyCode == VK_F5 || k.wVirtualKeyCode == VK_F6) && !ctrlAlt && !rename && !restore)
	{
		bool move = k.wVirtualKeyCode == VK_F6;
		try { if (DriveToDriveTransfer(p, info->hPanel, move, shift)) return TRUE; }
		catch (const std::exception& ex) { ShowError(ex.what()); return TRUE; }
	}
	if (!rename && !restore && !enter) return FALSE;

	try
	{
		if (enter)
		{
			// Enter on a Google document opens it in the browser (Far would otherwise run a
			// plain-text export without an extension). A typed command line takes precedence.
			if (PSI.PanelControl(PANEL_ACTIVE, FCTL_GETCMDLINE, 0, nullptr) > 1) return FALSE;
			std::wstring name = CurrentItemName(info->hPanel);
			if (name.empty() || name == L"..") return FALSE;
			const Entry* e = p->find(name.c_str());
			if (!e || e->isFolder() || !e->isGoogleDoc() || e->webViewLink.empty()) return FALSE;
			ShellExecuteW(nullptr, L"open", utf8_to_wide(e->webViewLink).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			return TRUE;
		}

		if (restore)
		{
			// F6 inside the Trash restores instead of moving (a move out of the Trash would be a
			// permanent delete of the original).
			std::vector<const Entry*> targets;
			for (auto& n : SelectedItemNames(info->hPanel))
				if (const Entry* e = p->find(n.c_str()); e && !e->pseudo) targets.push_back(e);
			if (targets.empty()) return TRUE;
			std::wstring what = targets.size() == 1 ? targets[0]->display : std::to_wstring(targets.size()) + L" " + M(MItems);
			if (Message(FMSG_WARNING, DeleteGuid, { M(MRestoreTitle), M(MRestoreQuestion), what }, { M(MRestore), M(MCancel) }) != 0)
				return TRUE;
			{
				Progress prog(M(MRestoreTitle), true);
				size_t i = 0;
				for (auto* e : targets)
				{
					ThrowIfCancelled(prog.update(e->display, L"", i++, targets.size(), true));
					g_drive->untrash(e->id);
				}
			}
			p->invalidate();
			RedrawPanel(info->hPanel);
			return TRUE;
		}

		// Shift+F6: rename the current item
		std::wstring name = CurrentItemName(info->hPanel);
		if (name.empty() || name == L"..") return TRUE;
		const Entry* e = p->find(name.c_str());
		if (!e) return TRUE;
		if (e->pseudo || p->inTrash()) { ShowError(M(MNotAllowedHere)); return TRUE; }
		wchar_t newName[512] = {};
		std::wstring old = utf8_to_wide(e->name);
		if (!PSI.InputBox(&PluginGuid, &RenameGuid, M(MRenameTitle), M(MRenamePrompt), L"GoogleDriveRename",
			old.c_str(), newName, 512, nullptr, FIB_BUTTONS))
			return TRUE;
		if (!newName[0] || old == newName) return TRUE;
		g_drive->rename(e->id, wide_to_utf8(newName));
		p->invalidate();
		RedrawPanel(info->hPanel);
	}
	catch (const GdCancel&) { p->invalidate(); RedrawPanel(info->hPanel); }
	catch (const std::exception& ex) { p->invalidate(); ShowError(ex.what()); RedrawPanel(info->hPanel); }
	return TRUE;
}

void WINAPI ExitFARW(const ExitInfo*)
{
	g_drive.reset();
	g_http.reset();
}

} // extern "C"
