/*  esserverdetect.h  --  EuroScope server detection, single header, version 2
 *
 *  Tells a plugin which server EuroScope is REALLY connected to, independent of
 *  CPlugIn::GetConnectionType(). EuroScope reports CONNECTION_TYPE_DIRECT
 *  whenever "Connect to VATSIM" is ticked, even when the server picked in the
 *  connect dialog is SweatBox -- so the SDK value cannot tell live VATSIM from
 *  SweatBox.
 *
 *  HOW IT WORKS
 *    - On connect EuroScope opens a TCP socket to <server>:6809 (FSD). This
 *      header finds that socket in the OS TCP table (owned by the EuroScope
 *      process, remote port 6809) and identifies the remote IP, in this order:
 *        1. VATSIM's published server lists, fetched once per session:
 *             https://data.vatsim.net/v3/vatsim-servers.json    (live)
 *             https://data.vatsim.net/v3/sweatbox-servers.json  (sweatbox)
 *        2. Built-in VATSIM host names, in case the lists cannot be fetched:
 *             fsd.connect.vatsim.net (live), sweatbox(-2).vatsim.net (sweatbox)
 *        3. %APPDATA%\EuroScope\ipaddr.txt (+ ipaddr_local.txt), the connect
 *           dialog's drop-down, for custom training servers. "AUTOMATIC" is
 *           live, any other name is training.
 *    - Via proxy: the socket belongs to the master EuroScope instance, so the
 *      established :6809 connection of another EuroScope.exe process is used.
 *    - Detection only ever concludes Sweatbox on positive evidence. An IP that
 *      none of the above knows falls back to the EuroScope connection type and
 *      sets info.fromFallback, so the plugin can double-check it.
 *
 *  CLASSIFICATION
 *      IP of a live VATSIM server         -> Network::Vatsim
 *      IP of a sweatbox / training server -> Network::Sweatbox
 *      simulator server/client, playback  -> Network::Simulator
 *      not connected                      -> Network::Offline
 *      detection still running            -> Network::Pending
 *      unknown IP, or no socket found     -> from GetConnectionType()
 *                                            (SWEATBOX -> Sweatbox, else Vatsim),
 *                                            info.fromFallback = true
 *
 *  USAGE (stb-style: exactly ONE .cpp defines ESSD_IMPLEMENTATION)
 *      // ServerDetect.cpp -- include BEFORE <windows.h> / EuroScopePlugIn.h
 *      #define ESSD_IMPLEMENTATION
 *      #include "esserverdetect.h"
 *
 *      // anywhere else
 *      #include "esserverdetect.h"
 *      essd::ServerDetector serverDetector_;
 *      void MyPlugin::OnTimer(int) { serverDetector_.Update(GetConnectionType()); }
 *      if (serverDetector_.IsSweatbox()) { ... }
 *
 *  THREADING
 *    Update()/getters must be called from the EuroScope main thread. The socket
 *    scan, HTTP and DNS lookups run on a worker thread; Update() never blocks.
 *    The change callback is invoked from Update(), i.e. on the main thread.
 *    The destructor waits for a running worker (bounded by the HTTP timeouts).
 *
 *  Links: ws2_32, iphlpapi, shell32, winhttp (auto-linked via #pragma comment).
 */
