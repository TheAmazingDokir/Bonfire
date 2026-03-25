#pragma once

// Set helper macros
#define SET_BIT(arr, i) (arr[(i) / 8] |= (1 << ((i) % 8)))
#define CLEAR_BIT(arr, i) (arr[(i) / 8] &= ~(1 << ((i) % 8)))
#define IS_BIT_SET(arr, i) (arr[(i) / 8] & (1 << ((i) % 8)))