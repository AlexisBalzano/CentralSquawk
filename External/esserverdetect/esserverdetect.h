/*  esserverdetect.h  --  EuroScope server detection, single header, version 1
 *
 *  Tells a plugin which server EuroScope is REALLY connected to, independent of
 *  CPlugIn::GetConnectionType(). EuroScope reports CONNECTION_TYPE_DIRECT
 *  whenever "Connect to VATSIM" is ticked, even when the server picked in the
 *  connect dialog is SweatBox -- so the SDK value cannot tell live VATSIM from
 *  SweatBox.
 *
 *  HOW IT WORKS (reverse-engineered from EuroScope 3.2.x)
 *    - The connect dialog's server drop-down is filled from
 *      %APPDATA%\EuroScope\ipaddr.txt (+ ipaddr_local.txt), one
 *      "<address> <name>" pair per line, e.g.
 *          134.122.97.128 AUTOMATIC
 *          sweatbox.vatsim.net SweatBox
 *          sweatbox-2.vatsim.net SweatBox-2
 *    - On connect EuroScope opens a TCP socket to <address>:6809 (FSD).
 *      The chosen name is only kept in the in-memory profile
 *      ("LastSession server"), which plugins cannot read.
 *    - This header finds that socket in the OS TCP table (owned by the
 *      EuroScope process, remote port 6809), resolves every ipaddr.txt address
 *      and matches the remote IP back to the drop-down name.
 *    - Via proxy: the socket belongs to the master EuroScope instance, so any
 *      local process with an established :6809 connection is used.
 *
 *  CLASSIFICATION
 *      server name "AUTOMATIC"           -> Network::Vatsim
 *      any other name / custom address   -> Network::Sweatbox
 *      simulator server/client, playback -> Network::Simulator
 *      not connected                     -> Network::Offline
 *      detection still running           -> Network::Pending
 *    If no :6809 socket can be found after kMaxAttempts, it falls back to the
 *    EuroScope connection type (info.fromFallback = true).
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
 *    scan and DNS lookups run on a worker thread; Update() never blocks.
 *    The change callback is invoked from Update(), i.e. on the main thread.
 *
 *  Links: ws2_32.lib, iphlpapi.lib (auto-linked via #pragma comment).
 */
#ifndef ESSERVERDETECT_H_INCLUDED
#define ESSERVERDETECT_H_INCLUDED

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <string>

namespace essd {

constexpr int kVersion = 1;
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
	Vatsim,    // live VATSIM network ("AUTOMATIC")
	Sweatbox,  // SweatBox / any non-AUTOMATIC server / custom address
	Simulator, // EuroScope simulator server/client or playback
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

struct ServerInfo {
	Network network = Network::Offline;
	int esConnectionType = kConnNo; // what GetConnectionType() reported
	std::string serverName;         // ipaddr.txt name ("AUTOMATIC", "SweatBox"...); empty if unmatched
	std::string serverAddress;      // ipaddr.txt address of the matched entry
	std::string remoteIp;           // remote IP of the FSD socket
	bool listMatched = false;       // remote IP matched an ipaddr.txt entry
	bool viaOtherProcess = false;   // socket found in another process (proxy master)
	bool fromFallback = false;      // no socket found; derived from esConnectionType
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

	// Synchronous one-shot detection (blocks on DNS). Returns nullopt if no FSD socket.
	static std::optional<ServerInfo> DetectNow(int esConnectionType, const std::string& euroscopeDataDir);

private:
	void SetInfo(ServerInfo info);
	void StartWorker();

