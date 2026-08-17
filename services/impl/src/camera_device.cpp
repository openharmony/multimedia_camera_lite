/*
 * Copyright (c) 2020 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "camera_device.h"

#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/io.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <thread>
#include <unistd.h>
#include "codec_interface.h"
#include "display_layer.h"
#include "media_log.h"
#include "meta_data.h"
#include "securec.h"

#include <iostream>
#include <string>
#include <thread>

using namespace OHOS;
using namespace OHOS::Media;
using namespace std;

/** Indicates that the current frame is an Instantaneous Decoder Refresh (IDR) frame. */
const int32_t KEY_IS_SYNC_FRAME = 1;
/** Indicates the frame timestamp. */
const int32_t KEY_TIME_US = 2;

const int32_t IMAGE_WIDTH = 3;       // "DATA_PIX_FORMAT"
const int32_t IMAGE_HEIGHT = 4;       // "DATA_PIX_FORMAT"
const int32_t IMAGE_SIZE = 5;       // "DATA_PIX_FORMAT"
const int32_t DELAY_TIME_ONE_FRAME = 30000;
const int32_t VIDEO_MAX_NUM = 2;       // "video max num"
const int32_t INVALID_STREAM_ID = -1;

const std::string CAMERA_ID_PREFIX = "Camera_";

