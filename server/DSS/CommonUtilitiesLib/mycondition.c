/*
	File:		mycondition.c

	Contains:	xxx put contents here xxx

	Written by:	Greg Vaughan

	Copyright:	� 1999-2000 by Apple Computer, Inc., all rights reserved.

	Writers:

		(GV)	Greg Vaughan
		(CNR)	Christopher Ryan

	Change History (most recent first):

		 <5>	 7/24/00	GV		changed Carbon to CarbonCore for header include
		 <4>	  1/6/00	GBV		got rid of libatomic
		 <3>	 12/8/99	CNR		Use OSAssert.h
		 <2>	10/27/99	GBV		update for beaker

	To Do:
*/

#include "mycondition.h"
#include <stdlib.h>

#include <CarbonCore/DriverSynchronization.h>
#include "MyAssert.h"

struct MyCondition
{
	mach_port_t				fWaitPort;
	long					fNumWaiting;
};

typedef struct MyCondition MyCondition;

MyCondition* MCAllocateCondition();
void MCDisposeCondition(MyCondition* theCondition);
void MCBroadcast(MyCondition* theCondition);
void MCSignal(MyCondition* theCondition);
void MCWait(MyCondition* theCondition, mymutex_t theMutex, int timeout);
void MCBlockThread(MyCondition* theCondition, int timeout);
void MCUnblockThread(MyCondition* theCondition);

mycondition_t mycondition_alloc()
{
	return (mycondition_t)MCAllocateCondition();
}

void mycondition_free(mycondition_t theCondition_t)
{
	MCDisposeCondition((MyCondition*)theCondition_t);
}

void mycondition_broadcast(mycondition_t theCondition_t)
{
	MCBroadcast((MyCondition*)theCondition_t);
}

void mycondition_signal(mycondition_t theCondition_t)
{
	MCSignal((MyCondition*)theCondition_t);
}

void mycondition_wait(mycondition_t theCondition_t, mymutex_t theMutex_t, int timeout)
{
	MCWait((MyCondition*)theCondition_t, theMutex_t, timeout);
}

MyCondition* MCAllocateCondition()
{
	kern_return_t ret;
	MyCondition* newCondition = (MyCondition*)malloc(sizeof(MyCondition));
	if (newCondition == NULL)
	{
		Assert(newCondition != NULL);
		return NULL;
	}
		
	newCondition->fNumWaiting = 0;

	ret = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &newCondition->fWaitPort);
	if (ret != KERN_SUCCESS)
	{
		free(newCondition);
		return NULL;
	}
	ret = mach_port_insert_right(mach_task_self(), newCondition->fWaitPort, newCondition->fWaitPort, MACH_MSG_TYPE_MAKE_SEND);
	if (ret != KERN_SUCCESS)
	{
		free(newCondition);
		return NULL;
	}
	return newCondition;
}

void MCDisposeCondition(MyCondition* theCondition)
{
	mach_port_deallocate(mach_task_self(), theCondition->fWaitPort);
	free(theCondition);
}

void MCBroadcast(MyCondition* theCondition)
{
	int numToSignal = theCondition->fNumWaiting;
	while (numToSignal > 0)
	{
		MCUnblockThread(theCondition);
		numToSignal--;
	}
}

void MCSignal(MyCondition* theCondition)
{
	MCUnblockThread(theCondition);
}

void MCWait(MyCondition* theCondition, mymutex_t theMutex, int timeout)
{
	mymutex_unlock(theMutex);
	IncrementAtomic(&theCondition->fNumWaiting);
	MCBlockThread(theCondition, timeout);
	DecrementAtomic(&theCondition->fNumWaiting);
	mymutex_lock(theMutex);
}


typedef struct {
	mach_msg_header_t header;
	mach_msg_trailer_t trailer;
} mHeader;

void MCBlockThread(MyCondition* theCondition, int timeout)
{
	kern_return_t ret;
	mHeader msg;

	memset(&msg, 0, sizeof(msg));
	if (timeout > 0)
		ret = mach_msg(&msg.header,MACH_RCV_MSG | MACH_RCV_TIMEOUT,0, sizeof(msg),
								theCondition->fWaitPort,timeout,MACH_PORT_NULL);
	else
		ret = mach_msg(&msg.header,MACH_RCV_MSG,0, sizeof(msg),
								theCondition->fWaitPort,MACH_MSG_TIMEOUT_NONE,MACH_PORT_NULL);
	AssertV(ret == 0, ret);
}

void MCUnblockThread(MyCondition* theCondition)
{
	kern_return_t ret;
	mHeader msg;

	memset(&msg, 0, sizeof(msg));
	msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	msg.header.msgh_size = sizeof msg - sizeof msg.trailer;
	msg.header.msgh_local_port = MACH_PORT_NULL; 
	msg.header.msgh_remote_port = theCondition->fWaitPort;
    msg.header.msgh_id = 0;
    ret = mach_msg(&msg.header,MACH_SEND_MSG | MACH_SEND_TIMEOUT,msg.header.msgh_size,0,MACH_PORT_NULL,0,
							MACH_PORT_NULL);
	AssertV(ret == 0, ret);
}
