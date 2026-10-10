#include "lines.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lines
{
	namespace
	{
		constexpr int kMaxFields = 20;

		/// Splits the line into space-separated fields (in place, in a copy).
		struct Fields
		{
			char buf[1024];
			const char *f[kMaxFields];
			int n = 0;
			const char *rest[kMaxFields]; // the line from field i on (for a game name with spaces)
		};

		bool split(const char *line, size_t length, Fields &out)
		{
			if (length >= sizeof(out.buf))
				return false;
			std::memcpy(out.buf, line, length);
			out.buf[length] = 0;
			if (length > 0 && out.buf[length - 1] == '\r')
				out.buf[--length] = 0;
			char *p = out.buf;
			while (*p != 0)
			{
				while (*p == ' ')
					++p;
				if (*p == 0)
					break;
				if (out.n == kMaxFields)
					return false;
				out.rest[out.n] = line + (p - out.buf);
				out.f[out.n++] = p;
				while (*p != 0 && *p != ' ')
					++p;
				if (*p == ' ')
					*p++ = 0;
			}
			return out.n > 0;
		}

		bool num(const char *s, double &v)
		{
			char *end = nullptr;
			v = std::strtod(s, &end);
			return end != s && *end == 0 && std::isfinite(v);
		}

		bool nums(const Fields &f, int from, int count, double *out)
		{
			if (f.n < from + count)
				return false;
			for (int i = 0; i < count; ++i)
				if (!num(f.f[from + i], out[i]))
					return false;
			return true;
		}

		void to_float(const double *in, float *out, int n)
		{
			for (int i = 0; i < n; ++i)
				out[i] = float(in[i]);
		}
	}

	bool parse(const char *line, size_t length, Sink &sink)
	{
		Fields f;
		if (!split(line, length, f) || f.f[0][1] != 0)
			return false;
		switch (f.f[0][0])
		{
		case 'C':
		{
			// C <frame> <px> <py> <pz> <fx> <fy> <fz> <ux> <uy> <uz> <fov> <v|h> <aspect> <near>
			if (f.n != 15)
				return false;
			char *end = nullptr;
			const unsigned long long frame = std::strtoull(f.f[1], &end, 10);
			if (end == f.f[1] || *end != 0)
				return false;
			double v[9], tail[2];
			if (!nums(f, 2, 9, v) || !nums(f, 13, 2, tail))
				return false;
			double fov;
			if (!num(f.f[11], fov))
				return false;
			const char *axis = f.f[12];
			if ((axis[0] != 'v' && axis[0] != 'h') || axis[1] != 0)
				return false;
			float fwd[3], up[3];
			to_float(v + 3, fwd, 3);
			to_float(v + 6, up, 3);
			sink.camera(frame, v, fwd, up, float(fov), axis[0] == 'h', float(tail[0]), float(tail[1]));
			return true;
		}
		case 'A':
		{
			// A <px> <py> <pz> <fx> <fy> <fz> <ux> <uy> <uz> <vx> <vy> <vz> <speed> <0|1> <scale>
			if (f.n != 16)
				return false;
			double v[15];
			if (!nums(f, 1, 15, v))
				return false;
			float fwd[3], up[3], vel[3];
			to_float(v + 3, fwd, 3);
			to_float(v + 6, up, 3);
			to_float(v + 9, vel, 3);
			sink.avatar(v, fwd, up, vel, float(v[12]), v[13] != 0.0, float(v[14]));
			return true;
		}
		case 'V':
		{
			double v;
			if (f.n != 2 || !num(f.f[1], v))
				return false;
			sink.visible(v != 0.0);
			return true;
		}
		case 'L':
		{
			if (f.n != 9)
				return false;
			double v[8];
			if (!nums(f, 1, 8, v))
				return false;
			float toward[3], color[3];
			to_float(v, toward, 3);
			to_float(v + 3, color, 3);
			sink.light(toward, color, float(v[6]), float(v[7]));
			return true;
		}
		case 'P':
		case 'T':
			if (f.n != 2)
				return false;
			sink.cue(f.f[0][0] == 'P' ? "play" : "trigger", f.f[1]);
			return true;
		case 'R':
			if (f.n != 1)
				return false;
			sink.cue("resume", "");
			return true;
		case 'I':
		{
			if (f.n < 3)
				return false;
			// the game's name is the rest of the line, spaces kept
			const size_t at = size_t(f.rest[2] - line);
			char game[128];
			size_t n = length - at;
			if (n > 0 && line[at + n - 1] == '\r')
				--n;
			if (n >= sizeof(game))
				n = sizeof(game) - 1;
			std::memcpy(game, line + at, n);
			game[n] = 0;
			sink.identity(f.f[1], game);
			return true;
		}
		case 'M':
		{
			char copy[256];
			size_t n = 0;
			if (f.n > 1)
			{
				const size_t at = size_t(f.rest[1] - line);
				n = length - at;
				if (n > 0 && line[at + n - 1] == '\r')
					--n;
				if (n >= sizeof(copy))
					n = sizeof(copy) - 1;
				std::memcpy(copy, line + at, n);
			}
			copy[n] = 0;
			sink.message(copy);
			return true;
		}
		default:
			return false;
		}
	}

	int Splitter::feed(const char *data, size_t n, Sink &sink)
	{
		int bad = 0;
		for (size_t i = 0; i < n; ++i)
		{
			const char c = data[i];
			if (c == '\n')
			{
				if (!m_overflow && m_len > 0 && !parse(m_line, m_len, sink))
					++bad;
				if (m_overflow)
					++bad;
				m_len = 0;
				m_overflow = false;
				continue;
			}
			if (m_len + 1 >= sizeof(m_line))
				m_overflow = true;
			else
				m_line[m_len++] = c;
		}
		return bad;
	}
}
