#ifndef __INCLUDE_LOADER_H__
#define __INCLUDE_LOADER_H__

#include <type.h>

int read_image_bytes(void *dest, uint64_t disk_offset, size_t len);
uint64_t load_task_img(const char *taskname, int tasknum, uintptr_t pgdir);

#endif