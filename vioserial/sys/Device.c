/**********************************************************************
 * Copyright (c) 2010  Red Hat, Inc.
 *
 * File: VIOSerialDevice.c
 *
 * Placeholder for the device related functions
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
**********************************************************************/

#include "precomp.h"
#include "vioser.h"

#if defined(EVENT_TRACING)
#include "Device.tmh"
#endif

EVT_WDF_DEVICE_PREPARE_HARDWARE     VIOSerialEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     VIOSerialEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY             VIOSerialEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT              VIOSerialEvtDeviceD0Exit;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED VIOSerialEvtDeviceD0ExitPreInterruptsDisabled;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED VIOSerialEvtDeviceD0EntryPostInterruptsEnabled;


static NTSTATUS VIOSerialInitInterruptHandling(IN WDFDEVICE hDevice);
static NTSTATUS VIOSerialInitAllQueues(IN WDFOBJECT hDevice);
static NTSTATUS VIOSerialShutDownAllQueues(IN WDFOBJECT WdfDevice, IN BOOLEAN bFinal);

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, VIOSerialEvtDeviceAdd)
#pragma alloc_text (PAGE, VIOSerialEvtDevicePrepareHardware)
#pragma alloc_text (PAGE, VIOSerialEvtDeviceReleaseHardware)
#pragma alloc_text (PAGE, VIOSerialEvtDeviceD0Exit)
#pragma alloc_text (PAGE, VIOSerialEvtDeviceD0ExitPreInterruptsDisabled)
#pragma alloc_text (PAGE, VIOSerialEvtDeviceD0EntryPostInterruptsEnabled)

#endif

static UINT gDeviceCount = 0;


static
NTSTATUS
VIOSerialInitInterruptHandling(
    IN WDFDEVICE hDevice)
{
    WDF_OBJECT_ATTRIBUTES        attributes;
    WDF_INTERRUPT_CONFIG         interruptConfig;
    PPORTS_DEVICE	         pContext = GetPortsDevice(hDevice);
    NTSTATUS                     status = STATUS_SUCCESS;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_HW_ACCESS, "--> %s\n", __FUNCTION__);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PORTS_DEVICE);
    WDF_INTERRUPT_CONFIG_INIT(
                                 &interruptConfig,
                                 VIOSerialInterruptIsr,
                                 VIOSerialInterruptDpc
                                 );

    interruptConfig.EvtInterruptEnable = VIOSerialInterruptEnable;
    interruptConfig.EvtInterruptDisable = VIOSerialInterruptDisable;

    status = WdfInterruptCreate(
                                 hDevice,
                                 &interruptConfig,
                                 &attributes,
                                 &pContext->WdfInterrupt
                                 );

    if (!NT_SUCCESS (status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "WdfInterruptCreate failed: %x\n", status);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return status;
}

NTSTATUS
VIOSerialEvtDeviceAdd(
    IN WDFDRIVER Driver,
    IN PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS                     status = STATUS_SUCCESS;
    WDF_OBJECT_ATTRIBUTES        Attributes;
    WDFDEVICE                    hDevice;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_CHILD_LIST_CONFIG        ChildListConfig;
    PNP_BUS_INFORMATION          busInfo;
    PPORTS_DEVICE                pContext = NULL;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "--> %s\n", __FUNCTION__);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = VIOSerialEvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = VIOSerialEvtDeviceReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry         = VIOSerialEvtDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit          = VIOSerialEvtDeviceD0Exit;
    PnpPowerCallbacks.EvtDeviceD0ExitPreInterruptsDisabled = VIOSerialEvtDeviceD0ExitPreInterruptsDisabled;
    PnpPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = VIOSerialEvtDeviceD0EntryPostInterruptsEnabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    WDF_CHILD_LIST_CONFIG_INIT(
                                 &ChildListConfig,
                                 sizeof(VIOSERIAL_PORT),
                                 VIOSerialDeviceListCreatePdo
                                 );

    ChildListConfig.EvtChildListIdentificationDescriptionDuplicate =
                                 VIOSerialEvtChildListIdentificationDescriptionDuplicate;

    ChildListConfig.EvtChildListIdentificationDescriptionCompare =
                                 VIOSerialEvtChildListIdentificationDescriptionCompare;

    ChildListConfig.EvtChildListIdentificationDescriptionCleanup =
                                 VIOSerialEvtChildListIdentificationDescriptionCleanup;

    WdfFdoInitSetDefaultChildListConfig(
                                 DeviceInit,
                                 &ChildListConfig,
                                 WDF_NO_OBJECT_ATTRIBUTES
                                 );

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, PORTS_DEVICE);
    Attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &Attributes, &hDevice);
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfDeviceCreate failed - 0x%x\n", status);
        return status;
    }

    status = VIOSerialInitInterruptHandling(hDevice);
    if(!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "VIOSerialInitInterruptHandling failed - 0x%x\n", status);
    }

    status = WdfDeviceCreateDeviceInterface(
                                 hDevice,
                                 &GUID_VIOSERIAL_CONTROLLER,
                                 NULL
                                 );
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfDeviceCreateDeviceInterface failed - 0x%x\n", status);
        return status;
    }

    pContext = GetPortsDevice(hDevice);
    pContext->DeviceId = gDeviceCount++;

    busInfo.BusTypeGuid = GUID_DEVCLASS_PORT_DEVICE;
    busInfo.LegacyBusType = PNPBus;
    busInfo.BusNumber = pContext->DeviceId;

    WdfDeviceSetBusInformationForChildren(hDevice, &busInfo);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return status;
}

