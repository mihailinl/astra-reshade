#pragma once
// The provider API's state (include/astra_provider.h exports it): what the game's mod said, for the
// present thread (link.cpp, layer.cpp).
#include "provider_core.h"

namespace provider
{
	State &state();
	/// The add-on's clock (ms, monotonic).
	int64_t now_ms();
	/// Calls refused for bad values (NaN, parallel basis...), for the overlay.
	unsigned long long refused();
	/// Calls the provider's present callback, if one is registered (fault-contained). True: one was called.
	bool run_present_callback();
}
