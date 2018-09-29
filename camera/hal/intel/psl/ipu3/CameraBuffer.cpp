/*
 * Copyright (C) 2013-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "CameraBuffer"
#include "LogHelper.h"
#include "Utils.h"
#include <sys/mman.h>
#include "PlatformData.h"
#include "CameraBuffer.h"
#include "CameraStream.h"
#include "Camera3GFXFormat.h"
#include <unistd.h>
#include <sync/sync.h>
#include <cros-camera/camera_buffer_manager.h>

NAMESPACE_DECLARATION {
extern int32_t gDumpInterval;
extern int32_t gDumpCount;

////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
////////////////////////////////////////////////////////////////////

/**
 * CameraBuffer
 *
 * Default constructor
 * This constructor is used when we pre-allocate the CameraBuffer object
 * The initialization will be done as a second stage wit the method
 * init(), where we initialize the wrapper with the gralloc buffer provided by
 * the framework
 */
CameraBuffer::CameraBuffer() :  mWidth(0),
                                mHeight(0),
                                mSize(0),
                                mFormat(0),
                                mV4L2Fmt(0),
                                mStride(0),
                                mInit(false),
                                mLocked(false),
                                mRegistered(false),
                                mType(BUF_TYPE_HANDLE),
                                mGbmBufferManager(nullptr),
                                mHandle(nullptr),
                                mHandlePtr(nullptr),
                                mOwner(nullptr),
                                mDataPtr(nullptr),
                                mRequestID(0),
                                mCameraId(0),
                                mDmaBufFd(-1)
{
    LOG1("%s default constructor for buf %p", __FUNCTION__, this);
    CLEAR(mUserBuffer);
    CLEAR(mTimestamp);
    mUserBuffer.release_fence = -1;
    mUserBuffer.acquire_fence = -1;

}

/**
 * CameraBuffer
 *
 * Constructor for buffers allocated using MemoryUtils::allocateHeapBuffer
 *
 * \param w [IN] width
 * \param h [IN] height
 * \param s [IN] stride
 * \param v4l2fmt [IN] V4l2 format
 * \param usrPtr [IN] Data pointer
 * \param cameraId [IN] id of camera being used
 * \param dataSizeOverride [IN] buffer size input. Default is 0 and frameSize()
                                is used in that case.
 */
CameraBuffer::CameraBuffer(int w,
                           int h,
                           int s,
                           int v4l2fmt,
                           void* usrPtr,
                           int cameraId,
                           int dataSizeOverride):
        mWidth(w),
        mHeight(h),
        mSize(0),
        mFormat(0),
        mV4L2Fmt(v4l2fmt),
        mStride(s),
        mInit(false),
        mLocked(true),
        mRegistered(false),
        mType(BUF_TYPE_MALLOC),
        mGbmBufferManager(nullptr),
        mHandle(nullptr),
        mHandlePtr(nullptr),
        mOwner(nullptr),
        mDataPtr(nullptr),
        mRequestID(0),
        mCameraId(cameraId),
        mDmaBufFd(-1)
{
    LOG1("%s create malloc camera buffer %p", __FUNCTION__, this);
    if (usrPtr != nullptr) {
        mDataPtr = usrPtr;
        mInit = true;
        mSize = dataSizeOverride ? dataSizeOverride : frameSize(mV4L2Fmt, mStride, mHeight);
        mFormat = v4L2Fmt2GFXFmt(v4l2fmt);
    } else {
        LOGE("Tried to initialize a buffer with nullptr ptr!!");
    }
    CLEAR(mUserBuffer);
    CLEAR(mTimestamp);
    mUserBuffer.release_fence = -1;
    mUserBuffer.acquire_fence = -1;
}

/**
 * init
 *
 * Constructor to wrap a camera3_stream_buffer
 *
 * \param aBuffer [IN] camera3_stream_buffer buffer
 */
