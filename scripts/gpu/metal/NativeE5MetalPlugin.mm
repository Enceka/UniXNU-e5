// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"

@interface _MTLDevice : NSObject
@end
@interface NativeE5MetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

static void consoleLine(const char *text) {
    while (text && *text) {
        char line[252];
        int prefix = snprintf(line, sizeof(line), "E5METAL pid=%d ", getpid());
        size_t count = strnlen(text, sizeof(line) - (size_t)prefix - 2);
        memcpy(line + prefix, text, count);
        size_t size = (size_t)prefix + count; line[size++] = '\n';
        if (write(229, line, size) != (ssize_t)size) (void)write(2, line, size);
        text += count;
    }
}

static void missing(const char *klass, const char *selector) {
    char line[240]; snprintf(line, sizeof(line), "missing %s %s", klass, selector);
    consoleLine(line);
}

static NSString *shaderCache() {
    NSMutableArray<NSString *> *parents = [NSMutableArray array];
    NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString *persistent = [[paths firstObject] stringByAppendingPathComponent:
        [NSString stringWithFormat:@"com.apple.%s", getprogname()]];
    if (persistent) [parents addObject:persistent];
    const int selectors[] = {_CS_DARWIN_USER_CACHE_DIR, _CS_DARWIN_USER_TEMP_DIR};
    for (int selector : selectors) {
        char path[1024]; size_t bytes = confstr(selector, path, sizeof(path));
        if (bytes && bytes < sizeof(path)) [parents addObject:[NSString stringWithUTF8String:path]];
    }
    if (NSTemporaryDirectory()) [parents addObject:NSTemporaryDirectory()];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (NSString *parent in parents) {
        NSString *cache = [parent stringByAppendingPathComponent:@"e5-metal"];
        if ([seen containsObject:cache]) continue;
        [seen addObject:cache];
        NSError *error = nil;
        if (![[NSFileManager defaultManager] createDirectoryAtPath:cache
                withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions: @0700} error:&error]) continue;
        char path[1200];
        int length = snprintf(path, sizeof(path), "%s/.write-test-XXXXXX", cache.fileSystemRepresentation);
        if (length <= 0 || length >= (int)sizeof(path)) continue;
        int fd = mkstemp(path);
        if (fd < 0) continue;
        close(fd); unlink(path);
        consoleLine([[@"SYSTEM CACHE " stringByAppendingString:cache] UTF8String]);
        return cache;
    }
    // The pinned bridge's cache-optional path keeps translation in memory
    // when a system application's sandbox provides no writable directory.
    setenv("METAL2VULKAN_SHADER_CACHE_OPTIONAL", "1", 1);
    consoleLine("SYSTEM CACHE memory-only");
    return [(persistent ?: @"/var/mobile/tmp") stringByAppendingPathComponent:@"e5-metal"];
}

extern "C" bool native_e5_metal_limit_pipeline_makers();

// The dedicated test supplies the same native ICD before system discovery is enabled.
extern "C" __attribute__((visibility("default")))
id native_e5_metal_open_for_test(const char *icd, const char *cache, unsigned port) {
    uint64_t registry = 0;
    kern_return_t result = port ? IORegistryEntryGetRegistryEntryID(port, &registry) : KERN_INVALID_ARGUMENT;
    if (result != KERN_SUCCESS) {
        char message[160]; snprintf(message, sizeof(message), "SYSTEM REGISTRY FAIL port=%u error=%x", port, result);
        consoleLine(message);
        return nil;
    }
    static const bool limited = native_e5_metal_limit_pipeline_makers();
    consoleLine(limited ? "COMPILE BUDGET makers=1 ahead=1" : "COMPILE BUDGET already initialized");
    native_bridge_metal_set_console(consoleLine);
    return native_bridge_metal_open(icd, cache, registry, missing);
}

@implementation NativeE5MetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    char entry[120]; snprintf(entry, sizeof(entry), "SYSTEM OPEN accelerator=%u", port);
    consoleLine(entry);
    self = [super init];
    if (!self || !port) return nil;
    Dl_info info = {};
    if (!dladdr((const void *)&native_e5_metal_open_for_test, &info) || !info.dli_fname) return nil;
    NSString *directory = [[NSString stringWithUTF8String:info.dli_fname] stringByDeletingLastPathComponent];
    NSString *icd = [directory stringByAppendingPathComponent:@"libvulkan_panfrost.dylib"];
    // The E5 stack renders ordinary rows; it cannot produce Apple's private
    // compressed framebuffer layout. Match the upstream frontend policy.
    setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
    CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
    NSString *cache = shaderCache();
    return native_e5_metal_open_for_test(icd.fileSystemRepresentation, cache.fileSystemRepresentation, port);
}
@end
