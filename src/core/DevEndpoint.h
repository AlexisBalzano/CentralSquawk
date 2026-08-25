#pragma once

// ============================================================================
//  TEMPORARY -- PRE-RELEASE TESTING ONLY.  DELETE BEFORE THE FIRST RELEASE.
// ============================================================================
//
//  Lets a development build talk to an API running somewhere other than
//  production, so the plugin can be exercised against a local container.
//
//  REMOVAL CHECKLIST (four steps, nothing else references this):
//
//    1. Delete this file:            src/core/DevEndpoint.h
//    2. CMakeLists.txt:              remove src/core/DevEndpoint.h from HEADERS
//    3. src/CentralSquawk.cpp:       remove the #include of this file
//    4. src/CentralSquawk.cpp:       remove the block marked
//                                    "TEMPORARY DEV ENDPOINT OVERRIDE"
//                                    in CentralSquawk::WorkerThread()
//
//  After that the plugin can only ever reach the production endpoint.
//
//  Note that even before removal a RELEASE build is already safe: without
//  -DDEV=1 the function below compiles to `return {};`, so the override cannot
//  be switched on at runtime by an environment variable or anything else.
//
// ============================================================================

#include <cstdlib>
#include <string>

namespace centralSquawk::dev {

	/// Base URL to use instead of production, or an empty string to use production.
	///
	/// In a development build (-DDEV=1) this reads CENTRALSQUAWK_API and falls
	/// back to a local container. Both http:// and https:// are accepted; the
	/// scheme decides whether TLS is used.
	///
	///   set CENTRALSQUAWK_API=http://localhost:3000
	///
	inline std::string EndpointOverride()
	{
#if defined(DEV)
		if (const char* fromEnv = std::getenv("CENTRALSQUAWK_API");
			fromEnv != nullptr && *fromEnv != '\0') {
			return fromEnv;
		}
		return "http://localhost:3000";
#else
		return {};
#endif
	}

} // namespace centralSquawk::dev
