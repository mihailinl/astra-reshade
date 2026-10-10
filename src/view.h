#pragma once
// Camera math for re-projection, portable (tests/ runs it natively). Camera space is the engine's and the
// shader's: x right, y up, looking down -z.
#include <cmath>

namespace view
{
	struct Pose
	{
		double pos[3] = {};  // world (glTF, metres)
		float col[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; // camera -> world columns: right, up, back (= -forward)
		float tan_half_v = 0.5f; // tan(vertical fov / 2)
		bool valid = false;
	};

	inline void cross(const float a[3], const float b[3], float out[3])
	{
		out[0] = a[1] * b[2] - a[2] * b[1];
		out[1] = a[2] * b[0] - a[0] * b[2];
		out[2] = a[0] * b[1] - a[1] * b[0];
	}

	inline bool normalize(float v[3])
	{
		const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (!(l > 1e-6f))
			return false;
		for (int i = 0; i < 3; ++i)
			v[i] /= l;
		return true;
	}

	/// tan(vertical fov / 2) from a fov along either axis and the aspect it refers to.
	inline float tan_half_vertical(float fov_deg, bool horizontal, float aspect)
	{
		const float t = std::tan(fov_deg * 3.14159265f / 360.0f);
		return horizontal && aspect > 0.0f ? t / aspect : t;
	}

	/// A camera from its eye, forward and up (re-orthogonalised). False when fwd and up are parallel or zero.
	inline bool make_pose(const double pos[3], const float fwd_in[3], const float up_in[3], float tan_half_v, Pose &out)
	{
		float f[3] = {fwd_in[0], fwd_in[1], fwd_in[2]}, u[3] = {up_in[0], up_in[1], up_in[2]}, r[3];
		if (!normalize(f))
			return false;
		cross(f, u, r); // right = forward x up (right-handed)
		if (!normalize(r))
			return false;
		cross(r, f, u);
		for (int i = 0; i < 3; ++i)
		{
			out.pos[i] = pos[i];
			out.col[0][i] = r[i];
			out.col[1][i] = u[i];
			out.col[2][i] = -f[i];
		}
		out.tan_half_v = tan_half_v;
		out.valid = tan_half_v > 0.0f;
		return out.valid;
	}

	/// Host camera ray -> the picture camera's: rows of R_from^T R_host, and the host's eye in the picture
	/// camera's space. A point p in host camera space is R p + t in the picture camera's.
	struct Warp
	{
		float row[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float t[3] = {};
		bool identity = true;
	};

	inline Warp warp(const Pose &host, const Pose &from, const double moved[3])
	{
		Warp w;
		const double fp[3] = {from.pos[0] + moved[0], from.pos[1] + moved[1], from.pos[2] + moved[2]};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				w.row[i][j] = from.col[i][0] * host.col[j][0] + from.col[i][1] * host.col[j][1] + from.col[i][2] * host.col[j][2];
		const float d[3] = {float(host.pos[0] - fp[0]), float(host.pos[1] - fp[1]), float(host.pos[2] - fp[2])};
		for (int i = 0; i < 3; ++i)
			w.t[i] = from.col[i][0] * d[0] + from.col[i][1] * d[1] + from.col[i][2] * d[2];
		bool same = std::fabs(host.tan_half_v - from.tan_half_v) < 1e-5f;
		for (int i = 0; i < 3 && same; ++i)
		{
			same = std::fabs(d[i]) < 1e-4f;
			for (int j = 0; j < 3 && same; ++j)
				same = std::fabs(host.col[i][j] - from.col[i][j]) < 1e-5f;
		}
		w.identity = same;
		return w;
	}
}