namespace OHOS {
namespace Media {
extern Surface *g_surface;

struct SizeMap {
    PicSize res;
    int32_t width;
    int32_t height;
};

static SizeMap g_sizeMap[] = {
    {RESOLUTION_CIF, 352, 288},         {RESOLUTION_360P, 640, 360},        {RESOLUTION_D1_PAL, 720, 576},
    {RESOLUTION_D1_NTSC, 720, 480},     {RESOLUTION_720P, 1280, 720},       {RESOLUTION_1080P, 1920, 1080},
    {RESOLUTION_2560X1440, 2560, 1440}, {RESOLUTION_2592X1520, 2592, 1520}, {RESOLUTION_2592X1536, 2592, 1536},
    {RESOLUTION_2592X1944, 2592, 1944}, {RESOLUTION_2688X1536, 2688, 1536}, {RESOLUTION_2716X1524, 2716, 1524},
    {RESOLUTION_3840X2160, 3840, 2160}, {RESOLUTION_4096X2160, 4096, 2160}, {RESOLUTION_3000X3000, 3000, 3000},
    {RESOLUTION_4000X3000, 4000, 3000}, {RESOLUTION_7680X4320, 7680, 4320}, {RESOLUTION_3840X8640, 3840, 8640}
};

inline PicSize Convert2CodecSize(int32_t width, int32_t height)
{
    for (uint32_t i = 0; i < sizeof(g_sizeMap) / sizeof(SizeMap); i++) {
        if (g_sizeMap[i].width == width && g_sizeMap[i].height == height) {
            return g_sizeMap[i].res;
        }
    }
    return RESOLUTION_INVALID;
}

AvCodecMime ConverFormat(ImageFormat format)
{
    if (format == FORMAT_JPEG) {
        return MEDIA_MIMETYPE_IMAGE_JPEG;
    } else if (format == FORMAT_AVC) {
        return MEDIA_MIMETYPE_VIDEO_AVC;
    } else if (format == FORMAT_HEVC) {
        return MEDIA_MIMETYPE_VIDEO_HEVC;
    } else {
        return MEDIA_MIMETYPE_INVALID;
    }
}

static int32_t SetVencSource(CODEC_HANDLETYPE codecHdl, uint32_t deviceId)
{
    Param param = {.key = KEY_DEVICE_ID, .val = (void *)&deviceId, .size = sizeof(uint32_t)};
    int32_t ret = CodecSetParameter(codecHdl, &param, 1);
    if (ret != 0) {
        MEDIA_ERR_LOG("Set enc source failed.(ret=%d)", ret);
        return ret;
    }
    return MEDIA_OK;
}

#ifdef MEDIA_INTERFACE_V1_0
static int32_t SetVencDeBreatheEffect(FrameConfig &fc, CODEC_HANDLETYPE codecHdl)
{
    int32_t deBreatheEffect = -1;
    fc.GetParameter(PARAM_KEY_DEBREATHE_EFFECT, deBreatheEffect);
    MEDIA_DEBUG_LOG("deBreatheEffect = %d\n", deBreatheEffect);
    if (deBreatheEffect == 1) {
        Param deBreatheParam = {
            .key = KEY_DEBREATHE_EFFECT,
            .val = &deBreatheEffect,
            .size = sizeof(deBreatheEffect)
        };
        int32_t ret = CodecSetParameter(codecHdl, &deBreatheParam, 1);
        if (ret != 0) {
            MEDIA_ERR_LOG("set DeBreatheEffect is faild, (ret=%d)", ret);
            return ret;
        }
    }
    return MEDIA_OK;
}
#endif

static uint32_t GetDefaultBitrate(PicSize size)
{
    const uint32_t KBPS_2048 = 2048;
    const uint32_t KBPS_1024 = 1024;
    const uint32_t KBPS_6144 = 6144;
    const uint32_t KBPS_40960 = 40960;
    uint32_t rate; /* auto calc bitrate if set 0 */
    if (size == RESOLUTION_360P) {
        rate = KBPS_2048; /* kbps */
    } else if (size == RESOLUTION_720P) {
        rate = KBPS_1024; /* kbps */
    } else if (size >= RESOLUTION_2560X1440 && size <= RESOLUTION_2716X1524) {
        rate = KBPS_6144; /* kbps */
    } else if (size == RESOLUTION_3840X2160 || size == RESOLUTION_4096X2160) {
        rate = KBPS_40960; /* kbps */
    } else {
        rate = 0;
    }
    return rate;
}

static int32_t CameraCreateVideoEnc(FrameConfig &fc,
                                    StreamAttr stream,
                                    uint32_t srcDev,
                                    CODEC_HANDLETYPE *codecHdl)
{
    const uint32_t maxParamNum = 10;
    uint32_t paramIndex = 0;
    Param param[maxParamNum];

    CodecType domainKind = VIDEO_ENCODER;
    param[paramIndex].key = KEY_CODEC_TYPE;
    param[paramIndex].val = &domainKind;
    param[paramIndex].size = sizeof(CodecType);
    paramIndex++;

    AvCodecMime codecMime = ConverFormat(stream.format);
    param[paramIndex].key = KEY_MIMETYPE;
    param[paramIndex].val = &codecMime;
    param[paramIndex].size = sizeof(AvCodecMime);
    paramIndex++;

#ifdef MEDIA_INTERFACE_V1_0
    VenCodeRcMode rcMode = VENCOD_RC_CBR;
    VenCodeGopMode gopMode = VENCOD_GOPMODE_NORMALP;
#else
    VideoCodecRcMode rcMode = VID_CODEC_RC_CBR;
    VideoCodecGopMode gopMode = VID_CODEC_GOPMODE_NORMALP;
#endif
    param[paramIndex].key = KEY_VIDEO_RC_MODE;
    param[paramIndex].val = &rcMode;
#ifdef MEDIA_INTERFACE_V1_0
    param[paramIndex].size = sizeof(rcMode);
#else
    param[paramIndex].size = sizeof(VideoCodecRcMode);
#endif
    paramIndex++;

    param[paramIndex].key = KEY_VIDEO_GOP_MODE;
    param[paramIndex].val = &gopMode;
#ifdef MEDIA_INTERFACE_V1_0
    param[paramIndex].size = sizeof(gopMode);
#else
    param[paramIndex].size = sizeof(VideoCodecGopMode);
#endif
    paramIndex++;

    Profile profile = HEVC_MAIN_PROFILE;
    param[paramIndex].key = KEY_VIDEO_PROFILE;
    param[paramIndex].val = &profile;
    param[paramIndex].size = sizeof(Profile);
    paramIndex++;

    PicSize picSize;
#ifdef MEDIA_INTERFACE_V1_0
    picSize = Convert2CodecSize(stream.width, stream.height);

    MEDIA_DEBUG_LOG("picSize=%d", picSize);
    param[paramIndex].key = KEY_VIDEO_PIC_SIZE;
    param[paramIndex].val = &picSize;
    param[paramIndex].size = sizeof(PicSize);
    paramIndex++;
#else
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    uint32_t width = stream.width;
    uint32_t height = stream.height;
#else
    uint32_t width = g_surface->GetWidth();
    uint32_t height = g_surface->GetHeight();
#endif

    MEDIA_DEBUG_LOG("width=%d", width);
    param[paramIndex].key = KEY_VIDEO_WIDTH;
    param[paramIndex].val = &width;
    param[paramIndex].size = sizeof(uint32_t);
    paramIndex++;

    MEDIA_DEBUG_LOG("height=%d", height);
    param[paramIndex].key = KEY_VIDEO_HEIGHT;
    param[paramIndex].val = &height;
    param[paramIndex].size = sizeof(uint32_t);
    paramIndex++;

    picSize = Convert2CodecSize(width, height);
#endif

    uint32_t frameRate = stream.fps;
    MEDIA_DEBUG_LOG("frameRate=%u", frameRate);
    param[paramIndex].key = KEY_VIDEO_FRAME_RATE;
    param[paramIndex].val = &frameRate;
    param[paramIndex].size = sizeof(uint32_t);
    paramIndex++;

    uint32_t bitRate = GetDefaultBitrate(picSize);
    MEDIA_DEBUG_LOG("bitRate=%u kbps", bitRate);
    param[paramIndex].key = KEY_BITRATE;
    param[paramIndex].val = &bitRate;
    param[paramIndex].size = sizeof(uint32_t);
    paramIndex++;

#ifdef MEDIA_INTERFACE_V1_0
    const char *name = "codec.video.hardware.encoder";
    int32_t ret = CodecCreate(name, param, paramIndex, codecHdl);
    if (ret != 0) {
        MEDIA_ERR_LOG("Create video encoder failed.");
        return MEDIA_ERR;
    }
#else
    int32_t ret = CodecCreateByType(domainKind, codecMime, codecHdl);
    if (ret != 0) {
        MEDIA_ERR_LOG("Create video encoder failed.");
        return MEDIA_ERR;
    }

    ret = CodecSetParameter(*codecHdl, param, paramIndex);
    if (ret != 0) {
        CodecDestroy(*codecHdl);
        MEDIA_ERR_LOG("video CodecSetParameter failed.");
        return MEDIA_ERR;
    }
#endif
    ret = SetVencSource(*codecHdl, srcDev);
    if (ret != 0) {
        CodecDestroy(*codecHdl);
        return MEDIA_ERR;
    }
#ifdef MEDIA_INTERFACE_V1_0
    ret = SetVencDeBreatheEffect(fc, *codecHdl);
    if (ret != 0) {
        CodecDestroy(*codecHdl);
        return MEDIA_ERR;
    }
#endif

    return MEDIA_OK;
}

static void SetPicEncHevcParams(Param param[], uint32_t &paramIndex, StreamAttr stream)
{
#ifdef MEDIA_INTERFACE_V1_0
    VenCodeRcMode rcMode = VENCOD_RC_FIXQP;
#else
    VideoCodecRcMode rcMode = VID_CODEC_RC_FIXQP;
#endif
    param[paramIndex].key = KEY_VIDEO_RC_MODE;
    param[paramIndex].val = &rcMode;
    param[paramIndex].size = sizeof(rcMode);
    MEDIA_DEBUG_LOG("CameraCreatePicEnc val:%p, mode:%d", param[paramIndex].val,
        *((int *)param[paramIndex].val));
    paramIndex++;

    Profile profile = HEVC_MAIN_PROFILE;
    param[paramIndex].key = KEY_VIDEO_PROFILE;
    param[paramIndex].val = &profile;
    param[paramIndex].size = sizeof(Profile);
    MEDIA_DEBUG_LOG("CameraCreatePicEnc val:%p, profile:0x%x", param[paramIndex].val,
        *((Profile *)param[paramIndex].val));
    paramIndex++;

    uint32_t frameRate = stream.fps;
    MEDIA_DEBUG_LOG("frameRate=%u", frameRate);
    param[paramIndex].key = KEY_VIDEO_FRAME_RATE;
    param[paramIndex].val = &frameRate;
    param[paramIndex].size = sizeof(uint32_t);
    paramIndex++;
}

static void InitPicEncParam(FrameConfig &fc, StreamAttr stream, Param param[], uint32_t &paramIndex)
{
    CodecType domainKind = VIDEO_ENCODER;
    param[paramIndex].key = KEY_CODEC_TYPE;
    param[paramIndex].val = &domainKind;
    param[paramIndex].size = sizeof(CodecType);
    paramIndex++;

    AvCodecMime codecMime = ConverFormat(stream.format);
    param[paramIndex].key = KEY_MIMETYPE;
    param[paramIndex].val = &codecMime;
    param[paramIndex].size = sizeof(AvCodecMime);
    paramIndex++;

    auto surfaceList = fc.GetSurfaces();
    Surface *surface = surfaceList.front();

#ifdef MEDIA_INTERFACE_V1_0
    PicSize picSize = Convert2CodecSize(surface->GetWidth(), surface->GetHeight());
    param[paramIndex].key = KEY_VIDEO_PIC_SIZE;
    param[paramIndex].val = &picSize;
    param[paramIndex].size = sizeof(PicSize);
    paramIndex++;
#else
    uint32_t width = surface->GetWidth();
    MEDIA_DEBUG_LOG("width=%d", width);
    param[paramIndex].key = KEY_VIDEO_WIDTH;
    param[paramIndex].val = &width;
    param[paramIndex].size = sizeof(uint32_t);
    paramIndex++;

    uint32_t height = surface->GetHeight();
    MEDIA_DEBUG_LOG("height=%d", height);
    param[paramIndex].key = KEY_VIDEO_HEIGHT;
    param[paramIndex].val = &height;
    param[paramIndex].size = sizeof(uint32_t);
    paramIndex++;
#endif

    if (codecMime == MEDIA_MIMETYPE_VIDEO_HEVC) {
        MEDIA_DEBUG_LOG("CameraCreatePicEnc set fixQp");
        SetPicEncHevcParams(param, paramIndex, stream);
    }
}

static int32_t SetPicEncQFactor(FrameConfig &fc, CODEC_HANDLETYPE codecHdl)
{
    int32_t qfactor = -1;
    fc.GetParameter(PARAM_KEY_IMAGE_ENCODE_QFACTOR, qfactor);
    if (qfactor != -1) {
        Param jpegParam = {
            .key = KEY_IMAGE_Q_FACTOR,
            .val = &qfactor,
            .size = sizeof(qfactor)
        };
        int32_t ret = CodecSetParameter(codecHdl, &jpegParam, 1);
        if (ret != 0) {
            MEDIA_ERR_LOG("CodecSetParameter set jpeg qfactor failed.(ret=%u)", ret);
        }
    }
    return MEDIA_OK;
}

static int32_t CameraCreatePicEnc(FrameConfig &fc, StreamAttr stream, uint32_t srcDev, CODEC_HANDLETYPE *codecHdl)
{
    const uint32_t maxParamNum = 10;
    Param param[maxParamNum];
    uint32_t paramIndex = 0;

    InitPicEncParam(fc, stream, param, paramIndex);

#ifdef MEDIA_INTERFACE_V1_0
    const char *videoEncName = "codec.jpeg.hardware.encoder";
    int32_t ret = CodecCreate(videoEncName, param, paramIndex, codecHdl);
    if (ret != 0) {
        MEDIA_ERR_LOG("CodecCreate failed ret=%d", ret);
        return MEDIA_ERR;
    }
#else
    AvCodecMime codecMime = ConverFormat(stream.format);
    CODEC_HANDLETYPE handle = nullptr;
    if (CodecCreateByType(VIDEO_ENCODER, codecMime, &handle) != 0) {
        MEDIA_ERR_LOG("Create video encoder failed.");
        return MEDIA_ERR;
    }

    int32_t ret = CodecSetParameter(handle, param, paramIndex);
    if (ret != 0) {
        CodecDestroy(handle);
        MEDIA_ERR_LOG("video CodecSetParameter failed.");
        return MEDIA_ERR;
    }
    *codecHdl = handle;
#endif
    SetPicEncQFactor(fc, *codecHdl);

    ret = SetVencSource(*codecHdl, srcDev);
    if (ret != 0) {
        MEDIA_ERR_LOG("Set video encoder source failed.");
        CodecDestroy(*codecHdl);
        return MEDIA_ERR;
    }
#ifdef MEDIA_INTERFACE_V1_0
    ret = SetVencDeBreatheEffect(fc, *codecHdl);
    if (ret != 0) {
        CodecDestroy(*codecHdl);
        return MEDIA_ERR;
    }
#endif

    return MEDIA_OK;
}

#ifdef MEDIA_INTERFACE_V1_0
static int32_t CopyCodecOutput(void *dst, uint32_t *size, OutputInfo *buffer)
{
    char *dstBuf = reinterpret_cast<char *>(dst);
    for (uint32_t i = 0; i < buffer->bufferCnt; i++) {
        uint32_t packSize = buffer->buffers[i].length - buffer->buffers[i].offset;
        if (buffer->buffers[i].addr == nullptr) {
            return MEDIA_ERR;
        }
        errno_t ret = memcpy_s(dstBuf, *size, buffer->buffers[i].addr + buffer->buffers[i].offset, packSize);
        if (ret != EOK) {
            return MEDIA_ERR;
        }
        *size -= packSize;
        dstBuf += packSize;
    }
    return MEDIA_OK;
}
#else
static int32_t CopyCodecOutput(uint8_t *dst, uint32_t *size, CodecBuffer *buffer)
{
    if (dst == nullptr || size == nullptr || buffer == nullptr) {
        return MEDIA_ERR;
    }
    char *dstBuf = reinterpret_cast<char *>(dst);
    for (uint32_t i = 0; i < buffer->bufferCnt; i++) {
        uint32_t packSize = buffer->buffer[i].length - buffer->buffer[i].offset;
        errno_t ret = memcpy_s(dstBuf, *size, (void *)(buffer->buffer[i].buf + buffer->buffer[i].offset), packSize);
        if (ret != EOK) {
            return MEDIA_ERR;
        }
        *size -= packSize;
        dstBuf += packSize;
    }
    return MEDIA_OK;
}
#endif

static void StreamAttrInitialize(StreamAttr *streamAttr, Surface *surface,
                                 StreamType streamType, FrameConfig &fc)
{
    if (streamAttr == nullptr || surface == nullptr) {
        MEDIA_ERR_LOG("streamAttr surface is illegal.");
        return;
    }
    memset_s(streamAttr, sizeof(StreamAttr), 0, sizeof(StreamAttr));
    streamAttr->type = streamType;
    fc.GetParameter(CAM_IMAGE_FORMAT, streamAttr->format);
#ifdef MEDIA_INTERFACE_V1_0
    streamAttr->displayWidth = surface->GetWidth();
    streamAttr->displayHeight = surface->GetHeight();
#endif
    fc.GetParameter(CAM_IMAGE_WIDTH, streamAttr->width);
    fc.GetParameter(CAM_IMAGE_HEIGHT, streamAttr->height);
    if (streamAttr->width == 0 || streamAttr->height == 0) {
        streamAttr->width = surface->GetWidth();
        streamAttr->height = surface->GetHeight();
    }
    fc.GetParameter(CAM_FRAME_FPS, streamAttr->fps);
    fc.GetParameter(CAM_IMAGE_INVERT_MODE, streamAttr->invertMode);
    fc.GetParameter(CAM_IMAGE_CROP_RECT, streamAttr->crop);
}

static ImageFormat Convert2HalImageFormat(uint32_t format)
{
    if (format == CAM_IMAGE_RAW12) {
        return FORMAT_RGB_BAYER_12BPP;
    }
    return FORMAT_YVU420;
}

static void CancelSurfaceBuf(Surface *surface, Surface *gSurf, SurfaceBuffer *surfaceBuf)
{
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    (void)gSurf;
    surface->CancelBuffer(surfaceBuf);
#else
    (void)surface;
    gSurf->CancelBuffer(surfaceBuf);
#endif
}

static int32_t FlushSurfaceBuf(Surface *surface, Surface *gSurf, SurfaceBuffer *surfaceBuf, uint32_t size)
{
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    surfaceBuf->SetSize(surface->GetSize() - size);
    if (surface->FlushBuffer(surfaceBuf) != 0) {
        MEDIA_ERR_LOG("Flush surface failed.");
        surface->CancelBuffer(surfaceBuf);
        return MEDIA_ERR;
    }
#else
    surfaceBuf->SetSize(gSurf->GetSize() - size);
    if (gSurf->FlushBuffer(surfaceBuf) != 0) {
        MEDIA_ERR_LOG("Flush surface failed.");
        gSurf->CancelBuffer(surfaceBuf);
        return MEDIA_ERR;
    }
#endif
    return MEDIA_OK;
}

#ifdef MEDIA_INTERFACE_V1_0
static int32_t ProcessSurfaceBuffer(Surface *surface, Surface *gSurf, OutputInfo *buffer)
{
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    SurfaceBuffer *surfaceBuf = surface->RequestBuffer();
#else
    SurfaceBuffer *surfaceBuf = gSurf->RequestBuffer();
#endif
    if (surfaceBuf == nullptr) {
        MEDIA_ERR_LOG("No available buffer in surface.");
        return MEDIA_ERR;
    }
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    uint32_t size = surface->GetSize();
#else
    uint32_t size = gSurf->GetSize();
#endif
    void *buf = surfaceBuf->GetVirAddr();
    if (buf == nullptr) {
        MEDIA_ERR_LOG("Invalid buffer address.");
        CancelSurfaceBuf(surface, gSurf, surfaceBuf);
        return MEDIA_ERR;
    }
    int32_t ret = CopyCodecOutput(buf, &size, buffer);
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG("No available buffer in surface.");
        CancelSurfaceBuf(surface, gSurf, surfaceBuf);
        return MEDIA_ERR;
    }
    surfaceBuf->SetInt32(KEY_IS_SYNC_FRAME, (((buffer->flag & STREAM_FLAG_KEYFRAME) == 0) ? 0 : 1));
    surfaceBuf->SetInt64(KEY_TIME_US, buffer->timeStamp);
    return FlushSurfaceBuf(surface, gSurf, surfaceBuf, size);
}

int32_t RecordAssistant::OnVencBufferAvailble(UINTPTR hComponent, UINTPTR dataIn, OutputInfo *buffer)
{
    CODEC_HANDLETYPE hdl = reinterpret_cast<CODEC_HANDLETYPE>(hComponent);
    RecordAssistant *assistant = reinterpret_cast<RecordAssistant *>(dataIn);
    if (assistant == nullptr) {
        MEDIA_ERR_LOG("assistant is null.");
        return MEDIA_ERR;
    }
    list<Surface *> *surfaceList = nullptr;
    for (uint32_t idx = 0; idx < assistant->vencHdls_.size(); idx++) {
        if (assistant->vencHdls_[idx] == hdl) {
            surfaceList = &(assistant->vencSurfaces_[idx]);
            break;
        }
    }
    if (surfaceList == nullptr || surfaceList->empty()) {
        MEDIA_ERR_LOG("Encoder handle is illegal.");
        return MEDIA_ERR;
    }
    int32_t ret = -1;
    for (auto &surface : *surfaceList) {
        ret = ProcessSurfaceBuffer(surface, g_surface, buffer);
        if (ret != MEDIA_OK) {
            break;
        }
    }
    if (CodecQueueOutput(hdl, buffer, 0, -1) != 0) {
        MEDIA_ERR_LOG("Codec queue output failed.");
    }
    return ret;
}
#else
static int32_t SurfaceSetSize(SurfaceBuffer* surfaceBuf, Surface* surface, uint32_t size)
{
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    surfaceBuf->SetSize(surface->GetSize() - size);
    if (surface->FlushBuffer(surfaceBuf) != 0) {
        MEDIA_ERR_LOG("Flush g_surface failed.");
        surface->CancelBuffer(surfaceBuf);
        return -1;
    }
#else
    surfaceBuf->SetSize(g_surface->GetSize() - size);
    if (g_surface->FlushBuffer(surfaceBuf) != 0) {
        MEDIA_ERR_LOG("Flush surface failed.");
        g_surface->CancelBuffer(surfaceBuf);
        return -1;
    }
#endif
    return 0;
}

int32_t RecordAssistant::OnVencBufferAvailble(UINTPTR userData, CodecBuffer* outBuf, int32_t *acquireFd)
{
    (void)acquireFd;
    CodecDesc* codecInfo = reinterpret_cast<CodecDesc* >(userData);
    list<Surface*> *surfaceList = &codecInfo->vencSurfaces_;
    if (surfaceList == nullptr || surfaceList->empty()) {
        MEDIA_ERR_LOG("Encoder handle is illegal.");
        return MEDIA_ERR;
    }
    int32_t ret = -1;
    for (auto &surface : *surfaceList) {
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
        SurfaceBuffer *surfaceBuf = surface->RequestBuffer();
#else
        SurfaceBuffer *surfaceBuf = g_surface->RequestBuffer();
#endif
        if (surfaceBuf == nullptr) {
            MEDIA_ERR_LOG("No available buffer in surface.");
            break;
        }
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
        uint32_t size = surface->GetSize();
#else
        uint32_t size = g_surface->GetSize();
#endif
        void *buf = surfaceBuf->GetVirAddr();
        if (buf == nullptr) {
            MEDIA_ERR_LOG("Invalid buffer address.");
            break;
        }
        ret = CopyCodecOutput((uint8_t*)buf, &size, outBuf);
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("No available outBuf in surface.");
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
            surface->CancelBuffer(surfaceBuf);
#else
            g_surface->CancelBuffer(surfaceBuf);
#endif
            break;
        }
        surfaceBuf->SetInt32(KEY_IS_SYNC_FRAME, (((outBuf->flag & STREAM_FLAG_KEYFRAME) == 0) ? 0 : 1));
        surfaceBuf->SetInt64(KEY_TIME_US, outBuf->timeStamp);
        ret = SurfaceSetSize(surfaceBuf, surface, size);
        if (ret != 0) {
            break;
        }
    }
    if (CodecQueueOutput(codecInfo->vencHdl_, reinterpret_cast<OutputInfo*>(outBuf), 0, -1) != 0) {
        MEDIA_ERR_LOG("Codec queue output failed.");
    }
    return ret;
}
#endif

