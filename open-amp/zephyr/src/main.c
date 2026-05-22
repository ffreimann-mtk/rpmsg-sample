/*
 * Copyright (c) 2018, NXP
 * Copyright (c) 2018, Nordic Semiconductor ASA
 * Copyright (c) 2018-2019, Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/virtualization/ivshmem.h>
#include <zephyr/kernel.h>

#include <openamp/open_amp.h>
#include <metal/device.h>

#include "common.h"


#define ERROR_STR  "[ERROR] "
#define INFO_STR   "[INFO]  "

#define RPMSG_SERVICE_NAME "rpmsg"

#define MSG_STR_SIZE 64

#define MSG_STR "Hello from Zephyr!"

#define RPMSG_THREAD_STACK_SIZE    2048
#define DOORBELL_THREAD_STACK_SIZE 2048


typedef struct {
	/* IVSHMEM device */
	const struct device* dev;

	/* Virtual and physical address of IVSHMEM RW memory region. */
	uintptr_t ivshmem_paddr;
	uintptr_t ivshmem_vaddr;

	/* Size of IVSHMEM RW memory region. */
	size_t ivshmem_size;
}   rpmsg_ctx_t;


static int msg_cnt = 0;

static char msg_tx [MSG_STR_SIZE];

static rpmsg_ctx_t rpmsg_ctx;

static uint32_t target = UINT32_MAX;
static uint32_t rpmsg_dest = UINT32_MAX;

static uintptr_t vdev_status_vaddr;

static struct metal_io_region metal_io;

static metal_phys_addr_t rpmsg_paddr_map;

static struct rpmsg_virtio_shm_pool shm_pool;

static struct virtqueue* vq [VRING_COUNT];

static struct virtio_vring_info vrings [VRING_COUNT];

static struct virtio_device vdev;

static struct rpmsg_virtio_device rpmsg_vdev;

static struct rpmsg_device* rpmsg_dev;

static struct rpmsg_endpoint rpmsg_ept;

static struct k_thread rpmsg_thread;
static struct k_thread doorbell_thread;

static K_THREAD_STACK_DEFINE (rpmsg_stack, RPMSG_THREAD_STACK_SIZE);
static K_THREAD_STACK_DEFINE (doorbell_stack, DOORBELL_THREAD_STACK_SIZE);

static K_SEM_DEFINE (ept_sem, 0, 1);

static struct k_poll_signal doorbell_sig = K_POLL_SIGNAL_INITIALIZER (doorbell_sig);

static struct k_poll_event doorbell_evt = K_POLL_EVENT_INITIALIZER (K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &doorbell_sig);

static volatile bool tx_msg = false;


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

static void doorbell_task (void* arg1,
                           void* arg2,
                           void* arg3);
static void rpmsg_task (void* arg1,
                        void* arg2,
                        void* arg3);


static const struct virtio_dispatch vdispatch = {
	.get_status   = vdev_get_status,
	.set_status   = vdev_set_status,
	.get_features = vdev_get_features,
	.set_features = vdev_set_features,
	.notify       = vdev_nfy,
};


static unsigned char vdev_get_status (struct virtio_device* vdev)
{
	unsigned char status = *((volatile uint8_t*) vdev_status_vaddr);


    printf (INFO_STR "vdev_get_status  status: 0x%02x\n", status);

	return (status);
}

static void vdev_set_status (struct virtio_device* vdev,
                             unsigned char         status)
{
    printf (INFO_STR "vdev_set_status  status: 0x%02x\n", status);

    *((volatile uint8_t*) vdev_status_vaddr) = status;
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
    ivshmem_int_peer (rpmsg_ctx.dev, target, 0);
}

static void rpmsg_ns_nfy (struct rpmsg_device* rdev,
                          const char*          name,
                          uint32_t             dest)
{
    printf (INFO_STR "rpmsg_ns_nfy  name: %s  dest: 0x%08x\n", name, dest);

	rpmsg_dev  = rdev;
	rpmsg_dest = dest;

	k_sem_give (&ept_sem);
}