#ifndef ESSERVERDETECT_H_INCLUDED
#define ESSERVERDETECT_H_INCLUDED

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace essd {

constexpr int kVersion = 2;
constexpr std::uint16_t kFsdPort = 6809;

// Mirrors EuroScopePlugIn.h so this header does not depend on it.
constexpr int kConnNo = 0;
constexpr int kConnDirect = 1;
constexpr int kConnViaProxy = 2;
constexpr int kConnSimulatorServer = 3;
constexpr int kConnPlayback = 4;
constexpr int kConnSimulatorClient = 5;
constexpr int kConnSweatbox = 6;

enum class Network {
	Offline,   // not connected
	Pending,   // connected, detection not finished yet
	Vatsim,    // live VATSIM network
	Sweatbox,  // SweatBox / training server / custom address
	Simulator, // EuroScope simulator server/client or playback
};

// What identified the server.
enum class MatchSource {
	None,        // nothing yet (offline, pending, simulator)
	VatsimList,  // data.vatsim.net server lists
	BuiltIn,     // built-in VATSIM host names
	ServerFile,  // ipaddr.txt / ipaddr_local.txt
	Fallback,    // unknown IP or no socket: derived from GetConnectionType()
};

inline const char* ToString(Network n) {
	switch (n) {
	case Network::Offline:   return "Offline";
	case Network::Pending:   return "Pending";
	case Network::Vatsim:    return "VATSIM";
	case Network::Sweatbox:  return "Sweatbox";
	case Network::Simulator: return "Simulator";
	}
	return "?";
}

inline const char* ToString(MatchSource s) {
	switch (s) {
	case MatchSource::None:       return "none";
	case MatchSource::VatsimList: return "VATSIM server list";
	case MatchSource::BuiltIn:    return "built-in VATSIM hosts";
	case MatchSource::ServerFile: return "ipaddr.txt";
	case MatchSource::Fallback:   return "fallback";
	}
	return "?";
}

struct ServerInfo {
	Network network = Network::Offline;
	int esConnectionType = kConnNo; // what GetConnectionType() reported
	std::string serverName;         // "AUTOMATIC", "SWEATBOX", ipaddr.txt name...; empty if unmatched
	std::string serverAddress;      // host or IP of the matched entry
	std::string remoteIp;           // remote IP of the FSD socket; empty if no socket found
	MatchSource source = MatchSource::None;
	bool listMatched = false;       // remote IP was identified (source is not Fallback)
	bool viaOtherProcess = false;   // socket found in another EuroScope process (proxy master)
	bool fromFallback = false;      // not identified; network derived from esConnectionType
};

class ServerDetector {
public:
	using ChangeCallback = std::function<void(const ServerInfo&)>;

	static constexpr int kMaxAttempts = 10;
	static constexpr std::chrono::milliseconds kRetryDelay{1000};

	// euroscopeDataDir: folder holding ipaddr.txt; empty = %APPDATA%\EuroScope
	explicit ServerDetector(std::string euroscopeDataDir = {});
	~ServerDetector();
	ServerDetector(const ServerDetector&) = delete;
	ServerDetector& operator=(const ServerDetector&) = delete;

	// Call regularly (OnTimer) with CPlugIn::GetConnectionType(). Cheap, non-blocking.
	void Update(int esConnectionType);

	// Fired from Update() whenever the resolved ServerInfo changes.
	void SetOnChange(ChangeCallback cb) { onChange_ = std::move(cb); }

	const ServerInfo& GetInfo() const { return info_; }
	Network GetNetwork() const { return info_.network; }
	bool IsVatsim() const { return info_.network == Network::Vatsim; }
	bool IsSweatbox() const { return info_.network == Network::Sweatbox; }
	bool IsPending() const { return info_.network == Network::Pending; }
	bool IsResolved() const { return info_.network != Network::Pending; }

	// Forces a new detection on the next Update() (e.g. after a manual reconnect).
	void Invalidate() { lastConnType_ = -1; }

	// Synchronous one-shot detection (blocks on HTTP/DNS). Returns nullopt if no FSD socket.
	static std::optional<ServerInfo> DetectNow(int esConnectionType, const std::string& euroscopeDataDir);

private:
	void SetInfo(ServerInfo info);
	void StartWorker();
	void PruneStale();

	std::string dataDir_;
	ServerInfo info_;
	ChangeCallback onChange_;
	int lastConnType_ = -1;
	int attempts_ = 0;
	std::chrono::steady_clock::time_point nextAttempt_{};
	std::future<std::optional<ServerInfo>> worker_;
	// Workers of a connection that has since changed. Kept (not waited on) so the
	// main thread never blocks on their HTTP calls; dropped once they finish.
	std::vector<std::future<std::optional<ServerInfo>>> stale_;
};

} // namespace essd

#endif // ESSERVERDETECT_H_INCLUDED

/* ======================================================================= */
#ifdef ESSD_IMPLEMENTATION
#ifndef ESSD_IMPLEMENTATION_DONE
#define ESSD_IMPLEMENTATION_DONE

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")

