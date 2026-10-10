#define ASTRA_PROVIDER_EXPORTS
#include "astra_provider.h"
#include "provider.h"
#include <windows.h>
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
}

using provider::finite3;

extern "C" int astra_provider_version(void)
{
	return ASTRA_PROVIDER_VERSION;
}

extern "C" void astra_provider_camera(unsigned long long frame, const double pos[3], const float fwd[3], const float up[3], float fov_deg,
	int fov_is_horizontal, float aspect, float near_m)
{
	provider::State &s = provider::state();
	++s.calls;
	if (!finite3(pos) || !finite3(fwd) || !finite3(up) || !(fov_deg > 1.0f && fov_deg < 179.0f) || !(aspect >= 0.0f && aspect < 20.0f) ||
		!(near_m >= 0.0f && near_m < 100.0f))
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
