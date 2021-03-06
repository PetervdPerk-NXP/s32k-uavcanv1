/****************************************************************************
 * examples/canard/canard_main.c
 *
 *   Copyright (C) 2016 ETH Zuerich. All rights reserved.
 *   Author: Matthias Renner <rennerm@ethz.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <canard.h>
#include <canard_dsdl.h>
#include <o1heap.h>

#include <sched.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <net/if.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <poll.h>

#include <nuttx/can.h>
#include <netpacket/can.h>

#include "socketcan.h"

#include <regulated/drone/sensor/BMSStatus_1_0.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Arena for memory allocation, used by the library */

#define O1_HEAP_SIZE CONFIG_S32KUAVCANV1EXAMPLE_NODE_MEM_POOL_SIZE

/* Temporary development UAVCAN topic service ID to publish/subscribe from */
#define PORT_ID                                  6100
#define TOPIC_SIZE                               512

O1HeapInstance *my_allocator;
static uint8_t uavcan_heap[O1_HEAP_SIZE]
__attribute__((aligned(O1HEAP_ALIGNMENT)));

/* Node status variables */

static bool g_canard_daemon_started;

static uint8_t my_message_transfer_id;  // Must be static or heap-allocated to retain state between calls.

struct pollfd fd;


/* Struct for regulated.drone.sensor.BMSStatus message */
regulated_drone_sensor_bmsstatus bmsstatus;


/* Buffer to store serialized to be send using uavcan v1 */
uint8_t bmsstatus_serialized[REGULATED_DRONE_SENSOR_BMSSTATUS_MSG_SIZE];

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: memAllocate
 *
 * Description:
 *
 ****************************************************************************/
static void *memAllocate(CanardInstance *const ins, const size_t amount)
{
	(void) ins;
	return o1heapAllocate(my_allocator, amount);
}

/****************************************************************************
 * Name: memFree
 *
 * Description:
 *
 ****************************************************************************/

static void memFree(CanardInstance *const ins, void *const pointer)
{
	(void) ins;
	o1heapFree(my_allocator, pointer);
}

/****************************************************************************
 * Name: getMonotonicTimestampUSec
 *
 * Description:
 *
 ****************************************************************************/
uint64_t getMonotonicTimestampUSec(void)
{
	struct timespec ts;

	memset(&ts, 0, sizeof(ts));

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		abort();
	}

	return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/****************************************************************************
 * Name: process1HzTasks
 *
 * Description:
 *   This function is called at 1 Hz rate from the main loop.
 *
 ****************************************************************************/

void process1HzTasks(CanardInstance *ins, uint64_t timestamp_usec)
{

	CanardMicrosecond transmission_deadline = getMonotonicTimestampUSec() + 1000 * 10;
    
    bmsstatus.voltage = 12.0f; // Dummy data
    
    regulated_drone_sensor_bmsstatus_serializeToBuffer(&bmsstatus, &bmsstatus_serialized, 0);

	const CanardTransfer transfer = {
		.timestamp_usec = transmission_deadline,      // Zero if transmission deadline is not limited.
		.priority       = CanardPriorityNominal,
		.transfer_kind  = CanardTransferKindMessage,
		.port_id        = PORT_ID,                       // This is the subject-ID.
		.remote_node_id = CANARD_NODE_ID_UNSET,       // Messages cannot be unicast, so use UNSET.
		.transfer_id    = my_message_transfer_id,
		.payload_size   = sizeof(struct regulated_drone_sensor_bmsstatusType),
		.payload        = &bmsstatus_serialized,
	};

	++my_message_transfer_id;  // The transfer-ID shall be incremented after every transmission on this subject.
	int32_t result = canardTxPush(ins, &transfer);

	if (result < 0) {
		// An error has occurred: either an argument is invalid or we've ran out of memory.
		// It is possible to statically prove that an out-of-memory will never occur for a given application if the
		// heap is sized correctly; for background, refer to the Robson's Proof and the documentation for O1Heap.
		fprintf(stderr, "Transmit error %d\n", result);
	}
}


static void processReceivedTransfer(CanardTransfer *receive)
{
	printf("Received transfer remote_node_id %d transfer_id: %d payload size: %d\n",
	       receive->remote_node_id, receive->transfer_id, receive->payload_size);

}

/****************************************************************************
 * Name: processTxRxOnce
 *
 * Description:
 *   Transmits all frames from the TX queue, receives up to one frame.
 *
 ****************************************************************************/