namespace essd {
namespace detail {

// A server the remote IP can be matched against.
struct KnownServer {
	std::string address; // host name or IP
	std::string name;
	bool sweatbox = false;
};

struct Endpoint {
	std::string ip;
	DWORD pid = 0;
	bool ownProcess = false;
};

inline bool IEquals(const std::string& a, const char* b) {
	size_t n = std::strlen(b);
	if (a.size() != n) return false;
	for (size_t i = 0; i < n; ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	return true;
}

inline std::string Trim(std::string s) {
	auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
	while (!s.empty() && ws(s.back())) s.pop_back();
	size_t i = 0;
	while (i < s.size() && ws(s[i])) ++i;
	return s.substr(i);
}

inline std::string DefaultDataDir() {
	char path[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path)))
		return std::string(path) + "\\EuroScope";
	return {};
}

inline std::string Ipv4ToString(DWORD addrNetworkOrder) {
	in_addr a{};
	a.S_un.S_addr = addrNetworkOrder;
	char buf[INET_ADDRSTRLEN] = {};
	inet_ntop(AF_INET, &a, buf, sizeof(buf));
	return buf;
}

inline std::string Ipv6ToString(const UCHAR bytes[16]) {
	// IPv4-mapped (::ffff:a.b.c.d) -> plain IPv4 so it compares with A records.
	static const UCHAR mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
	if (std::memcmp(bytes, mapped, 12) == 0) {
		DWORD v4;
		std::memcpy(&v4, bytes + 12, 4);
		return Ipv4ToString(v4);
	}
	in6_addr a{};
	std::memcpy(&a, bytes, 16);
	char buf[INET6_ADDRSTRLEN] = {};
	inet_ntop(AF_INET6, &a, buf, sizeof(buf));
	return buf;
}

inline bool IsLoopback(const std::string& ip) {
	return ip.rfind("127.", 0) == 0 || ip == "::1";
}

inline bool IsEuroScopeProcess(DWORD pid) {
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h) return false;
	char path[MAX_PATH] = {};
	DWORD n = MAX_PATH;
	const BOOL ok = QueryFullProcessImageNameA(h, 0, path, &n);
	CloseHandle(h);
	if (!ok) return false;
	std::string p(path, n);
	const size_t slash = p.find_last_of("\\/");
	return IEquals(slash == std::string::npos ? p : p.substr(slash + 1), "EuroScope.exe");
}

// Established TCP connections whose remote port is the FSD port.
inline std::vector<Endpoint> FindFsdEndpoints() {
	std::vector<Endpoint> out;
	const DWORD myPid = GetCurrentProcessId();

	auto query = [](ULONG family, std::vector<char>& buf) -> bool {
		DWORD size = 0;
		DWORD rc = ERROR_INSUFFICIENT_BUFFER;
		for (int i = 0; i < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++i) {
			buf.resize(size ? size : 16 * 1024);
			size = static_cast<DWORD>(buf.size());
			rc = GetExtendedTcpTable(buf.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
		}
		return rc == NO_ERROR;
	};

	std::vector<char> buf;
	if (query(AF_INET, buf)) {
		auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
		for (DWORD i = 0; i < t->dwNumEntries; ++i) {
			const auto& r = t->table[i];
			if (r.dwState != MIB_TCP_STATE_ESTAB) continue;
			if (ntohs(static_cast<u_short>(r.dwRemotePort)) != kFsdPort) continue;
			out.push_back({Ipv4ToString(r.dwRemoteAddr), r.dwOwningPid, r.dwOwningPid == myPid});
		}
	}
	if (query(AF_INET6, buf)) {
		auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
		for (DWORD i = 0; i < t->dwNumEntries; ++i) {
			const auto& r = t->table[i];
			if (r.dwState != MIB_TCP_STATE_ESTAB) continue;
			if (ntohs(static_cast<u_short>(r.dwRemotePort)) != kFsdPort) continue;
			out.push_back({Ipv6ToString(r.ucRemoteAddr), r.dwOwningPid, r.dwOwningPid == myPid});
		}
	}
	return out;
}

// ipaddr.txt: "<address> <name>" per line. Tolerates a UTF-8 BOM and UTF-16 LE
// files (the content is ASCII), since a user editing the file can save either.
inline void LoadServerFile(const std::string& path, std::vector<KnownServer>& out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return;
	std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) {
		std::string narrow;
		for (size_t i = 2; i + 1 < raw.size(); i += 2) narrow.push_back(raw[i + 1] == 0 ? raw[i] : '?');
		raw.swap(narrow);
	} else if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) {
		raw.erase(0, 3);
	}

	std::istringstream lines(raw);
	std::string line;
	while (std::getline(lines, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == ';') continue;
		size_t sp = line.find_first_of(" \t");
		if (sp == std::string::npos) continue;
		KnownServer e{line.substr(0, sp), Trim(line.substr(sp + 1)), false};
		if (e.address.empty() || e.name.empty()) continue;
		e.sweatbox = !IEquals(e.name, "AUTOMATIC");
		out.push_back(std::move(e));
	}
}