	std::string dataDir_;
	ServerInfo info_;
	ChangeCallback onChange_;
	int lastConnType_ = -1;
	int attempts_ = 0;
	std::chrono::steady_clock::time_point nextAttempt_{};
	std::future<std::optional<ServerInfo>> worker_;
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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "shell32.lib")

namespace essd {
namespace detail {

struct ServerEntry {
	std::string address;
	std::string name;
};

struct Endpoint {
	std::string ip;
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
			out.push_back({Ipv4ToString(r.dwRemoteAddr), r.dwOwningPid == myPid});
		}
	}
	if (query(AF_INET6, buf)) {
		auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
		for (DWORD i = 0; i < t->dwNumEntries; ++i) {
			const auto& r = t->table[i];
			if (r.dwState != MIB_TCP_STATE_ESTAB) continue;
			if (ntohs(static_cast<u_short>(r.dwRemotePort)) != kFsdPort) continue;
			out.push_back({Ipv6ToString(r.ucRemoteAddr), r.dwOwningPid == myPid});
		}
	}
	return out;
}

inline void LoadServerFile(const std::string& path, std::vector<ServerEntry>& out) {
	std::ifstream f(path);
	std::string line;
	while (std::getline(f, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == ';') continue;
		size_t sp = line.find_first_of(" \t");
		if (sp == std::string::npos) continue;
		ServerEntry e{line.substr(0, sp), Trim(line.substr(sp + 1))};
		if (!e.address.empty() && !e.name.empty()) out.push_back(std::move(e));
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

struct WsaGuard {
	bool ok;
	WsaGuard() { WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
	~WsaGuard() { if (ok) WSACleanup(); }
};

} // namespace detail

std::optional<ServerInfo> ServerDetector::DetectNow(int esConnectionType, const std::string& euroscopeDataDir) {
	using namespace detail;
	WsaGuard wsa;

	// 1. Find the FSD socket: our own first, else (proxy) the master instance's.
	std::vector<Endpoint> eps = FindFsdEndpoints();
	const Endpoint* ep = nullptr;
	for (const auto& e : eps)
		if (e.ownProcess) { ep = &e; break; }
	if (!ep && esConnectionType == kConnViaProxy && !eps.empty()) ep = &eps.front();
	if (!ep) return std::nullopt;

	ServerInfo info;
	info.esConnectionType = esConnectionType;
	info.remoteIp = ep->ip;
	info.viaOtherProcess = !ep->ownProcess;

	// 2. Map the remote IP back to the connect-dialog server name.
	const std::string dir = euroscopeDataDir.empty() ? DefaultDataDir() : euroscopeDataDir;
	std::vector<ServerEntry> servers;
	LoadServerFile(dir + "\\ipaddr.txt", servers);
	LoadServerFile(dir + "\\ipaddr_local.txt", servers);

	for (const auto& s : servers) {
		bool match = false;
		if (IsLoopback(info.remoteIp) && (IEquals(s.address, "localhost") || IEquals(s.address, "loopback")))
			match = true;
		if (!match) {
			for (const auto& ip : Resolve(s.address))
				if (ip == info.remoteIp) { match = true; break; }
		}
		if (match) {
			info.serverName = s.name;
			info.serverAddress = s.address;
			info.listMatched = true;
			break;
		}
	}

	// 3. Only AUTOMATIC is the live network; everything else is training.
	info.network = (info.listMatched && IEquals(info.serverName, "AUTOMATIC")) ? Network::Vatsim : Network::Sweatbox;
	return info;
}

ServerDetector::ServerDetector(std::string euroscopeDataDir) : dataDir_(std::move(euroscopeDataDir)) {}

ServerDetector::~ServerDetector() {
	if (worker_.valid()) worker_.wait();
}

void ServerDetector::SetInfo(ServerInfo info) {
	const bool changed = info.network != info_.network || info.serverName != info_.serverName ||
		info.remoteIp != info_.remoteIp || info.esConnectionType != info_.esConnectionType;
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

void ServerDetector::Update(int esConnectionType) {
	using clock = std::chrono::steady_clock;

	if (esConnectionType != lastConnType_) {
		// A previous worker belongs to a stale connection: let it finish, drop its result.
		if (worker_.valid()) worker_.wait();
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
		ServerInfo info;
		info.esConnectionType = esConnectionType;
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
