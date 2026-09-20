#pragma once
#include <Windows.h>
#include <EuroScopePlugIn.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/BridgePublisher.h"

// Only referenced through a reference in declarations below, so the full
// httplib header (which is large, and needs its OpenSSL defines set first) is
// pulled in by the translation unit rather than by everyone including this.
namespace httplib { class Client; }

using namespace EuroScopePlugIn;

namespace centralSquawk {

	class CentralSquawk;

	/// One entry of the server snapshot: `{"ssr":"7201","dupe":false}`.
	struct SsrInfo {
		std::string ssr;
		bool dupe = false;
	};

	// Values are part of the cross-plugin contract: CoFrance invokes these by
	// number through StartTagFunction. Append only; never renumber.
	enum TagActionID : int {
		OpenMENU = 0,
		AssignAuto,      // re-run the server's decision, Mode S included
		AssignCode,      // force a code the controller types
		AssignCurrent,   // force the code the aircraft is already squawking
		AssignDiscrete,  // force a pool code, bypassing Mode S
	};

	/// Which network the user is on. It decides both what the plugin is allowed
	/// to do and which server it talks to.
	///
	/// EuroScope's connection type is NOT sufficient to work this out. It
	/// reports SWEATBOX only for the machine actually running the simulator
	/// session; a student who connects normally and simply picks a training
	/// server shows up as DIRECT, indistinguishable from VATSIM. So the mode is
	/// resolved from three things, in this order of authority:
	///
	///   1. a manual override, if the controller set one;
	///   2. the connection type, which is only ever conclusive when it says
	///      simulator;
	///   3. the live server's answer to "is this callsign on my network?",
	///      which is the only thing that can tell a student apart from a
	///      controller, and which the live server enforces anyway.
	enum class NetworkMode {
		/// Not connected, or replaying a recording. Nothing to assign.
		Offline,
		/// VATSIM. The server reads the datafeed and needs nothing from us.
		Live,
		/// A simulator: sweatbox, or a local sim server. No datafeed describes
		/// this world, so one client has to supply the picture itself.
		Sweatbox,
	};

	/// `.centralsquawk mode`. An escape hatch, not a safety mechanism: the live
	/// server refuses a callsign it cannot see whatever this says, so forcing
	/// the wrong one costs codes rather than correctness.
	enum class ModeOverride { Auto, ForceLive, ForceSim };

	/// One aircraft as the VATSIM datafeed would have described it.
	///
	/// Serialised two different ways, because it is trusted two different ways:
	///
	///  - As the `flight` on a manual request (see SeedJson), for a flight the
	///    datafeed has not reached yet. A controller asks for a code the moment
	///    a pilot connects, which is well before the feed carries them, and
	///    EuroScope already holds the flight plan over the same FSD connection
	///    the feed is built from. The `transponder` is NOT sent: the server
	///    reserves every observed code before allocating, so a code taken on a
	///    client's word would let any client drain the live pool. A controller
	///    adopting the code an aircraft really is squawking sends it as `code`.
	///
	///  - As one entry of a pushed feed (see ObservationJson), in Sweatbox mode.
	///    There the whole struct is sent, `transponder` included, because the
	///    push is not competing with a datafeed -- it is the only description of
	///    that world there will ever be.
	struct FlightObservation {
		bool valid = false;
		std::string callsign;
		double latitude = 0.0;
		double longitude = 0.0;
		int altitude = 0;
		int groundspeed = 0;
		std::string transponder;  // what the aircraft is actually squawking
		std::string flightRules;  // field 8: I, V, Y or Z
		std::string departure;
		std::string arrival;
		std::string equipment;    // field 10, unextracted: B738/M-SDE3FGHIRWY/LB1
		std::string route;        // field 15
	};

	/// A manual request waiting for the worker thread to send it.
	struct AssignRequest {
		enum class Kind {
			Auto,      // server decides, and may hand back 1000
			Discrete,  // pool code regardless of Mode S eligibility
			SetCode,   // exactly `code`
		};
		Kind kind = Kind::Auto;
		std::string code;  // only meaningful for SetCode
		/// Gathered on the main thread when the request was queued: the worker
		/// cannot read a flight plan, because EuroScope is not thread safe.
		FlightObservation seed;
	};


	class CentralSquawk : public EuroScopePlugIn::CPlugIn
	{
		static constexpr int PERIODIC_FETCH_TIME_INTERVAL = 5; // seconds
		/// How often the feeder pushes the simulator picture. Matched to the
		/// snapshot poll so a controller sees a code about as fast as on VATSIM.
		static constexpr int SIM_PUSH_TIME_INTERVAL = 5;      // seconds
		/// After losing the feeder lease, how long before trying to claim again.
		/// Someone else is feeding, so this is a takeover probe, not a retry.
		static constexpr int SIM_FEED_RETRY_INTERVAL = 30;    // seconds
		/// How often to ask the live server whether this callsign is on VATSIM.
		static constexpr int NETWORK_PROBE_INTERVAL = 15;     // seconds
		/// How long the answer must stay "no" before believing it.
		///
		/// A controller who has just logged on is genuinely absent from the
		/// datafeed for a generation or two, and bouncing them onto the
		/// simulator server for that would be worse than waiting.
		static constexpr int OFF_NETWORK_CONFIRM = 60;        // seconds

		static constexpr const char* API_URL = "squawk.vatsim.fr";
		/// The simulator instance: a separate deployment with its own pool, so a
		/// sweatbox can never consume a code real traffic is using.
		static constexpr const char* SIM_API_URL = "sweatbox.squawk.vatsim.fr";

