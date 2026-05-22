
#ifndef __UIO_H__
#define __UIO_H__


#include <stddef.h>
#include <stdint.h>


#define MEM_IDX_REGS     0
#define MEM_IDX_IVSHMEM  2


extern int uio_get_mem_addr (size_t     uio_id,
                             size_t     mem_idx,
                             uintptr_t* addr);
extern int uio_get_mem_size (size_t  uio_id,
                             size_t  mem_idx,
                             size_t* size);

extern int uio_open (size_t uio_id);


#endif /* __UIO_H__ */
