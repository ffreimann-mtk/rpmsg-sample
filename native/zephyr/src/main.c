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

#define MSG_STR_SIZE 64

#define MSG_STR "Hello from Zephyr!"

#define RPMSG_THREAD_STACK_SIZE    2048
#define DOORBELL_THREAD_STACK_SIZE 2048

#define VIRTIO_CONFIG_READY          (VIRTIO_CONFIG_STATUS_ACK | VIRTIO_CONFIG_STATUS_DRIVER | VIRTIO_CONFIG_STATUS_DRIVER_OK | VIRTIO_CONFIG_FEATURES_OK)

#define VIRTIO_F_VERSION_1          32
#define VIRTIO_F_ACCESS_PLATFORM    33
#define VIRTIO_F_IOMMU_PLATFORM     VIRTIO_F_ACCESS_PLATFORM
#define VIRTIO_F_RING_PACKED        34
#define VIRTIO_F_IN_ORDER           35
#define VIRTIO_F_ORDER_PLATFORM     36
#define VIRTIO_F_SR_IOV             37
#define VIRTIO_F_NOTIFICATION_DATA  38
#define VIRTIO_F_RING_RESET         40


typedef struct {
	uint16_t size;
	uint16_t device_vector;
	uint16_t driver_vector;
	uint16_t enable;
	uint64_t desc;
	uint64_t driver;
	uint64_t device;
}   vq_config_t;

typedef struct {
	uint32_t revision;
	uint32_t size;

	uint32_t write_transaction;

	uint32_t device_features;
	uint32_t device_features_sel;
	uint32_t driver_features;
	uint32_t driver_features_sel;

	uint32_t queue_sel;

	vq_config_t vq_config;

	uint8_t config_event;
	uint8_t queue_event;
	uint8_t __reserved [2];
	uint32_t device_status;

	uint32_t config_generation;
	uint8_t config [];
}   ivshmem_hdr_t;

#define IVSHMEM_HDR_REG_OFFSET(_reg) offsetof (ivshmem_hdr_t, _reg)

typedef struct {
	/* IVSHMEM device */
	const struct device* dev;

	/* Virtual and physical address of IVSHMEM RW memory region. */
	uintptr_t ivshmem_paddr;
	uintptr_t ivshmem_vaddr;

	/* Size of IVSHMEM RW memory region. */
	size_t ivshmem_size;

	uint32_t id;
	uint32_t target;

	ivshmem_hdr_t* ivshmem_hdr;
}   rpmsg_ctx_t;


static int msg_cnt = 0;
static int vq_curr_sel = -1;

static char msg_tx [MSG_STR_SIZE];

static rpmsg_ctx_t rpmsg_ctx;

static vq_config_t vq_config [VRING_COUNT];


static uint32_t rpmsg_dest = UINT32_MAX;

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

static K_SEM_DEFINE (virtio_config_sem, 0, 1);

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

static metal_phys_addr_t* io_offset_to_phys (struct metal_io_region* io,
                                             unsigned long           offset);
static unsigned long* io_phys_to_offset (struct metal_io_region* io,
                                         metal_phys_addr_t       phys);

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

static const struct metal_io_ops io_ops = {
    .offset_to_phys = io_offset_to_phys,
    .phys_to_offset = io_phys_to_offset,
};

static unsigned char vdev_get_status (struct virtio_device* vdev)
{
	unsigned char status = (unsigned char) (rpmsg_ctx.ivshmem_hdr->device_status);


    /* printf (INFO_STR "vdev_get_status  status: 0x%02x\n", status); */

	return (status);
}

static void vdev_set_status (struct virtio_device* vdev,
                             unsigned char         status)
{
    printf (INFO_STR "vdev_set_status  status: 0x%02x\n", status);
#if 0
    *((volatile uint8_t*) vdev_status_vaddr) = status;
#endif
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
    printf (INFO_STR "vdev_nfy  target: %d\n", rpmsg_ctx.target);

    rpmsg_ctx.ivshmem_hdr->queue_event = 1;
    barrier_dmem_fence_full();

    /* For the moment, the interrupt number is always '0'. */
    ivshmem_int_peer (rpmsg_ctx.dev, rpmsg_ctx.target, 0);
}

