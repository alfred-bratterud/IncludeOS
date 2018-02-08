#include "common.hpp"
#include <cstdint>
#include <sys/mman.h>
#include <errno.h>
#include <os>
#include <util/minialloc.hpp>

extern uintptr_t heap_begin;
extern uintptr_t heap_end = 0;
static uintptr_t current_pos = 0;

using Alloc = util::alloc::Lstack<4096>;
static Alloc alloc;

void init_mmap(uintptr_t addr_begin){
  if (alloc.empty()){
    printf("Alloc not empty: %s\n ", alloc.begin() == nullptr ? "nullptr" : "size not 0");
  }
  Expects(alloc.empty());
  auto aligned_begin = (addr_begin + Alloc::align - 1) & ~(Alloc::align - 1);
  alloc.donate((void*)aligned_begin, (OS::heap_max() - aligned_begin) & ~(Alloc::align - 1));

}

extern "C"
void* __kalloc(size_t size){
  return alloc.allocate(size);
}

extern "C"
void __kfree (void* ptr, size_t size){
  alloc.deallocate(ptr, size);
}

extern "C"
void* syscall_SYS_mmap(void *addr, size_t length, int prot, int flags,
                      int fd, off_t offset)
{

  STRACE("syscall mmap: addr=%p len=%u prot=%d fl=%d fd=%d off=%d \n",
         addr, length, prot, flags, fd, offset);

  // TODO: Mapping to file descriptor
  if (fd > 0) {
    assert(false && "Mapping to file descriptor not yet implemented");
  }

  // TODO: mapping virtual address
  if (addr) {
    errno = ENODEV;
    return MAP_FAILED;
  }

  void* res = __kalloc(length);
  STRACE("syscall mmap: addr=%p len=%u prot=%d fl=%d fd=%d off=%d res=%p\n",
         addr, length, prot, flags, fd, offset, res);

  return res;
}


/**
  The mmap2() system call provides the same interface as mmap(2),
  except that the final argument specifies the offset into the file in
  4096-byte units (instead of bytes, as is done by mmap(2)).  This
  enables applications that use a 32-bit off_t to map large files (up
  to 2^44 bytes).

  http://man7.org/linux/man-pages/man2/mmap2.2.html
**/
extern "C"
void* syscall_SYS_mmap2(void *addr, size_t length, int prot,
                       int flags, int fd, off_t offset) {
  uintptr_t res = heap_begin + current_pos;
  current_pos += length;
  STRACE("syscall mmap2: addr=%p len=%u prot=%d fl=%d fd=%d off=%d\n",
         addr, length, prot, flags, fd, offset);
  return (void*)res;
}
