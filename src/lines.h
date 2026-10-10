#pragma once
// The pipe's text protocol (include/astra_provider.h, PIPE PROTOCOL): one line -> one provider call.
// Portable: tests/ runs it natively.
#include <cstddef>

namespace lines
{
	/// What the parsed line asks the provider to do (calls the astra_provider_* functions).
	struct Sink
	{
		virtual ~Sink() = default;
		virtual void camera(unsigned long long frame, const double pos[3], const float fwd[3], const float up[3], float fov_deg, int fov_horizontal,
			float aspect, float near_m) = 0;
		virtual void avatar(const double pos[3], const float fwd[3], const float up[3], const float vel[3], float speed, int airborne, float scale) = 0;
		virtual void visible(int on) = 0;
		virtual void light(const float toward[3], const float color[3], float intensity, float visible) = 0;
		virtual void cue(const char *kind, const char *name) = 0;
		virtual void identity(const char *integration, const char *game) = 0;
		virtual void message(const char *text) = 0;
	};

	/// One line (no '\n'; a trailing '\r' is ignored). False: malformed (nothing was called).
	bool parse(const char *line, size_t length, Sink &sink);

	/// Splits a byte stream into lines across reads; lines over 1 KiB are dropped whole.
	class Splitter
	{
	public:
		/// Feeds bytes; calls parse for each complete line. Returns how many lines were malformed.
		int feed(const char *data, size_t n, Sink &sink);

	private:
		char m_line[1024];
		size_t m_len = 0;
		bool m_overflow = false;
	};
}