inline std::vector<std::string> Resolve(const std::string& host) {
	std::vector<std::string> ips;
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return ips;
	for (addrinfo* p = res; p; p = p->ai_next) {
		if (p->ai_family == AF_INET) {
			ips.push_back(Ipv4ToString(reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr.S_un.S_addr));
		} else if (p->ai_family == AF_INET6) {
			ips.push_back(Ipv6ToString(reinterpret_cast<sockaddr_in6*>(p->ai_addr)->sin6_addr.u.Byte));
		}
	}
	freeaddrinfo(res);
	return ips;
}

inline bool AddressMatches(const std::string& address, const std::string& remoteIp) {
	if (IsLoopback(remoteIp) && (IEquals(address, "localhost") || IEquals(address, "loopback")))
		return true;
	for (const auto& ip : Resolve(address))
		if (ip == remoteIp) return true;
	return false;
}

// HTTPS GET, short timeouts. Empty on any failure.
inline std::string HttpGet(const wchar_t* host, const wchar_t* path) {
	std::string body;
	HINTERNET session = WinHttpOpen(L"esserverdetect/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) return body;
	WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);
	HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;

	DWORD status = 0, statusSize = sizeof(status);
	if (request
		&& WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
		&& WinHttpReceiveResponse(request, nullptr)
		&& WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)
		&& status == 200)
	{
		for (;;) {
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
			const size_t old = body.size();
			body.resize(old + avail);
			DWORD read = 0;
			if (!WinHttpReadData(request, &body[old], avail, &read)) { body.clear(); break; }
			body.resize(old + read);
			if (body.size() > 1024 * 1024) { body.clear(); break; } // not a server list
		}
	}
	if (request) WinHttpCloseHandle(request);
	if (connect) WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	return body;
}

// Value of a string field in a flat JSON object, empty if absent.
inline std::string JsonString(const std::string& obj, const char* key) {
	const std::string k = std::string("\"") + key + "\"";
	size_t p = obj.find(k);
	if (p == std::string::npos) return {};
	p = obj.find(':', p + k.size());
	if (p == std::string::npos) return {};
	p = obj.find_first_not_of(" \t\r\n", p + 1);
	if (p == std::string::npos || obj[p] != '"') return {};
	std::string out;
	for (++p; p < obj.size() && obj[p] != '"'; ++p) {
		if (obj[p] == '\\' && p + 1 < obj.size()) ++p;
		out.push_back(obj[p]);
	}
	return out;
}

// Value of a boolean field in a flat JSON object.
inline std::optional<bool> JsonBool(const std::string& obj, const char* key) {
	const std::string k = std::string("\"") + key + "\"";
	size_t p = obj.find(k);
	if (p == std::string::npos) return std::nullopt;
	p = obj.find(':', p + k.size());
	if (p == std::string::npos) return std::nullopt;
	p = obj.find_first_not_of(" \t\r\n", p + 1);
	if (p == std::string::npos) return std::nullopt;
	if (obj.compare(p, 4, "true") == 0) return true;
	if (obj.compare(p, 5, "false") == 0) return false;
	return std::nullopt;
}

