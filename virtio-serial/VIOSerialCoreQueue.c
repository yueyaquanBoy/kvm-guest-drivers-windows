#include "osdep.h"
#include "kdebugprint.h"
#include "VIOSerialDriver.h"
#include "VIOSerialMemUtils.h"
#include "VIOSerialCoreQueue.h"

static void FreeBufferDescriptor(pIODescriptor pBufferDescriptor)
{
	if(pBufferDescriptor)
	{
		if (pBufferDescriptor->DataInfo.Virtual)
		{
			FreePhysical(pBufferDescriptor->DataInfo.Virtual);
		}
		
		ExFreePoolWithTag(pBufferDescriptor, VIOSERIAL_DRIVER_MEMORY_TAG);

		DPrintf(6, ("Freeing %x", pBufferDescriptor));
		pBufferDescriptor = NULL;
	}
}

static void FreeDescriptorsFromList(PLIST_ENTRY pListRoot, PKSPIN_LOCK pLock)
{
	pIODescriptor pBufferDescriptor;
	LIST_ENTRY TempList;
	KIRQL IRQL;

	InitializeListHead(&TempList);
	KeAcquireSpinLock(pLock, &IRQL);
	while(!IsListEmpty(pListRoot))
	{
		pBufferDescriptor = (pIODescriptor)RemoveHeadList(pListRoot);
		InsertTailList(&TempList, &pBufferDescriptor->listEntry);
	}
	KeReleaseSpinLock(pLock, IRQL);

	while(!IsListEmpty(&TempList))
	{
		pBufferDescriptor = (pIODescriptor)RemoveHeadList(&TempList);
		FreeBufferDescriptor(pBufferDescriptor);
	}
}

static pIODescriptor AllocateIOBuffer(ULONG size)
{
	pIODescriptor p;
	p = (pIODescriptor)ExAllocatePoolWithTag(NonPagedPool,
											 size,
											 VIOSERIAL_DRIVER_MEMORY_TAG);

	if (p)
	{
		BOOLEAN b1 = FALSE, b2 = FALSE;
		RtlZeroMemory(p, sizeof(IODescriptor));
		p->DataInfo.size = size;
		p->DataInfo.Virtual = AllocatePhysical(size);

		if(p->DataInfo.Virtual)
		{
			p->DataInfo.Physical = GetPhysicalAddress(p->DataInfo.Virtual);
			RtlZeroMemory(p->DataInfo.Virtual, p->DataInfo.size);
		}
		else
		{
			FreeBufferDescriptor(p);
			p = NULL;
			DPrintf(0, ("[INITPHYS](Failed to allocate physical memory block"));
		}
	}
	else
	{
		DPrintf(0, ("[INITPHYS] Failed to allocate IODescriptor!"));
	}

	if (p)
	{
		DPrintf(3, ("[INITPHYS] Data v%p(p%08lX)",
					p->DataInfo.Virtual, p->DataInfo.Physical.LowPart));
	}

	return p;
}

static BOOLEAN AddRxBufferToQueue(PVIOSERIAL_PORT pPort, pIODescriptor pBufferDescriptor)
{
	struct VirtIOBufferDescriptor sg[1];

	sg[0].physAddr = pBufferDescriptor->DataInfo.Physical;
	sg[0].ulSize = pBufferDescriptor->DataInfo.size;

	return 0 == pPort->ReceiveQueue->vq_ops->add_buf(pPort->ReceiveQueue,
													 sg,
													 0,
													 1,		// How many buffers to add
													 pBufferDescriptor);
}

// **********************************************************
// Allocates maximum RX buffers for incoming dara
// Buffers are chained in pPort->ReceiveBuffers
// Parameters:
//     PVIOSERIAL_PORT
// ***********************************************************
static int PrepareReceiveBuffers(PVIOSERIAL_PORT pPort)
{
	int nRet = 0;
	unsigned int i;
	DEBUG_ENTRY(4);

	for (i = 0; i < pPort->MaxReceiveBuffers; ++i)
	{
		pIODescriptor pBuffersDescriptor = AllocateIOBuffer(PAGE_SIZE);
		if (!pBuffersDescriptor) break;

		if (!AddRxBufferToQueue(pPort, pBuffersDescriptor))
		{
			FreeBufferDescriptor(pBuffersDescriptor);
			break;
		}

		InsertTailList(&pPort->ReceiveBuffers, &pBuffersDescriptor->listEntry);

		pPort->NofReceiveBuffers++;
	}

	DPrintf(0, ("[%s] Port %x, NofReceiveBuffers %d\n", __FUNCTION__, pPort, pPort->NofReceiveBuffers));

	return nRet;
}

