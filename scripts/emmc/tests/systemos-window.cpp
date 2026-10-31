// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../SystemOSWindow.h"
#include <stdio.h>
#include <stdint.h>

namespace {
int dataWindowChecks() {
  using namespace E5Data;
  // The Data window begins exactly where the SystemOS window ends and holds
  // a 32 MiB container; the card has 30244864 sectors total.
  if (base != E5SystemOS::base + E5SystemOS::sectors) return 11;
  if (sectors * 512 != 33554432ULL) return 12;
  if (!fits(30244864) || fits(base + sectors - 1)) return 13;
  if (!contains(0, sectors) || contains(sectors, 1)) return 14;
  return 0;
}
}

int main() {
  using namespace E5SystemOS;
  if (!fits(30244864) || !fits(base + sectors)) return 1;
  if (fits(base - 1) || fits(base + sectors - 1)) return 2;
  if (base * 512 < 6077580800ULL || sectors * 512 != 3303014400ULL) return 3;
  if (!contains(0, sectors) || !contains(sectors - 1, 1)) return 4;
  if (contains(0, 0) || contains(sectors, 1)) return 5;
  if (contains(sectors - 1, 2) || contains(1, sectors)) return 6;
  if (contains(UINT64_MAX, 1) || contains(1, UINT64_MAX)) return 7;
  if (int r = dataWindowChecks()) return r;
  puts("PASS: SystemOS card bounds, OS separation, final sector and overflow refusals");
  puts("PASS: Data window bounds, SystemOS adjacency and size");
}
