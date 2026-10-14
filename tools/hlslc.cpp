// Compiles the HLSL ReShade generates for each entry point of an effect (tools/fxcheck --hlsl) with the
// d3dcompiler_47.dll this process finds, the way ReShade 6.8's runtime does (effect_codegen_dxbc.cpp: the profile, strict,
// optimisation level 1 - ReShade.ini's PerformanceMode=0). Run under Wine it is Wine's own compiler (vkd3d-shader's
// HLSL front end): what ReShade uses under Proton when the game ships no d3dcompiler_47.dll of its own - most Unreal
// games ship none.
//   hlslc <dir>/<profile>.<entry point>.hlsl ...      (profile: ps_5_1 ...)  exit 1 if any entry point fails
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <string>

int main(int argc, char **argv)
{
	HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
	const auto compile = dll != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
	if (compile == nullptr)
	{
		std::fprintf(stderr, "hlslc: no d3dcompiler_47.dll\n");
		return 2;
	}
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	int failed = 0;
	for (int i = 1; i < argc; ++i)
	{
		const std::string path = argv[i];
		FILE *f = std::fopen(path.c_str(), "rb");
		if (f == nullptr)
		{
			std::fprintf(stderr, "hlslc: cannot read %s\n", path.c_str());
			return 2;
		}
		std::string src;
		char buf[65536];
		for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;)
			src.append(buf, n);
		std::fclose(f);
		const size_t slash = path.find_last_of("/\\");
		const std::string base = path.substr(slash == std::string::npos ? 0 : slash + 1);
		const std::string profile = base.substr(0, 6), entry = base.substr(7, base.size() - 7 - 5); // "ps_5_1.<entry>.hlsl"
		ID3DBlob *code = nullptr, *errors = nullptr;
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		const HRESULT hr = compile(src.data(), src.size(), base.c_str(), nullptr, nullptr, entry.c_str(), profile.c_str(),
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
		QueryPerformanceCounter(&t1);
		const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
		if (FAILED(hr))
		{
			++failed;
			std::printf("FAILED %s (%s): %s\n", entry.c_str(), profile.c_str(),
				errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "(no message)");
		}
		else
			std::printf("ok %-28s %s %8.1f ms\n", entry.c_str(), profile.c_str(), ms);
		if (code != nullptr)
			code->Release();
		if (errors != nullptr)
			errors->Release();
	}
	return failed > 0 ? 1 : 0;
}
