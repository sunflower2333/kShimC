#pragma once
#define BIT(x)          (0x1ULL << x)
// Generates bits from position y to position (x-1). Note: x must be greater than y.
#define GENBITS(x, y)   (((1ULL << ((x) - (y) + 1)) - 1) << (y))

#define SIZE_B(x) (x)
#define SIZE_K(x) (x * 1024 * SIZE_B(1))
#define SIZE_M(x) (x * 1024 * SIZE_K(1))
#define SIZE_G(x) (x * 1024 * SIZE_M(1))