static void rpmsg_ns_nfy (struct rpmsg_device* rdev,
                          const char*          name,
                          uint32_t             dest)
{
    printf (INFO_STR "rpmsg_ns_nfy  name: %s  dest: 0x%08x\n", name, dest);

	rpmsg_dev  = rdev;
	rpmsg_dest = dest;
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

static metal_phys_addr_t* io_offset_to_phys (struct metal_io_region* io,
                                             unsigned long           offset)
{
    metal_phys_addr_t* paddr = (metal_phys_addr_t*) (offset + ((unsigned long) io->physmap [0]));
#if 0
    printf (INFO_STR "io_offset_to_phys  offset: 0x%08llx  paddr: 0x%016llx\n",
        offset, (uintptr_t) paddr);
#endif
    return (paddr);
}                                      

static unsigned long* io_phys_to_offset (struct metal_io_region* io,
                                         metal_phys_addr_t       phys)
{
    unsigned long offset = (((unsigned long) phys) - ((unsigned long) io->physmap [0]));
#if 0
    printf (INFO_STR "io_phys_to_offset  paddr:   0x%016llx  offset: 0x%08llx\n",
        (unsigned long) phys, offset);
#endif
    return ((unsigned long) phys);
}                                         

static void doorbell_task (void* arg1,
                           void* arg2,
                           void* arg3)
{
    bool                    virtio_ready = false;
    volatile ivshmem_hdr_t* ivshmem_hdr;


	ARG_UNUSED (arg1);
	ARG_UNUSED (arg2);
	ARG_UNUSED (arg3);


    printf (INFO_STR "DOORBELL_TASK started ...\n");

	/* Signal that the IVSHMEM vPCI device is ready for initialization. */
	ivshmem_set_state (rpmsg_ctx.dev, 1);

    ivshmem_hdr = rpmsg_ctx.ivshmem_hdr;

	while (1) {
		int          vector;
		unsigned int signaled;

		k_poll (&doorbell_evt, 1, K_FOREVER);

		k_poll_signal_check (&doorbell_sig, &signaled, &vector);
		if (signaled == 0) {
			continue;
		}

		if (! (virtio_ready)) {
            switch (ivshmem_hdr->write_transaction) {
                case IVSHMEM_HDR_REG_OFFSET (device_features):
                    printf (INFO_STR "IVSHMEM_HDR_REG  device_features: 0x%08x\n", ivshmem_hdr->device_features);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (device_features_sel):
                    printf (INFO_STR "IVSHMEM_HDR_REG  device_features_sel: %d\n", ivshmem_hdr->device_features_sel);

                    if (ivshmem_hdr->device_features_sel == 0) {
                        ivshmem_hdr->device_features = (1 << VIRTIO_RPMSG_F_NS);
                    } else {
                        ivshmem_hdr->device_features = 
                            (1 << (VIRTIO_F_VERSION_1 - 32))      |
        	    			(1 << (VIRTIO_F_IOMMU_PLATFORM - 32)) |
		            		(1 << (VIRTIO_F_ORDER_PLATFORM - 32));
                    }

                    barrier_dmem_fence_full();
                    break;

                case IVSHMEM_HDR_REG_OFFSET (driver_features):
                    printf (INFO_STR "IVSHMEM_HDR_REG  driver_features: 0x%08x\n", ivshmem_hdr->driver_features);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (driver_features_sel):
                    printf (INFO_STR "IVSHMEM_HDR_REG  driver_features_sel: %d\n", ivshmem_hdr->driver_features_sel);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (queue_sel):
                    printf (INFO_STR "IVSHMEM_HDR_REG  queue_sel: %d\n", ivshmem_hdr->queue_sel);

		            if (vq_curr_sel >= 0) {
		                memcpy (&(vq_config [vq_curr_sel]), &(ivshmem_hdr->vq_config), sizeof (vq_config_t));
		            }

                    vq_curr_sel = ivshmem_hdr->queue_sel;

	    	        memcpy (&(ivshmem_hdr->vq_config), &(vq_config [vq_curr_sel]), sizeof (vq_config_t));

                    barrier_dmem_fence_full();
                    break;

                case IVSHMEM_HDR_REG_OFFSET (vq_config.size):
                    printf (INFO_STR "IVSHMEM_HDR_REG  vq_config.size: %d\n", ivshmem_hdr->vq_config.size);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (vq_config.device_vector):
                    printf (INFO_STR "IVSHMEM_HDR_REG  vq_config.device_vector: %d\n", ivshmem_hdr->vq_config.device_vector);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (vq_config.driver_vector):
                    printf (INFO_STR "IVSHMEM_HDR_REG  vq_config.driver_vector: %d\n", ivshmem_hdr->vq_config.driver_vector);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (vq_config.enable):
                    printf (INFO_STR "IVSHMEM_HDR_REG  vq_config.enable: %d\n", ivshmem_hdr->vq_config.enable);

    		        if ((vq_curr_sel >= 0)                             &&
                        (ivshmem_hdr->vq_config.enable > 0)) {
		                memcpy (&(vq_config [vq_curr_sel]), &(ivshmem_hdr->vq_config), sizeof (vq_config_t));
                    }
                    break;

                case IVSHMEM_HDR_REG_OFFSET (vq_config.desc):
                    printf (INFO_STR "IVSHMEM_HDR_REG  vq_config.desc: 0x%016llx\n", (unsigned long long) ivshmem_hdr->vq_config.desc);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (vq_config.driver):
                    printf (INFO_STR "IVSHMEM_HDR_REG  vq_config.driver: 0x%016llx\n", (unsigned long long) ivshmem_hdr->vq_config.driver);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (vq_config.device):
                    printf (INFO_STR "IVSHMEM_HDR_REG  vq_config.device: 0x%016llx\n", (unsigned long long) ivshmem_hdr->vq_config.device);
                    break;

                case IVSHMEM_HDR_REG_OFFSET (device_status):
                    printf (INFO_STR "IVSHMEM_HDR_REG  device_status: 0x%04x\n", ivshmem_hdr->device_status);

                    if (ivshmem_hdr->device_status == VIRTIO_CONFIG_READY) {
                        virtio_ready = true;
                        k_sem_give (&virtio_config_sem);
                    }
                    break;

                default:
                    /* Ignore the rest. */
                    printf (INFO_STR "IVSHMEM_HDR_REG  Unknown write_transaction: %d\n", ivshmem_hdr->write_transaction);
                    break;
            }

            if (ivshmem_hdr->write_transaction) {
                ivshmem_hdr->write_transaction = 0;

                barrier_dmem_fence_full();
            }
        } else {
            virtqueue_notification (vq [VRING_RX]);
        }

		k_poll_signal_init (&doorbell_sig);
	}
}

static void rpmsg_task (void* arg1,
                        void* arg2,
                        void* arg3)
{
    int                      ret;
	size_t                   rpmsg_size;
    size_t                   vring_size;
    uint32_t                 max_peers;
    uintptr_t                vaddr;
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

	/* Initialize the IVSHMEM VDEV header. */
	rpmsg_ctx.ivshmem_hdr = (ivshmem_hdr_t*) rpmsg_ctx.ivshmem_vaddr;
	memset ((void*) rpmsg_ctx.ivshmem_hdr, 0, VDEV_OFFSET);
    
	rpmsg_ctx.ivshmem_hdr->revision = 1;
	rpmsg_ctx.ivshmem_hdr->size     = sizeof (*rpmsg_ctx.ivshmem_hdr);

	memset (vq_config, 0, sizeof (vq_config));
	vq_config [VRING_TX].size          = VRING_NUM_DESCRS;
	vq_config [VRING_TX].device_vector = 1;
	vq_config [VRING_RX].size          = VRING_NUM_DESCRS;
	vq_config [VRING_RX].device_vector = 2;

    /* Start the IVSHMEM doorbell thread handler. */    
	ret = ivshmem_register_handler (rpmsg_ctx.dev, &doorbell_sig, 0);
    if (ret != 0) {
		printf (ERROR_STR "ivshmem_register_handler () failed with: %d\n", ret);

		return;
	}

    /* Retrieve the IVSHMEM device id. */
	rpmsg_ctx.id = ivshmem_get_id (rpmsg_ctx.dev);

    /* Calculate the target id. */
	max_peers = ivshmem_get_max_peers (rpmsg_ctx.dev);
    if (max_peers == 0) {
        printf (ERROR_STR "Invalid max_peers value: %d\n", max_peers);
        
        return;
    }

    rpmsg_ctx.target = rpmsg_ctx.id + 1;
    if (rpmsg_ctx.target >= max_peers) {
        rpmsg_ctx.target = 0;
    }

    /* [ffr] testing start ... */
    vaddr = (uintptr_t) rpmsg_ctx.ivshmem_vaddr + 0x0200;
    while (vaddr < (rpmsg_ctx.ivshmem_vaddr + 0x0600)) {
        (*((uint32_t*) vaddr)) = 0;

        vaddr += sizeof (uint32_t);
    }
    /* [ffr] testing end ... */

    /* Create the doorbell task to allow signalling. */
    k_thread_create (&doorbell_thread, doorbell_stack, DOORBELL_THREAD_STACK_SIZE, doorbell_task, NULL, NULL, NULL, K_PRIO_COOP (2), 0, K_NO_WAIT);

	ret = ivshmem_enable_interrupts (rpmsg_ctx.dev, true);
    if (ret != 0) {
		printf (ERROR_STR "ivshmem_enable_interrupts () failed with: %d\n", ret);

		return;
	}

	printf (INFO_STR "Id: %d  Target: %d\n", rpmsg_ctx.id, rpmsg_ctx.target);

    /* Wait for the other side to have initialized the IVSHMEM layer. */
	k_sem_take (&virtio_config_sem, K_FOREVER);

	printf (INFO_STR "VIRTIO config ready ...\n");

    /* Initialize OPEN-AMP's METAL layer. */
	/* metal_params.log_level = METAL_LOG_DEBUG; */
	ret = metal_init (&metal_params);
	if (ret < 0) {
		printf (ERROR_STR "metal_init () failed with: %d\n", ret);

		return;
	}

    rpmsg_paddr_map = (metal_phys_addr_t) rpmsg_ctx.ivshmem_paddr;

	/* Declare shared memory region */
	metal_io_init (&metal_io, (void*) rpmsg_ctx.ivshmem_vaddr, &rpmsg_paddr_map, rpmsg_ctx.ivshmem_size, -1, 0, &io_ops);

	printf (INFO_STR "State: %d\n", ivshmem_get_state (rpmsg_ctx.dev, rpmsg_ctx.id));
	printf (INFO_STR "IVSHMEM      Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        rpmsg_ctx.ivshmem_paddr, rpmsg_ctx.ivshmem_vaddr, (unsigned int) rpmsg_ctx.ivshmem_size);

    /* Calculate addresses for the shared memory region. */
    /* Skip over the IVSHMEM header. */
    vring_size = (size_t) (vq_config [VRING_RX].desc - vq_config [VRING_TX].desc);

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
	vrings [VRING_TX].info.vaddr     = (void*) (rpmsg_ctx.ivshmem_vaddr + vq_config [VRING_TX].desc);
	vrings [VRING_TX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_TX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_TX].vq             = vq [VRING_TX];

	vrings [VRING_RX].io             = &metal_io;
	vrings [VRING_RX].info.vaddr     = (void*) (rpmsg_ctx.ivshmem_vaddr + vq_config [VRING_RX].desc);
	vrings [VRING_RX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_RX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_RX].vq             = vq [VRING_RX];

	vdev.role        = RPMSG_REMOTE;
	vdev.vrings_num  = VRING_COUNT;
	vdev.func        = &vdispatch;
	vdev.vrings_info = vrings;

    rpmsg_paddr = rpmsg_ctx.ivshmem_paddr + (3 * vring_size);
    rpmsg_vaddr = rpmsg_ctx.ivshmem_vaddr + (3 * vring_size);
    rpmsg_size  = rpmsg_ctx.ivshmem_size  - (3 * vring_size);
    
	printf (INFO_STR "VBUFFER      Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        rpmsg_paddr, rpmsg_vaddr, (unsigned int) rpmsg_size);
	printf (INFO_STR "VRING [TX]   Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        (rpmsg_ctx.ivshmem_paddr + vq_config [VRING_TX].desc), (uintptr_t) vrings [VRING_TX].info.vaddr, vring_size);
	printf (INFO_STR "VRING [RX]   Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        (rpmsg_ctx.ivshmem_paddr + vq_config [VRING_RX].desc), (uintptr_t) vrings [VRING_RX].info.vaddr, vring_size);

	rpmsg_virtio_init_shm_pool (&shm_pool, (void*) rpmsg_vaddr, rpmsg_size);

    /* Initialize the RPMSG VDEV. */
	ret = rpmsg_init_vdev (&rpmsg_vdev, &vdev, rpmsg_ns_nfy, &metal_io, &shm_pool);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_init_vdev () failed with: %d\n", ret);

		return;
	}

	rpmsg_dev = rpmsg_virtio_get_rpmsg_device (&rpmsg_vdev);
    if (! (rpmsg_dev)) {
        printf (ERROR_STR "rpmsg_virtio_get_rpmsg_device failed\n");

        return (-EINVAL);
    }

    /* Create endpoint with the other side. */
	ret = rpmsg_create_ept (&rpmsg_ept, rpmsg_dev, RPMSG_SERVICE_NAME, RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, rpmsg_ept_nfy, rpmsg_service_unbind);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_create_ept failed with: %d\n", ret);

		return;
	}

	printf (INFO_STR "Created endpoint for service \'%s\'\n", RPMSG_SERVICE_NAME);

    /* Wait for endpoint to become read. */
    while (! (is_rpmsg_ept_ready(&rpmsg_ept))){
        k_msleep(100);
    };

    printf (INFO_STR "Endpoint for service \'%s\' is connected\n", RPMSG_SERVICE_NAME);

    /* And now, send / receive messages. */
	while (1) {
        while (! (tx_msg)) {
            k_msleep(100);
        }

		ret = send_message ();
		if (ret < 0) {
            printf (ERROR_STR "rpmsg_send () failed with: %d\n", ret);

			return;
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
