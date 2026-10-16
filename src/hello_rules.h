#pragma once
// The hello's rules, portable (no ReShade, no Windows): tests/core_tests.cpp runs them. link.cpp sends what they say.
//   picture_for()  her picture's size for the game's back buffer: the game's own (1:1, sharp), fit inside the largest ring
//                  the engine makes - never a fixed 1080p one the game scales up (blurry, a fringe where her outline and
//                  her depth stop matching the game's pixels: Scarlet Skips at 2560x1440, 2026-10-16)
//   hello()        the hello the link sends (BRIDGE.md), with `want`: the largest picture it will ask for
//   read_reply()   what the engine's hello reply says of its ring (maxW/maxH), and whether it takes `want`
//   Asked          the re-hello rule: her picture outgrew the ring the engine made -> one more hello, asking for it
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace hello_rules
{
	/// The largest ring the engine makes (its RingBounds::CAP, BRIDGE.md `want`): 4K, ~570 MiB of Windows commit. Her
	/// picture never asks for more; a bigger back buffer gets a picture scaled down to fit, aspect kept.
	constexpr int kMaxW = 3840, kMaxH = 2160;
	constexpr int kMinSide = 16;

	struct Size
	{
		int w = 0, h = 0;
		bool known() const { return w > 0 && h > 0; }
		bool operator==(const Size &o) const { return w == o.w && h == o.h; }
	};

	/// Her picture for a back buffer of bw x bh: its own size, scaled down only to fit kMaxW x kMaxH (aspect kept);
	/// unknown (0x0) while the back buffer is.
	inline Size picture_for(int bw, int bh)
	{
		if (bw <= 0 || bh <= 0)
			return {};
		const double scale = std::min({ 1.0, double(kMaxW) / bw, double(kMaxH) / bh });
		return { std::clamp(int(bw * scale + 0.5), kMinSide, kMaxW), std::clamp(int(bh * scale + 0.5), kMinSide, kMaxH) };
	}

	/// The hello (BRIDGE.md, game -> engine). `want` (when known): the largest picture this connection will ask for, so
	/// the engine makes its ring that size; `token` only when one is set (already cleaned: config_rules::clean_token).
	inline std::string hello(const std::string &game, const std::string &foundation, const std::string &integration, const std::string &token,
		Size want)
	{
		std::string s = "{\"t\":\"hello\",\"v\":1,\"client\":\"astra-reshade/" + game + "\",\"game\":\"" + game + "\",\"foundation\":\"" + foundation +
			"\",\"integration\":\"" + integration + "\",\"crop\":true";
		if (want.known())
			s += ",\"want\":[" + std::to_string(want.w) + "," + std::to_string(want.h) + "]";
		if (!token.empty())
			s += ",\"token\":\"" + token + "\"";
		return s + "}";
	}

	/// The engine's hello reply, as far as the ring's size goes.
	struct Reply
	{
		Size ring;               // maxW x maxH: the ring it made (unknown: an engine that did not say)
		bool takes_want = false; // its caps lists `want`: an engine that ignores it would answer every hello the same
	};

	inline Reply read_reply(const std::string &m)
	{
		const auto number = [&m](const char *key) {
			const size_t at = m.find(key);
			if (at == std::string::npos)
				return 0;
			const long v = std::strtol(m.c_str() + at + std::strlen(key), nullptr, 10);
			return v > 0 && v <= 65536 ? int(v) : 0;
		};
		Reply r;
		r.ring = { number("\"maxW\":"), number("\"maxH\":") };
		const size_t caps = m.find("\"caps\":[");
		if (caps != std::string::npos)
		{
			const size_t end = m.find(']', caps);
			r.takes_want = m.substr(caps, end == std::string::npos ? std::string::npos : end - caps).find("\"want\"") != std::string::npos;
		}
		return r;
	}

	/// One connection's hellos. Its first asks for her picture's size; when her picture later outgrows the ring the reply
	/// named (the game's window grew), one more hello asks for the larger size - only to an engine that takes `want`, only
	/// once the last hello was answered (one in flight), and only past what was already asked for: an engine pinned
	/// smaller answers the same size again, and is not asked again every frame. The engine grows a ring by at least a
	/// quarter a side, so a window dragged larger costs a few hellos, not one per frame. What was asked never shrinks.
	struct Asked
	{
		Size want;            // the largest picture asked for on this connection
		Reply reply;          // the engine's last answer
		bool waiting = false; // a hello is in flight

		/// A new connection: its first hello's `want` (unknown: none, the engine's default size).
		Size first(Size picture)
		{
			*this = Asked();
			want = picture;
			waiting = true;
			return want;
		}

		void answered(const Reply &r)
		{
			reply = r;
			waiting = false;
		}

		/// The `want` of a hello due now, or unknown when none is.
		Size again(Size picture) const
		{
			if (waiting || !reply.takes_want || !picture.known())
				return {};
			if (picture.w <= reply.ring.w && picture.h <= reply.ring.h)
				return {}; // it fits the ring there is
			if (picture.w <= want.w && picture.h <= want.h)
				return {}; // asked for already: the engine made what it could
			return { std::max(picture.w, want.w), std::max(picture.h, want.h) };
		}

		void sent(Size w)
		{
			want = w;
			waiting = true;
		}
	};
}
