/*
 * RPMSG Test Application for Linux
 *
 * Copyright (c) MediaTek, 2026
 *
 */

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <unistd.h>

#include <openamp/open_amp.h>

#include "common.h"
#include "uio.h"


#define ERROR_STR  "[ERROR] "
#define INFO_STR   "[INFO]  "

#define MSG_STR_SIZE 64

#define MSG_STR "Hello from Linux!"

#define ALARM_TIME  2


typedef struct {
	uint32_t id;
	uint32_t max_peers;
	uint32_t int_control;
	uint32_t doorbell;
	uint32_t state;
}   regs_t;


static int uio_fd;

static int msg_cnt = 0;

static char msg_tx [MSG_STR_SIZE];

static regs_t* regs_vaddr;

static uint32_t target = UINT32_MAX;

static uintptr_t vdev_status_vaddr;

static struct metal_io_region metal_io;

static metal_phys_addr_t ivshmem_paddr_map;

static struct virtqueue* vq [VRING_COUNT];

static struct virtio_vring_info vrings [VRING_COUNT];

static struct virtio_device vdev;

static struct rpmsg_virtio_device rpmsg_vdev;

static struct rpmsg_device* rpmsg_dev;

static struct rpmsg_endpoint rpmsg_ept;


static inline uint8_t mmio_read8 (void* addr);
static inline void mmio_write8 (void*    addr,
	                            uint8_t value);

static inline uint32_t mmio_read32 (void* addr);
static inline void mmio_write32 (void*    addr,
	                             uint32_t value);

static unsigned char vdev_get_status (struct virtio_device* vdev);
static void vdev_set_status (struct virtio_device* vdev,
                             unsigned char         status);
static uint32_t vdev_get_features (struct virtio_device* vdev);
static void vdev_set_features (struct virtio_device* vdev,
                               uint32_t              features);
static void vdev_nfy (struct virtqueue* vq);

static void rpmsg_ns_nfy (struct rpmsg_device* rdev,
                          const char*          name,
                          uint32_t             dest);
static int rpmsg_ept_nfy (struct rpmsg_endpoint* ept,
                          void*                  data,
		                  size_t                 len,
                          uint32_t               src,
                          void*                  priv);
static void rpmsg_service_unbind(struct rpmsg_endpoint* ept);

static int send_message (void);

static void alarm_nfy (int sig);

static int rpmsg_open (uintptr_t ivshmem_paddr,
                       uintptr_t ivshmem_vaddr,
					   size_t    ivshmem_size);								 


static const struct virtio_dispatch vdispatch = {
	.get_status   = vdev_get_status,
	.set_status   = vdev_set_status,
	.get_features = vdev_get_features,
	.set_features = vdev_set_features,
	.notify       = vdev_nfy,
};


static inline uint8_t mmio_read8 (void* addr)
{
	return (*(volatile uint8_t*) addr);
}

static inline void mmio_write8 (void*    addr,
	                            uint8_t value)
{
	(*(volatile uint8_t*) addr) = value;
}

static inline uint32_t mmio_read32 (void* addr)
{
	return (*(volatile uint32_t*) addr);
}

static inline void mmio_write32 (void*    addr,
	                             uint32_t value)
{
	(*(volatile uint32_t*) addr) = value;
}

static unsigned char vdev_get_status (struct virtio_device* vdev)
{
    unsigned char status = mmio_read8 ((void*) vdev_status_vaddr);


    printf (INFO_STR "vdev_get_status  status: 0x%02x\n", status);

	return (status);
}

static void vdev_set_status (struct virtio_device* vdev,
                             unsigned char         status)
{
    printf (INFO_STR "vdev_set_status  status: 0x%02x\n", status);

    mmio_write8 ((void*) vdev_status_vaddr, status);
}

static uint32_t vdev_get_features (struct virtio_device* vdev)
{
    uint32_t features = 1 << VIRTIO_RPMSG_F_NS;


    printf (INFO_STR "vdev_get_features  features: 0x%08x\n", features);

	return (features);
}

static void vdev_set_features (struct virtio_device* vdev,
                               uint32_t              features)
{
    printf (INFO_STR "vdev_set_features  features: 0x%08x\n", features);
}

static void vdev_nfy (struct virtqueue* vq)
{
    printf (INFO_STR "vdev_nfy  target: %d\n", target);

    /* For the moment, the interrupt number is always '0'. */
	mmio_write32 (&(regs_vaddr->doorbell), 0 | (target << 16));
}

