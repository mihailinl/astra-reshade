#pragma once
// The provider API over a named pipe, for mods that cannot call native code (UE4SS Lua): a thread serves
// \\.\pipe\astra-provider (one client at a time, local only) and turns each line into the provider call it
// names (include/astra_provider.h, PIPE PROTOCOL). It reads all the time, so a writer never fills the pipe.

namespace pipeserver
{
	void start();
	/// Joins the thread (FreeLibrary only: at process exit it is already gone).
	void stop();

	struct Stats
	{
		bool serving, connected;
		unsigned long long lines, malformed, clients;
	};
	Stats stats();
}