status_t CameraBuffer::init(const camera3_stream_buffer *aBuffer, int cameraId)
{
    mType = BUF_TYPE_HANDLE;
    mGbmBufferManager = cros::CameraBufferManager::GetInstance();
    mHandle = *aBuffer->buffer;
    mHandlePtr = aBuffer->buffer;
    mWidth = aBuffer->stream->width;
    mHeight = aBuffer->stream->height;
    mFormat = aBuffer->stream->format;
    mV4L2Fmt = mGbmBufferManager->GetV4L2PixelFormat(mHandle);
    // Use actual width from platform native handle for stride
    mStride = mGbmBufferManager->GetPlaneStride(*aBuffer->buffer, 0);
    mSize = 0;
    mLocked = false;
    mOwner = static_cast<CameraStream*>(aBuffer->stream->priv);
    mInit = true;
    mDataPtr = nullptr;
    mUserBuffer = *aBuffer;
    mUserBuffer.release_fence = -1;
    mCameraId = cameraId;
    LOG2("@%s, mHandle:%p, mFormat:%d, mWidth:%d, mHeight:%d, mStride:%d",
        __FUNCTION__, mHandle, mFormat, mWidth, mHeight, mStride);

    if (mHandle == nullptr) {
        LOGE("@%s: invalid buffer handle", __FUNCTION__);
        mUserBuffer.status = CAMERA3_BUFFER_STATUS_ERROR;
        return BAD_VALUE;
    }

    int ret = registerBuffer();
    if (ret) {
        mUserBuffer.status = CAMERA3_BUFFER_STATUS_ERROR;
        return UNKNOWN_ERROR;
    }

    /* TODO: add some consistency checks here and return an error */
    return NO_ERROR;
}

status_t CameraBuffer::init(int width, int height, int format,
                            buffer_handle_t handle,
                            int cameraId)
{
    mType = BUF_TYPE_HANDLE;
    mGbmBufferManager = cros::CameraBufferManager::GetInstance();
    mHandle = handle;
    mWidth = width;
    mHeight = height;
    mFormat = format;
    mV4L2Fmt = mGbmBufferManager->GetV4L2PixelFormat(mHandle);
    // Use actual width from platform native handle for stride
    mStride = mGbmBufferManager->GetPlaneStride(handle, 0);
    mSize = 0;
    mLocked = false;
    mOwner = nullptr;
    mInit = true;
    CLEAR(mUserBuffer);
    mUserBuffer.acquire_fence = -1;
    mUserBuffer.release_fence = -1;
    mCameraId = cameraId;
    LOG2("@%s, mHandle:%p, mFormat:%d, mWidth:%d, mHeight:%d, mStride:%d",
        __FUNCTION__, mHandle, mFormat, mWidth, mHeight, mStride);

    return NO_ERROR;
}

status_t CameraBuffer::deinit()
{
    return deregisterBuffer();
}

CameraBuffer::~CameraBuffer()
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);

    if (mInit) {
        switch(mType) {
        case BUF_TYPE_MALLOC:
            free(mDataPtr);
            mDataPtr = nullptr;
            break;
        default:
            break;
        }
    }
    LOG1("%s destroying buf %p", __FUNCTION__, this);
}

status_t CameraBuffer::waitOnAcquireFence()
{
    const int WAIT_TIME_OUT_MS = 300;
    const int BUFFER_READY = -1;

    if (mUserBuffer.acquire_fence != BUFFER_READY) {
        LOG2("%s: Fence in HAL is %d", __FUNCTION__, mUserBuffer.acquire_fence);
        int ret = sync_wait(mUserBuffer.acquire_fence, WAIT_TIME_OUT_MS);
        if (ret) {
            mUserBuffer.release_fence = mUserBuffer.acquire_fence;
            mUserBuffer.acquire_fence = -1;
            mUserBuffer.status = CAMERA3_BUFFER_STATUS_ERROR;
            LOGE("Buffer sync_wait fail!");
            return TIMED_OUT;
        } else {
            close(mUserBuffer.acquire_fence);
        }
        mUserBuffer.acquire_fence = BUFFER_READY;
    }

    return NO_ERROR;
}

/**
 * getFence
 *
 * return the fecne to request result
 */
