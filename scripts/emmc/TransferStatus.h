// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace E5Emmc {
// SDHCI error summary is bit 15 and detailed errors occupy bits 16..31.
// A failed transfer can also set both completion bits: errors always win.
constexpr bool transferOk(uint32_t status) {
  return (status & 0xFFFF8000u) == 0 && (status & 0x03u) == 0x03u;
}
}
