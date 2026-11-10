#include "xf86drm.h"
#include <errno.h>
#include <stdlib.h>

int drmIoctl(int fd, unsigned long request, void *arg)
{ (void)fd; (void)request; (void)arg; errno = ENOTSUP; return -1; }

drmVersionPtr drmGetVersion(int fd)
{ (void)fd; errno = ENOTSUP; return NULL; }

void drmFreeVersion(drmVersionPtr v)
{ (void)v; if (v) { free(v->name); free(v->date); free(v->desc); free(v); } }

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{ (void)fd; (void)flags; (void)device; errno = ENOTSUP; return -1; }

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{ (void)flags; (void)devices; (void)max_devices; return 0; }

void drmFreeDevice(drmDevicePtr *device)
{ (void)device; if (device) *device = NULL; }

void drmFreeDevices(drmDevicePtr devices[], int count)
{ (void)devices; (void)count; }

int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{ (void)a; (void)b; return 0; }

int drmGetMagic(int fd, drm_magic_t *magic)
{ (void)fd; (void)magic; errno = ENOTSUP; return -1; }

int drmGetCap(int fd, uint64_t capability, uint64_t *value)
{ (void)fd; (void)capability; (void)value; errno = ENOTSUP; return -1; }

int drmCloseBufferHandle(int fd, uint32_t handle)
{ (void)fd; (void)handle; errno = ENOTSUP; return -1; }

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{ (void)fd; (void)handle; (void)flags; (void)prime_fd; errno = ENOTSUP; return -1; }

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{ (void)fd; (void)prime_fd; (void)handle; errno = ENOTSUP; return -1; }

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{ (void)fd; (void)flags; (void)handle; errno = ENOTSUP; return -1; }

int drmSyncobjDestroy(int fd, uint32_t handle)
{ (void)fd; (void)handle; errno = ENOTSUP; return -1; }

int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{ (void)fd; (void)handle; (void)obj_fd; errno = ENOTSUP; return -1; }

int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{ (void)fd; (void)obj_fd; (void)handle; errno = ENOTSUP; return -1; }

int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; errno = ENOTSUP; return -1; }

int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{ (void)fd; (void)handle; (void)sync_file_fd; errno = ENOTSUP; return -1; }

int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{ (void)fd; (void)handles; (void)num_handles; (void)timeout_nsec; (void)flags; (void)first_signaled; errno = ENOTSUP; return -1; }

int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{ (void)fd; (void)handles; (void)handle_count; errno = ENOTSUP; return -1; }

int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{ (void)fd; (void)handles; (void)handle_count; errno = ENOTSUP; return -1; }

int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{ (void)fd; (void)handles; (void)points; (void)num_handles; (void)timeout_nsec; (void)flags; (void)first_signaled; errno = ENOTSUP; return -1; }

int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count)
{ (void)fd; (void)handles; (void)points; (void)handle_count; errno = ENOTSUP; return -1; }

int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{ (void)fd; (void)handles; (void)points; (void)handle_count; errno = ENOTSUP; return -1; }

int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{ (void)fd; (void)handles; (void)points; (void)handle_count; (void)flags; errno = ENOTSUP; return -1; }

int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags)
{ (void)fd; (void)dst_handle; (void)dst_point; (void)src_handle; (void)src_point; (void)flags; errno = ENOTSUP; return -1; }