	public:
		CentralSquawk();
		~CentralSquawk();

	public:
		// Plugin lifecycle methods
		void Initialize();
		void Shutdown();

		// Message management
		void DisplayMessage(const std::string& message);
		void QueueMessage(const std::string& message); // Needed since Euroscope is not threadsafe
		void DisplayError(const std::string& message);
		void QueueError(const std::string& message); // Needed since Euroscope is not threadsafe

		// Scope events
		void OnTimer(int Counter) override;
		void OnFunctionCall(int functionId, const char* itemString, POINT pt, RECT area) override;
		bool OnCompileCommand(const char* sCommandLine) override;

		// Tag function management. No tag ITEMS yet: the menu is bound to an
		// existing tag item (EuroScope's own squawk field, typically) from the
		// tag settings dialog.
		void RegisterTagActions();


	private:
		bool IsController();
		/// Read EuroScope's connection type. MUST run on the main thread.
		NetworkMode DetectConnectionMode();
		/// Combine connection type, override and the live server's verdict.
		/// Cheap and lock free, so either thread may call it.
		NetworkMode ResolveMode() const;
		/// Whether the plugin should be doing anything at all.
		bool IsOnline() const { return ResolveMode() != NetworkMode::Offline; }
		/// Base URL for a mode, honouring the per-profile simulator override.
		std::string EndpointFor(NetworkMode mode);
		std::string LiveEndpoint() const { return std::string("https://") + API_URL; }

		void WorkerThread();
		void FetchAssignedSSR(httplib::Client& cli);
		void SendAssignRequest(httplib::Client& cli, const std::string& userCallsign,
							   const std::string& callsign, const AssignRequest& request);
		/// What came of a push, which decides how soon to try again.
		enum class FeedOutcome {
			Fed,        // accepted: keep feeding at the normal interval
			Displaced,  // another client holds the lease: probe again rarely
			Failed,     // network or server error: retry at the normal interval
		};
		/// Send the captured picture to a push-mode server. Sweatbox only.
		FeedOutcome PushSimFeed(httplib::Client& cli, const std::string& userCallsign,
								std::vector<FlightObservation> picture);

		/// What the live server said about this callsign's presence on VATSIM.
		enum class NetworkVerdict { Online, Offline, Unknown };
		/// Ask the live server whether this callsign is logged on to VATSIM.
		/// Always asked of the LIVE server, whichever one we are using.
		NetworkVerdict ProbeNetwork(httplib::Client& liveCli, const std::string& userCallsign);
		const std::string GenerateToken(const std::string& controllerCallsign);

		/// Push central codes into EuroScope for flights this controller tracks.
		/// MUST run on the main thread: EuroScope is not thread safe.
		void ApplyAssignments();

		/// Walk every radar target and describe it as the datafeed would.
		/// MUST run on the main thread: EuroScope is not thread safe.
		std::vector<FlightObservation> CapturePicture();
		/// Hand the worker a picture if it has asked for one. Main thread.
		void ServePictureRequest();

	private:
		// Plugin state
		bool initialized_ = false;
		bool printError = true;
		std::atomic<bool> m_stop{false};
		std::thread m_thread;


		// Message management
		std::mutex messageQueueMutex_;
		std::vector<std::pair<std::string, bool>> messageQueue_; // Pair of message and isError flag

		// User state
		std::mutex userCallsignMutex_;
		std::atomic<bool> isController_ = false;
		/// What EuroScope reports. Conclusive only when it says Sweatbox.
		std::atomic<NetworkMode> connectionMode_{ NetworkMode::Offline };
		/// Set by the worker once the live server has denied knowing this
		/// callsign for long enough to believe it. The confirmation timer lives
		/// in the worker thread, so only the settled answer is published here.
		std::atomic<bool> inferredSim_{ false };
		std::atomic<ModeOverride> modeOverride_{ ModeOverride::Auto };
		/// Last mode OnTimer told the controller about, so a switch is announced
		/// once. Main thread only, hence no atomic.
		NetworkMode announcedMode_ = NetworkMode::Offline;
		std::string userCallsign_;

		// SSR data cache: callsign -> central assignment
		std::mutex SSRCacheMutex_;
		std::unordered_map<std::string, SsrInfo> SSRCache_;

		/// Publishes the DUPE flag on the EuroScope Plugin Bridge, where CoFrance
		/// reads it. Only touched from OnTimer, so it needs no lock.
		BridgePublisher bridge_{ this };

		// Manual requests waiting to be sent by the worker thread.
		std::mutex apiRequestQueueMutex_;
		std::unordered_map<std::string, AssignRequest> pendingAssignRequests_;

		// --- simulator feed -------------------------------------------------
		// Only the main thread may read radar targets, and only the worker may
		// make HTTP calls, so the picture is handed across explicitly: the
		// worker asks, the next OnTimer fills, the worker sends. Capturing on a
		// schedule instead would walk every radar target once a second whether
		// or not anything was going to be sent.
		std::atomic<bool> pictureRequested_{false};
		std::mutex pictureMutex_;
		std::vector<FlightObservation> picture_;
		bool pictureReady_ = false;
		/// Whether this client currently holds the feeder lease. Written by the
		/// worker, read by `.centralsquawk status` on the main thread.
		std::atomic<bool> feeding_{false};
	};
} // namespace centralSquawk