// [{"ident":"AUTOMATIC","hostname_or_ip":"fsd.connect.vatsim.net",...,"is_sweatbox":false}, ...]
// The objects are flat, so splitting on braces is enough.
inline bool ParseServerList(const std::string& json, bool sweatboxList, std::vector<KnownServer>& out) {
	bool any = false;
	size_t pos = 0;
	while ((pos = json.find('{', pos)) != std::string::npos) {
		const size_t end = json.find('}', pos);
		if (end == std::string::npos) break;
		const std::string obj = json.substr(pos, end - pos + 1);
		pos = end + 1;

		KnownServer s;
		s.address = JsonString(obj, "hostname_or_ip");
		if (s.address.empty()) continue;
		s.name = JsonString(obj, "ident");
		if (s.name.empty()) s.name = JsonString(obj, "name");
		s.sweatbox = JsonBool(obj, "is_sweatbox").value_or(sweatboxList);
		out.push_back(std::move(s));
		any = true;
	}
	return any;
}

// VATSIM's server lists, fetched once per process. A failed fetch is retried at
// most once a minute; detection uses the built-in hosts meanwhile.
inline std::vector<KnownServer> VatsimServerList() {
	static std::mutex m;
	static std::vector<KnownServer> cached;
	static bool fetched = false;
	static std::chrono::steady_clock::time_point lastTry{};

	std::lock_guard<std::mutex> lock(m);
	const auto now = std::chrono::steady_clock::now();
	if (fetched || (lastTry != std::chrono::steady_clock::time_point{} && now - lastTry < std::chrono::minutes(1)))
		return cached;
	lastTry = now;

	std::vector<KnownServer> list;
	const bool live = ParseServerList(HttpGet(L"data.vatsim.net", L"/v3/vatsim-servers.json"), false, list);
	const bool sweat = ParseServerList(HttpGet(L"data.vatsim.net", L"/v3/sweatbox-servers.json"), true, list);
	if (live || sweat) cached = std::move(list);
	fetched = live && sweat;
	return cached;
}

inline const std::vector<KnownServer>& BuiltInServers() {
	static const std::vector<KnownServer> servers = {
		{"fsd.connect.vatsim.net", "AUTOMATIC", false},
		{"sweatbox.vatsim.net", "SWEATBOX", true},
		{"sweatbox-2.vatsim.net", "SWEATBOX-2", true},
	};
	return servers;
}

// First server whose address resolves to remoteIp. Within one source a live
// match wins, so a duplicate entry can never turn the live network into training.
inline const KnownServer* Match(const std::vector<KnownServer>& servers, const std::string& remoteIp) {
	const KnownServer* found = nullptr;
	for (const auto& s : servers) {
		if (!AddressMatches(s.address, remoteIp)) continue;
		if (!s.sweatbox) return &s;
		if (!found) found = &s;
	}
	return found;
}

struct WsaGuard {
	bool ok;
	WsaGuard() { WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
	~WsaGuard() { if (ok) WSACleanup(); }
};

} // namespace detail

