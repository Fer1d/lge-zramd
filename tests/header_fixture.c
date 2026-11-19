/* SPDX-License-Identifier: Apache-2.0 */
/* Both include orders use libc/sysroot linux headers, NOT the kernel tree. */
#ifdef RB_UAPI_FIRST
#include "uapi/linux/zram.h"
#include "request_backend.h"
#endif
#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdint.h>
#ifndef RB_UAPI_FIRST
#include "request_backend.h"
#include "uapi/linux/zram.h"
#endif

#define AT(type, member, offset) \
	_Static_assert(offsetof(struct type, member) == (offset), #type "." #member)

_Static_assert(sizeof(struct zram_wb_exec) == 160, "EXEC160");
_Static_assert(sizeof(struct zram_wb_status) == 192, "STATUS192");
_Static_assert(_Alignof(struct zram_wb_exec) == 8, "compat EXEC alignment");
_Static_assert(_Alignof(struct zram_wb_status) == 8, "compat STATUS alignment");
_Static_assert(_IOC_SIZE(ZRAM_IOC_WB_EXEC) == 160, "EXEC ioctl encoding");
_Static_assert(_IOC_SIZE(ZRAM_IOC_WB_STATUS) == 192, "STATUS ioctl encoding");
_Static_assert(_IOC_DIR(ZRAM_IOC_WB_EXEC) == (_IOC_READ | _IOC_WRITE), "EXEC direction");
_Static_assert(_IOC_DIR(ZRAM_IOC_WB_STATUS) == (_IOC_READ | _IOC_WRITE), "STATUS direction");
_Static_assert(_IOC_TYPE(ZRAM_IOC_WB_EXEC) == 'Z' && _IOC_NR(ZRAM_IOC_WB_EXEC) == 0x20, "EXEC number");
_Static_assert(_IOC_TYPE(ZRAM_IOC_WB_STATUS) == 'Z' && _IOC_NR(ZRAM_IOC_WB_STATUS) == 0x21, "STATUS number");
_Static_assert(_Generic(((struct zram_wb_exec){0}).op_errno, int32_t: 1, default: 0), "s32 op_errno");
_Static_assert(_Generic(((struct zram_wb_exec){0}).error_stage, uint32_t: 1, default: 0), "u32 stage");
_Static_assert(_Generic(((struct zram_wb_exec){0}).reserved_out, uint32_t: 1, default: 0), "u32 reserved out");

AT(zram_wb_exec, size, 0);
AT(zram_wb_exec, version, 4);
AT(zram_wb_exec, flags, 8);
AT(zram_wb_exec, mode, 12);
AT(zram_wb_exec, request_id, 16);
AT(zram_wb_exec, expected_instance, 24);
AT(zram_wb_exec, expected_generation, 32);
AT(zram_wb_exec, max_attempt_pages, 40);
AT(zram_wb_exec, max_installed_pages, 48);
AT(zram_wb_exec, page_index, 56);
AT(zram_wb_exec, idle_count, 64);
AT(zram_wb_exec, reserved0, 68);
AT(zram_wb_exec, reserved, 72);
AT(zram_wb_exec, scanned_pages, 96);
AT(zram_wb_exec, selected_pages, 104);
AT(zram_wb_exec, submitted_pages, 112);
AT(zram_wb_exec, installed_pages, 120);
AT(zram_wb_exec, invalidated_pages, 128);
AT(zram_wb_exec, duration_ns, 136);
AT(zram_wb_exec, stop_reason, 144);
AT(zram_wb_exec, op_errno, 148);
AT(zram_wb_exec, error_stage, 152);
AT(zram_wb_exec, reserved_out, 156);
AT(zram_wb_status, size, 0);
AT(zram_wb_status, version, 4);
AT(zram_wb_status, flags, 8);
AT(zram_wb_status, reserved0, 12);
AT(zram_wb_status, reserved, 16);
AT(zram_wb_status, capabilities, 32);
AT(zram_wb_status, instance, 40);
AT(zram_wb_status, generation, 48);
AT(zram_wb_status, disksize_bytes, 56);
AT(zram_wb_status, page_size, 64);
AT(zram_wb_status, state_flags, 68);
AT(zram_wb_status, wb_limit_remaining_4k, 72);
AT(zram_wb_status, pages_stored, 80);
AT(zram_wb_status, same_pages, 88);
AT(zram_wb_status, huge_pages, 96);
AT(zram_wb_status, lowratio_pages, 104);
AT(zram_wb_status, bd_count_pages, 112);
AT(zram_wb_status, bd_reads_pages, 120);
AT(zram_wb_status, bd_writes_pages, 128);
AT(zram_wb_status, compr_data_bytes, 136);
AT(zram_wb_status, mem_used_bytes, 144);
AT(zram_wb_status, reserved_out, 152);

#ifdef RB_EXPECT_ANDROID
#ifndef __ANDROID__
#error Android build must use the actual NDK sysroot
#endif
_Static_assert(__ANDROID_API__ == 29, "Android API 29 required");
#endif

int main(void)
{
	return 0;
}