static void rpmsg_ns_nfy (struct rpmsg_device* rdev,
                          const char*          name,
                          uint32_t             dest)
{
    printf (INFO_STR "rpmsg_ns_nfy  name: %s  dest: 0x%08x\n", name, dest);
}

static int rpmsg_ept_nfy (struct rpmsg_endpoint* ept,
                          void*                  data,
		                  size_t                 len,
                          uint32_t               src,
                          void*                  priv)
{
    int      ret;
    uint32_t int_count;


    printf (INFO_STR "RPMSG RX  len: %d  msg: \'%s\'\n", (int) len, (const char*) data);

    ret = read (uio_fd, &int_count, sizeof (int_count));
    if (ret != sizeof (int_count)) {
		printf (ERROR_STR "read (fd: %d) failure with: %d\n", uio_fd, ret);

		return (ret);
    }

	mmio_write32 (&(regs_vaddr->int_control), 1);

    alarm (ALARM_TIME);

	return (RPMSG_SUCCESS);
}

static void rpmsg_service_unbind (struct rpmsg_endpoint* ept)
{
    printf (INFO_STR "rpmsg_service_unbind\n");

	rpmsg_destroy_ept (ept);
}

static int send_message (void)
{
    msg_tx [0] = '\0';
    msg_cnt++;

    snprintf (msg_tx, sizeof (msg_tx), "%s  %d", MSG_STR, msg_cnt);

	printf(INFO_STR "send_message  Size: %zu\n", strlen (msg_tx) + 1);

	return (rpmsg_send (&rpmsg_ept, msg_tx, strlen (msg_tx) + 1));
}

static void alarm_nfy (int sig)
{
    int ret = send_message ();

    if (ret < 0) {
        printf (ERROR_STR "alarm_nfy  rpmsg_send () failed with: %d\n", ret);
    }
}

static int rpmsg_open (uintptr_t ivshmem_paddr,
                       uintptr_t ivshmem_vaddr,
					   size_t    ivshmem_size)
{
    int                      ret;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;


	if (((void*) ivshmem_paddr == NULL) ||
        ((void*) ivshmem_vaddr == NULL)     ||
	    (ivshmem_size < IVSHMEM_MIN_SIZE)) {
		printf (ERROR_STR "Invalid  ivshmem_paddr: %p, ivshmem_vaddr: %p or ivshmem_size: %zu\n", (void*) ivshmem_paddr, (void*) ivshmem_vaddr, ivshmem_size);
		
        return (-EINVAL);
	}

	ret = metal_init (&metal_params);
	if (ret < 0) {
		printf (ERROR_STR "metal_init () failed with: %d\n", ret);

		return (ret);
	}

    /* Calculate addresses for the shared memory region. */
	ivshmem_paddr += (VDEV_OFFSET + VDEV_STATUS_SIZE);
	ivshmem_vaddr += (VDEV_OFFSET + VDEV_STATUS_SIZE);
	ivshmem_size  -= (VDEV_OFFSET + VDEV_STATUS_SIZE);

    ivshmem_paddr_map = (metal_phys_addr_t) ivshmem_paddr;

    /* The VDEV status start right before the shared memory region. */
    vdev_status_vaddr = ivshmem_vaddr - VDEV_STATUS_SIZE;

	/* Declare shared memory region */
	metal_io_init (&metal_io, (void*) ivshmem_vaddr, &ivshmem_paddr_map, ivshmem_size, -1, 0, NULL);

	/* Setup VDEV */
	vq [VRING_TX] = virtqueue_allocate (VRING_NUM_DESCRS);
	if (vq [VRING_TX] == NULL) {
		printf (ERROR_STR "virtqueue_allocate (num_desc_extra: %d) failed with: %d\n", VRING_NUM_DESCRS, -ENOMEM);

        return (-ENOMEM);
    }

	vq [VRING_RX] = virtqueue_allocate (VRING_NUM_DESCRS);
	if (vq [VRING_RX] == NULL) {
		printf (ERROR_STR "virtqueue_allocate (num_desc_extra: %d) failed with: %d\n", VRING_NUM_DESCRS, -ENOMEM);

        return (-ENOMEM);
    }

	vrings [VRING_TX].io             = &metal_io;
	vrings [VRING_TX].info.vaddr     = (void*) (ivshmem_vaddr + ivshmem_size - VRING_SIZE);
	vrings [VRING_TX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_TX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_TX].vq             = vq [VRING_TX];

	vrings [VRING_RX].io             = &metal_io;
	vrings [VRING_RX].info.vaddr     = (void*) (ivshmem_vaddr + ivshmem_size - (2 * VRING_SIZE));
	vrings [VRING_RX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_RX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_RX].vq             = vq [VRING_RX];

	vdev.role        = RPMSG_REMOTE;
	vdev.vrings_num  = VRING_COUNT;
	vdev.func        = &vdispatch;
	vdev.vrings_info = vrings;

	printf (INFO_STR "VDEV STATUS  Phy: 0x%016" PRIxPTR "  Virt: %p  Size: 0x%08x\n",
        (ivshmem_paddr - VDEV_STATUS_SIZE),
        (void*) vdev_status_vaddr,
        VDEV_STATUS_SIZE);
	printf (INFO_STR "VBUFFER      Phy: 0x%016" PRIxPTR "  Virt: %p  Size: 0x%08zu\n",
        ivshmem_paddr,
        (void*) ivshmem_vaddr,
        (ivshmem_size - (2 * VRING_SIZE)));
	printf (INFO_STR "VRING [TX]   Phy: 0x%016" PRIxPTR "  Virt: %p  Size: 0x%08x\n",
        (ivshmem_paddr + ivshmem_size - VRING_SIZE),
        vrings [VRING_TX].info.vaddr,
        VRING_SIZE);
	printf (INFO_STR "VRING [RX]   Phy: 0x%016" PRIxPTR "  Virt: %p  Size: 0x%08x\n",
        (ivshmem_paddr + ivshmem_size - (2 * VRING_SIZE)),
        vrings [VRING_RX].info.vaddr,
        VRING_SIZE);

	ret = rpmsg_init_vdev (&rpmsg_vdev, &vdev, rpmsg_ns_nfy, &metal_io, NULL);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_init_vdev () failed with: %d\n", ret);

        return (ret);
	}

	rpmsg_dev = rpmsg_virtio_get_rpmsg_device (&rpmsg_vdev);
    if (! (rpmsg_dev)) {
        printf (ERROR_STR "rpmsg_virtio_get_rpmsg_device failed\n");

        return (-EINVAL);
    }

    /* And now, create the RPMSG endpoint. */
	ret = rpmsg_create_ept (&rpmsg_ept, rpmsg_dev, RPMSG_SERVICE_NAME, RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, rpmsg_ept_nfy, rpmsg_service_unbind);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_create_ept failed with: %d\n", ret);

        return (ret);
	}

    /* All is and ready for RPMSG RX/TX. */
    return (0);
}