static void PrepareTransmitBuffers(PVIOSERIAL_PORT pPort)
{
	unsigned int nBuffers, nMaxBuffers;
	DEBUG_ENTRY(4);
	nMaxBuffers = VirtIODeviceGetQueueSize(pPort->SendQueue);
	
	//TBD - if needed
	//if (nMaxBuffers > pContext->maxFreeTxDescriptors) nMaxBuffers = pContext->maxFreeTxDescriptors;

	for (nBuffers = 0; nBuffers < nMaxBuffers; ++nBuffers)
	{
		pIODescriptor pBufferDescriptor = AllocateIOBuffer(PAGE_SIZE);
		if (!pBufferDescriptor) break;

		InsertTailList(&pPort->SendFreeBuffers, &pBufferDescriptor->listEntry);
		pPort->NofSendFreeBuffers++;
	}

	DPrintf(0, ("[%s] available %d Tx descriptors for port %x",
		__FUNCTION__, pPort->NofSendFreeBuffers, pPort));
}

static u32 VSCMapIndexToID(int index)
{
	////////////////////////////
	// The assignment of the queues:
	// 0 - Port 0
	// 1 - Port 0
	// 2 - control
	// 3 - control 
	// All the above because of legacy
	// 4 - Port 1
	// 5 - Port 1
	// and etc....
	////////////////////////////
	// Each port has 2 queus and one control for the device
	// Index for the array of queue's pairs (ports)

	return (index >= 2)? (index - 1) : 0;
}

void VSCCleanupQueues(IN PDEVICE_CONTEXT pContext)
{
	int i;

	DEBUG_ENTRY(0);

	for(i = 0; i < VIRTIO_SERIAL_MAX_QUEUES_COUPLES; i++ )
	{
		/* TBD - check if needed
	// list NetReceiveBuffersWaiting must be free 
			do
			{
				NdisAcquireSpinLock(&pContext->ReceiveLock);
				b = !IsListEmpty(&pContext->NetReceiveBuffersWaiting);
				NdisReleaseSpinLock(&pContext->ReceiveLock);
				if (b)
				{
					DPrintf(0, ("[%s] There are waiting buffers", __FUNCTION__));
					PrintStatistics(pContext);
					NdisMSleep(5000000);
				}
			}while (b);
		*/

		pContext->SerialPorts[i].id = VSCMapIndexToID(i);

		if(pContext->SerialPorts[i].ReceiveQueue)
		{
			pContext->SerialPorts[i].ReceiveQueue->vq_ops->shutdown(pContext->SerialPorts[i].ReceiveQueue);
			VirtIODeviceDeleteVirtualQueue(pContext->SerialPorts[i].ReceiveQueue);
			pContext->SerialPorts[i].ReceiveQueue = NULL;
		}

		if(pContext->SerialPorts[i].SendQueue)
		{
			pContext->SerialPorts[i].SendQueue->vq_ops->shutdown(pContext->SerialPorts[i].SendQueue);
			VirtIODeviceDeleteVirtualQueue(pContext->SerialPorts[i].SendQueue);
			pContext->SerialPorts[i].SendQueue = NULL;
		}

		// this can be freed, queue shut down
		FreeDescriptorsFromList(&pContext->SerialPorts[i].ReceiveBuffers,
								&pContext->DPCLock);

		// this can be freed, queue shut down
		//FreeDescriptorsFromList(&pContext->SerialPorts[i].SendInUseBuffers,
		//						&pContext->DPCLock);

		// this can be freed, send disabled
		FreeDescriptorsFromList(&pContext->SerialPorts[i].SendFreeBuffers,
								&pContext->DPCLock);
	}
}

