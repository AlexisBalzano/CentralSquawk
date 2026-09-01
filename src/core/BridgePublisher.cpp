//
// EuroScope Plugin Bridge publisher.
//

#include "core/BridgePublisher.h"

#include <iterator>

#define ESB_CLIENT_SHIM
#include <esbridge.h>

#include "CentralSquawk.h"

namespace centralSquawk {

	namespace {

		/// Claimed namespace on the bridge. A conflict is settled with the other
		/// author, not retried around, so a taken id is reported once and dropped.
		constexpr const char* PROVIDER_ID = "centralsquawk";
		constexpr const char* DUPE_FIELD = "dupe";

		/// The bridge is declared missing only after it has had a fair chance to load.
		constexpr int MISSING_TICKS_BEFORE_NOTICE = 10;

		// Deliberately NOT ESB_F_SYNC. Every Central Squawk instance polls the same
		// authoritative server and receives the same dupe flag for the same flight,
		// so relaying it between controllers would add a slower, staler second path
		// to a fact everyone already has locally.
		//
		// Not ESB_F_DENSE either: a duplicate is by definition the exception.
		const ESB_FieldDecl FIELDS[] = {
			{
				DUPE_FIELD,
				ESB_T_BOOL,
				ESB_SCOPE_AIRCRAFT,
				0u,
				0u,
				"Central Squawk sees this code on more than one aircraft",
			},
		};

	}

	BridgePublisher::~BridgePublisher()
	{
		// Unregistering drops every value we own, which is what should happen: a
		// dupe flag with nobody left to maintain it is worse than no flag at all.
		if (api_ != nullptr && provider_ != nullptr) api_->unregister_provider(provider_);
	}

	void BridgePublisher::OnTimer()
	{
		if (api_ == nullptr) {
			api_ = ESB_Attach();
			if (api_ == nullptr) {
				// Said once. Without the bridge the DUPE flag simply does not reach
				// CoFrance -- there is no annotation fallback any more -- so the
				// controller does need to be told, but only the one time.
				if (!missingReported_ && ++missingTicks_ >= MISSING_TICKS_BEFORE_NOTICE) {
					missingReported_ = true;
					plugin_->DisplayError(ESB_MISSING_MESSAGE);
				}
				return;
			}
		}

		if (provider_ == nullptr && !providerConflict_) RegisterProvider();
	}

	bool BridgePublisher::RegisterProvider()
	{
		ESB_ProviderDecl decl = {};
		decl.struct_size = sizeof decl;
		decl.provider_id = PROVIDER_ID;
		decl.schema_major = 1;
		decl.schema_minor = 0;
		decl.display_name = "Central Squawk";
		decl.contact = "https://github.com/AlexisBalzano/CentralSquawk";
		decl.fields = FIELDS;
		decl.field_count = static_cast<uint32_t>(std::size(FIELDS));
		decl.module = ESB_SelfModule();

		const ESB_Status status = api_->register_provider(&decl, &provider_);
		if (status != ESB_OK) {
			provider_ = nullptr;

			if (status == ESB_E_PROVIDER_TAKEN) {
				providerConflict_ = true;
				plugin_->DisplayError("Another loaded plugin already owns the \"" +
									  std::string(PROVIDER_ID) + "\" bridge provider id.");
			}
			return false;
		}

		// Owning the field is what grants write authority; resolve() alone would
		// only ever produce a read handle.
		if (api_->own_field(provider_, DUPE_FIELD, &dupeField_) != ESB_OK) {
			dupeField_ = ESB_FIELD_NONE;
			return false;
		}

		return true;
	}

	void BridgePublisher::PublishDupe(const std::string& callsign, bool dupe)
	{
		if (dupeField_ == ESB_FIELD_NONE || callsign.empty()) return;

		// The bridge already holds this value, and a redundant write would wake
		// every subscriber for nothing.
		if (dupe == published_.contains(callsign)) return;

		ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
		if (api_->aircraft(callsign.c_str(), &aircraft) != ESB_OK) {
			// Not on the network: a flight plan without a target, or one that has
			// just disconnected. Nothing to clear in the second case, and leaving
			// the callsign in the set would only make us retry a write that cannot
			// land.
			if (!dupe) published_.erase(callsign);
			return;
		}

		if (dupe) {
			// A named local, not &ESB_Bool(1): /permissive- rejects taking the
			// address of a temporary, whatever the example in esbridge.h suggests.
			const ESB_Value value = ESB_Bool(1);
			if (api_->set_ac(provider_, aircraft, dupeField_, &value) != ESB_OK) return;
			published_.insert(callsign);
		}
		else {
			if (api_->clear_ac(provider_, aircraft, dupeField_) != ESB_OK) return;
			published_.erase(callsign);
		}
	}

	void BridgePublisher::ForgetAbsent(const std::unordered_set<std::string>& present)
	{
		if (published_.size() <= present.size()) return;
		std::erase_if(published_, [&present](const std::string& cs) { return !present.contains(cs); });
	}

} // namespace centralSquawk
