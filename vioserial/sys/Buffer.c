#include "precomp.h"
#include "vioser.h"

#if defined(EVENT_TRACING)
#include "Buffer.tmh"
#endif

PPORT_BUFFER
VIOSerialAllocateBuffer(
    IN size_t buf_size
)
{
    PPORT_BUFFER buf;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    buf = ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 sizeof(PORT_BUFFER),
                                 VIOSERIAL_DRIVER_MEMORY_TAG
                                 );
    if (buf == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_QUEUEING, "ExAllocatePoolWithTag failed, %s::%d\n", __FUNCTION__, __LINE__);
        return NULL;
    }
    buf->va_buf = ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 buf_size,
                                 VIOSERIAL_DRIVER_MEMORY_TAG
                                 );
    if(buf->va_buf == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_QUEUEING, "ExAllocatePoolWithTag failed, %s::%d\n", __FUNCTION__, __LINE__);
        ExFreePoolWithTag(buf, VIOSERIAL_DRIVER_MEMORY_TAG);
        return NULL;
    }
    buf->pa_buf = MmGetPhysicalAddress(buf->va_buf);
    buf->len = 0;
    buf->offset = 0;
    buf->size = buf_size;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return buf;
}

SSIZE_T
VIOSerialSendBuffers(
    IN PVIOSERIAL_PORT port,
    IN PVOID buf,
    IN SIZE_T count,
    IN BOOLEAN nonblock
)
{
    UINT dummy;
    SSIZE_T ret;
    struct VirtIOBufferDescriptor sg;
    struct virtqueue *vq = GetOutQueue(port);
    PVOID ptr = buf;
    SIZE_T len = count;
    SIZE_T sent = 0;
    UINT elements = 0;
    UINT retries = 0;
    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "--> %s buf = %p, length = %d\n", __FUNCTION__, buf, (int)count);

    WdfSpinLockAcquire(port->OutVqLock);
    VIOSerialReclaimConsumedBuffers(port);

    while (len && vq)
    {
        do
        {
           sg.physAddr = MmGetPhysicalAddress(ptr);
           sg.ulSize = min(PAGE_SIZE, (unsigned long)len);

           ret = vq->vq_ops->add_buf(vq, &sg, 1, 0, ptr, NULL, 0);
           if (ret >= 0)
           {
              ptr = (PVOID)((LONG_PTR)ptr + sg.ulSize);
              len -= sg.ulSize;
              sent += sg.ulSize;
              elements++;
           }
        } while ((ret >= 0) && (len > 0));

        vq->vq_ops->kick(vq);
        port->OutVqFull = (ret < 0);
        if (!nonblock && sent)
        {
           TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "<-> %s !nonblock\n", __FUNCTION__);
           while(elements && retries < RETRY_THRESHOLD)
           {
              if(vq->vq_ops->get_buf(vq, &dummy))
              {
                 elements--;
                 retries = 0;
              }
              else
              {
                 KeStallExecutionProcessor(100);
                 retries++;
              }
           }
           if (retries == RETRY_THRESHOLD)
           {
              TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "<-> %s retries = %d\n", __FUNCTION__, retries);
              break;
           }
        }
    }
    WdfSpinLockRelease(port->OutVqLock);
    TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "<-- %s\n", __FUNCTION__);
    return sent;
}

VOID
VIOSerialFreeBuffer(
    IN PPORT_BUFFER buf
)
{
    ASSERT(buf);
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s  buf = %p, buf->va_buf = %p\n", __FUNCTION__, buf, buf->va_buf);
    if (buf->va_buf)
    {
        ExFreePoolWithTag(buf->va_buf, VIOSERIAL_DRIVER_MEMORY_TAG);
        buf->va_buf = NULL;
    }
    ExFreePoolWithTag(buf, VIOSERIAL_DRIVER_MEMORY_TAG);
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
}

VOID
VIOSerialReclaimConsumedBuffers(
    IN PVIOSERIAL_PORT port
)
{
    PVOID buf;
    UINT len;
    struct virtqueue *vq = GetOutQueue(port);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    while (vq && (buf = vq->vq_ops->get_buf(vq, &len)) != NULL)
    {
        KeStallExecutionProcessor(100);
        port->OutVqFull = FALSE;
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s port->OutVqFull = %d\n", __FUNCTION__, port->OutVqFull);
}

// this procedure must be called with port InBuf spinlock held
SSIZE_T
VIOSerialFillReadBufLocked(
    IN PVIOSERIAL_PORT port,
    IN PVOID outbuf,
    IN SIZE_T count
)
{
    PPORT_BUFFER buf;
    NTSTATUS  status = STATUS_SUCCESS;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    if (!count || !VIOSerialPortHasDataLocked(port))
        return 0;

    buf = port->InBuf;
    count = min(count, buf->len - buf->offset);

    RtlCopyMemory(outbuf, (PVOID)((LONG_PTR)buf->va_buf + buf->offset), count);

    buf->offset += count;

    if (buf->offset == buf->len)
    {
        port->InBuf = NULL;

        status = VIOSerialAddInBuf(GetInQueue(port), buf);
        if (!NT_SUCCESS(status))
        {
           TraceEvents(TRACE_LEVEL_INFORMATION, DBG_QUEUEING, "%s::%d  VIOSerialAddInBuf failed\n", __FUNCTION__, __LINE__);
        }
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return count;
}


NTSTATUS
VIOSerialAddInBuf(
    IN struct virtqueue *vq,
    IN PPORT_BUFFER buf)
{
    NTSTATUS  status = STATUS_SUCCESS;
    struct VirtIOBufferDescriptor sg;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s  buf = %p\n", __FUNCTION__, buf);
    if (buf == NULL)
    {
        ASSERT(0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (vq == NULL)
    {
        ASSERT(0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    sg.physAddr = buf->pa_buf;
    sg.ulSize = buf->size;

    if(0 > vq->vq_ops->add_buf(vq, &sg, 0, 1, buf, NULL, 0))
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    vq->vq_ops->kick(vq);
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return status;
}


PVOID
VIOSerialGetInBuf(
    IN PVIOSERIAL_PORT port
)
{
    PPORT_BUFFER buf = NULL;
    struct virtqueue *vq = GetInQueue(port);
    UINT len;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    if (vq)
    {
        buf = vq->vq_ops->get_buf(vq, &len);
        if (buf)
        {
           buf->len = len;
           buf->offset = 0;
        }
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return buf;
}
