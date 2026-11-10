// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opt-in registry publication for the original iOS 17 Metal plug-in loader.
// IOAcceleratorES contributes no operational overrides on this exact ABI.
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
namespace E5MetalDiscovery {
class Meta;
static const OSMetaClass *meta;
static IOService *accelerator;
class Accelerator : public IOService {
public:
    Accelerator();
    const OSMetaClass *getMetaClass() const override { return meta; }
protected:
    ~Accelerator() override;
};
class Meta : public OSMetaClass {
public:
    static void *operator new(size_t, void *where) { return where; }
    Meta(const OSMetaClass *parent, const char *name)
        : OSMetaClass(name, parent, sizeof(IOService)) {}
    OSObject *alloc() const override { return new Accelerator; }
    void constructed() const { instanceConstructed(); }
    void destructed() const { instanceDestructed(); }
};
Accelerator::Accelerator() : IOService(meta) { static_cast<const Meta *>(meta)->constructed(); }
Accelerator::~Accelerator() { static_cast<const Meta *>(meta)->destructed(); }
static void publish(IOService *gpu) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("e5-metal", &enabled, sizeof(enabled)) || enabled != 1) return;
    static bool attempted;
    if (attempted) return;
    attempted = true;
    // This is the native_sandbox_grant user-client contract in this OS profile.
    static const char clients[] = "backboardd,SpringBoard,iconservicesagen,Preferences";
    gpu->setProperty("NativeSandboxGrantProcess", clients);
    kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (result) { note("E5METALREG FAIL IOGPUFamily=%x", result); return; }
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!parent || parent->getClassSize() != sizeof(IOService)) {
        note("E5METALREG FAIL superclass size=%u expected=%u", parent ? parent->getClassSize() : 0, (unsigned)sizeof(IOService));
        return;
    }
    // The target sandbox uses the AGX accelerator class name for its Metal
    // discovery ABI. Preserve an existing original class if it is loaded.
    const OSSymbol *compat = OSSymbol::withCString("AGXAccelerator");
    const bool taken = compat && OSMetaClass::getMetaClassWithName(compat);
    if (compat) compat->release();
    const char *nameForABI = taken ? "NativeE5Accelerator" : "AGXAccelerator";
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { note("E5METALREG FAIL preModLoad"); return; }
    alignas(16) static unsigned char storage[sizeof(Meta)];
    meta = new (static_cast<void *>(storage)) Meta(parent, nameForABI);
    result = OSMetaClass::postModLoad(handle);
    if (result) { note("E5METALREG FAIL registration=%x", result); return; }
    accelerator = static_cast<IOService *>(meta->alloc());
    if (!accelerator || !accelerator->init()) {
        if (accelerator) accelerator->release();
        accelerator = nullptr;
        note("E5METALREG FAIL allocation"); return;
    }
    accelerator->setProperty("MetalPluginName", "NativeE5Metal");
    accelerator->setProperty("MetalPluginClassName", "NativeE5MetalDevice");
    accelerator->setProperty("NativeSandboxGrantProcess", clients);
    if (!accelerator->attach(gpu)) {
        accelerator->release(); accelerator = nullptr;
        note("E5METALREG FAIL attach"); return;
    }
    accelerator->registerService();
    note("E5METALREG PASS class=%s plugin=NativeE5Metal entry=NativeE5MetalDevice", nameForABI);
}
} // namespace E5MetalDiscovery
