#include <algorithm>
#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include "CentralSquawk.h"
#include "version.h"
#include "core/Helpers.h"
#include "core/CompileCommands.h"
#include "core/TagFunctions.h"
#include "Secret.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

using namespace centralSquawk;
using namespace EuroScopePlugIn;

std::unique_ptr<centralSquawk::CentralSquawk> myPluginInstance = nullptr;

CentralSquawk::CentralSquawk() : CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE, "Central Squawk", PLUGIN_VERSION, "French vACC", "Open Source")
{
	m_stop.store(false, std::memory_order_relaxed);
	Initialize();
};

CentralSquawk::~CentralSquawk()
{
	Shutdown();
};


void __declspec (dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance)
{
	myPluginInstance.reset();
	myPluginInstance = std::make_unique<CentralSquawk>();
	*ppPlugInInstance = myPluginInstance.get();
}


void __declspec (dllexport) EuroScopePlugInExit()
{
	myPluginInstance.reset();
}

void CentralSquawk::Initialize()
{
	try
	{
		initialized_ = true;
		RegisterTagActions();

		// Start the persistent worker thread
		m_stop.store(false, std::memory_order_release);
		m_thread = std::thread(&CentralSquawk::WorkerThread, this);
	}
	catch (const std::exception& e)
	{
		DisplayError("Failed to initialize Central Squawk: " + std::string(e.what()));
	}
}

void CentralSquawk::Shutdown()
{
	if (initialized_)
	{
		initialized_ = false;
	}

	// Signal worker thread to stop with proper memory ordering
	m_stop.store(true, std::memory_order_release);

	// Wait for worker thread to finish
	if (m_thread.joinable())
		m_thread.join();

	DisplayMessage("Central Squawk shutdown complete");
}

void CentralSquawk::DisplayMessage(const std::string& message) {
	DisplayUserMessage("Central Squawk", "", message.c_str(), true, true, false, false, false);
}

void CentralSquawk::DisplayError(const std::string& message)
{
	DisplayUserMessage("Central Squawk", "ERROR", message.c_str(), true, true, true, true, true);
}

void CentralSquawk::QueueError(const std::string& message)
{
	std::lock_guard<std::mutex> lock(messageQueueMutex_);
	messageQueue_.push_back({message, true});
}

void CentralSquawk::QueueMessage(const std::string& message)
{
	std::lock_guard<std::mutex> lock(messageQueueMutex_);
	messageQueue_.push_back({message, false});
}

void CentralSquawk::WorkerThread() {
	std::string baseUrl = std::string("https://") + API_URL;

	// One client for every call. The scheme in the URL decides whether TLS is
	// used, so http:// and https:// endpoints are both reachable.
	auto cli = std::make_unique<httplib::Client>(baseUrl);
	cli->set_connection_timeout(2, 0);		 // 2s
	cli->set_read_timeout(3, 0);             // 3s
	cli->set_write_timeout(3, 0);            // 3s
	cli->set_keep_alive(true);

	size_t counter = 0;

	while (m_stop.load(std::memory_order_acquire) == false) {
		std::string userCallsign;
		{
			std::lock_guard<std::mutex> lock(userCallsignMutex_);
			userCallsign = userCallsign_;
		}

		// Manual requests first, so a controller's action is not delayed by up
		// to a full fetch interval before it is sent.
		if (isController_.load(std::memory_order_acquire)) {
			std::unordered_map<std::string, AssignRequest> pending;
			{
				// Swap the queue out rather than holding the lock across the
				// network calls: OnFunctionCall runs on the UI thread and must
				// never block on HTTP.
				std::lock_guard<std::mutex> lock(apiRequestQueueMutex_);
				pending.swap(pendingAssignRequests_);
			}
			for (const auto& [callsign, request] : pending) {
				SendAssignRequest(*cli, userCallsign, callsign, request);
			}
		}

		// Fetch the whole snapshot periodically.
		if (isConnected_.load(std::memory_order_acquire) && counter % (PERIODIC_FETCH_TIME_INTERVAL * 10) == 0) {
			FetchAssignedSSR(*cli);
		}

		++counter;
		std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Avoid busy waiting
	}
}