NTSTATUS VSCInitQueues(IN PDEVICE_CONTEXT pContext)
{
	NTSTATUS status = STATUS_SUCCESS;
	int i;

	DEBUG_ENTRY(0);

	for(i = 0; i < VIRTIO_SERIAL_MAX_QUEUES_COUPLES; i++)
	{
		if(i == 1) // Control Port
		{
			pContext->SerialPorts[i].MaxReceiveBuffers = VIRTIO_SERIAL_MAX_CONTROL_RECEIVE_BUFFERS;
		}
		else
		{
			pContext->SerialPorts[i].MaxReceiveBuffers = VIRTIO_SERIAL_MAX_PORT_RECEIVE_BUFFERS;
		}

		pContext->SerialPorts[i].ReceiveQueue = VirtIODeviceFindVirtualQueue(&pContext->IODevice, i * 2, NULL);
		pContext->SerialPorts[i].SendQueue = VirtIODeviceFindVirtualQueue(&pContext->IODevice, (i * 2 ) + 1, NULL);

		if (pContext->SerialPorts[i].ReceiveQueue && pContext->SerialPorts[i].SendQueue)
		{
			PrepareTransmitBuffers(&pContext->SerialPorts[i]);
			PrepareReceiveBuffers(&pContext->SerialPorts[i]);
		}
		else
		{
			VSCCleanupQueues(pContext);
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
	}

	return status;
}

NTSTATUS VSCSendCopyBuffer(PVIOSERIAL_PORT pPort,
						   PVOID buffer,
						   unsigned int size,
						   PKSPIN_LOCK pLock,
						   BOOLEAN bKick) 	//size already devided in chuncks
{
	KIRQL IRQL;
	pIODescriptor pBufferDescriptor;
	struct VirtIOBufferDescriptor sg[1];

	KeAcquireSpinLock(pLock, &IRQL);

	if(IsListEmpty(&pPort->SendFreeBuffers))
	{
		KeReleaseSpinLock(pLock, IRQL);
		DPrintf (3, ("[%s] No free buffers for send operation on port %x.", 
			__FUNCTION__, pPort));

		return STATUS_INSUFFICIENT_RESOURCES;
	}

	pBufferDescriptor = (pIODescriptor)RemoveHeadList(&pPort->SendFreeBuffers);
	KeReleaseSpinLock(pLock, IRQL);

	if(pBufferDescriptor->DataInfo.size < size)
	{
		//Adding buffer back to free list
		KeAcquireSpinLock(pLock, &IRQL);
		InsertTailList(&pPort->SendFreeBuffers, &pBufferDescriptor->listEntry);
		KeReleaseSpinLock(pLock, IRQL);
		DPrintf (0, ("[%s] Buffer too small! s: %d d: %d.", 
			__FUNCTION__, size, pBufferDescriptor->DataInfo.size));

		return STATUS_BUFFER_TOO_SMALL;
	}

	RtlZeroMemory(pBufferDescriptor->DataInfo.Virtual, pBufferDescriptor->DataInfo.size);
	RtlCopyMemory(pBufferDescriptor->DataInfo.Virtual, buffer, size);
	sg[0].physAddr = pBufferDescriptor->DataInfo.Physical;
	sg[0].ulSize = pBufferDescriptor->DataInfo.size;

	KeAcquireSpinLock(pLock, &IRQL);
	if (0 == pPort->SendQueue->vq_ops->add_buf(pPort->SendQueue, sg, 1, 0, pBufferDescriptor))
	{
		pPort->NofSendFreeBuffers--;
		InsertTailList(&pPort->SendInUseBuffers, &pBufferDescriptor->listEntry);

		if(bKick)
		{
			pPort->SendQueue->vq_ops->kick(pPort->SendQueue);
		}
	}
	else
	{
		DPrintf(0, ("[%s] Unexpected ERROR adding buffer to TX engine!..", __FUNCTION__));
		//Adding buffer back to free list
		InsertTailList(&pPort->SendFreeBuffers, &pBufferDescriptor->listEntry);
	}

	KeReleaseSpinLock(pLock, IRQL);

	return STATUS_SUCCESS;
}

NTSTATUS VSCRecieveCopyBuffer(PVIOSERIAL_PORT pPort,
							  WDFMEMORY * buffer,
							  unsigned int * pSize,
							  PKSPIN_LOCK pLock)
{
	NTSTATUS status = STATUS_SUCCESS;
	unsigned int len;

	KIRQL IRQL;
	pIODescriptor pBufferDescriptor;

	KeAcquireSpinLock(pLock, &IRQL);

	if(NULL == (pBufferDescriptor = pPort->ReceiveQueue->vq_ops->get_buf(pPort->ReceiveQueue, &len)))
	{
		//status = STATUS_FAILED;
	}
	KeReleaseSpinLock(pLock, IRQL);

	if(NT_SUCCESS(status))
	{
		*pSize = len;
		return WdfMemoryCopyFromBuffer(*buffer,
									   0,
									   pBufferDescriptor->DataInfo.Virtual,
									   len);

	}

	return status;
}
