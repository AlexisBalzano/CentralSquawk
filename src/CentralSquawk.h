#pragma once
#include <Windows.h>
#include <EuroScopePlugIn.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

	enum TagActionID : int {
		OpenMENU = 0,
		AssignAuto,     // ask the server for a fresh code
		AssignCode,     // force a code the controller types
		AssignCurrent,  // force the code the aircraft is already squawking
	};


	class CentralSquawk : public EuroScopePlugIn::CPlugIn
	{
		static constexpr int PERIODIC_FETCH_TIME_INTERVAL = 5; // seconds
		static constexpr const char* API_URL = "centralsquawk.vatsim.fr";
		static constexpr int API_PORT = 443;

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
		bool IsConnected();

		void WorkerThread();
		void FetchAssignedSSR(httplib::Client& cli);
		void SendAssignRequest(httplib::Client& cli, const std::string& userCallsign,
							   const std::string& callsign, const std::string& code);
		const std::string GenerateToken(const std::string& controllerCallsign);

		/// Push central codes into EuroScope for flights this controller tracks.
		/// MUST run on the main thread: EuroScope is not thread safe.
		void ApplyAssignments();

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
		std::atomic<bool> isConnected_ = false;
		std::string userCallsign_;

		// SSR data cache: callsign -> central assignment
		std::mutex SSRCacheMutex_;
		std::unordered_map<std::string, SsrInfo> SSRCache_;

		// Manual requests waiting to be sent by the worker thread.
		// Value is the code to force; an EMPTY value means "force a reassignment".
		std::mutex apiRequestQueueMutex_;
		std::unordered_map<std::string, std::string> pendingAssignRequests_;
	};
} // namespace centralSquawk