#ifdef MEDIA_INTERFACE_V1_0
HalCameraManager *DeviceAssistant::halCameraDev_ = nullptr;
#endif

CodecCallback RecordAssistant::recordCodecCb_ = {nullptr, nullptr, RecordAssistant::OnVencBufferAvailble};

void RecordAssistant::ClearFrameConfig()
{
#ifdef MEDIA_INTERFACE_V1_0
    for (uint32_t i = 0; i < vencHdls_.size(); i++) {
        CodecStop(vencHdls_[i]);
        CodecDestroy(vencHdls_[i]);
    }
    vencHdls_.clear();
    vencSurfaces_.clear();
#else
    for (uint32_t i = 0; i < codecInfo_.size(); i++) {
        CodecStop(codecInfo_[i].vencHdl_);
        CodecDestroy(codecInfo_[i].vencHdl_);
    }
    codecInfo_.clear();
#endif
}

static int32_t SetupVideoEncoderCallback(RecordAssistant *recAsst, CODEC_HANDLETYPE codecHdl, Surface *surface)
{
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    list<Surface*> conList({surface});
#else
    list<Surface*> conList({g_surface});
#endif
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = CodecSetCallback(codecHdl, &recAsst->recordCodecCb_, reinterpret_cast<UINTPTR>(recAsst));
    if (ret != 0) {
        MEDIA_ERR_LOG("Set codec callback failed.(ret=%d)", ret);
        CodecDestroy(codecHdl);
        recAsst->ClearFrameConfig();
        return MEDIA_ERR;
    }
    recAsst->vencHdls_.emplace_back(codecHdl);
    recAsst->vencSurfaces_.emplace_back(conList);
#else
    CodecDesc info;
    info.vencHdl_ = codecHdl;
    info.vencSurfaces_ = conList;
    recAsst->codecInfo_.emplace_back(info);
#endif
    return MEDIA_OK;
}

