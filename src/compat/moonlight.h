/**
 * @file src/compat/moonlight.h
 * @brief Import Moonlight's internal C declarations with C++ atomic support.
 */
#pragma once

// standard includes
// C++23's compatibility header defines atomic types using C++ templates. Load it before the C linkage block.
#include <stdatomic.h>

// lib includes
extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
}
