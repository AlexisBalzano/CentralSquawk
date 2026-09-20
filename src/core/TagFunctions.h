#pragma once
#include <tuple>

#include "CentralSquawk.h"
#include "Helpers.h"

using namespace centralSquawk;

namespace {
	/// Popup label that opens the code editor. Not a valid squawk, so it can
	/// never collide with something the controller types.
	constexpr const char* SET_CODE_LABEL = "SET...";
}

inline void CentralSquawk::RegisterTagActions()
{
	// No tag ITEM of our own yet: bind this to an existing tag item (EuroScope's
	// squawk field is the natural one) from the tag settings dialog.
	RegisterTagItemFunction("Central Squawk menu", static_cast<int>(TagActionID::OpenMENU));
	RegisterTagItemFunction("Assign Auto", static_cast<int>(TagActionID::AssignAuto));
}

inline void CentralSquawk::OnFunctionCall(int functionId, const char* itemString, POINT pt, RECT area)
{
	std::ignore = pt;

	if (!IsOnline()) return;
	if (!isController_.load(std::memory_order_acquire)) {
		QueueError("Only controllers may request a squawk assignment.");
		return;
	}

	auto fp = FlightPlanSelectASEL();
	if (!fp.IsValid()) return;

	const char* callsignPtr = fp.GetCallsign();
	if (callsignPtr == nullptr || *callsignPtr == '\0') return;
	const std::string callsign = ToUpper(callsignPtr);

	// The datafeed-shaped view of this flight, read here because this is the
	// main thread and the worker that sends the request cannot touch EuroScope.
	const auto captureSeed = [&]() -> FlightObservation {
		FlightObservation seed;
		seed.callsign = callsign;

		auto data = fp.GetFlightPlanData();
		if (!data.IsReceived()) return seed;  // nothing filed yet: send without one

		// The FP track position rather than the correlated radar target. A pilot
		// who has just connected may not be correlated yet, which is precisely
		// the case this exists for; EuroScope falls back to its own computed
		// position when there is no radar return, and uses the last real one
		// when there is.
		const auto track = fp.GetFPTrackPosition();
		if (!track.IsValid()) return seed;  // unplaceable, so unusable as a seed

		const auto position = track.GetPosition();
		seed.latitude = position.m_Latitude;
		seed.longitude = position.m_Longitude;
		seed.altitude = track.GetPressureAltitude();
		seed.groundspeed = track.GetReportedGS();

		seed.flightRules = ToUpper(SafeString(data.GetPlanType()));
		seed.departure = ToUpper(SafeString(data.GetOrigin()));
		seed.arrival = ToUpper(SafeString(data.GetDestination()));
		// Unextracted field 10, the same string the datafeed carries as
		// `flight_plan.aircraft`, which is what the Mode S test parses.
		seed.equipment = ToUpper(SafeString(data.GetAircraftInfo()));
		seed.route = ToUpper(SafeString(data.GetRoute()));

		seed.valid = true;
		return seed;
	};

	// Hand a request to the worker thread.
	const auto queueRequest = [&](AssignRequest::Kind kind, const std::string& code = {}) {
		FlightObservation seed = captureSeed();
		std::lock_guard<std::mutex> lock(apiRequestQueueMutex_);
		pendingAssignRequests_[callsign] = AssignRequest{ kind, code, std::move(seed) };
	};

	switch (static_cast<TagActionID>(functionId)) {
	case TagActionID::OpenMENU:
	{
		OpenPopupList(area, "Squawk", 1);
		AddPopupListElement("AUTO", NULL, static_cast<int>(TagActionID::AssignAuto), false, 2, false, false);
		AddPopupListElement("DISCRETE", NULL, static_cast<int>(TagActionID::AssignDiscrete), false, 2, false, false);
		AddPopupListElement("CURRENT", NULL, static_cast<int>(TagActionID::AssignCurrent), false, 2, false, false);
		AddPopupListElement(SET_CODE_LABEL, NULL, static_cast<int>(TagActionID::AssignCode), false, 2, false, true);
		break;
	}

	case TagActionID::AssignAuto:
	{
		queueRequest(AssignRequest::Kind::Auto);
		break;
	}

	case TagActionID::AssignDiscrete:
	{
		// AUTO hands 1000 back to a flight that still qualifies, so this is the
		// only way off conspicuity without typing a code by hand.
		queueRequest(AssignRequest::Kind::Discrete);
		break;
	}

	case TagActionID::AssignCurrent:
	{
		// The code the aircraft is actually transmitting, which is not
		// necessarily the one anybody assigned it.
		const auto radarTarget = fp.GetCorrelatedRadarTarget();
		if (!radarTarget.IsValid()) {
			QueueError("No radar target for " + callsign + ", cannot read its squawk.");
			return;
		}
		const std::string current = radarTarget.GetPosition().GetSquawk();
		if (!IsWellFormedSquawk(current)) {
			DisplayError(callsign + " is not squawking a usable code (" + current + ").");
			return;
		}
		queueRequest(AssignRequest::Kind::SetCode, current);
		break;
	}

	case TagActionID::AssignCode:
	{
		// This action has two roles: the menu entry that opens the editor, and
		// the editor handing back what was typed.
		if (itemString == nullptr || *itemString == '\0' || std::string(itemString) == SET_CODE_LABEL) {
			OpenPopupEdit(area, static_cast<int>(TagActionID::AssignCode), "");
			return;
		}

		const std::string code = ToUpper(itemString);
		if (!IsWellFormedSquawk(code)) {
			QueueError("\"" + code + "\" is not a squawk code (four digits, 0-7).");
			return;
		}
		queueRequest(AssignRequest::Kind::SetCode, code);
		break;
	}

	default:
		break;
	}
}