std::optional<ServerInfo> ServerDetector::DetectNow(int esConnectionType, const std::string& euroscopeDataDir) {
	using namespace detail;
	WsaGuard wsa;

	// 1. Find the FSD socket: our own first, else (proxy) the master EuroScope's.
	std::vector<Endpoint> eps = FindFsdEndpoints();
	const Endpoint* ep = nullptr;
	for (const auto& e : eps)
		if (e.ownProcess) { ep = &e; break; }
	if (!ep && esConnectionType == kConnViaProxy) {
		// Only another EuroScope: a pilot client on the same PC also talks to :6809.
		for (const auto& e : eps)
			if (IsEuroScopeProcess(e.pid)) { ep = &e; break; }
	}
	if (!ep) return std::nullopt;

	ServerInfo info;
	info.esConnectionType = esConnectionType;
	info.remoteIp = ep->ip;
	info.viaOtherProcess = !ep->ownProcess;

	// 2. Identify the remote IP, most authoritative source first.
	const KnownServer* match = nullptr;
	const std::vector<KnownServer> vatsim = VatsimServerList();
	if ((match = Match(vatsim, info.remoteIp))) {
		info.source = MatchSource::VatsimList;
	} else if ((match = Match(BuiltInServers(), info.remoteIp))) {
		info.source = MatchSource::BuiltIn;
	}

	std::vector<KnownServer> fileServers;
	if (!match) {
		const std::string dir = euroscopeDataDir.empty() ? DefaultDataDir() : euroscopeDataDir;
		LoadServerFile(dir + "\\ipaddr.txt", fileServers);
		LoadServerFile(dir + "\\ipaddr_local.txt", fileServers);
		if ((match = Match(fileServers, info.remoteIp))) info.source = MatchSource::ServerFile;
	}

	if (match) {
		info.serverName = match->name;
		info.serverAddress = match->address;
		info.listMatched = true;
		info.network = match->sweatbox ? Network::Sweatbox : Network::Vatsim;
		return info;
	}

	// 3. Unknown server. Not knowing is not evidence of training: fall back to
	// what EuroScope says, and let the plugin double-check.
	info.source = MatchSource::Fallback;
	info.fromFallback = true;
	info.network = esConnectionType == kConnSweatbox ? Network::Sweatbox : Network::Vatsim;
	return info;
}

ServerDetector::ServerDetector(std::string euroscopeDataDir) : dataDir_(std::move(euroscopeDataDir)) {}

ServerDetector::~ServerDetector() {
	if (worker_.valid()) worker_.wait();
	for (auto& f : stale_) f.wait();
}

void ServerDetector::SetInfo(ServerInfo info) {
	const bool changed = info.network != info_.network || info.serverName != info_.serverName ||
		info.remoteIp != info_.remoteIp || info.esConnectionType != info_.esConnectionType ||
		info.source != info_.source;
	info_ = std::move(info);
	if (changed && onChange_) onChange_(info_);
}

void ServerDetector::StartWorker() {
	const int type = lastConnType_;
	const std::string dir = dataDir_;
	worker_ = std::async(std::launch::async, [type, dir] { return DetectNow(type, dir); });
	nextAttempt_ = std::chrono::steady_clock::now() + kRetryDelay;
	++attempts_;
}

void ServerDetector::PruneStale() {
	stale_.erase(std::remove_if(stale_.begin(), stale_.end(), [](auto& f) {
		return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
	}), stale_.end());
}

void ServerDetector::Update(int esConnectionType) {
	using clock = std::chrono::steady_clock;

	PruneStale();

	if (esConnectionType != lastConnType_) {
		// A running worker belongs to a stale connection: park it, drop its result.
		if (worker_.valid()) stale_.push_back(std::move(worker_));
		worker_ = {};
		lastConnType_ = esConnectionType;
		attempts_ = 0;

		ServerInfo info;
		info.esConnectionType = esConnectionType;
		switch (esConnectionType) {
		case kConnNo:
			info.network = Network::Offline;
			SetInfo(std::move(info));
			return;
		case kConnSimulatorServer:
		case kConnSimulatorClient:
		case kConnPlayback:
			info.network = Network::Simulator;
			SetInfo(std::move(info));
			return;
		default:
			info.network = Network::Pending;
			SetInfo(std::move(info));
			StartWorker();
			return;
		}
	}

	if (info_.network != Network::Pending) return;

	if (worker_.valid()) {
		if (worker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
		std::optional<ServerInfo> result = worker_.get();
		if (result) {
			SetInfo(std::move(*result));
			return;
		}
	}

	if (attempts_ >= kMaxAttempts) {
		// No FSD socket at all.
		ServerInfo info;
		info.esConnectionType = esConnectionType;
		info.source = MatchSource::Fallback;
		info.fromFallback = true;
		info.network = (esConnectionType == kConnSweatbox) ? Network::Sweatbox : Network::Vatsim;
		SetInfo(std::move(info));
		return;
	}

	if (clock::now() >= nextAttempt_) StartWorker();
}

} // namespace essd

#endif // ESSD_IMPLEMENTATION_DONE
#endif // ESSD_IMPLEMENTATION