static int32_t AddVideoEncoder(RecordAssistant *recAsst, FrameConfig &fc, Surface *surface,
    uint32_t *streamId, uint32_t num)
{
    CODEC_HANDLETYPE codecHdl = nullptr;
    StreamAttr stream = {};
#if (!defined(__LINUX__)) || (defined(ENABLE_PASSTHROUGH_MODE))
    StreamAttrInitialize(&stream, surface, STREAM_VIDEO, fc);
#else
    StreamAttrInitialize(&stream, g_surface, STREAM_VIDEO, fc);
#endif
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = recAsst->halCameraDev_->HalCameraStreamCreate(recAsst->cameraId_.c_str(), &stream, streamId);
#else
    int32_t ret = HalCameraStreamCreate(recAsst->cameraId_, &stream, streamId);
#endif
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG(" creat recorder stream failed.");
        recAsst->ClearFrameConfig();
        return MEDIA_ERR;
    }
    recAsst->streamId_ = *streamId;
    recAsst->streamIdNum_[num] = *streamId;
    StreamInfo streamInfo;
    streamInfo.type = STERAM_INFO_PRIVATE;
    fc.GetVendorParameter(streamInfo.u.data, PRIVATE_TAG_LEN);
#ifdef MEDIA_INTERFACE_V1_0
    recAsst->halCameraDev_->HalCameraStreamSetInfo(recAsst->cameraId_.c_str(), *streamId, &streamInfo);
#else
    HalCameraStreamSetInfo(recAsst->cameraId_, *streamId, &streamInfo);
#endif

    uint32_t deviceId = 0;
#ifdef MEDIA_INTERFACE_V1_0
    recAsst->halCameraDev_->HalCameraGetDeviceId(recAsst->cameraId_.c_str(), *streamId, &deviceId);
#else
    HalCameraGetDeviceId(recAsst->cameraId_, *streamId, &deviceId);
#endif
    ret = CameraCreateVideoEnc(fc, stream, deviceId, &codecHdl);
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG("Cannot create suitble video encoder.");
        recAsst->ClearFrameConfig();
        return MEDIA_ERR;
    }

    return SetupVideoEncoderCallback(recAsst, codecHdl, surface);
}

#ifndef MEDIA_INTERFACE_V1_0
int32_t RecordAssistant::SetFrameConfigEnd(int32_t result)
{
    if (result != MEDIA_OK) {
        for (uint32_t i = 0; i < codecInfo_.size(); i++) {
            CodecDestroy(codecInfo_[i].vencHdl_);
        }
        codecInfo_.clear();
        return result;
    }
    for (uint32_t i = 0; i < codecInfo_.size(); i++) {
        result = CodecSetCallback(codecInfo_[i].vencHdl_, &recordCodecCb_,
            reinterpret_cast<UINTPTR>(&codecInfo_[i]));
        if (result != 0) {
            MEDIA_ERR_LOG("set CodecSetCallback failed ret:%d", result);
            CodecDestroy(codecInfo_[i].vencHdl_);
            break;
        }
    }

    if (result == MEDIA_OK) {
        state_ = LOOP_READY;
    } else {
        for (uint32_t i = 0; i < codecInfo_.size(); i++) {
            CodecDestroy(codecInfo_[i].vencHdl_);
        }
        codecInfo_.clear();
    }
    return result;
}
#endif

int32_t RecordAssistant::SetFrameConfig(FrameConfig &fc, uint32_t *streamId)
{
    fc_ = &fc;
    auto surfaceList = fc.GetSurfaces();
    if (surfaceList.size() > VIDEO_MAX_NUM || surfaceList.size() == 0) {
        MEDIA_ERR_LOG("the number of surface in frame config must 1 or 2 now.\n");
        return MEDIA_ERR;
    }
    uint32_t num = 0;
    int32_t ret = MEDIA_OK;
    for (auto &surface : surfaceList) {
        ret = AddVideoEncoder(this, fc, surface, streamId, num);
        if (ret != MEDIA_OK) {
#ifdef MEDIA_INTERFACE_V1_0
            return MEDIA_ERR;
#else
            break;
#endif
        }
        num++;
    }
#ifdef MEDIA_INTERFACE_V1_0
    state_ = LOOP_READY;
    return MEDIA_OK;
#else
    return SetFrameConfigEnd(ret);
#endif
}

int32_t RecordAssistant::Start(uint32_t streamId)
{
    if (state_ != LOOP_READY) {
        return MEDIA_ERR;
    }
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamOn(cameraId_.c_str(), streamId);
#else
    HalCameraStreamOn(cameraId_, streamId);
#endif
    int32_t ret = MEDIA_OK;
    int32_t i;
#ifdef MEDIA_INTERFACE_V1_0
    for (i = 0; static_cast<uint32_t>(i) < vencHdls_.size(); i++) {
        ret = CodecStart(vencHdls_[i]);
#else
    for (i = 0; static_cast<uint32_t>(i) < codecInfo_.size(); i++) {
        ret = CodecStart(codecInfo_[i].vencHdl_);
#endif
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("Video encoder start failed.");
            ret = MEDIA_ERR;
            break;
        }
    }
    if (ret == MEDIA_ERR) {
        /* rollback */
        for (; i >= 0; i--) {
#ifdef MEDIA_INTERFACE_V1_0
            CodecStop(vencHdls_[i]);
#else
            CodecStop(codecInfo_[i].vencHdl_);
#endif
        }
        return MEDIA_ERR;
    }
    state_ = LOOP_LOOPING;
    MEDIA_INFO_LOG("Start camera recording succeed.");
    return MEDIA_OK;
}