NTSTATUS
VIOSerialEvtDevicePrepareHardware(
    IN WDFDEVICE Device,
    IN WDFCMRESLIST ResourcesRaw,
    IN WDFCMRESLIST ResourcesTranslated)
{
    int nListSize = 0;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR pResDescriptor;
    WDF_INTERRUPT_INFO interruptInfo;
    int i = 0;
    PPORTS_DEVICE pContext = GetPortsDevice(Device);
    bool bPortFound = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    UINT nr_ports, max_queues, size_to_allocate;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<--> %s\n", __FUNCTION__);

    max_queues = 64; // 2 for each of max 32 ports
    size_to_allocate = VirtIODeviceSizeRequired((USHORT)max_queues);

    pContext->pIODevice = (VirtIODevice *)ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 size_to_allocate,
                                 VIOSERIAL_DRIVER_MEMORY_TAG);
    if (NULL == pContext->pIODevice)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    nListSize = WdfCmResourceListGetCount(ResourcesTranslated);

    for (i = 0; i < nListSize; i++)
    {
        if(pResDescriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i))
        {
            switch(pResDescriptor->Type)
            {
                case CmResourceTypePort :
                    pContext->bPortMapped = (pResDescriptor->Flags & CM_RESOURCE_PORT_IO) ? FALSE : TRUE;
                    pContext->PortBasePA = pResDescriptor->u.Port.Start;
                    pContext->uPortLength = pResDescriptor->u.Port.Length;
                    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "IO Port Info  [%08I64X-%08I64X]\n",
                                 pResDescriptor->u.Port.Start.QuadPart,
                                 pResDescriptor->u.Port.Start.QuadPart +
                                 pResDescriptor->u.Port.Length);

                    if (pContext->bPortMapped )
                    {
                        pContext->pPortBase = MmMapIoSpace(pContext->PortBasePA,
                                                           pContext->uPortLength,
                                                           MmNonCached);

                        if (!pContext->pPortBase) {
                            TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "%s>>> Failed to map IO port!\n", __FUNCTION__);
                            return STATUS_INSUFFICIENT_RESOURCES;
                        }
                    }
                    else
                    {
                        pContext->pPortBase = (PVOID)(ULONG_PTR)pContext->PortBasePA.QuadPart;
                    }

                    bPortFound = TRUE;

                    break;
                case CmResourceTypeInterrupt:
                    break;
            }
        }
    }

    if(!bPortFound)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_HW_ACCESS, "%s>>> %s", __FUNCTION__, "IO port wasn't found!\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    WDF_INTERRUPT_INFO_INIT(&interruptInfo);
    WdfInterruptGetInfo(pContext->WdfInterrupt, &interruptInfo);

    VirtIODeviceInitialize(pContext->pIODevice, (ULONG_PTR)pContext->pPortBase, size_to_allocate);
    VirtIODeviceSetMSIXUsed(pContext->pIODevice, interruptInfo.MessageSignaled);

    VirtIODeviceReset(pContext->pIODevice);

    VirtIODeviceAddStatus(pContext->pIODevice, VIRTIO_CONFIG_S_ACKNOWLEDGE);

    pContext->consoleConfig.max_nr_ports = 1;
    if(pContext->isHostMultiport = VirtIODeviceGetHostFeature(pContext->pIODevice, VIRTIO_CONSOLE_F_MULTIPORT))
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "We have multiport host\n");
        VirtIODeviceEnableGuestFeature(pContext->pIODevice, VIRTIO_CONSOLE_F_MULTIPORT);
        VirtIODeviceGet(pContext->pIODevice,
                                 FIELD_OFFSET(CONSOLE_CONFIG, max_nr_ports),
                                 &pContext->consoleConfig.max_nr_ports,
                                 sizeof(pContext->consoleConfig.max_nr_ports));
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
                                "VirtIOConsoleConfig->max_nr_ports %d\n", pContext->consoleConfig.max_nr_ports);
        if (pContext->consoleConfig.max_nr_ports > pContext->pIODevice->maxQueues / 2)
        {
           pContext->consoleConfig.max_nr_ports = pContext->pIODevice->maxQueues / 2;
           TraceEvents(TRACE_LEVEL_WARNING, DBG_PNP,
                                "VirtIOConsoleConfig->max_nr_ports limited to %d\n", pContext->consoleConfig.max_nr_ports);
        }
    }

    if(pContext->isHostMultiport)
    {
        WDF_OBJECT_ATTRIBUTES  attributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = Device;
        status = WdfSpinLockCreate(
                                &attributes,
                                &pContext->CVqLock
                                );
        if (!NT_SUCCESS(status))
        {
           TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,
                "WdfSpinLockCreate failed 0x%x\n", status);
           return status;
        }
    }
    else
    {
//FIXME
//        VIOSerialAddPort(Device, 0);
    }

    nr_ports = pContext->consoleConfig.max_nr_ports;
    pContext->in_vqs = (struct virtqueue**)ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 nr_ports * sizeof(struct virtqueue*),
                                 VIOSERIAL_DRIVER_MEMORY_TAG);

    if(pContext->in_vqs == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT,"ExAllocatePoolWithTag failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memset(pContext->in_vqs, 0, nr_ports * sizeof(struct virtqueue*));
    pContext->out_vqs = (struct virtqueue**)ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 nr_ports * sizeof(struct virtqueue*),
                                 VIOSERIAL_DRIVER_MEMORY_TAG
                                 );

    if(pContext->out_vqs == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "ExAllocatePoolWithTag failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    memset(pContext->out_vqs, 0, nr_ports * sizeof(struct virtqueue*));

    pContext->DeviceOK = TRUE;
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return status;
}

