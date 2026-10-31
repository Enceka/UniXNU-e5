// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../SystemOSWindow.h"
#include <stdio.h>
#include <stdint.h>

int main() {
  using namespace E5SystemOS;
  if (!fits(30244864) || !fits(base + sectors)) return 1;
  if (fits(base - 1) || fits(base + sectors - 1)) return 2;
  if (base * 512 < 6077580800ULL || sectors * 512 != 3303014400ULL) return 3;
  if (!contains(0, sectors) || !contains(sectors - 1, 1)) return 4;
  if (contains(0, 0) || contains(sectors, 1)) return 5;
  if (contains(sectors - 1, 2) || contains(1, sectors)) return 6;
  if (contains(UINT64_MAX, 1) || contains(1, UINT64_MAX)) return 7;
  puts("PASS: SystemOS card bounds, OS separation, final sector and overflow refusals");
}
