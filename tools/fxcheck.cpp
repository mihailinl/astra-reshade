// Preprocess + parse a ReShade FX file with ReShade's own compiler front end (HLSL code generation): syntax and type
// errors before a game ever loads it.
//   fxcheck [--hlsl <dir>] <file.fx> <include dir>...
// --hlsl: also write the HLSL ReShade hands to D3DCompile for each entry point, for Direct3D 11 (shader model 5.0) and
// 12 (5.1), as <dir>/<profile>.<entry point>.hlsl - tools/hlslc.cpp compiles them with a real d3dcompiler_47.dll.
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"
#include "effect_codegen.hpp"
#include <cstdio>
#include <memory>
#include <string>
int main(int argc, char **argv)
{
	const char *hlsl_dir = nullptr;
	if (argc > 2 && std::string(argv[1]) == "--hlsl") { hlsl_dir = argv[2]; argc -= 2; argv += 2; }
	if (argc < 3) { std::fprintf(stderr, "usage: fxcheck [--hlsl <dir>] <file.fx> <include dir>...\n"); return 2; }
	reshadefx::preprocessor pp;
	for (int i = 2; i < argc; ++i) pp.add_include_path(argv[i]);
	const char *defs[][2] = {{"__RESHADE__", "60800"}, {"__RESHADE_PERFORMANCE_MODE__", "0"}, {"__RENDERER__", "0xc000"}, {"__VENDOR__", "0x10de"},
		{"__DEVICE__", "0"}, {"__APPLICATION__", "0"}, {"BUFFER_WIDTH", "1920"}, {"BUFFER_HEIGHT", "1080"}, {"BUFFER_RCP_WIDTH", "(1.0 / 1920)"},
		{"BUFFER_RCP_HEIGHT", "(1.0 / 1080)"}, {"BUFFER_COLOR_SPACE", "1"}, {"BUFFER_COLOR_BIT_DEPTH", "8"}};
	for (auto &d : defs) pp.add_macro_definition(d[0], d[1]);
	if (!pp.append_file(argv[1])) { std::fprintf(stderr, "preprocess failed:\n%s\n", pp.errors().c_str()); return 1; }
	if (!pp.errors().empty()) std::fprintf(stderr, "%s", pp.errors().c_str());
	std::unique_ptr<reshadefx::codegen> cg(reshadefx::create_codegen_hlsl(50, false, false));
	reshadefx::parser parser;
	if (!parser.parse(pp.output(), cg.get())) { std::fprintf(stderr, "parse failed:\n%s\n", parser.errors().c_str()); return 1; }
	if (!parser.errors().empty()) std::fprintf(stderr, "%s", parser.errors().c_str());
	for (const int sm : {50, 51})
	{
		if (hlsl_dir == nullptr)
			break;
		std::unique_ptr<reshadefx::codegen> smcg(reshadefx::create_codegen_hlsl(sm, false, false));
		reshadefx::parser smparser;
		if (!smparser.parse(pp.output(), smcg.get())) { std::fprintf(stderr, "parse failed (sm %d)\n", sm); return 1; }
		for (const auto &[name, type] : smcg->module().entry_points)
		{
			std::string code, unused1, unused2;
			if (!smcg->assemble_code_for_entry_point(name, code, unused1, unused2)) { std::fprintf(stderr, "no code for %s\n", name.c_str()); return 1; }
			const char *kind = type == reshadefx::shader_type::vertex ? "vs" : type == reshadefx::shader_type::pixel ? "ps" : "cs";
			const std::string path = std::string(hlsl_dir) + "/" + kind + "_5_" + std::to_string(sm % 10) + "." + name + ".hlsl";
			FILE *f = std::fopen(path.c_str(), "wb");
			if (f == nullptr) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
			std::fwrite(code.data(), 1, code.size(), f);
			std::fclose(f);
		}
	}
	std::printf("%s: OK\n", argv[1]);
	return 0;
}