int32_t RecordAssistant::Stop()
{
    if (state_ != LOOP_LOOPING) {
        return MEDIA_ERR;
    }
    ClearFrameConfig();
#ifdef MEDIA_INTERFACE_V1_0
    for (uint32_t i = 0; i < MAX_STREAM_NUM; i++) {
#else
    for (uint32_t i = 0; i < RECORDER_MAX_NUM; i++) {
#endif
        if (streamIdNum_[i] != INVALID_STREAM_ID) {
#ifdef MEDIA_INTERFACE_V1_0
            halCameraDev_->HalCameraStreamOff(cameraId_.c_str(), streamIdNum_[i]);
            halCameraDev_->HalCameraStreamDestroy(cameraId_.c_str(), streamIdNum_[i]);
#else
            HalCameraStreamOff(cameraId_, streamIdNum_[i]);
            HalCameraStreamDestroy(cameraId_, streamIdNum_[i]);
#endif
        }
        streamIdNum_[i] = INVALID_STREAM_ID;
    }
    state_ = LOOP_STOP;
    return MEDIA_OK;
}

static void GetSurfaceRect(Surface *surface, IRect *attr)
{
    attr->x = std::stoi(surface->GetUserData(string("region_position_x")));
    attr->y = std::stoi(surface->GetUserData(string("region_position_y")));
    attr->w = std::stoi(surface->GetUserData(string("region_width")));
    attr->h = std::stoi(surface->GetUserData(string("region_height")));
}

uint32_t PreviewAssistant::GetFreeStreamIndex()
{
    for (uint32_t i = 0; i < MAX_STREAM_NUM; i++) {
        if (streamIdNum_[i] == INVALID_STREAM_ID) {
            return i;
        }
    }
    return MAX_STREAM_NUM;
}

int32_t PreviewAssistant::SetFrameConfig(FrameConfig &fc, uint32_t *streamId)
{
    uint32_t streamIndex = GetFreeStreamIndex();
    if (streamIndex >= MAX_STREAM_NUM) {
        MEDIA_ERR_LOG("Only support stream :%d.", MAX_STREAM_NUM);
        return MEDIA_ERR;
    }
    fc_ = &fc;
    auto surfaceList = fc.GetSurfaces();
    if (surfaceList.size() != 1) {
        MEDIA_ERR_LOG("Only support one surface in frame config now.");
        return MEDIA_ERR;
    }
    Surface *surface = surfaceList.front();
    StreamAttr stream = {};
    StreamAttrInitialize(&stream, surface, STREAM_PREVIEW, fc);
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = halCameraDev_->HalCameraStreamCreate(cameraId_.c_str(), &stream, streamId);
#else
    int32_t ret = HalCameraStreamCreate(cameraId_, &stream, streamId);
#endif
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG(" creat preview stream failed.");
        return MEDIA_ERR;
    }
    StreamInfo streamInfo;
    streamInfo.type = STREAM_INFO_POS;
    streamInfo.u.pos.x = std::stoi(surface->GetUserData(string("region_position_x")));
    streamInfo.u.pos.y = std::stoi(surface->GetUserData(string("region_position_y")));
#ifdef MEDIA_INTERFACE_V1_0
    streamInfo.u.pos.w = std::stoi(surface->GetUserData(string("region_width")));
    streamInfo.u.pos.h = std::stoi(surface->GetUserData(string("region_height")));
#endif

#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamSetInfo(cameraId_.c_str(), *streamId, &streamInfo);
#else
    HalCameraStreamSetInfo(cameraId_, *streamId, &streamInfo);
#endif
    streamId_ = *streamId;
    streamIdNum_[streamIndex] = *streamId;
    state_ = LOOP_READY;
    return MEDIA_OK;
}

int32_t PreviewAssistant::Start(uint32_t streamId)
{
    if (state_ != LOOP_READY) {
        return MEDIA_ERR;
    }

#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = halCameraDev_->HalCameraStreamOn(cameraId_.c_str(), streamId);
#else
    int32_t ret = HalCameraStreamOn(cameraId_, streamId);
#endif
    if (ret != 0) {
        MEDIA_ERR_LOG("Preview start failed of HalCameraStreamOn.(ret=%d)", ret);
        Stop();
        return MEDIA_ERR;
    }
    state_ = LOOP_LOOPING;
    return MEDIA_OK;
}

int32_t PreviewAssistant::Stop()
{
    if (state_ != LOOP_LOOPING) {
        return MEDIA_ERR;
    }
    state_ = LOOP_STOP;
    for (uint32_t i = 0; i < MAX_STREAM_NUM; i++) {
        if (streamIdNum_[i] == INVALID_STREAM_ID) {
            continue;
        }
#ifdef MEDIA_INTERFACE_V1_0
        halCameraDev_->HalCameraStreamOff(cameraId_.c_str(), streamIdNum_[i]);
#else
        HalCameraStreamOff(cameraId_, streamIdNum_[i]);
#endif
#ifdef MEDIA_INTERFACE_V1_0
        halCameraDev_->HalCameraStreamDestroy(cameraId_.c_str(), streamIdNum_[i]);
#else
        HalCameraStreamDestroy(cameraId_, streamIdNum_[i]);
#endif
        streamIdNum_[i] = INVALID_STREAM_ID;
    }
    return MEDIA_OK;
}

int32_t CaptureAssistant::SetFrameConfig(FrameConfig &fc, uint32_t *streamId)
{
    auto surfaceList = fc.GetSurfaces();
    if (surfaceList.size() != 1) {
        MEDIA_ERR_LOG("Only support one surface in frame config now.");
        return MEDIA_ERR;
    }
    if (surfaceList.empty()) {
        MEDIA_ERR_LOG("Frame config with empty surface list.");
        return MEDIA_ERR;
    }
    if (surfaceList.size() > 1) {
        MEDIA_WARNING_LOG("Capture only fullfill the first surface in frame config.");
    }
    Surface *surface = surfaceList.front();

    StreamAttr stream = {};
    StreamAttrInitialize(&stream, surface, STREAM_CAPTURE, fc);

    uint32_t deviceId = 0;
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = halCameraDev_->HalCameraStreamCreate(cameraId_.c_str(), &stream, streamId);
#else
    int32_t ret = HalCameraStreamCreate(cameraId_, &stream, streamId);
#endif
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG(" creat capture stream failed.");
        return MEDIA_ERR;
    }
    streamId_ = *streamId;
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraGetDeviceId(cameraId_.c_str(), *streamId, &deviceId);
#else
    HalCameraGetDeviceId(cameraId_, *streamId, &deviceId);
#endif
    ret = CameraCreatePicEnc(fc, stream, deviceId, &vencHdl_);
    if (ret != MEDIA_OK) {
#ifdef MEDIA_INTERFACE_V1_0
        halCameraDev_->HalCameraStreamDestroy(cameraId_.c_str(), *streamId);
#else
        HalCameraStreamDestroy(cameraId_, *streamId);
#endif
        MEDIA_ERR_LOG("Create capture venc failed.");
        return MEDIA_ERR;
    }

    capSurface_ = surface;
    state_ = LOOP_READY;
    return MEDIA_OK;
}

/* Block method, waiting for capture completed */
int32_t CaptureAssistant::Start(uint32_t streamId)
{
    state_ = LOOP_LOOPING;
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamOn(cameraId_.c_str(), streamId);
#else
    HalCameraStreamOn(cameraId_, streamId);
#endif
    int32_t ret = CodecStart(vencHdl_);
    if (ret != 0) {
        MEDIA_ERR_LOG("Start capture encoder failed.(ret=%d)", ret);
        state_ = LOOP_STOP;
        return MEDIA_ERR;
    }
#ifdef MEDIA_INTERFACE_V1_0
    do {
        SurfaceBuffer *surfaceBuf = capSurface_->RequestBuffer();
        if (surfaceBuf == nullptr) {
            MEDIA_ERR_LOG("No available buffer in surface.");
            break;
        }

        OutputInfo outInfo;
        ret = CodecDequeueOutput(vencHdl_, 0, nullptr, &outInfo);
        if (ret != 0) {
            capSurface_->CancelBuffer(surfaceBuf);
            MEDIA_ERR_LOG("Dequeue capture frame failed.(ret=%d)", ret);
            ret = MEDIA_ERR;
            break;
        }

        uint32_t size = capSurface_->GetSize();
        void *buf = surfaceBuf->GetVirAddr();
        if (buf == nullptr) {
            MEDIA_ERR_LOG("Invalid buffer address.");
            ret = MEDIA_ERR;
            break;
        }
        ret = CopyCodecOutput(buf, &size, &outInfo);
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("No available buffer in capSurface_.");
            capSurface_->CancelBuffer(surfaceBuf);
            ret = MEDIA_ERR;
            break;
        }
        surfaceBuf->SetSize(capSurface_->GetSize() - size);

        if (capSurface_->FlushBuffer(surfaceBuf) != 0) {
            MEDIA_ERR_LOG("Flush surface buffer failed.");
            capSurface_->CancelBuffer(surfaceBuf);
            ret = MEDIA_ERR;
            break;
        }

        ret = MEDIA_OK;
        CodecQueueOutput(vencHdl_, &outInfo, 0, -1); // 0:no timeout -1:no fd
    } while (0);
#else
    CodecBuffer* outInfo = (CodecBuffer*)new char[sizeof(CodecBuffer) + sizeof(CodecBufferInfo) * 3]; /* 3 buffCnt */
    if (outInfo == NULL) {
        MEDIA_ERR_LOG("malloc Dequeue buffer failed!");
        return MEDIA_ERR;
    }
    SurfaceBuffer *surfaceBuf = NULL;
    do {
        if (memset_s(outInfo, sizeof(CodecBuffer) + sizeof(CodecBufferInfo) * 0x3, 0,
            sizeof(CodecBuffer) + sizeof(CodecBufferInfo) * 3) != MEDIA_OK) { /* 3 buffCnt */
            MEDIA_ERR_LOG("memset_s failed!");
            delete(outInfo);
            return MEDIA_ERR;
        }
        outInfo->bufferCnt = 3; /* 3 buffCnt */
        ret = CodecDequeueOutput(vencHdl_, 0, nullptr, reinterpret_cast<OutputInfo*>(outInfo));
        if (ret != 0) {
            MEDIA_ERR_LOG("Dequeue capture frame failed.(ret=%d)", ret);
            break;
        }

        surfaceBuf = capSurface_->RequestBuffer();
        if (surfaceBuf == NULL) {
            break;
        }

        uint32_t size = capSurface_->GetSize();
        void *buf = surfaceBuf->GetVirAddr();
        if (buf == nullptr) {
            MEDIA_ERR_LOG("Invalid buffer address.");
            break;
        }
        if (CopyCodecOutput((uint8_t*)buf, &size, outInfo) != MEDIA_OK) {
            MEDIA_ERR_LOG("No available buffer in capSurface_.");
            break;
        }
        surfaceBuf->SetSize(capSurface_->GetSize() - size);
        if (capSurface_->FlushBuffer(surfaceBuf) != 0) {
            MEDIA_ERR_LOG("Flush surface buffer failed.");
            break;
        }
    } while (0);
