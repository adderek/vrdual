// Build identity, set per binary by the Makefile at compile time
// (tools/build_info.sh): "vrdual v0.1.0-3-gabc1234-dirty, built
// 2026-10-21 18:05:09 +0200 on fractal". Layers log it once per process;
// tools print it with --version.
#pragma once
#ifndef VRDUAL_BUILD_INFO
#define VRDUAL_BUILD_INFO "vrdual (unknown build)"
#endif