static int rpmsg_ept_nfy (struct rpmsg_endpoint* ept,
                          void*                  data,
		                  size_t                 len,
                          uint32_t               src,
                          void*                  priv)
{
    printf (INFO_STR "RPMSG RX  len: %d  msg: \'%s\'\n", (int) len, (const char*) data);

    tx_msg = true;

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

static void doorbell_task (void* arg1,
                           void* arg2,
                           void* arg3)
{
	ARG_UNUSED (arg1);
	ARG_UNUSED (arg2);
	ARG_UNUSED (arg3);


    printf (INFO_STR "DOORBELL_TASK started ...\n");

	while (1) {
		int          vector;
		unsigned int signaled;

		k_poll (&doorbell_evt, 1, K_FOREVER);

		k_poll_signal_check (&doorbell_sig, &signaled, &vector);
		if (signaled == 0) {
			continue;
		}

    	virtqueue_notification (vq [VRING_TX]);

		k_poll_signal_init (&doorbell_sig);
	}
}

static void rpmsg_task (void* arg1,
                        void* arg2,
                        void* arg3)
{
    int                      ret;
	size_t                   rpmsg_size;
    uint32_t                 id;
    uint32_t                 max_peers;
    uintptr_t                rpmsg_paddr;
    uintptr_t                rpmsg_vaddr;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;


	ARG_UNUSED (arg1);
	ARG_UNUSED (arg2);
	ARG_UNUSED (arg3);


    printf (INFO_STR "RPMSG_TASK started ...\n");

    /* Retrieve the IVSHMEM device memory region. */
	rpmsg_ctx.dev = DEVICE_DT_GET (DT_NODELABEL (ivshmem0));
	if (! (device_is_ready (rpmsg_ctx.dev))) {
		printf (ERROR_STR "Could not get IVSHMEM device\n");

		return;
	}

    /* Retrieve the IVSHMEM RQ memory section. */
	rpmsg_ctx.ivshmem_size = ivshmem_get_rw_mem_section (rpmsg_ctx.dev, &rpmsg_ctx.ivshmem_vaddr);
	if (rpmsg_ctx.ivshmem_size == 0) {
		printf (ERROR_STR "IVSHMEM RW size cannot be 0\n");
		
        return;
	}

	if (((void*) rpmsg_ctx.ivshmem_vaddr) == NULL) {
		printf (ERROR_STR "IVSHMEM virtual memory address cannot be null\n");
		
        return;
	}

    if (arch_page_phys_get ((void*) rpmsg_ctx.ivshmem_vaddr, &rpmsg_ctx.ivshmem_paddr) != 0) {
        printf (ERROR_STR "Failed to get physical address for IVSHMEM memory\n");
        
        return;
    }

    /* Retrieve the IVSHMEM device id. */
	id = ivshmem_get_id (rpmsg_ctx.dev);

    /* Calculate the target id. */
	max_peers = ivshmem_get_max_peers (rpmsg_ctx.dev);
    if (max_peers == 0) {
        printf (ERROR_STR "Invalid max_peers value: %d\n", max_peers);
        
        return;
    }

    target = id + 1;
    if (target >= max_peers) {
        target = 0;
    }

	printf (INFO_STR "IVSHMEM      Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08zu\n",
        rpmsg_ctx.ivshmem_paddr, rpmsg_ctx.ivshmem_vaddr, rpmsg_ctx.ivshmem_size);
	printf (INFO_STR "Id: %d  Target: %d\n", id, target);

    /* Initialize OPEN-AMP's METAL layer. */
	/* metal_params.log_level = METAL_LOG_DEBUG; */
	ret = metal_init (&metal_params);
	if (ret < 0) {
		printf (ERROR_STR "metal_init () failed with: %d\n", ret);

		return;
	}

    /* Calculate addresses for the shared memory region. */
    rpmsg_paddr = rpmsg_ctx.ivshmem_paddr + (VDEV_OFFSET + VDEV_STATUS_SIZE);
    rpmsg_vaddr = rpmsg_ctx.ivshmem_vaddr + (VDEV_OFFSET + VDEV_STATUS_SIZE);
    rpmsg_size  = rpmsg_ctx.ivshmem_size  - (VDEV_OFFSET + VDEV_STATUS_SIZE);

    rpmsg_paddr_map = (metal_phys_addr_t) rpmsg_paddr;

    /* The VDEV status start right before the shared memory region. */
    vdev_status_vaddr = rpmsg_vaddr - VDEV_STATUS_SIZE;

	/* Declare shared memory region */
	metal_io_init (&metal_io, (void*) rpmsg_vaddr, &rpmsg_paddr_map, rpmsg_size, -1, 0, NULL);

	/* Setup RPMSG queues */
	vq [VRING_TX] = virtqueue_allocate (VRING_NUM_DESCRS);
	if (vq [VRING_TX] == NULL) {
		printf (ERROR_STR "virtqueue_allocate (num_desc_extra: %d) failed with: %d\n", VRING_NUM_DESCRS, -ENOMEM);

        return;
    }

	vq [VRING_RX] = virtqueue_allocate (VRING_NUM_DESCRS);
	if (vq [VRING_RX] == NULL) {
		printf (ERROR_STR "virtqueue_allocate (num_desc_extra: %d) failed with: %d\n", VRING_NUM_DESCRS, -ENOMEM);

        return;
    }

	vrings [VRING_TX].io             = &metal_io;
	vrings [VRING_TX].info.vaddr     = (void*) (rpmsg_vaddr + rpmsg_size - VRING_SIZE);
	vrings [VRING_TX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_TX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_TX].vq             = vq [VRING_TX];

	vrings [VRING_RX].io             = &metal_io;
	vrings [VRING_RX].info.vaddr     = (void*) (rpmsg_vaddr + rpmsg_size - (2 * VRING_SIZE));
	vrings [VRING_RX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_RX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_RX].vq             = vq [VRING_RX];

	vdev.role        = RPMSG_HOST;
	vdev.vrings_num  = VRING_COUNT;
	vdev.func        = &vdispatch;
	vdev.vrings_info = vrings;
    
	printf (INFO_STR "VDEV STATUS  Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        (rpmsg_paddr - VDEV_STATUS_SIZE), vdev_status_vaddr, VDEV_STATUS_SIZE);
	printf (INFO_STR "VBUFFER      Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08zu\n",
        rpmsg_paddr, rpmsg_vaddr, (rpmsg_size - (2 * VRING_SIZE)));
	printf (INFO_STR "VRING [TX]   Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        (rpmsg_paddr + rpmsg_size - VRING_SIZE), (uintptr_t) vrings [VRING_TX].info.vaddr, VRING_SIZE);
	printf (INFO_STR "VRING [RX]   Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        (rpmsg_paddr + rpmsg_size - (2 * VRING_SIZE)), (uintptr_t) vrings [VRING_RX].info.vaddr, VRING_SIZE);

    /* Start the IVSHMEM doorbell thread ha ndler. */    
	ret = ivshmem_register_handler (rpmsg_ctx.dev, &doorbell_sig, 0);
    if (ret != 0) {
		printf (ERROR_STR "ivshmem_register_handler () failed with: %d\n", ret);

		return;
	}

    k_thread_create (&doorbell_thread, doorbell_stack, DOORBELL_THREAD_STACK_SIZE, doorbell_task, NULL, NULL, NULL, K_PRIO_COOP (2), 0, K_NO_WAIT);

	ret = ivshmem_enable_interrupts (rpmsg_ctx.dev, true);
    if (ret != 0) {
		printf (ERROR_STR "ivshmem_enable_interrupts () failed with: %d\n", ret);

		return;
	}

	rpmsg_virtio_init_shm_pool (&shm_pool, (void*) rpmsg_vaddr, rpmsg_size);

    /* Initialize the RPMSG VDEV. */
	ret = rpmsg_init_vdev (&rpmsg_vdev, &vdev, rpmsg_ns_nfy, &metal_io, &shm_pool);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_init_vdev () failed with: %d\n", ret);

		return;
	}

    /* Wait for the other side to connect. */
	k_sem_take (&ept_sem, K_FOREVER);

    /* Create endpoint with the other side. */
	ret = rpmsg_create_ept (&rpmsg_ept, rpmsg_dev, RPMSG_SERVICE_NAME, RPMSG_ADDR_ANY, rpmsg_dest, rpmsg_ept_nfy, rpmsg_service_unbind);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_create_ept failed with: %d\n", ret);

		return;
	}

    /* Wait for endpoint to become read. */
    while (! (is_rpmsg_ept_ready(&rpmsg_ept))) {};

    /* And now, send / receive messages. */
	while (1) {
		ret = send_message ();
		if (ret < 0) {
            printf (ERROR_STR "alarm_nfy  rpmsg_send () failed with: %d\n", ret);

			return;
		}

        while (! (tx_msg)) {
            k_msleep(100);
        }

        tx_msg = false;
	}

    while (1) {};
}

int main (void)
{
    printf ("RPMSG-TEST ...\n");

	k_thread_create (&rpmsg_thread, rpmsg_stack, RPMSG_THREAD_STACK_SIZE, rpmsg_task, NULL, NULL, NULL, K_PRIO_COOP (7), 0, K_NO_WAIT);

	return (0);
}