#endif

    CodecStop(vencHdl_);
    CodecDestroy(vencHdl_);
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamOff(cameraId_.c_str(), streamId);
    halCameraDev_->HalCameraStreamDestroy(cameraId_.c_str(), streamId);
#else
    HalCameraStreamOff(cameraId_, streamId);
    HalCameraStreamDestroy(cameraId_, streamId);
    delete outInfo;
    outInfo = NULL;
#endif
    state_ = LOOP_STOP;

    return ret;
}

int32_t CaptureAssistant::Stop()
{
    MEDIA_DEBUG_LOG("No support method.");
    return MEDIA_OK;
}

int32_t CallbackAssistant::SetFrameConfig(FrameConfig &fc, uint32_t *streamId)
{
    fc_ = &fc;
    auto surfaceList = fc.GetSurfaces();
    if (surfaceList.size() != 1) {
        MEDIA_ERR_LOG("Only support one surface in frame config now.");
        return MEDIA_ERR;
    }
    uint32_t imageFormat = 0;
    fc.GetParameter(CAM_IMAGE_FORMAT, imageFormat);
    ImageFormat halImageFormat = Convert2HalImageFormat(imageFormat);
    MEDIA_INFO_LOG("Imageformat is %d", imageFormat);
    Surface *surface = surfaceList.front();
    StreamAttr stream = {};
    StreamAttrInitialize(&stream, surface, STREAM_CALLBACK, fc);
    stream.format = halImageFormat;
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = halCameraDev_->HalCameraStreamCreate(cameraId_.c_str(), &stream, streamId);
#else
    int32_t ret = HalCameraStreamCreate(cameraId_, &stream, streamId);
#endif
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG(" creat callback stream failed.");
        return MEDIA_ERR;
    }
    streamId_ = *streamId;
    capSurface_ = surface;
    state_ = LOOP_READY;
    return MEDIA_OK;
}

int32_t CallbackAssistant::Start(uint32_t streamId)
{
    if (state_ == LOOP_LOOPING) {
        return MEDIA_ERR;
    }
    state_ = LOOP_LOOPING;
    int32_t retCode = pthread_create(&threadId, nullptr, StreamCopyProcess, this);
    if (retCode != 0) {
        MEDIA_ERR_LOG("fork thread StreamCopyProcess failed: %d.", retCode);
    }
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamOn(cameraId_.c_str(), streamId);
#else
    HalCameraStreamOn(cameraId_, streamId);
#endif
    return MEDIA_OK;
}

static bool ProcessStreamFrame(CallbackAssistant *assistant, HalBuffer &streamBuffer)
{
    SurfaceBuffer *surfaceBuf = assistant->capSurface_->RequestBuffer();
    if (surfaceBuf == nullptr) {
        usleep(DELAY_TIME_ONE_FRAME);
        return true;
    }

    if (streamBuffer.size != 0) {
#ifdef MEDIA_INTERFACE_V1_0
        DeviceAssistant::halCameraDev_->HalCameraQueueBuf(assistant->cameraId_.c_str(),
            assistant->streamId_, &streamBuffer);
#else
        HalCameraQueueBuf(assistant->cameraId_, assistant->streamId_, &streamBuffer);
#endif
        (void)memset_s(&streamBuffer, sizeof(HalBuffer), 0, sizeof(HalBuffer));
    }

    streamBuffer.format = FORMAT_PRIVATE;
    streamBuffer.size = assistant->capSurface_->GetSize();
    if (surfaceBuf->GetVirAddr() == NULL) {
        MEDIA_ERR_LOG("Invalid buffer address.");
        return false;
    }
    streamBuffer.virAddr = surfaceBuf->GetVirAddr();

    int32_t ret;
#ifdef MEDIA_INTERFACE_V1_0
    ret = DeviceAssistant::halCameraDev_->HalCameraDequeueBuf(assistant->cameraId_.c_str(),
        assistant->streamId_, &streamBuffer);
#else
    ret = HalCameraDequeueBuf(assistant->cameraId_, assistant->streamId_, &streamBuffer);
#endif
    if (ret != MEDIA_OK) {
        usleep(DELAY_TIME_ONE_FRAME);
        return true;
    }

    if (assistant->capSurface_->FlushBuffer(surfaceBuf) != 0) {
        MEDIA_ERR_LOG("Flush surface failed.");
        assistant->capSurface_->CancelBuffer(surfaceBuf);
        return false;
    }
    usleep(DELAY_TIME_ONE_FRAME);
    return true;
}

void* CallbackAssistant::StreamCopyProcess(void *arg)
{
    CallbackAssistant *assistant = (CallbackAssistant *)arg;
    if (assistant == nullptr) {
        MEDIA_ERR_LOG("CallbackAssistant create failed.");
        return nullptr;
    }
    if (assistant->capSurface_ == nullptr) {
        MEDIA_ERR_LOG("capSurface_ is null.\n");
        return nullptr;
    }
    HalBuffer streamBuffer;
    (void)memset_s(&streamBuffer, sizeof(HalBuffer), 0, sizeof(HalBuffer));
    while (assistant->state_ == LOOP_LOOPING) {
        if (!ProcessStreamFrame(assistant, streamBuffer)) {
            break;
        }
    }
    if (streamBuffer.size != 0) {
#ifdef MEDIA_INTERFACE_V1_0
        halCameraDev_->HalCameraQueueBuf(assistant->cameraId_.c_str(), assistant->streamId_, &streamBuffer);
#else
        HalCameraQueueBuf(assistant->cameraId_, assistant->streamId_, &streamBuffer);
#endif
    }
    MEDIA_DEBUG_LOG(" yuv thread joined \n");
    return nullptr;
}

int32_t CallbackAssistant::Stop()
{
    if (state_ != LOOP_LOOPING) {
        return MEDIA_ERR;
    }
    state_ = LOOP_STOP;
    pthread_join(threadId, NULL);
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamOff(cameraId_.c_str(), streamId_);
#else
    HalCameraStreamOff(cameraId_, streamId_);
#endif
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamDestroy(cameraId_.c_str(), streamId_);
#else
    HalCameraStreamDestroy(cameraId_, streamId_);
#endif
    return MEDIA_OK;
}

