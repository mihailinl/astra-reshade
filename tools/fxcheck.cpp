// Preprocess + parse a ReShade FX file with ReShade's own compiler front end (HLSL code generation): syntax and type
// errors before a game ever loads it.
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"
#include "effect_codegen.hpp"
#include <cstdio>
#include <memory>
int main(int argc, char **argv)
{
	if (argc < 3) { std::fprintf(stderr, "usage: fxcheck <file.fx> <include dir>...\n"); return 2; }
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
	std::printf("%s: OK\n", argv[1]);
	return 0;
}