void CentralSquawk::FetchAssignedSSR(httplib::Client& cli)
{
	httplib::Headers headers = { {"User-Agent", "CentralSquawk"} };
	auto res = cli.Get("/squawk/api/squawks", headers);

	if (!res) {
		if (printError) {
			printError = false;
			QueueError("Cannot reach the Central Squawk server: " + httplib::to_string(res.error()));
		}
		return;
	}

	// The server answers 503 until its first reconciliation sweep completes.
	// That is expected on a server restart, not a fault, so keep the last
	// snapshot and stay quiet about it.
	if (res->status == 503) return;

	if (res->status < 200 || res->status >= 300) {
		if (printError) {
			printError = false;
			QueueError("Central Squawk server returned HTTP " + std::to_string(res->status));
		}
		return;
	}

	if (!printError) { // reset error printing flag on success
		QueueMessage("Reconnected to the Central Squawk server.");
		printError = true;
	}

	if (res->body.empty()) {
		QueueError("Received an empty snapshot from the Central Squawk server.");
		return;
	}

	std::unordered_map<std::string, SsrInfo> parsed;
	try {
		// Payload is a flat object keyed by callsign:
		//   {"AFR1234":{"ssr":"7201","dupe":false}, ...}
		const auto snapshot = nlohmann::json::parse(res->body);
		if (!snapshot.is_object()) {
			QueueError("Malformed snapshot: expected a JSON object.");
			return;
		}

		parsed.reserve(snapshot.size());
		for (const auto& [callsign, entry] : snapshot.items()) {
			if (!entry.is_object()) continue;

			const auto ssrIt = entry.find("ssr");
			if (ssrIt == entry.end() || !ssrIt->is_string()) continue;

			SsrInfo info;
			info.ssr = ssrIt->get<std::string>();
			if (!IsWellFormedSquawk(info.ssr)) continue;

			if (const auto dupeIt = entry.find("dupe");
				dupeIt != entry.end() && dupeIt->is_boolean()) {
				info.dupe = dupeIt->get<bool>();
			}

			parsed.emplace(ToUpper(callsign), std::move(info));
		}
	}
	catch (const std::exception& e) {
		QueueError("Failed to parse the Central Squawk snapshot: " + std::string(e.what()));
		return;
	}

	std::lock_guard<std::mutex> lock(SSRCacheMutex_);
	SSRCache_ = std::move(parsed);
}

void CentralSquawk::SendAssignRequest(httplib::Client& cli, const std::string& userCallsign,
									  const std::string& callsign, const AssignRequest& request)
{
	if (userCallsign.empty()) {
		QueueError("Not connected as a controller, cannot request an assignment.");
		return;
	}

	nlohmann::json body{
		{"callsign", callsign},
		{"controller", userCallsign},
		{"token", GenerateToken(userCallsign)},
	};
	// "code" wins over "mode": present means "set exactly this". Otherwise the
	// mode decides whether the server may hand back conspicuity ("auto") or
	// must draw a discrete code ("discrete").
	switch (request.kind) {
	case AssignRequest::Kind::SetCode:  body["code"] = request.code; break;
	case AssignRequest::Kind::Discrete: body["mode"] = "discrete";   break;
	case AssignRequest::Kind::Auto:     body["mode"] = "auto";       break;
	}

	httplib::Headers headers = { {"User-Agent", "CentralSquawk"} };
	auto res = cli.Post("/squawk/api/assign", headers, body.dump(), "application/json");

	if (!res) {
		QueueError("Assignment request for " + callsign + " failed: " + httplib::to_string(res.error()));
		return;
	}

	nlohmann::json response;
	if (!res->body.empty()) {
		try {
			response = nlohmann::json::parse(res->body);
		}
		catch (const std::exception&) {
			QueueError("Malformed response to the assignment request for " + callsign + ".");
			return;
		}
	}

	if (res->status < 200 || res->status >= 300) {
		std::string reason = "HTTP " + std::to_string(res->status);
		if (response.is_object()) {
			if (const auto errIt = response.find("error");
				errIt != response.end() && errIt->is_string()) {
				reason = errIt->get<std::string>();
			}
		}
		QueueError("Assignment refused for " + callsign + ": " + reason);
		return;
	}

	const auto ssrIt = response.is_object() ? response.find("ssr") : response.end();
	if (ssrIt == response.end() || !ssrIt->is_string()) {
		QueueError("Assignment response for " + callsign + " carried no code.");
		return;
	}

	SsrInfo info;
	info.ssr = ssrIt->get<std::string>();
	if (const auto dupeIt = response.find("dupe");
		dupeIt != response.end() && dupeIt->is_boolean()) {
		info.dupe = dupeIt->get<bool>();
	}

	{
		// Reflect it immediately rather than waiting for the next snapshot, so
		// the code reaches the aircraft on the next OnTimer.
		std::lock_guard<std::mutex> lock(SSRCacheMutex_);
		SSRCache_[callsign] = info;
	}

	QueueMessage("Squawk " + info.ssr + " assigned to " + callsign +
				 (info.dupe ? " (DUPE)" : ""));
}