#ifdef MEDIA_INTERFACE_V1_0
int32_t CallbackH264Assistant::OnVencBufferAvailble(UINTPTR hComponent, UINTPTR dataIn, OutputInfo *buffer)
{
    CODEC_HANDLETYPE hdl = reinterpret_cast<CODEC_HANDLETYPE>(hComponent);
    CallbackH264Assistant *assistant = reinterpret_cast<CallbackH264Assistant *>(dataIn);
    list<Surface *> *surfaceList = nullptr;
    for (uint32_t idx = 0; idx < assistant->vencHdls_.size(); idx++) {
        if (assistant->vencHdls_[idx] == hdl) {
            surfaceList = &(assistant->vencSurfaces_[idx]);
            break;
        }
    }
    if (surfaceList == nullptr || surfaceList->empty()) {
        MEDIA_ERR_LOG("Encoder handle is illegal.");
        return MEDIA_ERR;
    }
    int32_t ret = -1;
    for (auto &surface : *surfaceList) {
        SurfaceBuffer *surfaceBuf = surface->RequestBuffer();
        if (surfaceBuf == nullptr) {
            MEDIA_ERR_LOG("No available buffer in surface.");
            break;
        }
        uint32_t size = surface->GetSize();
        void *buf = surfaceBuf->GetVirAddr();
        if (buf == nullptr) {
            MEDIA_ERR_LOG("Invalid buffer address.");
            break;
        }
        ret = CopyCodecOutput(buf, &size, buffer);
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("No available buffer in surface.");
        surface->CancelBuffer(surfaceBuf);
            break;
        }
        surfaceBuf->SetInt32(KEY_IS_SYNC_FRAME, (((buffer->flag & STREAM_FLAG_KEYFRAME) == 0) ? 0 : 1));
        surfaceBuf->SetInt64(KEY_TIME_US, buffer->timeStamp);
        surfaceBuf->SetSize(surface->GetSize() - size);
        if (surface->FlushBuffer(surfaceBuf) != 0) {
            MEDIA_ERR_LOG("Flush surface failed.");
            surface->CancelBuffer(surfaceBuf);
            ret = -1;
            break;
        }
    }
    if (CodecQueueOutput(hdl, buffer, 0, -1) != 0) {
        MEDIA_ERR_LOG("Codec queue output failed.");
    }
    return ret;
}
#else
int32_t CallbackH264Assistant::OnVencBufferAvailble(UINTPTR userData, CodecBuffer* outBuf, int32_t *acquireFd)
{
    (void)acquireFd;
    CodecDesc* codecInfo = reinterpret_cast<CodecDesc* >(userData);
    list<Surface*> *surfaceList = &codecInfo->vencSurfaces_;
    if (surfaceList == nullptr || surfaceList->empty()) {
        MEDIA_ERR_LOG("Encoder handle is illegal.");
        return MEDIA_ERR;
    }
    int32_t ret = -1;
    for (auto &surface : *surfaceList) {
        SurfaceBuffer *surfaceBuf = surface->RequestBuffer();
        if (surfaceBuf == nullptr) {
            MEDIA_ERR_LOG("No available buffer in surface.");
            break;
        }
        uint32_t size = surface->GetSize();
        void *buf = surfaceBuf->GetVirAddr();
        if (buf == nullptr) {
            MEDIA_ERR_LOG("Invalid buffer address.");
            break;
        }
        ret = CopyCodecOutput((uint8_t*)buf, &size, outBuf);
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("No available outBuf in surface.");
            surface->CancelBuffer(surfaceBuf);
            break;
        }
        surfaceBuf->SetInt32(KEY_IS_SYNC_FRAME, (((outBuf->flag & STREAM_FLAG_KEYFRAME) == 0) ? 0 : 1));
        surfaceBuf->SetInt64(KEY_TIME_US, outBuf->timeStamp);
        surfaceBuf->SetSize(surface->GetSize() - size);
        if (surface->FlushBuffer(surfaceBuf) != 0) {
            MEDIA_ERR_LOG("Flush surface failed.");
            surface->CancelBuffer(surfaceBuf);
            ret = -1;
            break;
        }
    }
    if (CodecQueueOutput(codecInfo->vencHdl_, reinterpret_cast<OutputInfo*>(outBuf), 0, -1) != 0) {
        MEDIA_ERR_LOG("Codec queue output failed.");
    }
    return ret;
}
#endif

CodecCallback CallbackH264Assistant::callbackH264CodecCb_ = {
    nullptr, nullptr, CallbackH264Assistant::OnVencBufferAvailble};

void CallbackH264Assistant::ClearFrameConfig()
{
#ifdef MEDIA_INTERFACE_V1_0
    for (uint32_t i = 0; i < vencHdls_.size(); i++) {
        CodecStop(vencHdls_[i]);
        CodecDestroy(vencHdls_[i]);
    }
    vencHdls_.clear();
    vencSurfaces_.clear();
#else
    for (uint32_t i = 0; i < codecInfo_.size(); i++) {
        CodecStop(codecInfo_[i].vencHdl_);
        CodecDestroy(codecInfo_[i].vencHdl_);
    }
    codecInfo_.clear();
#endif
}

#ifndef MEDIA_INTERFACE_V1_0
int32_t CallbackH264Assistant::SetFrameConfigEnd(int32_t result)
{
    if (result != MEDIA_OK) {
        for (uint32_t i = 0; i < codecInfo_.size(); i++) {
            CodecDestroy(codecInfo_[i].vencHdl_);
        }
        codecInfo_.clear();
        return result;
    }
    for (uint32_t i = 0; i < codecInfo_.size(); i++) {
        result = CodecSetCallback(codecInfo_[i].vencHdl_, &callbackH264CodecCb_,
            reinterpret_cast<UINTPTR>(&codecInfo_[i]));
        if (result != 0) {
            MEDIA_ERR_LOG("set CodecSetCallback failed ret:%d", result);
            CodecDestroy(codecInfo_[i].vencHdl_);
            break;
        }
    }

    if (result == MEDIA_OK) {
        state_ = LOOP_READY;
    } else {
        for (uint32_t i = 0; i < codecInfo_.size(); i++) {
            CodecDestroy(codecInfo_[i].vencHdl_);
        }
        codecInfo_.clear();
    }
    return result;
}
#endif

static int32_t SetupH264EncoderCallback(CallbackH264Assistant *assistant, CODEC_HANDLETYPE codecHdl,
    Surface *surface)
{
    list<Surface*> conList({surface});
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = CodecSetCallback(codecHdl, &assistant->callbackH264CodecCb_,
        reinterpret_cast<UINTPTR>(assistant));
    if (ret != 0) {
        MEDIA_ERR_LOG("Set codec callback failed.(ret=%d)", ret);
        CodecDestroy(codecHdl);
        assistant->ClearFrameConfig();
        return MEDIA_ERR;
    }
    assistant->vencHdls_.emplace_back(codecHdl);
    assistant->vencSurfaces_.emplace_back(conList);
    assistant->state_ = LOOP_READY;
    MEDIA_ERR_LOG("sucess.");
    return MEDIA_OK;
#else
    CodecDesc info;
    info.vencHdl_ = codecHdl;
    info.vencSurfaces_ = conList;
    assistant->codecInfo_.emplace_back(info);
    return assistant->SetFrameConfigEnd(MEDIA_OK);
#endif
}

int32_t CallbackH264Assistant::SetFrameConfig(FrameConfig &fc, uint32_t *streamId)
{
    fc_ = &fc;
    auto surfaceList = fc.GetSurfaces();
    if (surfaceList.size() != 1) {
        MEDIA_ERR_LOG("Only support one surface in frame config now.");
        return MEDIA_ERR;
    }
    uint32_t num = 0;
    Surface *surface = surfaceList.front();
    CODEC_HANDLETYPE codecHdl = nullptr;
    StreamAttr stream = {};
    StreamAttrInitialize(&stream, surface, STREAM_CALLBACK, fc);
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = halCameraDev_->HalCameraStreamCreate(cameraId_.c_str(), &stream, streamId);
#else
    int32_t ret = HalCameraStreamCreate(cameraId_, &stream, streamId);
#endif
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG(" creat callback stream failed.");
        return MEDIA_ERR;
    }
    streamId_ = *streamId;
    streamIdNum_[num] = *streamId;
    num++;
    StreamInfo streamInfo;
    streamInfo.type = STERAM_INFO_PRIVATE;
    fc.GetVendorParameter(streamInfo.u.data, PRIVATE_TAG_LEN);
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamSetInfo(cameraId_.c_str(), *streamId, &streamInfo);
#else
    HalCameraStreamSetInfo(cameraId_, *streamId, &streamInfo);
#endif
    uint32_t deviceId = 0;
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraGetDeviceId(cameraId_.c_str(), *streamId, &deviceId);
#else
    HalCameraGetDeviceId(cameraId_, *streamId, &deviceId);
#endif
    ret = CameraCreateVideoEnc(fc, stream, deviceId, &codecHdl);
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG("Cannot create suitble video encoder.");
        ClearFrameConfig();
        return MEDIA_ERR;
    }

    return SetupH264EncoderCallback(this, codecHdl, surface);
}

int32_t CallbackH264Assistant::Start(uint32_t streamId)
{
    if (state_ != LOOP_READY) {
        return MEDIA_ERR;
    }
#ifdef MEDIA_INTERFACE_V1_0
    halCameraDev_->HalCameraStreamOn(cameraId_.c_str(), streamId);
#else
    HalCameraStreamOn(cameraId_, streamId);
#endif
    int32_t ret = MEDIA_OK;
    int32_t i;
#ifdef MEDIA_INTERFACE_V1_0
    for (i = 0; static_cast<uint32_t>(i) < vencHdls_.size(); i++) {
        ret = CodecStart(vencHdls_[i]);
#else
    for (i = 0; static_cast<uint32_t>(i) < codecInfo_.size(); i++) {
        ret = CodecStart(codecInfo_[i].vencHdl_);
#endif
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("Video encoder start failed.");
            ret = MEDIA_ERR;
            break;
        }
    }
    if (ret == MEDIA_ERR) {
        /* rollback */
        for (; i >= 0; i--) {
#ifdef MEDIA_INTERFACE_V1_0
            CodecStop(vencHdls_[i]);
#else
            CodecStop(codecInfo_[i].vencHdl_);
#endif
        }
        return MEDIA_ERR;
    }
    state_ = LOOP_LOOPING;
    MEDIA_INFO_LOG("Start camera recording succeed.");
    return MEDIA_OK;
}

