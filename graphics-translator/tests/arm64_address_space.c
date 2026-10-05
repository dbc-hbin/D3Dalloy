/* Observe the supported Darwin APIs needed for Wine's fixed shared-data page. */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/sysctl.h>

int main(void)
{
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t task_result = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count);
    int translated = -1;
    size_t length = sizeof(translated);
    if (sysctlbyname("sysctl.proc_translated", &translated, &length, NULL, 0) && errno == ENOENT)
        translated = 0;
    mach_vm_range_recipe_v1_t recipe = {0};
    recipe.range_tag = MACH_VM_RANGE_FIXED;
    recipe.range.min_address = 0x7ffe0000;
    recipe.range.max_address = 0x7ffe4000;
    kern_return_t range_result = mach_vm_range_create(mach_task_self(), MACH_VM_RANGE_FLAVOR_V1,
                                                      (mach_vm_range_recipes_raw_t)&recipe, sizeof(recipe));
    mach_vm_address_t address = recipe.range.min_address;
    kern_return_t allocate_result = mach_vm_allocate(mach_task_self(), &address, 0x4000, VM_FLAGS_FIXED);
    if (allocate_result == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), address, 0x4000);
    address = recipe.range.min_address;
    kern_return_t map_result = mach_vm_map(mach_task_self(), &address, 0x4000, 0, VM_FLAGS_FIXED,
                                          MEMORY_OBJECT_NULL, 0, 0, VM_PROT_READ,
                                          VM_PROT_ALL, VM_INHERIT_COPY);
    if (map_result == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), address, 0x4000);
    void *mapping = mmap((void *)0x7ffe0000, 0x4000, PROT_READ, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    int mmap_error = mapping == MAP_FAILED ? errno : 0;
    if (mapping != MAP_FAILED) munmap(mapping, 0x4000);
    printf("{\"native_arch\":\"arm64\",\"proc_translated\":%d,\"task_info_result\":%d,"
           "\"min_address\":%llu,\"max_address\":%llu,\"fixed_address\":2147352576,"
           "\"range_create_result\":%d,\"allocate_result\":%d,\"map_result\":%d,\"mmap_errno\":%d}\n",
           translated, task_result, (unsigned long long)info.min_address, (unsigned long long)info.max_address,
           range_result, allocate_result, map_result, mmap_error);
    return task_result != KERN_SUCCESS || translated != 0;
}