void CentralSquawk::ApplyAssignments()
{
	if (!isConnected_.load(std::memory_order_acquire)) return;
	if (!isController_.load(std::memory_order_acquire)) return;

	std::unordered_map<std::string, SsrInfo> snapshot;
	{
		// Copy out: iterating EuroScope's flight plans while holding the lock
		// would block the worker thread for the whole sweep.
		std::lock_guard<std::mutex> lock(SSRCacheMutex_);
		snapshot = SSRCache_;
	}

	// An empty snapshot still has to be swept while anything is flagged: the server
	// having dropped every flight is exactly when a stale DUPE must be retracted.
	if (snapshot.empty() && !bridge_.HasPublished()) return;

	std::unordered_set<std::string> seen;

	for (auto fp = FlightPlanSelectFirst(); fp.IsValid(); fp = FlightPlanSelectNext(fp)) {
		const char* callsignPtr = fp.GetCallsign();
		if (callsignPtr == nullptr || *callsignPtr == '\0') continue;
		const std::string callsign = ToUpper(callsignPtr);
		seen.insert(callsign);

		const char* tracker = fp.GetTrackingControllerCallsign();
		const bool untracked = (tracker == nullptr || *tracker == '\0');
		const bool mine = fp.GetTrackingControllerIsMe();

		auto assigned = fp.GetControllerAssignedData();
		const auto it = snapshot.find(callsign);

		// --- DUPE -----------------------------------------------------------
		// Published for every flight in the snapshot, tracked or not. Bridge
		// values are local to this EuroScope instance, so unlike the flight strip
		// annotation this replaced there is no shared slot for two controllers to
		// overwrite, and therefore no need to elect an owner: every instance
		// derives the same flag from the same authoritative snapshot.
		bridge_.PublishDupe(callsign, it != snapshot.end() && it->second.dupe);

		// --- squawk ---------------------------------------------------------
		// Write for flights this controller tracks, and for flights nobody is
		// tracking at all: an untracked flight has no owner, so setting the
		// central code steps on nobody. A flight tracked by SOMEONE ELSE is
		// theirs to set, even though the central assignment still stands.
		if (!mine && !untracked) continue;
		if (it == snapshot.end()) continue;

		const std::string& wanted = it->second.ssr;
		if (!IsWellFormedSquawk(wanted)) continue;

		const char* current = assigned.GetSquawk();
		if (current != nullptr && wanted == current) continue; // already correct

		// The server is authoritative: a code set elsewhere is replaced.
		assigned.SetSquawk(wanted.c_str());
	}

	bridge_.ForgetAbsent(seen);
}

void CentralSquawk::OnTimer(int Counter) {
	std::ignore = Counter;

	// Update user state
	bool connected = IsConnected();
	isConnected_.store(connected, std::memory_order_release);
	bool controller = IsController();
	isController_.store(controller, std::memory_order_release);

	// Display queued messages from worker thread
	{
		std::lock_guard<std::mutex> lock(messageQueueMutex_);
		for (const auto& [msg, isError] : messageQueue_) {
			if (isError) DisplayError(msg);
			else DisplayMessage(msg);
		}
		messageQueue_.clear();
	}

	// Attach to the bridge and claim our provider id. Only worth doing once the
	// user is actually controlling: complaining about a missing bridge to
	// somebody sitting disconnected in the observer seat would be noise.
	if (connected && controller) bridge_.OnTimer();

	// Push central codes into EuroScope. This has to happen here rather than on
	// the worker thread: EuroScope is not thread safe.
	ApplyAssignments();
}

bool CentralSquawk::IsConnected()
{
	bool userIsConnected = this->GetConnectionType() == EuroScopePlugIn::CONNECTION_TYPE_DIRECT;
	return userIsConnected;
}

bool CentralSquawk::IsController()
{
	const std::string callsign = this->ControllerMyself().GetCallsign();
	if (callsign.size() < 3) return false;

	bool userIsObserver = callsign.substr(callsign.size() - 3) == "OBS" || this->ControllerMyself().GetFacility() == 0;

	std::lock_guard<std::mutex> lock(userCallsignMutex_);
	userCallsign_ = callsign;

	return !userIsObserver;
}

const std::string CentralSquawk::GenerateToken(const std::string& callsign)
{
	std::string s = AUTH_SECRET + callsign;
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
	std::ostringstream oss;
	oss << std::hex << std::setfill('0');
	for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
		oss << std::setw(2) << static_cast<int>(hash[i]);
	}
	return oss.str();
}