int32_t CallbackH264Assistant::Stop()
{
    if (state_ != LOOP_LOOPING) {
        return MEDIA_ERR;
    }
    ClearFrameConfig();
#ifdef MEDIA_INTERFACE_V1_0
    for (uint32_t i = 0; i < MAX_STREAM_NUM; i++) {
#else
    for (uint32_t i = 0; i < RECORDER_MAX_NUM; i++) {
#endif
        if (streamIdNum_[i] != INVALID_STREAM_ID) {
#ifdef MEDIA_INTERFACE_V1_0
            halCameraDev_->HalCameraStreamOff(cameraId_.c_str(), streamIdNum_[i]);
            halCameraDev_->HalCameraStreamDestroy(cameraId_.c_str(), streamIdNum_[i]);
#else
            HalCameraStreamOff(cameraId_, streamIdNum_[i]);
            HalCameraStreamDestroy(cameraId_, streamIdNum_[i]);
#endif
        }
        streamIdNum_[i] = INVALID_STREAM_ID;
    }
    state_ = LOOP_STOP;
    return MEDIA_OK;
}

CameraDevice::CameraDevice() {}
#ifdef MEDIA_INTERFACE_V1_0
CameraDevice::CameraDevice(string cameraId, HalCameraManager *halCameraDev)
{
    this->cameraId_ = cameraId;
    this->halCameraDev_ = halCameraDev;
}
#else
CameraDevice::CameraDevice(uint32_t cameraId)
{
    this->cameraId_ = cameraId;
}
#endif

CameraDevice::~CameraDevice() {}

int32_t CameraDevice::Initialize()
{
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret;
    if (cameraId_.find(CAMERA_ID_PREFIX) == std::string::npos) {
        // Need to be Refactored when delete config file
        ret = CodecInit();
        if (ret != 0) {
            MEDIA_ERR_LOG("Codec module init failed.(ret=%d)", ret);
            return MEDIA_ERR;
        }
        MEDIA_INFO_LOG("Codec module init succeed.");
    }
    ret = halCameraDev_->HalCameraDeviceOpen(cameraId_.c_str());
    if (ret != 0) {
        MEDIA_ERR_LOG("HalCameraDeviceOpen failed. ret(%d)", ret);
        return MEDIA_ERR;
    }
    captureAssistant_.state_ = LOOP_READY;
    previewAssistant_.state_ = LOOP_READY;
    recordAssistant_.state_ = LOOP_READY;
    callbackAssistant_.state_ = LOOP_READY;
    callbackH264Assistant_.state_ = LOOP_READY;
    captureAssistant_.cameraId_ = cameraId_;
    captureAssistant_.halCameraDev_ = halCameraDev_;
    previewAssistant_.cameraId_ = cameraId_;
    previewAssistant_.halCameraDev_ = halCameraDev_;
    recordAssistant_.cameraId_ = cameraId_;
    recordAssistant_.halCameraDev_ = halCameraDev_;
    callbackAssistant_.cameraId_ = cameraId_;
    callbackAssistant_.halCameraDev_ = halCameraDev_;
    callbackH264Assistant_.cameraId_ = cameraId_;
    callbackH264Assistant_.halCameraDev_ = halCameraDev_;
#else
    // Need to be Refactored when delete config file
    int32_t ret = CodecInit();
    if (ret != 0) {
        MEDIA_ERR_LOG("Codec module init failed.(ret=%d)", ret);
        return MEDIA_ERR;
    }
    MEDIA_INFO_LOG("Codec module init succeed.");
    captureAssistant_.state_ = LOOP_READY;
    previewAssistant_.state_ = LOOP_READY;
    recordAssistant_.state_ = LOOP_READY;
    callbackAssistant_.state_ = LOOP_READY;
    captureAssistant_.cameraId_ = cameraId_;
    previewAssistant_.cameraId_ = cameraId_;
    recordAssistant_.cameraId_ = cameraId_;
    callbackAssistant_.cameraId_ = cameraId_;
#endif
    return MEDIA_OK;
}

int32_t CameraDevice::UnInitialize()
{
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = halCameraDev_->HalCameraDeviceClose(cameraId_.c_str());
    if (ret != 0) {
        MEDIA_ERR_LOG("HalCameraDeviceClose failed. ret(%d)", ret);
    }
#endif
    MEDIA_INFO_LOG("CameraDevice:UnInitialize, success");
    return MEDIA_OK;
}

int32_t CameraDevice::GetAssistantByFrameConfig(FrameConfig &fc, DeviceAssistant *&assistant)
{
    int32_t fcType = fc.GetFrameConfigType();
    switch (fcType) {
        case FRAME_CONFIG_RECORD:
            assistant = &recordAssistant_;
            break;
        case FRAME_CONFIG_PREVIEW:
            assistant = &previewAssistant_;
            break;
        case FRAME_CONFIG_CAPTURE:
            assistant = &captureAssistant_;
            break;
        case FRAME_CONFIG_CALLBACK:
            assistant = &callbackAssistant_;
            break;
        case FRAME_CONFIG_CALLBACK_H264:
            assistant = &callbackH264Assistant_;
            break;
        default:
            assistant = nullptr;
            break;
    }
    return fcType;
}

int32_t CameraDevice::TriggerLoopingCapture(FrameConfig &fc, uint32_t *streamId)
{
    MEDIA_DEBUG_LOG("Camera device start looping capture.");
#ifdef MEDIA_INTERFACE_V1_0
    if (halCameraDev_ != nullptr) {
        halCameraDev_->HalCameraDeviceOpen(cameraId_.c_str());
    }
#else
    HalCameraDeviceOpen(cameraId_);
#endif

    DeviceAssistant *assistant = nullptr;
    int32_t fcType = GetAssistantByFrameConfig(fc, assistant);
    if (assistant == nullptr) {
        MEDIA_ERR_LOG("Invalid frame config type.(type=%d)", fcType);
        return MEDIA_ERR;
    }
    if (assistant->state_ == LOOP_LOOPING ||
        assistant->state_ == LOOP_ERROR) {
        MEDIA_ERR_LOG("Device state is %d, cannot start looping capture.", assistant->state_);
        return MEDIA_ERR;
    }
    uint8_t count = 1;
    if (fcType == FRAME_CONFIG_CAPTURE) {
        auto surfaceList = fc.GetSurfaces();
        if (surfaceList.size() != 1) {
            MEDIA_ERR_LOG("Only support one surface in frame config now.");
            return MEDIA_ERR;
        }
        Surface *surface = surfaceList.front();
        count = surface->GetQueueSize();
    }
    do {
        int32_t ret = assistant->SetFrameConfig(fc, streamId);
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("Check and set frame config failed.(ret=%d)", ret);
            return MEDIA_ERR;
        }

        ret = assistant->Start(*streamId);
        if (ret != MEDIA_OK) {
            MEDIA_ERR_LOG("Start looping capture failed.(ret=%d)", ret);
            return MEDIA_ERR;
        }
    } while (--count);
    return MEDIA_OK;
}

void CameraDevice::StopLoopingCapture(int32_t type)
{
    MEDIA_INFO_LOG("Stop looping capture in camera_device.cpp");
    switch (type) {
        case FRAME_CONFIG_RECORD:
            MEDIA_INFO_LOG("Stop recorder");
            recordAssistant_.Stop();
            break;
        case FRAME_CONFIG_PREVIEW:
            MEDIA_INFO_LOG("Stop preview");
            previewAssistant_.Stop();
            break;
        case FRAME_CONFIG_CALLBACK:
            MEDIA_INFO_LOG("Stop callback");
            callbackAssistant_.Stop();
#ifdef MEDIA_INTERFACE_V1_0
            callbackH264Assistant_.Stop();
#endif
            break;
        default:
            MEDIA_INFO_LOG("Stop all");
            previewAssistant_.Stop();
            recordAssistant_.Stop();
            callbackAssistant_.Stop();
#ifdef MEDIA_INTERFACE_V1_0
            callbackH264Assistant_.Stop();
#endif
            break;
    }
#ifdef MEDIA_INTERFACE_V1_0
    if (type == -1) {
        halCameraDev_->HalCameraDeviceClose(cameraId_.c_str());
    }
#endif
}

int32_t CameraDevice::TriggerSingleCapture(FrameConfig &fc, uint32_t *streamId)
{
    return TriggerLoopingCapture(fc, streamId);
}

int32_t CameraDevice::SetCameraConfig(const char *dataBuff, uint32_t len)
{
    if (dataBuff != nullptr) {
        int32_t ret = UpdataCameraSetting(dataBuff, len);
        if (ret != 0) {
            MEDIA_ERR_LOG("UpdataCameraSetting failed. ret(%d)", ret);
            return MEDIA_ERR;
        }
    }
    return MEDIA_OK;
}

int32_t CameraDevice::UpdataCameraSetting(const char *dataBuff, uint32_t len)
{
    if (dataBuff == nullptr) {
        MEDIA_ERR_LOG("dataBuff is null");
        return MEDIA_ERR;
    }
#ifdef MEDIA_INTERFACE_V1_0
    int32_t ret = halCameraDev_->HalCameraUpdateSettings(cameraId_.c_str(), dataBuff, len);
    if (ret != 0) {
        return MEDIA_ERR;
    }
#else
    (void)dataBuff;
    (void)len;
#endif
    return MEDIA_OK;
}
} // namespace Media
} // namespace OHOS
