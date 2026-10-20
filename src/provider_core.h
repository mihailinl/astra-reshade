#pragma once
// What providers told the add-on, held for the present thread: the game's newest cameras (a short ring, so
// the camera of the frame being PRESENTED can be picked, which in a game with a render thread is not the
// newest), her placement, the sun, cues and who is providing. Writers (the game thread, the pipe thread)
// never block the reader and the reader never sees half a write: every value sits behind a seqlock (the rare,
// sticky ones - her anchor, extra animator parameters, the present callback - behind a mutex).
// Portable C++ (no Windows): tests/ builds it natively.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace provider
{
	struct Camera
	{
		uint64_t frame = 0;      // the provider's frame counter
		uint64_t index = 0;      // the add-on's: the how-many-th camera written (ring position)
		int64_t at_ms = 0;       // when it arrived (the add-on's clock)
		double pos[3] = {};      // glTF world, metres
		float fwd[3] = {0, 0, -1}, up[3] = {0, 1, 0};
		float fov_deg = 60.0f;
		int fov_horizontal = 0;
		float aspect = 0.0f;     // 0: the back buffer's
		float near_m = 0.1f;
		float far_m = 0.0f;      // 0: no far plane (an infinite reversed-Z projection)
	};

	struct Avatar
	{
		double pos[3] = {};      // her feet, glTF world
		float fwd[3] = {0, 0, 1}, up[3] = {0, 1, 0}, vel[3] = {};
		bool has_up = false, has_vel = false;
		float speed = 0.0f;
		bool airborne = false;
		float scale = 1.0f;
		int64_t at_ms = 0;
	};

	struct Light
	{
		float toward[3] = {0, 1, 0}, color[3] = {1, 1, 1};
		float intensity = 1.0f, visible = 1.0f;
		int64_t at_ms = 0;
	};

	/// One value behind a seqlock: any number of writer threads (serialised by a spin flag, writes are a
	/// copy), one or more readers that retry while a write is in flight.
	template <class T>
	class SeqLocked
	{
	public:
		void write(const T &v)
		{
			while (m_writer.test_and_set(std::memory_order_acquire))
			{
			}
			const uint32_t s = m_seq.load(std::memory_order_relaxed);
			m_seq.store(s + 1, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			std::memcpy(static_cast<void *>(&m_value), &v, sizeof(T));
			std::atomic_thread_fence(std::memory_order_release);
			m_seq.store(s + 2, std::memory_order_release);
			m_writer.clear(std::memory_order_release);
		}
		/// False when nothing was ever written (or a writer kept it busy for every try).
		bool read(T &out) const
		{
			for (int tries = 0; tries < 1000; ++tries)
			{
				const uint32_t s1 = m_seq.load(std::memory_order_acquire);
				if (s1 & 1)
					continue;
				std::memcpy(static_cast<void *>(&out), &m_value, sizeof(T));
				std::atomic_thread_fence(std::memory_order_acquire);
				if (m_seq.load(std::memory_order_relaxed) == s1)
					return s1 != 0;
			}
			return false;
		}

	private:
		std::atomic<uint32_t> m_seq{0};
		std::atomic_flag m_writer = ATOMIC_FLAG_INIT;
		T m_value{};
	};

	/// The newest cameras, oldest overwritten.
	class CameraRing
	{
	public:
		static constexpr uint64_t kSlots = 16;
		void push(Camera c)
		{
			std::lock_guard<std::mutex> lock(m_push); // writers in order (index), readers never take it
			const uint64_t i = m_count.load(std::memory_order_relaxed);
			c.index = i;
			m_slots[i % kSlots].write(c);
			m_count.store(i + 1, std::memory_order_release);
		}
		uint64_t count() const { return m_count.load(std::memory_order_acquire); }
		/// The camera `back` writes before the newest (0 = the newest); clamped to the oldest still held.
		bool get(uint64_t back, Camera &out) const
		{
			for (int tries = 0; tries < 8; ++tries)
			{
				const uint64_t n = count();
				if (n == 0)
					return false;
				const uint64_t newest = n - 1;
				const uint64_t oldest = n > kSlots - 2 ? n - (kSlots - 2) : 0; // two slots of slack for writers
				const uint64_t want = newest >= back && newest - back >= oldest ? newest - back : oldest;
				if (m_slots[want % kSlots].read(out) && out.index == want)
					return true;
			}
			return false;
		}

		/// The NEWEST camera whose age (now_ms - at_ms) is at least age_ms - never more than one push
		/// staler than that bar requires, so a provider that pushes rarely is never walked back several
		/// pushes' worth (camera_pace.h's own fix). Falls back to the newest held camera when none is
		/// old enough (age_ms <= 0, or every push is still fresher than it: nothing to delay for).
		bool get_by_age(int64_t now_ms, int64_t age_ms, Camera &out) const
		{
			for (int tries = 0; tries < 8; ++tries)
			{
				const uint64_t n = count();
				if (n == 0)
					return false;
				const uint64_t newest = n - 1;
				const uint64_t oldest = n > kSlots - 2 ? n - (kSlots - 2) : 0; // two slots of slack for writers
				Camera c;
				bool ok = m_slots[newest % kSlots].read(c) && c.index == newest;
				if (!ok)
					continue; // a writer mid-push: retry the whole lookup
				if (age_ms > 0)
				{
					uint64_t idx = newest;
					while (idx > oldest && now_ms - c.at_ms < age_ms)
					{
						Camera older;
						if (!m_slots[(idx - 1) % kSlots].read(older) || older.index != idx - 1)
						{
							ok = false;
							break; // a writer mid-push on the way back: retry the whole lookup
						}
						--idx;
						c = older;
					}
				}
				if (!ok)
					continue;
				out = c;
				return true;
			}
			return false;
		}

	private:
		std::mutex m_push;
		std::atomic<uint64_t> m_count{0};
		SeqLocked<Camera> m_slots[kSlots];
	};

	/// A cue name the engine may get: letters, digits, '_', '.', '-'; 1..64 (it is put into JSON verbatim).
	inline bool valid_name(const char *s)
	{
		if (s == nullptr || *s == 0)
			return false;
		size_t n = 0;
		for (; s[n] != 0; ++n)
		{
			const char c = s[n];
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
			if (!ok || n >= 64)
				return false;
		}
		return true;
	}

	/// An identifier for the engine: a letter or '_', then letters, digits, '_'; 1..32 (an animator parameter's
	/// name, a bone's). Put into JSON verbatim.
	inline bool valid_ident(const char *s)
	{
		if (s == nullptr || !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_'))
			return false;
		size_t n = 0;
		for (; s[n] != 0; ++n)
		{
			const char c = s[n];
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
			if (!ok || n >= 32)
				return false;
		}
		return true;
	}

	/// Shown text (integration id, game name): printable ASCII, no quotes or backslashes, <= 64.
	inline std::string clean_text(const char *s)
	{
		std::string out;
		for (; s != nullptr && *s != 0 && out.size() < 64; ++s)
		{
			const unsigned char c = static_cast<unsigned char>(*s);
			if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\')
				out.push_back(char(c));
		}
		while (!out.empty() && out.back() == ' ')
			out.pop_back();
		return out;
	}

	struct Cue
	{
		std::string kind, name; // "play" / "trigger" / "resume"
	};

	/// An extra animator parameter (astra_provider_param). `clear`: send null once (back to the set's default).
	struct Param
	{
		std::string name;
		float value = 0.0f;
		bool clear = false;
	};
	constexpr size_t kMaxParams = 16;

	/// The provider's present callback (astra_provider_on_present).
	struct PresentCallback
	{
		void (*fn)(void *) = nullptr;
		void *context = nullptr;
	};

	/// Everything providers said, for the present thread.
	struct State
	{
		CameraRing cameras;
		SeqLocked<Avatar> avatar;
		SeqLocked<Light> light;
		std::atomic<bool> visible{true};
		std::atomic<uint64_t> calls{0}, refused{0};

		void push_cue(const char *kind, const char *name)
		{
			Cue c;
			c.kind = kind ? kind : "";
			if (c.kind == "resume")
				c.name.clear();
			else if ((c.kind == "play" || c.kind == "trigger") && valid_name(name))
				c.name = name;
			else
			{
				++refused;
				return;
			}
			std::lock_guard<std::mutex> lock(m_lock);
			if (m_cues.size() < 32)
				m_cues.push_back(std::move(c));
		}
		std::vector<Cue> take_cues()
		{
			std::lock_guard<std::mutex> lock(m_lock);
			std::vector<Cue> out;
			out.swap(m_cues);
			return out;
		}
		void set_identity(const char *integration, const char *game)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_integration = clean_text(integration);
			m_game = clean_text(game);
		}
		void identity(std::string &integration, std::string &game)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			integration = m_integration;
			game = m_game;
		}

		/// The game's engine as the provider declares it ("unreal"; "" none). False (kept as it was): not a name.
		bool set_engine(const char *engine)
		{
			const bool reset = engine == nullptr || *engine == 0;
			if (!reset && !valid_name(engine))
			{
				++refused;
				return false;
			}
			std::lock_guard<std::mutex> lock(m_lock);
			m_engine = reset ? "" : engine;
			return true;
		}
		std::string engine()
		{
			std::lock_guard<std::mutex> lock(m_lock);
			return m_engine;
		}

		/// The point of her that her placement names: "feet" or a bone. False (kept as it was): not an identifier.
		bool set_anchor(const char *point)
		{
			const bool reset = point == nullptr || *point == 0;
			if (!reset && !valid_ident(point))
			{
				++refused;
				return false;
			}
			std::lock_guard<std::mutex> lock(m_lock);
			m_anchor = reset ? "feet" : point;
			return true;
		}
		std::string anchor()
		{
			std::lock_guard<std::mutex> lock(m_lock);
			return m_anchor;
		}

		/// Sets (a number) or hands back (NaN) an extra parameter. Refused: a bad name, speed/airborne (they come
		/// with the placement), an infinity, or a 17th name.
		bool set_param(const char *name, float value)
		{
			const bool clear = value != value;
			if (!valid_ident(name) || std::strcmp(name, "speed") == 0 || std::strcmp(name, "airborne") == 0 || (!clear && value - value != 0.0f))
			{
				++refused;
				return false;
			}
			std::lock_guard<std::mutex> lock(m_lock);
			for (Param &p : m_params)
				if (p.name == name)
				{
					p.value = clear ? 0.0f : value;
					p.clear = clear;
					return true;
				}
			if (clear)
				return true; // never set: nothing to hand back
			if (m_params.size() >= kMaxParams)
			{
				++refused;
				return false;
			}
			m_params.push_back(Param{name, value, false});
			return true;
		}
		/// The parameters to send with this placement; one handed back is in it once (clear), then forgotten.
		std::vector<Param> take_params()
		{
			std::lock_guard<std::mutex> lock(m_lock);
			std::vector<Param> out = m_params;
			for (size_t i = m_params.size(); i-- > 0;)
				if (m_params[i].clear)
					m_params.erase(m_params.begin() + std::ptrdiff_t(i));
			return out;
		}

		void set_present_callback(void (*fn)(void *), void *context)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_present = PresentCallback{fn, fn != nullptr ? context : nullptr};
		}
		PresentCallback present_callback()
		{
			std::lock_guard<std::mutex> lock(m_lock);
			return m_present;
		}

	private:
		std::mutex m_lock;
		std::vector<Cue> m_cues;
		std::string m_integration, m_game, m_engine;
		std::string m_anchor = "feet";
		std::vector<Param> m_params;
		PresentCallback m_present;
	};

	/// No NaN, no infinity (x - x is 0 only for a finite x).
	inline bool finite3(const double v[3])
	{
		return v != nullptr && v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] - v[0] == 0 && v[1] - v[1] == 0 && v[2] - v[2] == 0;
	}
	inline bool finite3(const float v[3])
	{
		return v != nullptr && v[0] - v[0] == 0 && v[1] - v[1] == 0 && v[2] - v[2] == 0;
	}
}
