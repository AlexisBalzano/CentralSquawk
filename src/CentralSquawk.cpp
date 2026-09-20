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

namespace {
	/// Turn the API's machine-readable rejection into something a controller can
	/// act on. Anything unrecognised passes through unchanged: a rejection this
	/// build has not heard of should still be visible, just less politely.
	std::string ExplainRejection(const std::string& error)
	{
		if (error == "unknown_callsign")   return "the server does not know this flight yet";
		if (error == "pool_exhausted")     return "every code range serving that destination is full";
		if (error == "excluded_code")      return "that code is reserved and cannot be assigned";
		if (error == "malformed_code")     return "that is not a valid squawk code";
		if (error == "not_authorised")     return "not authorised";
		if (error == "seed_malformed")     return "the flight plan is too incomplete to place this flight";
		if (error == "seed_out_of_scope")  return "this flight is outside the managed area";
		if (error == "seed_limit")         return "too many flights still awaiting the datafeed, try again shortly";
		if (error == "not_on_network")     return "this callsign is not logged on to that network. If you have "
												  "just connected, try again in a moment; if this is a training "
												  "session, run .centralsquawk status";
		return error;
	}
} // namespace

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
	messageQueue_.emplace_back<std::pair<std::string, bool>>({message, true});
}

void CentralSquawk::QueueMessage(const std::string& message)
{
	std::lock_guard<std::mutex> lock(messageQueueMutex_);
	messageQueue_.emplace_back<std::pair<std::string, bool>>({message, false});
}

void CentralSquawk::WorkerThread() {
	// One client for every call. The scheme in the URL decides whether TLS is
	// used, so http:// and https:// endpoints are both reachable. It is rebuilt
	// whenever the endpoint changes, which happens when the user connects to a
	// simulator: the live and simulator servers are separate deployments, and
	// pointing one client at both in turn is the whole of switching worlds.
	std::unique_ptr<httplib::Client> cli;
	std::string builtFor;

	const auto configure = [](httplib::Client& c) {
		c.set_connection_timeout(2, 0);       // 2s
		c.set_read_timeout(3, 0);             // 3s
		c.set_write_timeout(3, 0);            // 3s
		c.set_keep_alive(true);
	};

	const auto clientFor = [&](const std::string& baseUrl) -> httplib::Client& {
		if (!cli || builtFor != baseUrl) {
			cli = std::make_unique<httplib::Client>(baseUrl);
			configure(*cli);
			builtFor = baseUrl;
		}
		return *cli;
	};

	// A second client, pinned to the live server for as long as the plugin
	// runs. "Is this callsign on VATSIM" is only ever the live server's
	// question to answer, including -- especially -- while we are pointed at
	// the simulator, since that is how a session is noticed to have ended.
	httplib::Client liveCli(LiveEndpoint());
	configure(liveCli);

	size_t counter = 0;
	/// Counter value before the next push may be attempted. Pushed forward when
	/// another client holds the feeder lease.
	size_t nextFeedAttempt = 0;
	/// When the live server first said it did not know this callsign, held here
	/// rather than in a member so the confirmation window is single threaded.
	std::chrono::steady_clock::time_point offNetworkSince{};
	bool offNetwork = false;

	while (m_stop.load(std::memory_order_acquire) == false) {
		const NetworkMode mode = ResolveMode();
		if (mode == NetworkMode::Offline) {
			// A disconnect ends whatever we had inferred: the next session gets
			// to be judged on its own, not on the last one's verdict.
			offNetwork = false;
			inferredSim_.store(false, std::memory_order_release);
			++counter;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		httplib::Client& client = clientFor(EndpointFor(mode));

		std::string userCallsign;
		{
			std::lock_guard<std::mutex> lock(userCallsignMutex_);
			userCallsign = userCallsign_;
		}

		// Which world is this? Asked only when EuroScope's own answer was
		// inconclusive -- it said an ordinary connection, which is what both a
		// real controller and a student on a training server look like.
		if (!userCallsign.empty()
			&& modeOverride_.load(std::memory_order_acquire) == ModeOverride::Auto
			&& connectionMode_.load(std::memory_order_acquire) == NetworkMode::Live
			&& counter % (NETWORK_PROBE_INTERVAL * 10) == 0)
		{
			switch (ProbeNetwork(liveCli, userCallsign)) {
			case NetworkVerdict::Online:
				offNetwork = false;
				inferredSim_.store(false, std::memory_order_release);
				break;

			case NetworkVerdict::Offline: {
				const auto now = std::chrono::steady_clock::now();
				if (!offNetwork) {
					offNetwork = true;
					offNetworkSince = now;
					break;
				}
				// Believed only after it has held for a while: a controller who
				// has just logged on really is absent from the datafeed for a
				// generation or two, and bouncing them to the simulator server
				// over that would be worse than making them wait.
				const auto held = std::chrono::duration_cast<std::chrono::seconds>(now - offNetworkSince);
				if (held.count() >= OFF_NETWORK_CONFIRM
					&& !inferredSim_.exchange(true, std::memory_order_acq_rel)) {
					QueueMessage("This callsign is not logged on to VATSIM, so this is a simulator "
								 "session: switching to the simulator squawk server. "
								 "Use .centralsquawk mode live to override.");
				}
				break;
			}

			case NetworkVerdict::Unknown:
				// Not knowing is not evidence. Leave the verdict where it was.
				break;
			}
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
				SendAssignRequest(client, userCallsign, callsign, request);
			}
		}

		// The simulator picture. Nobody else is going to describe this world, so
		// one client has to, and the server decides which one that is.
		if (mode == NetworkMode::Sweatbox && !userCallsign.empty() && counter >= nextFeedAttempt) {
			std::vector<FlightObservation> picture;
			bool havePicture = false;
			{
				std::lock_guard<std::mutex> lock(pictureMutex_);
				if (pictureReady_) {
					picture.swap(picture_);
					pictureReady_ = false;
					havePicture = true;
				}
			}

			if (havePicture) {
				// Only a lost lease backs off. A network error means the next
				// push is as urgent as this one was, not less.
				const FeedOutcome outcome = PushSimFeed(client, userCallsign, std::move(picture));
				const int wait = outcome == FeedOutcome::Displaced
					? SIM_FEED_RETRY_INTERVAL
					: SIM_PUSH_TIME_INTERVAL;
				nextFeedAttempt = counter + static_cast<size_t>(wait) * 10;
			}
			else {
				// Ask the main thread for one; it fills the buffer on its next
				// OnTimer and this branch collects it a tenth of a second later.
				pictureRequested_.store(true, std::memory_order_release);
			}
		}

		// Fetch the whole snapshot periodically.
		if (counter % (PERIODIC_FETCH_TIME_INTERVAL * 10) == 0) {
			FetchAssignedSSR(client);
		}

		++counter;
		std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Avoid busy waiting
	}
}

