#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>

// The bridge's C ABI stays inside BridgePublisher.cpp. Its client shim declares a
// file-static api pointer, so pulling esbridge.h into a header would hand every
// translation unit that includes it a separate copy, only one of which would ever
// be attached.
struct ESB_Api_v1;
struct ESB_Provider;

namespace centralSquawk {

	class CentralSquawk;

	/// Publisher-side view of the EuroScope Plugin Bridge.
	///
	/// Owns the "centralsquawk" provider id and publishes `centralsquawk/dupe`,
	/// the flag CoFrance reads to raise its DUPE notification. Attaching is lazy:
	/// EuroScope loads plugins in whatever order the user's settings file lists
	/// them, so the bridge may well appear after we do.
	///
	/// Every method must be called on the EuroScope main thread.
	class BridgePublisher {
	public:
		explicit BridgePublisher(CentralSquawk* plugin) : plugin_(plugin) {}
		~BridgePublisher();

		BridgePublisher(const BridgePublisher&) = delete;
		BridgePublisher& operator=(const BridgePublisher&) = delete;

		/// Attach and register, retrying until both succeed. Once settled this is a
		/// pointer test. Call once per OnTimer, before publishing.
		void OnTimer();

		/// Set or clear the flag for one flight. Writes only when the value differs
		/// from what we last published, so subscribers are not woken every tick.
		void PublishDupe(const std::string& callsign, bool dupe);

		/// Drop bookkeeping for flights that have left the flight plan list. The
		/// bridge retires their values on its own; this just stops our own set from
		/// growing for the whole session.
		void ForgetAbsent(const std::unordered_set<std::string>& present);

		/// Whether anything is currently flagged. Lets the caller tell an idle tick
		/// from one that still has flags to retract.
		bool HasPublished() const { return !published_.empty(); }

	private:
		bool RegisterProvider();

		CentralSquawk* plugin_;

		const ESB_Api_v1* api_ = nullptr;
		ESB_Provider* provider_ = nullptr;
		uint32_t dupeField_ = 0;          // 0 == still unresolved
		bool providerConflict_ = false;   // Another module owns "centralsquawk"
		int missingTicks_ = 0;
		bool missingReported_ = false;    // ESB_MISSING_MESSAGE is said once

		/// Callsigns currently published as dupe. Purely a write filter -- unlike
		/// the flight strip annotation this replaced, the value is local to this
		/// EuroScope instance, so there is no ownership to arbitrate.
		std::unordered_set<std::string> published_;
	};

} // namespace centralSquawk
