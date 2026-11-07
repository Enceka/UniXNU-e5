// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included inside E5Display. Queue and request contracts follow the pinned
// NativeLegacyDisplay reference; the E5 destination is 320x480 RGB565.
private:
  struct CpuState {
    IOMemoryMap *scanout;
    IOTimerEventSource *retry;
    const OriginalLegacySwapRequest *validated[8];
    uint32_t offered, accepted, copied, retired, failed, fenceWaits;
    bool kicking, stopped, timerAdded;
  };
  CpuState *cpu = nullptr;

  bool startCpuPresenter() {
    if (cpu) return cpu->scanout != nullptr;
    cpu = (CpuState *)IOMalloc(sizeof(CpuState));
    if (!cpu) return false;
    bzero(cpu, sizeof(*cpu));
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
    for (auto entry : cpu->validated) if (entry == request) return true;
    return false;
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
    // Compressed spellings require a producer contract E5 does not yet have.
    if (e5_surface_format(surface) != 0x42475241U || e5_surface_planes(surface)) return 8;
    IOMemoryDescriptor *memory = e5_surface_descriptor(surface);
    const uint64_t stride = e5_surface_stride(surface), allocation = e5_surface_allocation(surface);
    if (e5_surface_width(surface) < 320 || e5_surface_height(surface) < 480 ||
        !memory || !memory->metaCast("IOMemoryDescriptor") || stride < 1280 ||
        stride > UINT64_MAX / 480 || allocation < stride * 480 || memory->getLength() < allocation) return 9;
    return 0;
  }
  bool validateCpuRequest(const OriginalLegacySwapRequest *request) {
    if (!cpu) return false;
    unsigned reason = inspectRequest(request);
    ++cpu->offered;
    if (!reason && !knownRequest(request)) {
      unsigned i = 0;
      while (i < 8 && cpu->validated[i]) ++i;
      if (i == 8) reason = 10;
      else cpu->validated[i] = request;
    }
    if (!reason) ++cpu->accepted;
    OSObject *surface = request ? requestSurface(request, 0) : nullptr;
    if (cpu->offered <= 16 || !(cpu->offered & (cpu->offered - 1)))
      e5_cpu_log("E5CPU offer=%u accept=%u reason=%u req=%p surface=%p fmt=%x planes=%u masks=%x:%x",
          cpu->offered, cpu->accepted, reason, request, surface,
          surface && surface->metaCast("IOSurface") ? e5_surface_format(surface) : 0,
          surface && surface->metaCast("IOSurface") ? e5_surface_planes(surface) : 0,
          request ? requestWord(request, 792) : 0, request ? requestWord(request, 796) : 0);
    if (reason && cpu->offered <= 16 && request) {
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
    return reason == 0;
  }
  void copyCpuRequests() {
    if (!cpu || cpu->stopped || cpu->kicking) return;
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
      unsigned reason = inspectRequest(request);
      bool copied = false;
      OSObject *surface = requestSurface(request, 0);
      if (!reason) {
        volatile void *destination = (volatile void *)cpu->scanout->getVirtualAddress();
        if (!surface) copied = e5_blank_rgb565(destination, cpu->scanout->getLength(), 640, 320, 480);
        else {
          IOMemoryDescriptor *memory = e5_surface_descriptor(surface);
          IOMemoryMap *source = memory->map(kIOMapAnywhere | kIOMapReadOnly);
          if (source && source->getLength() >= e5_surface_allocation(surface))
            copied = e5_copy_bgra_rgb565(destination, cpu->scanout->getLength(), 640,
                (const void *)source->getVirtualAddress(), source->getLength(), e5_surface_stride(surface), 320, 480);
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
      ++cpu->copied;
      if (!activateOnlyPendingSwap(request)) {
        ++cpu->failed;
        e5_cpu_log("E5CPU activation-failed=%u req=%p", cpu->failed, request);
        break;
      }
      for (auto &entry : cpu->validated) if (entry == request) entry = nullptr;
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
