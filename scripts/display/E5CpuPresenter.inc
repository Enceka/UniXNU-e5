// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included inside E5Display. Queue and request contracts follow the pinned
// NativeLegacyDisplay reference; the E5 destination is 320x480 RGB565.
private:
  struct CpuState {
    struct Validated {
      const OriginalLegacySwapRequest *request;
      OSObject *surface; // Identity only; the original pending request owns it.
    };
    IOMemoryMap *scanout;
    IOTimerEventSource *retry;
    Validated validated[8];
    uint32_t offered, accepted, copied, retired, failed, fenceWaits, snapshots;
    bool kicking, stopped, timerAdded, canvas, pixels, gpuScanout, periodicCaptures;
    uint32_t lastReject, lastFormat, lastSourceRgb, lastDestinationRgb;
    bool lastBlank;
  };
  CpuState *cpu = nullptr;

  bool startCpuPresenter() {
    if (cpu) return cpu->canvas || cpu->gpuScanout || cpu->scanout != nullptr;
    cpu = (CpuState *)IOMalloc(sizeof(CpuState));
    if (!cpu) return false;
    bzero(cpu, sizeof(*cpu));
    unsigned trace=0;PE_parse_boot_argn("e5-ui-pixels",&trace,sizeof(trace));cpu->pixels=trace==1;
    unsigned capturePeriod=0;PE_parse_boot_argn("e5-hid-ui-capture",&capturePeriod,sizeof(capturePeriod));
    cpu->periodicCaptures=capturePeriod>=10 && capturePeriod<=60;
    cpu->canvas = e5_touch_canvas_selected();
    cpu->gpuScanout = e5_mali_g1_scanout_selected();
    if (cpu->gpuScanout) {
      e5_cpu_log("E5CPU scanout-owner=gpu-selftest compositor-copy=0");
      return true;
    }
    if (cpu->canvas) {
      // The diagnostic owns every scanout store. Do not map, copy, or retire
      // compositor frames as though those pixels had been presented.
      e5_cpu_log("E5CPU scanout-owner=touch-canvas compositor-copy=0");
      return true;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(0xbe000000ULL, 0x4b000ULL);
    if (memory) {
      cpu->scanout = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
      memory->release();
    }
    if (!cpu->scanout || !cpu->scanout->getVirtualAddress() ||
        cpu->scanout->getPhysicalAddress() != 0xbe000000ULL ||
        cpu->scanout->getLength() != 0x4b000ULL) return false;
    cpu->retry = IOTimerEventSource::timerEventSource(this,
        [](OSObject *owner, IOTimerEventSource *) { static_cast<E5Display *>(owner)->copyCpuRequests(); });
    IOWorkLoop *loop = getWorkLoop();
    if (!cpu->retry || !loop || loop->addEventSource(cpu->retry) != kIOReturnSuccess) return false;
    cpu->timerAdded = true;
    return true;
  }
  void stopCpuPresenter() {
    if (!cpu) return;
    if (cpu->gpuScanout)
      __atomic_store_n(&e5GpuScanoutOwned, 0U, __ATOMIC_RELEASE);
    auto action = [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
      auto display = static_cast<E5Display *>(owner);
      if (display->cpu) {
        display->cpu->stopped = true;
        if (display->cpu->retry) display->cpu->retry->cancelTimeout();
      }
      return kIOReturnSuccess;
    };
    if (IOWorkLoop *loop = getWorkLoop()) loop->runAction(action, this);
    else cpu->stopped = true;
    if (cpu->timerAdded) {
      getWorkLoop()->removeEventSource(cpu->retry);
      cpu->timerAdded = false;
    }
  }
  void releaseCpuPresenter() {
    if (!cpu) return;
    stopCpuPresenter();
    if (cpu->retry) cpu->retry->release();
    if (cpu->scanout) cpu->scanout->release();
    IOFree(cpu, sizeof(*cpu));
    cpu = nullptr;
  }
  static uint32_t requestWord(const OriginalLegacySwapRequest *request, unsigned offset) {
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *)request + offset, sizeof(value));
    return value;
  }
  static OSObject *requestSurface(const OriginalLegacySwapRequest *request, unsigned layer) {
    OSObject *surface = nullptr;
    memcpy(&surface, (const uint8_t *)request + 2624 + layer * sizeof(surface), sizeof(surface));
    return surface;
  }
  bool knownRequest(const OriginalLegacySwapRequest *request) const {
    if (!cpu || !request) return false;
    for (auto entry : cpu->validated) if (entry.request == request) return true;
    return false;
  }
  bool b3a8Layout(OSObject *surface, E5B3a8Layout &layout) {
    const uint64_t width = e5_surface_width(surface), height = e5_surface_height(surface);
    if (width < 320 || height < 480 || width > 16384 || height > 16384) return false;
    for (unsigned plane = 0; plane < 2; ++plane) {
      // Linear planes must describe the whole image and have no separate
      // compressed/header offset. Bindings are pinned to the original kext.
      if (e5_surface_plane_width(surface, plane) != width ||
          e5_surface_plane_height(surface, plane) != height ||
          e5_surface_plane_offset(surface, plane) != e5_surface_plane_data_offset(surface, plane)) return false;
    }
    return e5_b3a8_layout(e5_surface_allocation(surface), (uint32_t)width, (uint32_t)height,
        e5_surface_plane_data_offset(surface, 0), e5_surface_plane_stride(surface, 0),
        e5_surface_plane_data_offset(surface, 1), e5_surface_plane_stride(surface, 1), layout);
  }
  unsigned inspectSurface(OSObject *surface) {
    if (!surface) return 0; // Only called for a previously validated blank plan.
    if (!surface->metaCast("IOSurface")) return 6;
    const uint32_t format = e5_surface_format(surface), planes = e5_surface_planes(surface);
    const bool b3a8 = format == 0x62336138U && planes == 2;
    if (!b3a8 && (format != 0x42475241U || planes)) return 8;
    IOMemoryDescriptor *memory = e5_surface_descriptor(surface);
    const uint64_t stride = e5_surface_stride(surface), allocation = e5_surface_allocation(surface);
    if (!memory || !memory->metaCast("IOMemoryDescriptor") || memory->getLength() < allocation) return 9;
    if (b3a8) { E5B3a8Layout layout{}; return b3a8Layout(surface, layout) ? 0 : 9; }
    if (e5_surface_width(surface) < 320 || e5_surface_height(surface) < 480 ||
        !memory || !memory->metaCast("IOMemoryDescriptor") || stride < 1280 ||
        stride > UINT64_MAX / 480 || allocation < stride * 480 || memory->getLength() < allocation) return 9;
    return 0;
  }
  unsigned inspectRequest(const OriginalLegacySwapRequest *request) {
    if (!cpu || cpu->stopped || !cpu->scanout || !bootValidated || !request) return 1;
    const uint32_t updated = requestWord(request, 792), disabled = requestWord(request, 796);
    const uint32_t background = requestWord(request, 800), known = 0x8000000fU;
    if (((updated | disabled) & ~known) || (disabled & ~updated) ||
        requestWord(request, 804) || requestWord(request, 808) || requestWord(request, 812) ||
        requestWord(request, 816) != 320 || requestWord(request, 820) != 480) return 2;
    if ((updated & 0x80000000U) ? (!(disabled & 0x80000000U) || background != 0xff000000U) : background != 0) return 3;
    if (requestSurface(request, 1) || requestSurface(request, 2) || requestSurface(request, 3)) return 4;
    OSObject *surface = requestSurface(request, 0);
    if (!surface) return updated && updated == disabled ? 0 : 5;
    if ((updated & ~disabled) != 1U || !surface->metaCast("IOSurface")) return 6;
    const unsigned rectangleOffsets[] = {17U, 81U};
    for (unsigned offset : rectangleOffsets) {
      uint32_t rect[4]; memcpy(rect, (const uint8_t *)request + offset, sizeof(rect));
      if (rect[0] || rect[1] || rect[2] != 320 || rect[3] != 480) return 7;
    }
    // Only BGRA and the linear b3a8 producer contract are supported.
    return inspectSurface(surface);
  }
  bool validateCpuRequest(const OriginalLegacySwapRequest *request) {
    if (!cpu) return false;
    if (cpu->canvas || cpu->gpuScanout) {
      if (++cpu->offered <= 4 || !(cpu->offered & (cpu->offered - 1)))
        e5_cpu_log("E5CPU %s-rejected=%u", cpu->gpuScanout ? "gpu" : "canvas", cpu->offered);
      return false;
    }
    unsigned reason = inspectRequest(request);
    ++cpu->offered;
    if (!reason) {
      unsigned i = 0;
      while (i < 8 && cpu->validated[i].request != request) ++i;
      if (i == 8) {
        i = 0;
        while (i < 8 && cpu->validated[i].request) ++i;
      }
      if (i == 8) reason = 10;
      else cpu->validated[i] = {request, requestSurface(request, 0)};
    }
    if (reason) for (auto &entry : cpu->validated)
      if (entry.request == request) entry = {};
    if (!reason) ++cpu->accepted;
    OSObject *surface = request ? requestSurface(request, 0) : nullptr;
    const uint32_t format=surface && surface->metaCast("IOSurface")?e5_surface_format(surface):0;
    const bool changedReject=cpu->pixels && reason && (reason!=cpu->lastReject || format!=cpu->lastFormat);
    if (cpu->offered <= 16 || !(cpu->offered & (cpu->offered - 1)) || changedReject)
      e5_cpu_log("E5CPU offer=%u accept=%u reason=%u req=%p surface=%p fmt=%x planes=%u masks=%x:%x",
          cpu->offered, cpu->accepted, reason, request, surface,
          surface && surface->metaCast("IOSurface") ? e5_surface_format(surface) : 0,
          surface && surface->metaCast("IOSurface") ? e5_surface_planes(surface) : 0,
          request ? requestWord(request, 792) : 0, request ? requestWord(request, 796) : 0);
    if (format == 0x62336138U && (cpu->offered <= 4 || changedReject)) {
      for (unsigned plane = 0; plane < 2; ++plane)
        e5_cpu_log("E5CPUPLANE p=%u wh=%llu:%llu offsets=%llu:%llu stride=%llu",
            plane, (unsigned long long)e5_surface_plane_width(surface, plane),
            (unsigned long long)e5_surface_plane_height(surface, plane),
            (unsigned long long)e5_surface_plane_offset(surface, plane),
            (unsigned long long)e5_surface_plane_data_offset(surface, plane),
            (unsigned long long)e5_surface_plane_stride(surface, plane));
    }
    if (reason && (cpu->offered <= 16 || changedReject) && request) {
      e5_cpu_log("E5CPUREQ bg=%x opts=%x:%x:%x dims=%u:%u rect=%u:%u:%u:%u dst=%u:%u:%u:%u",
          requestWord(request, 800), requestWord(request, 804), requestWord(request, 808), requestWord(request, 812),
          requestWord(request, 816), requestWord(request, 820), requestWord(request, 17), requestWord(request, 21),
          requestWord(request, 25), requestWord(request, 29), requestWord(request, 81), requestWord(request, 85),
          requestWord(request, 89), requestWord(request, 93));
      if (surface && surface->metaCast("IOSurface")) {
        IOMemoryDescriptor *memory = e5_surface_descriptor(surface);
        e5_cpu_log("E5CPUSURF wh=%llu:%llu stride=%llu alloc=%llu descriptor=%llu",
            (unsigned long long)e5_surface_width(surface), (unsigned long long)e5_surface_height(surface),
            (unsigned long long)e5_surface_stride(surface), (unsigned long long)e5_surface_allocation(surface),
            (unsigned long long)(memory && memory->metaCast("IOMemoryDescriptor") ? memory->getLength() : 0));
      }
    }
    cpu->lastReject=reason;cpu->lastFormat=format;
    return reason == 0;
  }
  // Two bounded captures of the actual destination, while the presenter's
  // work loop owns it. RLE keeps flat UI regions small; the host rejects
  // incomplete rows or a mismatched whole-frame checksum. This is memory
  // readback, not evidence that the physical panel scanned the frame.
  void captureCpuFrame() {
    const volatile uint16_t *pixels =
        (const volatile uint16_t *)cpu->scanout->getVirtualAddress();
    const unsigned id = cpu->copied;
    uint32_t checksum = 2166136261U;
    e5_cpu_log("E5FRAME begin id=%u width=320 height=480 format=RGB565", id);
    static const char hex[] = "0123456789abcdef";
    for (unsigned y = 0; y < 480; ++y) {
      char prefix[64];
      snprintf(prefix, sizeof(prefix), "E5FRAMEROW id=%u y=%u ", id, y);
      for (const char *p = prefix; *p; ++p) e5_kputc(*p);
      for (unsigned x = 0; x < 320;) {
        const uint16_t pixel = pixels[y * 320 + x];
        unsigned run = 1;
        while (x + run < 320 && pixels[y * 320 + x + run] == pixel) ++run;
        for (unsigned i = 0; i < run; ++i) {
          checksum = (checksum ^ (pixel & 0xffU)) * 16777619U;
          checksum = (checksum ^ (pixel >> 8)) * 16777619U;
        }
        const uint32_t pair = (run << 16) | pixel;
        for (int shift = 28; shift >= 0; shift -= 4) e5_kputc(hex[(pair >> shift) & 15]);
        x += run;
      }
      e5_kputc('\n');
    }
    e5_cpu_log("E5FRAME end id=%u fnv=%08x", id, checksum);
    ++cpu->snapshots;
  }
  void copyCpuRequests() {
    if (!cpu || cpu->stopped || cpu->kicking || cpu->gpuScanout) return;
    cpu->kicking = true;
    while (const OriginalLegacySwapRequest *request = pendingSwapRequest()) {
      if (!knownRequest(request)) break;
      // Preserve the family's retained fence. A read reservation must be
      // active before touching the source, and completion belongs to the
      // original retirement path after our copy.
      const uint8_t *fence = nullptr;
      memcpy(&fence, (const uint8_t *)request + 0xa20, sizeof(fence));
      uint32_t completed = 0;
      uint8_t active = 0;
      if (fence) {
        memcpy(&completed, fence + 0x4c, sizeof(completed));
        active = *(volatile const uint8_t *)(fence + 0x53);
        if (!active || completed) {
          ++cpu->fenceWaits;
          if (!(cpu->fenceWaits & (cpu->fenceWaits - 1)))
            e5_cpu_log("E5CPU fence-wait=%u req=%p active=%u completed=%u", cpu->fenceWaits, request, active, completed);
          cpu->retry->setTimeoutMS(16);
          break;
        }
      }
      const uint64_t presentBegin=mach_absolute_time();
      bool copied = false;E5PixelSummary pixels{};bool sampled=false;
      const bool sample=cpu->pixels;
      OSObject *surface = requestSurface(request, 0);
      // Original post-validation layer bookkeeping ORs consumed bits into
      // request+796. Those mutable words no longer describe the submitted
      // composition. Use the accepted full-frame plan, require the same
      // retained surface, and recheck only its format/backing bounds here.
      unsigned reason = 11;
      for (auto entry : cpu->validated)
        if (entry.request == request && entry.surface == surface) reason = inspectSurface(surface);
      if (cpu->copied < 16)
        e5_cpu_log("E5CPU pending req=%p surface=%p masks=%x:%x reason=%u",
            request, surface, requestWord(request, 792), requestWord(request, 796), reason);
      if (!reason) {
        volatile void *destination = (volatile void *)cpu->scanout->getVirtualAddress();
        if (!surface) copied = e5_blank_rgb565(destination, cpu->scanout->getLength(), 640, 320, 480);
        else {
          IOMemoryDescriptor *memory = e5_surface_descriptor(surface);
          IOMemoryMap *source = memory->map(kIOMapAnywhere | kIOMapReadOnly);
          if (source && source->getLength() >= e5_surface_allocation(surface)) {
            const void *base = (const void *)source->getVirtualAddress();
            if (e5_surface_format(surface) == 0x62336138U) {
              E5B3a8Layout layout{};
              copied = b3a8Layout(surface, layout) && e5_copy_b3a8_rgb565(destination,
                  cpu->scanout->getLength(), 640, base, e5_surface_allocation(surface),
                  layout, 320, 480, sample ? &pixels : nullptr);
              sampled = sample && copied;
            } else {
              copied = e5_copy_bgra_rgb565(destination, cpu->scanout->getLength(), 640,
                  base, source->getLength(), e5_surface_stride(surface), 320, 480, sample ? &pixels : nullptr);
              sampled = sample && copied;
            }
          }
          if (source) source->release();
        }
      }
      if (!copied) {
        ++cpu->failed;
        e5_cpu_log("E5CPU copy-failed=%u reason=%u req=%p", cpu->failed, reason, request);
        break;
      }
      // Finish every device-memory store before the family releases the
      // request's source ownership. No hardware vblank is claimed here.
      __asm__ volatile("dsb sy" ::: "memory");
      if(sample) {
        uint32_t rgb=0,ink=0,hash=2166136261u;
        auto out=(const volatile uint16_t *)cpu->scanout->getVirtualAddress();
        const uint16_t background=out[0];
        auto words=(const volatile E5PixelWord *)out;
        for(unsigned i=0;i<320*480/2;++i) {
          const uint32_t pair=words[i];
          for(unsigned half=0;half<2;++half) {
            const uint16_t p=(uint16_t)(pair>>(half*16));
            rgb+=p!=0;ink+=p!=background;
            hash=(hash^(p&255))*16777619u;hash=(hash^(p>>8))*16777619u;
          }
        }
        __atomic_store_n(&e5CpuPresentedRgb,rgb,__ATOMIC_RELEASE);
        __atomic_store_n(&e5CpuPresentedInk,ink,__ATOMIC_RELEASE);
        if(cpu->copied<8 || (cpu->copied%64)==0 || (surface==nullptr)!=cpu->lastBlank ||
            (pixels.rgb<=64)!=(cpu->lastSourceRgb<=64) || (rgb<=64)!=(cpu->lastDestinationRgb<=64))
          e5_cpu_log("E5PIX next=%u ts=%llu blank=%u valid=%u source_rgb=%u alpha=%u opaque=%u rgb_alpha0=%u source_fnv=%x destination_rgb=%u destination_fnv=%x",
            cpu->copied+1,(unsigned long long)mach_absolute_time(),surface==nullptr,sampled?1u:0u,pixels.rgb,pixels.alpha,pixels.opaque,pixels.rgbWithZeroAlpha,pixels.hash,rgb,hash);
        cpu->lastSourceRgb=pixels.rgb;cpu->lastDestinationRgb=rgb;cpu->lastBlank=surface==nullptr;
      }
      uint64_t presentNs=0;absolutetime_to_nanoseconds(mach_absolute_time()-presentBegin,&presentNs);
      __atomic_store_n(&e5CpuLastPresentNs,presentNs,__ATOMIC_RELEASE);
      ++cpu->copied;
      __atomic_add_fetch(&e5CpuPresentedFrames,1U,__ATOMIC_RELEASE);
      if (surface && !cpu->periodicCaptures && (!cpu->snapshots || (cpu->snapshots == 1 && cpu->copied >= 64)))
        captureCpuFrame();
      if (!activateOnlyPendingSwap(request)) {
        ++cpu->failed;
        e5_cpu_log("E5CPU activation-failed=%u req=%p", cpu->failed, request);
        break;
      }
      for (auto &entry : cpu->validated) if (entry.request == request) entry = {};
      IOMobileFramebufferLegacy::completeCurrentSwap();
      ++cpu->retired;
      if (cpu->retired <= 16 || !(cpu->retired & (cpu->retired - 1)))
        e5_cpu_log("E5CPU copied=%u retired=%u failed=%u blank=%u", cpu->copied, cpu->retired, cpu->failed, surface == nullptr);
    }
    cpu->kicking = false;
  }
public:
  void free() override {
    releaseCpuPresenter();
    IOMobileFramebufferLegacy::free();
  }
  bool validateSwap(const OriginalLegacySwapRequest *request) override {
    IOWorkLoop *loop = getWorkLoop();
    if (!loop) return false;
    struct Validation { const OriginalLegacySwapRequest *request; bool accepted; } v = {request, false};
    loop->runAction([](OSObject *owner, void *argument, void *, void *, void *) -> IOReturn {
      auto v = static_cast<Validation *>(argument);
      v->accepted = static_cast<E5Display *>(owner)->validateCpuRequest(v->request);
      return kIOReturnSuccess;
    }, this, &v);
    return v.accepted;
  }
  void kickSwapQueue() override {
    if (IOWorkLoop *loop = getWorkLoop())
      loop->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
        static_cast<E5Display *>(owner)->copyCpuRequests();
        return kIOReturnSuccess;
      }, this);
  }
  IOReturn collectCompletionReports(uint32_t, void *reports, uint32_t *count, uint32_t capacity) override {
    if (count) *count = 0;
    // This CPU backend has no hardware presentation timestamp reports.
    return reports && (!count || !capacity) ? kIOReturnBadArgument : kIOReturnSuccess;
  }
