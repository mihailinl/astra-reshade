#define ASTRA_PROVIDER_EXPORTS
#include "astra_provider.h"
#include "guard.h"
#include "log.h"
#include "provider.h"
#include <windows.h>
#include <atomic>
#include <cmath>

namespace provider
{
	State &state()
	{
		static State s;
		return s;
	}

	int64_t now_ms()
	{
		return int64_t(GetTickCount64());
	}

	unsigned long long refused()
	{
		return state().refused;
	}

	bool run_present_callback()
	{
		const PresentCallback cb = state().present_callback();
		if (cb.fn == nullptr)
			return false;
		if (!guard::run("the provider's present callback", [&] { cb.fn(cb.context); }))
		{
			state().drop_present_callback(cb.fn); // never called again: a provider that faults once would every frame
			logf("provider: its present callback faulted and was unregistered");
		}
		return true;
	}
}

using provider::finite3;

extern "C" int astra_provider_version(void)
{
	return ASTRA_PROVIDER_VERSION;
}

extern "C" void astra_provider_camera(unsigned long long frame, const double pos[3], const float fwd[3], const float up[3], float fov_deg,
	int fov_is_horizontal, float aspect, float near_m)
{
	astra_provider_camera2(frame, pos, fwd, up, fov_deg, fov_is_horizontal, aspect, near_m, 0.0f);
}

extern "C" void astra_provider_camera2(unsigned long long frame, const double pos[3], const float fwd[3], const float up[3], float fov_deg,
	int fov_is_horizontal, float aspect, float near_m, float far_m)
{
	provider::State &s = provider::state();
	++s.calls;
	if (!finite3(pos) || !finite3(fwd) || !finite3(up) || !(fov_deg > 1.0f && fov_deg < 179.0f) || !(aspect >= 0.0f && aspect < 20.0f) ||
		!(near_m >= 0.0f && near_m < 100.0f) || !(far_m == 0.0f || (far_m > near_m && far_m < 1.0e9f)))
	{
		++s.refused;
		return;
	}
	provider::Camera c;
	c.frame = frame;
	c.at_ms = provider::now_ms();
	for (int i = 0; i < 3; ++i)
	{
		c.pos[i] = pos[i];
		c.fwd[i] = fwd[i];
		c.up[i] = up[i];
	}
	c.fov_deg = fov_deg;
	c.fov_horizontal = fov_is_horizontal != 0;
	c.aspect = aspect;
	c.near_m = near_m > 0.0f ? near_m : 0.1f;
	c.far_m = far_m > c.near_m ? far_m : 0.0f;
	s.cameras.push(c);
}

extern "C" void astra_provider_avatar(const double pos[3], const float fwd[3], const float up[3], const float vel[3], float speed, int airborne,
	float scale)
{
	provider::State &s = provider::state();
	++s.calls;
	if (!finite3(pos) || !finite3(fwd) || (up != nullptr && !finite3(up)) || (vel != nullptr && !finite3(vel)) || !std::isfinite(speed) ||
		!std::isfinite(scale))
	{
		++s.refused;
		return;
	}
	provider::Avatar a;
	for (int i = 0; i < 3; ++i)
	{
		a.pos[i] = pos[i];
		a.fwd[i] = fwd[i];
		if (up != nullptr)
			a.up[i] = up[i];
		if (vel != nullptr)
			a.vel[i] = vel[i];
	}
	a.has_up = up != nullptr;
	a.has_vel = vel != nullptr;
	a.speed = speed < 0.0f ? 0.0f : speed;
	a.airborne = airborne != 0;
	a.scale = scale > 0.0f ? scale : 1.0f;
	a.at_ms = provider::now_ms();
	s.avatar.write(a);
}

extern "C" void astra_provider_visible(int visible)
{
	provider::State &s = provider::state();
	++s.calls;
	s.visible = visible != 0;
}

extern "C" void astra_provider_light(const float toward[3], const float color[3], float intensity, float visible)
{
	provider::State &s = provider::state();
	++s.calls;
	if (!finite3(toward) || !finite3(color) || !std::isfinite(intensity) || !std::isfinite(visible))
	{
		++s.refused;
		return;
	}
	provider::Light l;
	for (int i = 0; i < 3; ++i)
	{
		l.toward[i] = toward[i];
		l.color[i] = color[i] < 0.0f ? 0.0f : color[i];
	}
	l.intensity = intensity < 0.0f ? 0.0f : intensity;
	l.visible = visible < 0.0f ? 0.0f : (visible > 1.0f ? 1.0f : visible);
	l.at_ms = provider::now_ms();
	s.light.write(l);
}

extern "C" void astra_provider_cue(const char *kind, const char *name)
{
	provider::State &s = provider::state();
	++s.calls;
	s.push_cue(kind, name);
}

extern "C" void astra_provider_identity(const char *integration, const char *game)
{
	provider::State &s = provider::state();
	++s.calls;
	s.set_identity(integration, game);
}

extern "C" void astra_provider_anchor(const char *point)
{
	provider::State &s = provider::state();
	++s.calls;
	s.set_anchor(point);
}

extern "C" void astra_provider_param(const char *name, float value)
{
	provider::State &s = provider::state();
	++s.calls;
	s.set_param(name, value);
}

extern "C" void astra_provider_log(const char *line)
{
	// a provider's own log lines: its first 1000, then every 100th (never a flood)
	static std::atomic<unsigned> lines{0};
	const unsigned n = lines++;
	if (line != nullptr && (n < 1000 || n % 100 == 0))
		logf("provider says: %.400s", line);
}

extern "C" void astra_provider_on_present(void (*callback)(void *context), void *context)
{
	provider::State &s = provider::state();
	++s.calls;
	s.set_present_callback(callback, context);
	logf("provider: present callback %s", callback != nullptr ? "registered" : "unregistered");
}