int main (int   argc,
	      char* argv [])
{
    int           i;
	int           ret;
	size_t        regs_size;
	size_t        ivshmem_size;
	size_t        uio_id = 0;
	uint32_t      id;
	uint32_t      max_peers;
	uintptr_t     regs_paddr;
	uintptr_t     ivshmem_paddr;
	uintptr_t     ivshmem_vaddr;
    struct pollfd poll_fd;


    printf ("RPMSG-TEST using OpenAMP RPMSG ...\n");

	for (i = 1; i < argc; i++) {
		if ((strcmp ("-t",       argv [i]) == 0)  ||
            (strcmp ("--target", argv [i]) == 0)) {
    		i++;
			target = atoi (argv [i]);
			continue;
		}
        
        if ((strcmp ("-u",      argv [i]) == 0)  ||
            (strcmp ("--uioid", argv [i]) == 0)) {
			i++;
			uio_id = atoi (argv [i]);
			continue;
		}
        
		printf (ERROR_STR "Invalid argument '%s'\n", argv [i]);
		printf (INFO_STR "Usage: rpmsg-test [-u UIO-ID] [-t TARGET]\n");
        exit (1);
    }

	/* Open the UIO device. */
	uio_fd = uio_open (uio_id);
	if (uio_fd < 0) {
		printf (ERROR_STR "uio_open (uio_id: %zu) failed with: %d\n", uio_id, uio_fd);

		exit (1);
	}

	/* Map the registers, which include the doorbell. */
	ret = uio_get_mem_size (uio_id, MEM_IDX_REGS, &regs_size);
	if (ret < 0) {
		printf (ERROR_STR "uio_get_mem_size (uio_id: %zu, mem_idx: %d) failed with: %d\n", uio_id, MEM_IDX_REGS, ret);

		close (uio_fd);

		exit (1);
	}

	ret = uio_get_mem_addr (uio_id, MEM_IDX_REGS, &regs_paddr);
	if (ret < 0) {
		printf (ERROR_STR "uio_get_mem_addr (uio_id: %zu, mem_idx: %d) failed with: %d\n", uio_id, MEM_IDX_REGS, ret);

		close (uio_fd);

		exit (1);
	}

	regs_vaddr = (regs_t*) mmap (NULL, regs_size, PROT_READ | PROT_WRITE, MAP_SHARED, uio_fd, (MEM_IDX_REGS * getpagesize ()));
	if (((void*) regs_vaddr) == MAP_FAILED) {
		printf (ERROR_STR "mmap (size: %zu, uio_fd: %d, mem_idx: %d) failed\n", regs_size, uio_fd, MEM_IDX_REGS);

		close (uio_fd);

		exit (1);
	}

	/* Detremine the target id. */
	id        = mmio_read32 (&(regs_vaddr->id));
	max_peers = mmio_read32 (&(regs_vaddr->max_peers));

	if (target == UINT32_MAX) {
		target = (id + 1) % max_peers;
	}
	if ((target >= max_peers) ||
	    (target == id)) {
		printf (ERROR_STR "Invalid id: %d, target: %d or max_peers: %d\n", id, target, max_peers);

		munmap (regs_vaddr, regs_size);
		close (uio_fd);

		exit (1);
	}

	/* Map the IVSHMEM memory. */
	ret = uio_get_mem_size (uio_id, MEM_IDX_IVSHMEM, &ivshmem_size);
	if (ret < 0) {
		printf (ERROR_STR "uio_get_mem_size (uio_id: %zu, mem_idx: %d) failed with: %d\n", uio_id, MEM_IDX_IVSHMEM, ret);

		munmap (regs_vaddr, regs_size);
		close (uio_fd);

		exit (1);
	}

	ret = uio_get_mem_addr (uio_id, MEM_IDX_IVSHMEM, &ivshmem_paddr);
	if (ret < 0) {
		printf (ERROR_STR "uio_get_mem_addr (uio_id: %zu, mem_idx: %d) failed with: %d\n", uio_id, MEM_IDX_IVSHMEM, ret);

		munmap (regs_vaddr, regs_size);
		close (uio_fd);

		exit (1);
	}

	ivshmem_vaddr = (uintptr_t) mmap (NULL, ivshmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, uio_fd, (MEM_IDX_IVSHMEM * getpagesize ()));
	if (((void*) ivshmem_vaddr) == MAP_FAILED) {
		printf (ERROR_STR "mmap (size: %zu, uio_fd: %d, mem_idx: %d) failed\n", ivshmem_size, uio_fd, MEM_IDX_IVSHMEM);

		munmap (regs_vaddr, regs_size);
		close (uio_fd);

		exit (1);
	}

	printf (INFO_STR "Registers    Phy: 0x%016" PRIxPTR "  Virt: %p  Size: 0x%08zu\n", regs_paddr,    (void*) regs_vaddr,    regs_size);
	printf (INFO_STR "IVSHMEM      Phy: 0x%016" PRIxPTR "  Virt: %p  Size: 0x%08zu\n", ivshmem_paddr, (void*) ivshmem_vaddr, ivshmem_size);
	printf (INFO_STR "Id: %d  Target: %d\n", id, target);

    /* Initialize RPMSG endpoint. */
    ret = rpmsg_open (ivshmem_paddr, ivshmem_vaddr, ivshmem_size);
    if (ret < 0) {
        munmap ((void*) ivshmem_vaddr, ivshmem_size);
        munmap ((void*) regs_vaddr, regs_size);
        close (uio_fd);

        exit (1);
    }

    /* Use the SIGALRM handler to send an RPMSG to the distant side. */
    signal (SIGALRM, alarm_nfy);

	mmio_write32 (&(regs_vaddr->state), (id + 1));
	mmio_write32 (&(regs_vaddr->int_control), 1);

    poll_fd.fd     = uio_fd;
    poll_fd.events = POLLIN;

    /* Do the RPMSG RX / TX */
	while (1) {
		ret = poll (&poll_fd, 1, -1);
		if ((ret < 0)         &&
            (errno != EINTR))
        {
    		printf (ERROR_STR "poll () failed with: %d\n", errno);

            munmap ((void*) ivshmem_vaddr, ivshmem_size);
            munmap ((void*) regs_vaddr, regs_size);
            close (uio_fd);

            exit (1);
        }

		if (poll_fd.revents & POLLIN) {
            virtqueue_notification (vq [VRING_RX]);
        }
	}

    return (0);
}