NTSTATUS
VIOSerialEvtDeviceReleaseHardware(
    IN WDFDEVICE Device,
    IN WDFCMRESLIST ResourcesTranslated)
{
    PPORTS_DEVICE pContext = GetPortsDevice(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "--> %s\n", __FUNCTION__);

    if (pContext->pPortBase && pContext->bPortMapped)
    {
        MmUnmapIoSpace(pContext->pPortBase, pContext->uPortLength);
    }

    pContext->pPortBase = (ULONG_PTR)NULL;

    if(pContext->in_vqs)
    {
        ExFreePoolWithTag(pContext->in_vqs, VIOSERIAL_DRIVER_MEMORY_TAG);
        pContext->in_vqs = NULL;
    }

    if(pContext->out_vqs)
    {
        ExFreePoolWithTag(pContext->out_vqs, VIOSERIAL_DRIVER_MEMORY_TAG);
        pContext->out_vqs = NULL;
    }

    if (pContext->pIODevice)
    {
        ExFreePoolWithTag(pContext->pIODevice, VIOSERIAL_DRIVER_MEMORY_TAG);
        pContext->pIODevice = NULL;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_HW_ACCESS, "<-- %s\n", __FUNCTION__);
    return STATUS_SUCCESS;
}

static struct virtqueue * FindVirtualQueue(VirtIODevice *dev, ULONG index)
{
	struct virtqueue *pq = NULL;
	PVOID p;
	ULONG size, allocSize;
	VirtIODeviceQueryQueueAllocation(dev, index, &size, &allocSize);
	if (allocSize)
	{
		PHYSICAL_ADDRESS HighestAcceptable;
		HighestAcceptable.QuadPart = 0xFFFFFFFFFF;
		p = MmAllocateContiguousMemory(allocSize, HighestAcceptable);
		if (p)
		{
			pq = VirtIODevicePrepareQueue(dev, index, MmGetPhysicalAddress(p), p, allocSize, p, FALSE);
		}
	}
	return pq;
}