void CentralSquawk::FetchAssignedSSR(httplib::Client& cli)
{
	httplib::Headers headers = { {"User-Agent", "CentralSquawk"} };
	auto res = cli.Get("/api/squawks", headers);

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

	// Sent on every request, not only when we suspect the server is behind: the
	// plugin has no way to know what the datafeed has reached, and the server
	// discards it the moment the feed carries the callsign. Note the absent
	// `transponder`: see FlightObservation for why it is never sent here.
	if (request.seed.valid) {
		body["flight"] = {
			{"latitude",    request.seed.latitude},
			{"longitude",   request.seed.longitude},
			{"altitude",    request.seed.altitude},
			{"groundspeed", request.seed.groundspeed},
			{"flightRules", request.seed.flightRules},
			{"departure",   request.seed.departure},
			{"arrival",     request.seed.arrival},
			{"equipment",   request.seed.equipment},
			{"route",       request.seed.route},
		};
	}

	httplib::Headers headers = { {"User-Agent", "CentralSquawk"} };
	auto res = cli.Post("/api/assign", headers, body.dump(), "application/json");

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
				reason = ExplainRejection(errIt->get<std::string>());
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
}

CentralSquawk::FeedOutcome CentralSquawk::PushSimFeed(httplib::Client& cli, const std::string& userCallsign,
													  std::vector<FlightObservation> picture)
{
	// Note that a displaced client still sends the WHOLE picture when it probes
	// for the lease, rather than something cheaper. Winning the lease with an
	// empty push would tick the engine against an empty world and start the
	// grace clock on every flight in the session.
	const auto announce = [&](bool nowFeeding, const std::string& message, bool isError) {
		// Said once per change of state: every client but one is displaced, and
		// saying so every thirty seconds for a whole session is just noise.
		if (feeding_.exchange(nowFeeding, std::memory_order_acq_rel) == nowFeeding) return;
		if (isError) QueueError(message);
		else QueueMessage(message);
	};

	nlohmann::json observations = nlohmann::json::array();
	for (const auto& obs : picture) {
		if (!obs.valid || obs.callsign.empty()) continue;
		observations.push_back({
			{"callsign",    obs.callsign},
			{"latitude",    obs.latitude},
			{"longitude",   obs.longitude},
			{"altitude",    obs.altitude},
			{"groundspeed", obs.groundspeed},
			{"transponder", obs.transponder},
			{"flightRules", obs.flightRules},
			{"departure",   obs.departure},
			{"arrival",     obs.arrival},
			{"equipment",   obs.equipment},
			{"route",       obs.route},
		});
	}

	nlohmann::json body{
		{"controller", userCallsign},
		{"token", GenerateToken(userCallsign)},
		{"observations", std::move(observations)},
	};

	httplib::Headers headers = { {"User-Agent", "CentralSquawk"} };
	auto res = cli.Post("/api/feed", headers, body.dump(), "application/json");

	if (!res) {
		announce(false, "Lost contact with the simulator squawk server: " + httplib::to_string(res.error()), true);
		return FeedOutcome::Failed;
	}

	// Somebody else is already describing this world, which is the normal
	// outcome for every client but one.
	if (res->status == 409) {
		announce(false, "Another controller is feeding the simulator session.", false);
		return FeedOutcome::Displaced;
	}

	if (res->status < 200 || res->status >= 300) {
		announce(false, "The simulator squawk server refused the feed: HTTP " + std::to_string(res->status), true);
		return FeedOutcome::Failed;
	}

	announce(true, "Feeding the simulator squawk server with this session's traffic.", false);
	return FeedOutcome::Fed;
}

