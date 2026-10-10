#!/bin/bash
# AstraPassthrough.fx through ReShade's own FX compiler front end (preprocessor + parser + HLSL codegen, built
# natively from ReShade's source at the version the add-on targets): a syntax or type error shows here, not as a
# red line in a game's ReShade overlay.
#   tools/fxcheck.sh [file.fx ...]     (default: shaders/*.fx)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
RESHADE=6.8.0
SRC="$HERE/build/reshade-fx-$RESHADE"
SHADERS="$HERE/extern/minecraft-in-death-stranding/third_party/shaders" # ReShade.fxh (fetch_deps.sh)
[ -f "$SHADERS/ReShade.fxh" ] || { echo "run extern/minecraft-in-death-stranding/tools/fetch_deps.sh first"; exit 1; }
mkdir -p "$SRC"
for f in effect_codegen.hpp effect_codegen_hlsl.cpp effect_expression.cpp effect_expression.hpp effect_lexer.cpp effect_lexer.hpp \
	effect_module.hpp effect_parser.hpp effect_parser_exp.cpp effect_parser_stmt.cpp effect_preprocessor.cpp effect_preprocessor.hpp \
	effect_symbol_table.cpp effect_symbol_table.hpp effect_symbol_table_intrinsics.inl effect_token.hpp; do
	[ -f "$SRC/$f" ] || curl -fsSL "https://raw.githubusercontent.com/crosire/reshade/v$RESHADE/source/$f" -o "$SRC/$f"
done
if [ ! -x "$SRC/fxcheck" ] || [ "$HERE/tools/fxcheck.cpp" -nt "$SRC/fxcheck" ]; then
	${CXX:-clang++} -std=c++20 -O1 -w -I"$SRC" "$HERE/tools/fxcheck.cpp" "$SRC"/effect_{lexer,preprocessor,parser_exp,parser_stmt,symbol_table,expression,codegen_hlsl}.cpp \
		-o "$SRC/fxcheck"
fi
[ $# -gt 0 ] || set -- "$HERE"/shaders/*.fx
for fx in "$@"; do "$SRC/fxcheck" "$fx" "$SHADERS"; done