#if 0
void DumpQueues(WDFOBJECT Device)
{
    ULONG i, nr_ports;
    PPORTS_DEVICE          pContext = GetPortsDevice(Device);
    nr_ports = pContext->consoleConfig.max_nr_ports;
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "pContext->c_ivq %p\n",  pContext->c_ivq);
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "pContext->c_ovq %p\n",  pContext->c_ovq);
    for (i = 0; i < nr_ports; ++i)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "pContext->in_vqs[%d] %p\n", i,  pContext->in_vqs[i]);
    }
    for (i = 0; i < nr_ports; ++i)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "pContext->out_vqs[%d] %p\n", i, pContext->out_vqs[i]);
    }
}
#endif

static
NTSTATUS
VIOSerialInitAllQueues(
    IN WDFOBJECT Device)
{
    NTSTATUS               status = STATUS_SUCCESS;
    PPORTS_DEVICE          pContext = GetPortsDevice(Device);
    UINT                   nr_ports, i, j;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    nr_ports = pContext->consoleConfig.max_nr_ports;
    if(pContext->isHostMultiport)
    {
        nr_ports++;
    }
    for(i = 0, j = 0; i < nr_ports; i++)
    {
        if(i == 1) // Control Port
        {
           if (pContext->c_ivq)
              VirtIODeviceRenewQueue(pContext->c_ivq);
           else
              pContext->c_ivq = FindVirtualQueue(pContext->pIODevice, 2);
           if (pContext->c_ovq)
              VirtIODeviceRenewQueue(pContext->c_ovq);
           else
              pContext->c_ovq = FindVirtualQueue(pContext->pIODevice, 3);
        }
        else
        {
           if (pContext->in_vqs[j])
              VirtIODeviceRenewQueue(pContext->in_vqs[j]);
           else
              pContext->in_vqs[j] = FindVirtualQueue(pContext->pIODevice, i * 2);
           if (pContext->out_vqs[j])
              VirtIODeviceRenewQueue(pContext->out_vqs[j]);
           else
              pContext->out_vqs[j] = FindVirtualQueue(pContext->pIODevice, (i * 2) + 1);
           ++j;
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
    return status;
}

static void DeleteQueue(struct virtqueue **ppq, BOOLEAN bFinal)
{
    PVOID p;
    struct virtqueue *pq = *ppq;
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);
    if (pq)
    {
        pq->vq_ops->shutdown(pq);
        if (bFinal)
        {
           VirtIODeviceDeleteQueue(pq, &p);
           *ppq = NULL;
           MmFreeContiguousMemory(p);
        }
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
}

NTSTATUS
VIOSerialShutDownAllQueues(
    IN WDFOBJECT WdfDevice,
    IN BOOLEAN   bFinal)
{
    NTSTATUS		status = STATUS_SUCCESS;
    PPORTS_DEVICE	pContext = GetPortsDevice(WdfDevice);
    UINT                nr_ports, i;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    VirtIODeviceRemoveStatus(pContext->pIODevice , VIRTIO_CONFIG_S_DRIVER_OK);

    if(pContext->isHostMultiport)
    {
        DeleteQueue(&pContext->c_ivq, bFinal);
        DeleteQueue(&pContext->c_ovq, bFinal);
    }

    nr_ports = pContext->consoleConfig.max_nr_ports;
    for(i = 0; i < nr_ports; i++ )
    {
        if(pContext->in_vqs && pContext->in_vqs[i])
        {
            DeleteQueue(&(pContext->in_vqs[i]), bFinal);
        }

        if(pContext->out_vqs && pContext->out_vqs[i])
        {
            DeleteQueue(&(pContext->out_vqs[i]), bFinal);
        }
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
    return status;
}

NTSTATUS
VIOSerialFillQueue(
    IN struct virtqueue *vq,
    IN WDFSPINLOCK Lock
)
{
    NTSTATUS     status = STATUS_SUCCESS;
    PPORT_BUFFER buf = NULL;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "--> %s\n", __FUNCTION__);

    for (;;)
    {
        buf = VIOSerialAllocateBuffer(PAGE_SIZE);
        if(buf == NULL)
        {
           TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "VIOSerialAllocateBuffer failed\n");
           return STATUS_INSUFFICIENT_RESOURCES;
        }

        WdfSpinLockAcquire(Lock);
        status = VIOSerialAddInBuf(vq, buf);
        if(!NT_SUCCESS(status))
        {
           VIOSerialFreeBuffer(buf);
           WdfSpinLockRelease(Lock);
           break;
        }
        WdfSpinLockRelease(Lock);
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT, "<-- %s\n", __FUNCTION__);
    return STATUS_SUCCESS;
}

NTSTATUS
VIOSerialEvtDeviceD0Entry(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PPORTS_DEVICE	pContext = GetPortsDevice(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    if(!pContext->DeviceOK)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Setting VIRTIO_CONFIG_S_FAILED flag\n");
        VirtIODeviceAddStatus(pContext->pIODevice, VIRTIO_CONFIG_S_FAILED);
    }
    else
    {
        VIOSerialInitAllQueues(Device);
        VIOSerialRenewAllPorts(Device);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
    return STATUS_SUCCESS;
}

NTSTATUS
VIOSerialEvtDeviceD0Exit(
    IN  WDFDEVICE Device,
    IN  WDF_POWER_DEVICE_STATE TargetState
    )
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    PAGED_CODE();

    VIOSerialShutdownAllPorts(Device);
    VIOSerialShutDownAllQueues(Device, TRUE);
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
    return STATUS_SUCCESS;
}

NTSTATUS
VIOSerialEvtDeviceD0ExitPreInterruptsDisabled(
    IN WDFDEVICE WdfDevice,
    IN WDF_POWER_DEVICE_STATE TargetState
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFCHILDLIST portList;
    WDF_CHILD_LIST_ITERATOR portIterator;

    UNREFERENCED_PARAMETER(TargetState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    PAGED_CODE();

    portList = WdfFdoGetDefaultChildList(WdfDevice);
    WDF_CHILD_LIST_ITERATOR_INIT(&portIterator, WdfRetrievePresentChildren);

    WdfChildListBeginIteration(portList, &portIterator);

    for (;;)
    {
        WDF_CHILD_RETRIEVE_INFO childInfo;
        WDFDEVICE hChild;
        VIOSERIAL_PORT port;

        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(
            &port.Header, sizeof(port));
        WDF_CHILD_RETRIEVE_INFO_INIT(&childInfo, &port.Header);

        status = WdfChildListRetrieveNextDevice(portList, &portIterator,
            &hChild, &childInfo);
        if (!NT_SUCCESS(status) || (status == STATUS_NO_MORE_ENTRIES))
        {
            break;
        }
        ASSERT(childInfo.Status == WdfChildListRetrieveDeviceSuccess);

        if (port.GuestConnected && !port.Removed)
        {
            VIOSerialSendCtrlMsg(port.BusDevice, port.PortId,
                VIRTIO_CONSOLE_PORT_OPEN, 0);
            port.GuestConnected = FALSE;
        }
        port.Removed = TRUE;
    }

    WdfChildListEndIteration(portList, &portIterator);

    if (status == STATUS_NO_MORE_ENTRIES)
    {
        status = STATUS_SUCCESS;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s: 0x%x\n",
        __FUNCTION__, status);

    return status;
}

NTSTATUS
VIOSerialEvtDeviceD0EntryPostInterruptsEnabled(
    IN  WDFDEVICE WdfDevice,
    IN  WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PPORTS_DEVICE	pContext = GetPortsDevice(WdfDevice);
    UNREFERENCED_PARAMETER(PreviousState);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    PAGED_CODE();

    if(!pContext->DeviceOK)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Sending VIRTIO_CONSOLE_DEVICE_READY 0\n");
        VIOSerialSendCtrlMsg(WdfDevice, VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 0);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Setting VIRTIO_CONFIG_S_DRIVER_OK flag\n");
        VirtIODeviceAddStatus(pContext->pIODevice, VIRTIO_CONFIG_S_DRIVER_OK);
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Sending VIRTIO_CONSOLE_DEVICE_READY 1\n");
        VIOSerialSendCtrlMsg(WdfDevice, VIRTIO_CONSOLE_BAD_ID, VIRTIO_CONSOLE_DEVICE_READY, 1);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
    return STATUS_SUCCESS;
}