void processTxRxOnce(CanardInstance *ins, CanardSocketInstance *sock_ins, int timeout_msec)
{
	int32_t result;

	/* Transmitting */


	for (const CanardFrame *txf = NULL; (txf = canardTxPeek(ins)) != NULL;) { // Look at the top of the TX queue.
		if (txf->timestamp_usec > getMonotonicTimestampUSec()) { // Check if the frame has timed out.
			if (socketcanTransmit(sock_ins, txf) == 0) {           // Send the frame. Redundant interfaces may be used here.
				break;                             // If the driver is busy, break and retry later.
			}
		}

		canardTxPop(ins);                         // Remove the frame from the queue after it's transmitted.
		ins->memory_free(ins, (CanardFrame *)txf); // Deallocate the dynamic memory afterwards.
	}


	/* Poll receive */
	if (poll(&fd, 1, timeout_msec) <= 0) {
		return;
	}

	/* Receiving */
	CanardFrame received_frame;

	socketcanReceive(sock_ins, &received_frame);

	CanardTransfer receive;
	result = canardRxAccept(ins,
				&received_frame, // The CAN frame received from the bus.
				0,               // If the transport is not redundant, use 0.
				&receive);

	if (result < 0) {
		// An error has occurred: either an argument is invalid or we've ran out of memory.
		// It is possible to statically prove that an out-of-memory will never occur for a given application if
		// the heap is sized correctly; for background, refer to the Robson's Proof and the documentation for O1Heap.
		// Reception of an invalid frame is NOT an error.
		fprintf(stderr, "Receive error %d\n", result);

	} else if (result == 1) {
		// A transfer has been received, process it. !!!!
        printf("Receive UAVCAN port id%d TODO process me\n",
		       receive.port_id);
        
        /* Processing !! */
        
		ins->memory_free(ins, (void *)receive.payload); // Deallocate the dynamic memory afterwards.

	} else {
		// Nothing to do.
		// The received frame is either invalid or it's a non-last frame of a multi-frame transfer.
		// Reception of an invalid frame is NOT reported as an error because it is not an error.
	}

}

/****************************************************************************
 * Name: canard_daemon
 *
 * Description:
 *
 ****************************************************************************/

static int canard_daemon(int argc, char *argv[])
{
	int errval = 0;
	int can_fd = 0;
	int pub = 1;

	if (argc > 2) {
		for (int args = 2; args < argc; args++) {
			if (!strcmp(argv[args], "canfd")) {
				can_fd = 1;
			}

			if (!strcmp(argv[args], "pub")) {
				pub = 1;
			}

			if (!strcmp(argv[args], "sub")) {
				pub = 0;
			}
		}
	}

	my_allocator = o1heapInit(&uavcan_heap, O1_HEAP_SIZE, NULL, NULL);

	if (my_allocator == NULL) {
		printf("o1heapInit failed with size %d\n", O1_HEAP_SIZE);
		errval = 2;
		goto errout_with_dev;
	}

	CanardInstance ins = canardInit(&memAllocate, &memFree);

	if (can_fd) {
		ins.mtu_bytes = CANARD_MTU_CAN_FD;

	} else {
		ins.mtu_bytes = CANARD_MTU_CAN_CLASSIC;
	}

	ins.node_id = (pub ? CONFIG_S32KUAVCANV1EXAMPLE_NODE_ID : CONFIG_S32KUAVCANV1EXAMPLE_NODE_ID + 1);

	/* Open the CAN device for reading */
	CanardSocketInstance sock_ins;
	socketcanOpen(&sock_ins, CONFIG_S32KUAVCANV1EXAMPLE_DEV, can_fd);


	/* setup poll fd */
	fd.fd = sock_ins.s;
	fd.events = POLLIN;

	if (sock_ins.s < 0) {
		printf("canard_daemon: ERROR: open %s failed: %d\n",
		       CONFIG_S32KUAVCANV1EXAMPLE_DEV, errno);
		errval = 2;
		goto errout_with_dev;
	}


	printf("canard_daemon: canard initialized\n");
	printf("start node (ID: %d MTU: %d PUB: %d TOPIC_SIZE: %d)\n", ins.node_id,
	       ins.mtu_bytes, pub, TOPIC_SIZE);

	CanardRxSubscription heartbeat_subscription;
	(void) canardRxSubscribe(&ins,   // Subscribe to messages uavcan.node.Heartbeat.
				 CanardTransferKindMessage,
				 32085,  // The fixed Subject-ID of the Heartbeat message type (see DSDL definition).
				 7,      // The maximum payload size (max DSDL object size) from the DSDL definition.
				 CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
				 &heartbeat_subscription);

	CanardRxSubscription my_subscription;
	(void) canardRxSubscribe(&ins,
				 CanardTransferKindMessage,
				 PORT_ID,                     // The Service-ID to subscribe to.
				 TOPIC_SIZE,                  // The maximum payload size (max DSDL object size).
				 CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC,
				 &my_subscription);

	g_canard_daemon_started = true;
	uint64_t next_1hz_service_at = getMonotonicTimestampUSec();

	for (;;) {
		processTxRxOnce(&ins, &sock_ins, 10);

		const uint64_t ts = getMonotonicTimestampUSec();

		if (ts >= next_1hz_service_at) {
			next_1hz_service_at += 1000000;
			process1HzTasks(&ins, ts);
		}
	}

errout_with_dev:

	g_canard_daemon_started = false;
	printf("canard_daemon: Terminating!\n");
	fflush(stdout);
	return errval;
}


/****************************************************************************
 * Name: canardv1_main
 *
 * Description:
 *
 ****************************************************************************/

int canardv1_main(int argc, FAR char *argv[])
{
	int ret;

	printf("canardv1_main: Starting canard_daemon\n");

	if (g_canard_daemon_started) {
		printf("canardv1_main: receive and send task already running\n");
		return EXIT_SUCCESS;
	}

	ret = task_create("canard_daemon", CONFIG_S32KUAVCANV1EXAMPLE_DAEMON_PRIORITY,
			  CONFIG_S32KUAVCANV1EXAMPLE_DAEMON_STACK_SIZE, canard_daemon,
			  argv);

	if (ret < 0) {
		int errcode = errno;
		printf("canardv1_main: ERROR: Failed to start canard_daemon: %d\n",
		       errcode);
		return EXIT_FAILURE;
	}

	printf("canardv1_main: canard_daemon started\n");
	return EXIT_SUCCESS;
}