status_t CameraBuffer::getFence(camera3_stream_buffer* buf)
{
    if (!buf)
        return BAD_VALUE;

    buf->acquire_fence = mUserBuffer.acquire_fence;
    buf->release_fence = mUserBuffer.release_fence;

    return NO_ERROR;
}

status_t CameraBuffer::registerBuffer()
{
    int ret = mGbmBufferManager->Register(mHandle);
    if (ret) {
        LOGE("@%s: call Register fail, mHandle:%p, ret:%d", __FUNCTION__, mHandle, ret);
        return UNKNOWN_ERROR;
    }

    mRegistered = true;
    return NO_ERROR;
}

status_t CameraBuffer::deregisterBuffer()
{
    if (mRegistered) {
        int ret = mGbmBufferManager->Deregister(mHandle);
        if (ret) {
            LOGE("@%s: call Deregister fail, mHandle:%p, ret:%d", __FUNCTION__, mHandle, ret);
            return UNKNOWN_ERROR;
        }
        mRegistered = false;
    }

    return NO_ERROR;
}

/**
 * lock
 *
 * lock the gralloc buffer with specified flags
 *
 * \param aBuffer [IN] int flags
 */
status_t CameraBuffer::lock(int flags)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);
    mDataPtr = nullptr;
    mSize = 0;
    int ret = 0;
    uint32_t planeNum = mGbmBufferManager->GetNumPlanes(mHandle);
    LOG2("@%s, planeNum:%d, mHandle:%p, mFormat:%d", __FUNCTION__, planeNum, mHandle, mFormat);

    if (planeNum == 1) {
        void* data = nullptr;
        ret = (mFormat == HAL_PIXEL_FORMAT_BLOB)
                ? mGbmBufferManager->Lock(mHandle, 0, 0, 0, mStride, 1, &data)
                : mGbmBufferManager->Lock(mHandle, 0, 0, 0, mWidth, mHeight, &data);
        if (ret) {
            LOGE("@%s: call Lock fail, mHandle:%p", __FUNCTION__, mHandle);
            return UNKNOWN_ERROR;
        }
        mDataPtr = data;
    } else if (planeNum > 1) {
        struct android_ycbcr ycbrData;
        ret = mGbmBufferManager->LockYCbCr(mHandle, 0, 0, 0, mWidth, mHeight, &ycbrData);
        if (ret) {
            LOGE("@%s: call LockYCbCr fail, mHandle:%p", __FUNCTION__, mHandle);
            return UNKNOWN_ERROR;
        }
        mDataPtr = ycbrData.y;
    } else {
        LOGE("ERROR @%s: planeNum is 0", __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    for (int i = 0; i < planeNum; i++) {
        mSize += mGbmBufferManager->GetPlaneSize(mHandle, i);
    }
    LOG2("@%s, mDataPtr:%p, mSize:%d", __FUNCTION__, mDataPtr, mSize);
    if (!mSize) {
        LOGE("ERROR @%s: Failed to GetPlaneSize, it's 0", __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    mLocked = true;

    return NO_ERROR;
}

status_t CameraBuffer::lock()
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);
    status_t status;
    int lockMode;

    if (!mInit) {
        LOGE("@%s: Error: Cannot lock now this buffer, not initialized", __FUNCTION__);
        return INVALID_OPERATION;
    }

    if (mType != BUF_TYPE_HANDLE) {
         mLocked = true;
         return NO_ERROR;
    }

    if (mLocked) {
        if (mOwner) {
            LOGE("@%s: Error: stream(%d) already locked", __FUNCTION__,mOwner->seqNo());
        }
        return INVALID_OPERATION;
    }

    lockMode = GRALLOC_USAGE_SW_READ_MASK | GRALLOC_USAGE_SW_WRITE_MASK |
        GRALLOC_USAGE_HW_CAMERA_MASK;
    if (!lockMode) {
        LOGW("@%s:trying to lock a buffer with no flags", __FUNCTION__);
        return INVALID_OPERATION;
    }

    status = lock(lockMode);
    if (status != NO_ERROR) {
        mUserBuffer.status = CAMERA3_BUFFER_STATUS_ERROR;
    }

    return status;
}

status_t CameraBuffer::unlock()
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);
    if (mLocked && mType != BUF_TYPE_HANDLE) {
         mLocked = false;
         return NO_ERROR;
    }

    if (mLocked) {
        LOG2("@%s, mHandle:%p, mFormat:%d", __FUNCTION__, mHandle, mFormat);
        int ret = mGbmBufferManager->Unlock(mHandle);
        if (ret) {
            LOGE("@%s: call Unlock fail, mHandle:%p, ret:%d", __FUNCTION__, mHandle, ret);
            return UNKNOWN_ERROR;
        }

        mLocked = false;
        return NO_ERROR;
    }
    LOGW("@%s:trying to unlock a buffer that is not locked", __FUNCTION__);
    return INVALID_OPERATION;
}

