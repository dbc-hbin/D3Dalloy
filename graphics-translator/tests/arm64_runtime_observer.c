/* Loaded into each Wine host process, not a parent-only architecture preflight. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int evidence = -1;

static void image_loaded(const struct mach_header *header, intptr_t slide)
{
    Dl_info info;
    (void)slide;
    if (evidence >= 0 && dladdr(header, &info) && info.dli_fname)
    {
        dprintf(evidence, "IMAGE\t%d\t%d\t%s\n", getpid(), header->cputype, info.dli_fname);
        dprintf(evidence, "ADDRESS\t%d\t%p\t%s\n", getpid(), (const void *)header, info.dli_fname);
    }
}

__attribute__((constructor)) static void observe_process(void)
{
    const char *directory = getenv("DXMT_ARM64_EVIDENCE");
    char path[4096], executable[4096];
    uint32_t length = sizeof(executable);
    int translated = -1;
    size_t size = sizeof(translated);
    if (!directory) return;
    snprintf(path, sizeof(path), "%s/%d.images", directory, getpid());
    evidence = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (evidence < 0) _exit(125);
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) && errno == ENOENT)
        translated = 0;
    if (_NSGetExecutablePath(executable, &length)) _exit(125);
    dprintf(evidence, "PROCESS\t%d\t%d\t%s\n", getpid(), translated, executable);
    _dyld_register_func_for_add_image(image_loaded);
}