void CentralSquawk::ApplyAssignments()
{
	if (!IsOnline()) return;
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

std::vector<FlightObservation> CentralSquawk::CapturePicture()
{
	std::vector<FlightObservation> picture;

	// Radar targets rather than flight plans: the server needs where each
	// aircraft IS and what it is squawking, and only a target carries either.
	// An aircraft with no flight plan still belongs in the picture -- the server
	// will decline to assign it, but it must still see the code it is squawking,
	// or it will hand that code to somebody else.
	for (auto rt = RadarTargetSelectFirst(); rt.IsValid(); rt = RadarTargetSelectNext(rt)) {
		const std::string callsign = ToUpper(SafeString(rt.GetCallsign()));
		if (callsign.empty()) continue;

		const auto position = rt.GetPosition();
		if (!position.IsValid()) continue;

		FlightObservation obs;
		obs.callsign = callsign;
		const auto coordinates = position.GetPosition();
		obs.latitude = coordinates.m_Latitude;
		obs.longitude = coordinates.m_Longitude;
		obs.altitude = position.GetPressureAltitude();
		obs.groundspeed = rt.GetGS();
		obs.transponder = SafeString(position.GetSquawk());

		if (auto fp = rt.GetCorrelatedFlightPlan(); fp.IsValid()) {
			if (auto data = fp.GetFlightPlanData(); data.IsReceived()) {
				obs.flightRules = ToUpper(SafeString(data.GetPlanType()));
				obs.departure = ToUpper(SafeString(data.GetOrigin()));
				obs.arrival = ToUpper(SafeString(data.GetDestination()));
				obs.equipment = ToUpper(SafeString(data.GetAircraftInfo()));
				obs.route = ToUpper(SafeString(data.GetRoute()));
			}
		}

		obs.valid = true;
		picture.push_back(std::move(obs));
	}

	return picture;
}

void CentralSquawk::ServePictureRequest()
{
	if (!pictureRequested_.load(std::memory_order_acquire)) return;

	// Captured before the lock is taken: walking every radar target while
	// holding it would block the worker thread for the whole sweep.
	auto picture = CapturePicture();

	std::lock_guard<std::mutex> lock(pictureMutex_);
	picture_ = std::move(picture);
	pictureReady_ = true;
	pictureRequested_.store(false, std::memory_order_release);
}

void CentralSquawk::OnTimer(int Counter) {
	std::ignore = Counter;

	// Update user state. Only the connection type is read here; the rest of the
	// mode is resolved from it, the override, and the live server's verdict.
	connectionMode_.store(DetectConnectionMode(), std::memory_order_release);
	const NetworkMode mode = ResolveMode();
	const bool connected = mode != NetworkMode::Offline;
	bool controller = IsController();
	isController_.store(controller, std::memory_order_release);

	if (mode != announcedMode_) {
		const NetworkMode previousMode = announcedMode_;
		announcedMode_ = mode;

		if (previousMode == NetworkMode::Sweatbox && mode == NetworkMode::Live) {
			// Nothing to hand back: the feeder lease is the server's to grant,
			// and it expires on its own a few seconds from now.
			feeding_.store(false, std::memory_order_release);
			if (connected) QueueMessage("Back on the live squawk server.");
		}
	}

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

	// Same reason: only this thread may walk radar targets.
	if (mode == NetworkMode::Sweatbox) ServePictureRequest();
}

NetworkMode CentralSquawk::DetectConnectionMode()
{
	// Only half the answer, and the weaker half. EuroScope reports SWEATBOX for
	// the machine actually running the simulator session, but a student who
	// connects normally and simply picks a training server as their server is
	// reported as DIRECT, indistinguishable from the real network. A Sweatbox
	// answer here is therefore conclusive; a Live answer is only a starting
	// point, which ResolveMode then tests against the live server.
	switch (auto it = this->GetConnectionType()) {
	case EuroScopePlugIn::CONNECTION_TYPE_DIRECT:
	case EuroScopePlugIn::CONNECTION_TYPE_VIA_PROXY:
		return NetworkMode::Live;

	case EuroScopePlugIn::CONNECTION_TYPE_SWEATBOX:
	case EuroScopePlugIn::CONNECTION_TYPE_SIMULATOR_SERVER:
	case EuroScopePlugIn::CONNECTION_TYPE_SIMULATOR_CLIENT:
		return NetworkMode::Sweatbox;

	// Playback is a recording: there is nobody to assign a code to, and writing
	// one would only corrupt the replay.
	case EuroScopePlugIn::CONNECTION_TYPE_PLAYBACK:
	default:
		return NetworkMode::Offline;
	}
}

NetworkMode CentralSquawk::ResolveMode() const
{
	if (connectionMode_.load(std::memory_order_acquire) == NetworkMode::Offline) {
		return NetworkMode::Offline;
	}

	switch (modeOverride_.load(std::memory_order_acquire)) {
	case ModeOverride::ForceSim:  return NetworkMode::Sweatbox;
	case ModeOverride::ForceLive: return NetworkMode::Live;
	case ModeOverride::Auto:      break;
	}

	// EuroScope says simulator only when it is certain, so take it at its word.
	if (connectionMode_.load(std::memory_order_acquire) == NetworkMode::Sweatbox) {
		return NetworkMode::Sweatbox;
	}

	// Otherwise the connection looks ordinary, which a student on a training
	// server also looks like. The live server settles it.
	return inferredSim_.load(std::memory_order_acquire) ? NetworkMode::Sweatbox : NetworkMode::Live;
}

std::string CentralSquawk::EndpointFor(NetworkMode mode)
{
	if (mode != NetworkMode::Sweatbox) return LiveEndpoint();

	return std::string("https://") + SIM_API_URL;
}

CentralSquawk::NetworkVerdict CentralSquawk::ProbeNetwork(httplib::Client& liveCli,
														 const std::string& userCallsign)
{
	httplib::Headers headers = { {"User-Agent", "CentralSquawk"} };
	auto res = liveCli.Get("/api/network?controller=" + userCallsign, headers);

	// Silence on every failure path. Not knowing is not evidence of anything,
	// and this runs every fifteen seconds for the whole session.
	if (!res || res->status < 200 || res->status >= 300) return NetworkVerdict::Unknown;

	try {
		const auto body = nlohmann::json::parse(res->body);
		const auto it = body.find("onNetwork");
		if (it == body.end() || !it->is_boolean()) return NetworkVerdict::Unknown;
		return it->get<bool>() ? NetworkVerdict::Online : NetworkVerdict::Offline;
	}
	catch (const std::exception&) {
		return NetworkVerdict::Unknown;
	}
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