void CameraBuffer::dump()
{
    if (mInit) {
        LOG1("Buffer dump: handle %p: locked :%d: dataPtr:%p",
            (void*)&mHandle, mLocked, mDataPtr);
    } else {
        LOG1("Buffer dump: Buffer not initialized");
    }
}

void CameraBuffer::dumpImage(const int type, const char *name)
{
    if (CC_UNLIKELY(LogHelper::isDumpTypeEnable(type))) {
        dumpImage(name);
    }
}

void CameraBuffer::dumpImage(const char *name)
{
#ifdef DUMP_IMAGE
    if (!isLocked()) {
        status_t status = lock();
        CheckError(status != NO_ERROR, VOID_VALUE, "@%s, lock fails", __FUNCTION__);
    }

    dumpToFile(mDataPtr, mSize, mWidth, mHeight, mRequestID, name);

    unlock();
#endif
}

/**
 * Utility methods to allocate CameraBuffers from HEAP or Gfx memory
 */
namespace MemoryUtils {

/**
 * Allocates the memory needed to store the image described by the parameters
 * passed during construction
 */
std::shared_ptr<CameraBuffer>
allocateHeapBuffer(int w,
                   int h,
                   int s,
                   int v4l2Fmt,
                   int cameraId,
                   int dataSizeOverride)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);
    void *dataPtr;

    int dataSize = dataSizeOverride ? dataSizeOverride : frameSize(v4l2Fmt, s, h);
    LOG1("@%s, dataSize:%d", __FUNCTION__, dataSize);

    int ret = posix_memalign(&dataPtr, sysconf(_SC_PAGESIZE), dataSize);
    if (dataPtr == nullptr || ret != 0) {
        LOGE("Could not allocate heap camera buffer of size %d", dataSize);
        return nullptr;
    }

    return std::shared_ptr<CameraBuffer>(new CameraBuffer(w, h, s, v4l2Fmt, dataPtr, cameraId, dataSizeOverride));
}

/**
 * Allocates internal GBM buffer
 */
std::shared_ptr<CameraBuffer>
allocateHandleBuffer(int w,
                     int h,
                     int gfxFmt,
                     int usage,
                     int cameraId)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1, LOG_TAG);
    cros::CameraBufferManager* bufManager = cros::CameraBufferManager::GetInstance();
    buffer_handle_t handle;
    uint32_t stride = 0;

    LOG1("%s, [wxh] = [%dx%d], format 0x%x, usage 0x%x",
          __FUNCTION__, w, h, gfxFmt, usage);
    int ret = bufManager->Allocate(w, h, gfxFmt, usage, cros::GRALLOC, &handle, &stride);
    if (ret != 0) {
        LOGE("Allocate handle failed! %d", ret);
        return nullptr;
    }

    std::shared_ptr<CameraBuffer> buffer(new CameraBuffer());
    ret = buffer->init(w, h, gfxFmt, handle, cameraId);
    CheckError(ret != NO_ERROR, nullptr, "Buffer initialization failed");

    return buffer;
}

} // namespace MemoryUtils

} NAMESPACE_DECLARATION_END
