// Shim para compilar las HERRAMIENTAS de XenonRecomp con MSVC (el C++ generado sigue necesitando Clang).
#pragma once
#ifdef _MSC_VER
#include <stdlib.h>
#include <intrin.h>
#define __builtin_bswap16 _byteswap_ushort
#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap64 _byteswap_uint64
static __inline int __builtin_ffsll(long long v) { unsigned long i; return _BitScanForward64(&i, (unsigned long long)v) ? (int)i + 1 : 0; }
static __inline int __builtin_clz(unsigned int v) { unsigned long i; _BitScanReverse(&i, v); return 31 - (int)i; }
static __inline int __builtin_clzll(unsigned long long v) { unsigned long i; _BitScanReverse64(&i, v); return 63 - (int)i; }
static __inline int __builtin_ctz(unsigned int v) { unsigned long i; _BitScanForward(&i, v); return (int)i; }
#define __builtin_unreachable() __assume(0)
#endif
