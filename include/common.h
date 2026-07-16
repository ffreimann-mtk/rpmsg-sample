/*
 */

#ifndef __COMMON_H__
#define __COMMON_H__

/* IVSHMEM memory layout for OpenAMP                        */
/*                                                          */
/*                         +----------------+ <- End of     */
/*                      ^  |                |    IVSHMEM    */
/*     VRING_SIZE       |  |    VRING_TX    |               */
/*                      v  |                |               */
/*                         +----------------+               */
/*                      ^  |                |               */
/*     VRING_SIZE       |  |    VRING_RX    |               */
/*                      v  |                |               */
/*                         +----------------+               */
/*                         |                |               */
/*                         |                |               */
/*                         |                |               */
/*                         |  VRING BUFFER  |               */
/*                         |                |               */
/*                         |                |               */
/*                         |                |               */
/*                         +----------------+               */
/*                      ^  |                |               */
/*     VDEV_STATUS_SIZE |  |  VDEV_STATUS   |               */
/*                      v  |                |               */
/*                         +----------------+               */
/*                         |                |               */
/*                         |  VDEV_OFFSET   |               */
/*                         |                |               */
/*                         +----------------+  <- Start of  */
/*                                                IVSHMEM   */

#define IVSHMEM_MIN_SIZE  0x4000    /* Minimum space for VDEV_OFFSET + (2 * VRING_SIZE) plus some space for VRING BUFFER. */

#define VDEV_OFFSET       0x1000
#define VDEV_STATUS_SIZE  0x0400

#define VRING_SIZE        0x0400
#define VRING_ALIGNMENT   32
#define VRING_NUM_DESCRS  16

#define VRING_TX           0
#define VRING_RX           1
#define VRING_COUNT		   2

#define RPMSG_SERVICE_NAME "rpmsg-raw"


#endif /* __COMMON_H__ */
