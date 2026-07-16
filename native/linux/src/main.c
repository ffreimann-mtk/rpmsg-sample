/*
 * RPMSG Test Application for Linux
 *
 * Copyright (c) MediaTek, 2026
 *
 */

#include <errno.h>
#include <inttypes.h>
#include <linux/rpmsg.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "common.h"


#define ERROR_STR  "[ERROR] "
#define INFO_STR   "[INFO]  "

#define PATH_MAX_SIZE   64
#define VALUE_MAX_SIZE  32

#define MSG_STR_SIZE 64

#define MSG_STR "Hello from Linux!"

#define ALARM_TIME  2


static const char* dev_rpmsg_path = "/dev/rpmsg%d";


static int rpmsg_fd = -1;

static int msg_cnt = 0;

static char msg_tx [MSG_STR_SIZE];
static char msg_rx [MSG_STR_SIZE];


static int send_message (void);
static int receive_message (void);
static void alarm_nfy (int sig);
static void rpmsg_open (size_t rpmsg_id);


static int send_message (void)
{
    msg_tx [0] = '\0';
    msg_cnt++;

    snprintf (msg_tx, sizeof (msg_tx), "%s  %d", MSG_STR, msg_cnt);

	printf(INFO_STR "send_message  Size: %zu\n", (strlen (msg_tx) + 1));

	return (write (rpmsg_fd, msg_tx, (strlen (msg_tx) + 1)));
}

static int receive_message (void)
{
	int ret;
	
	
	memset (msg_rx, 0, sizeof (msg_rx));

	ret = read (rpmsg_fd, msg_rx, sizeof (msg_rx));
	if (ret > 0)
	{
	    printf (INFO_STR "RPMSG RX  len: %d  msg: \'%s\'\n", ret, msg_rx);

	    alarm (ALARM_TIME);
	}

	return (ret);
}

static void alarm_nfy (int sig)
{
    int ret = send_message ();

    if (ret < 0) {
        printf (ERROR_STR "send_message () failed with: %d\n", errno);
    }
}

static void rpmsg_open (size_t rpmsg_id)
{
    char path [PATH_MAX_SIZE];


    snprintf (path, sizeof (path), dev_rpmsg_path, rpmsg_id);

    rpmsg_fd = open (path, (O_RDWR | O_NONBLOCK));
}

int main (int   argc,
	      char* argv [])
{
    int           i;
	int           ret;
	size_t        rpmsg_id = 0;
	uint32_t      id;
    struct pollfd poll_fd;


    printf ("RPMSG-TEST using LINUX native RPMSG ...\n");

	for (i = 1; i < argc; i++) {
        if ((strcmp ("-r",         argv [i]) == 0)  ||
            (strcmp ("--rpmsg-id", argv [i]) == 0)) {
			i++;
			rpmsg_id = atoi (argv [i]);
			continue;
		}

		printf (ERROR_STR "Invalid argument '%s'\n", argv [i]);
		printf (INFO_STR "Usage: rpmsg-test [-r RPMSG-ID]\n");
        exit (1);
    }

	/* Open the RPMSG device. */
	rpmsg_open (rpmsg_id);
	if (rpmsg_fd < 0) {
		printf (ERROR_STR "rpmsg_open (rpmsg_id: %zd) failed with: %d\n", rpmsg_id, rpmsg_fd);

		exit (1);
	}
    printf (INFO_STR "rpmsg_fd:      %d\n", rpmsg_fd);

    /* Use the SIGALRM handler to send an RPMSG to the distant side. */
    signal (SIGALRM, alarm_nfy);

	/* Send first message. */
	ret = send_message ();
	if (ret < 0) {
		printf (ERROR_STR "send_message () failed with: %d\n", ret);
    
        close (rpmsg_fd);

		exit (1);
	}

    poll_fd.fd     = rpmsg_fd;
    poll_fd.events = POLLIN;

    /* Do the RPMSG RX / TX */
	while (1) {
		ret = poll (&poll_fd, 1, -1);
		if ((ret < 0)         &&
            (errno != EINTR)) {
    		printf (ERROR_STR "poll () failed with: %d\n", errno);

			close (rpmsg_fd);

            exit (1);
        }

		if ((poll_fd.revents & POLLIN) != 0) {
			ret = receive_message ();
			if ((ret < 0)           &&
			    (errno != -EAGAIN)) {
				printf (ERROR_STR "receive_message () failed with: %d\n", errno);

		        close (rpmsg_fd);

				exit (1);
			}
		}
	}

	return (0);
}
