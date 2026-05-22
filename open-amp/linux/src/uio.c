#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "uio.h"


#define PATH_MAX_SIZE   64
#define VALUE_MAX_SIZE  32


static const char* dev_uio_path = "/dev/uio%d";
static const char* sys_uio_path = "/sys/class/uio/uio%d/maps/map%d/%s";


int uio_get_mem_addr (size_t     uio_id,
                      size_t     mem_idx,
                      uintptr_t* addr)
{
    int  ret;
    int  sys_fd;
    char path [PATH_MAX_SIZE];
    char value [VALUE_MAX_SIZE];


    snprintf (path, sizeof (path), sys_uio_path, uio_id, mem_idx, "addr");
	sys_fd = open (path, O_RDONLY);
	if (sys_fd < 0) {
        return (errno);
    }
	ret = read (sys_fd, value, sizeof (value));
	if (ret < 0) {
        return (errno);
    }
	close (sys_fd);

	if (sscanf (value, "%" SCNxPTR, addr) != 1) {
        return (-EINVAL);
    }

    return (0);
}

int uio_get_mem_size (size_t  uio_id,
                      size_t  mem_idx,
                      size_t* size)
{
    int  ret;
    int  sys_fd;
    char path [PATH_MAX_SIZE];
    char value [VALUE_MAX_SIZE];


    snprintf (path, sizeof (path), sys_uio_path, uio_id, mem_idx, "size");
	sys_fd = open (path, O_RDONLY);
	if (sys_fd < 0) {
        return (errno);
    }
	ret = read (sys_fd, value, sizeof (value));
	if (ret < 0) {
        return (errno);
    }
	close (sys_fd);

	if (sscanf (value, "%z" SCNx32, size) != 1) {
        return (-EINVAL);
    }

    return (0);
}

int uio_open (size_t uio_id)
{
    int  uio_fd;
    char path [PATH_MAX_SIZE];


    snprintf (path, sizeof (path), dev_uio_path, uio_id);
    return (open (path, O_RDWR));
}
