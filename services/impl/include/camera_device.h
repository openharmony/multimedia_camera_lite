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

#ifndef OHOS_CAMERA_DEVICE_H
#define OHOS_CAMERA_DEVICE_H

#include <cstdbool>
#include <thread>
#include <vector>

#include "camera_ability.h"
#include "camera_config.h"
#include "codec_type.h"
#include "frame_config.h"
#include "surface.h"
#include "hal_camera.h"

using namespace std;
namespace OHOS {
namespace Media {
enum LoopState {
    LOOP_IDLE,
    LOOP_READY,
    LOOP_LOOPING,
    LOOP_STOP,
    LOOP_ERROR,
};
const int32_t MAX_STREAM_NUM = 5;
#ifndef MEDIA_INTERFACE_V1_0
const int32_t RECORDER_MAX_NUM = 2;
struct CodecDesc {
    CODEC_HANDLETYPE vencHdl_;
    list<Surface *> vencSurfaces_;
};
#endif
class DeviceAssistant {
public:
    std::thread *thrd_ = nullptr;
    LoopState state_ = LOOP_IDLE;
    FrameConfig *fc_ = nullptr;
#ifdef MEDIA_INTERFACE_V1_0
    string cameraId_;
    static HalCameraManager *halCameraDev_;
#else
    uint32_t cameraId_;
#endif
    uint32_t streamId_;

    virtual int32_t SetFrameConfig(FrameConfig &fc, uint32_t *streamId)
    {
        return -1;
    }
    virtual int32_t Start(uint32_t streamId)
    {
        return -1;
    }
    virtual int32_t Stop()
    {
        return -1;
    }
};

class RecordAssistant : public DeviceAssistant {
public:
    int32_t SetFrameConfig(FrameConfig &fc, uint32_t *streamId) override;
    int32_t Start(uint32_t streamId) override;
    int32_t Stop() override;
    void ClearFrameConfig();

#ifdef MEDIA_INTERFACE_V1_0
    vector<CODEC_HANDLETYPE> vencHdls_;
    vector<list<Surface *>> vencSurfaces_;
    static int OnVencBufferAvailble(UINTPTR hComponent, UINTPTR dataIn, OutputInfo *buffer);
    int32_t streamIdNum_[MAX_STREAM_NUM] = {-1, -1, -1, -1, -1};
#else
    static int OnVencBufferAvailble(UINTPTR userData, CodecBuffer *outBuf, int32_t *acquireFd);
    int32_t SetFrameConfigEnd(int32_t result);
    vector<CodecDesc> codecInfo_;
    int32_t streamIdNum_[RECORDER_MAX_NUM] = {-1, -1};
#endif
    static CodecCallback recordCodecCb_;
};

class PreviewAssistant : public DeviceAssistant {
public:
    int32_t SetFrameConfig(FrameConfig &fc, uint32_t *streamId) override;
    int32_t Start(uint32_t streamId) override;
    virtual int32_t Stop() override;
    Surface *capSurface_ = nullptr;
private:
    uint32_t GetFreeStreamIndex();
    int32_t sourceIdx = 0;
    int32_t streamIdNum_[MAX_STREAM_NUM] = {-1, -1, -1, -1, -1};
};

class CaptureAssistant : public DeviceAssistant {
    int32_t SetFrameConfig(FrameConfig &fc, uint32_t *streamId) override;
    int32_t Start(uint32_t streamId) override;
    virtual int32_t Stop() override;
    CODEC_HANDLETYPE vencHdl_ = nullptr;
    Surface *capSurface_ = nullptr;
};

class CallbackAssistant : public DeviceAssistant {
public:
    int32_t SetFrameConfig(FrameConfig &fc, uint32_t *streamId) override;
    int32_t Start(uint32_t streamId) override;
    virtual int32_t Stop() override;
    Surface *capSurface_ = nullptr;
private:
    pthread_t threadId;
    static void *StreamCopyProcess(void *arg);
};

class CallbackH264Assistant : public DeviceAssistant {
public:
    int32_t SetFrameConfig(FrameConfig &fc, uint32_t *streamId) override;
    int32_t Start(uint32_t streamId) override;
    virtual int32_t Stop() override;
    void ClearFrameConfig();

#ifdef MEDIA_INTERFACE_V1_0
    vector<CODEC_HANDLETYPE> vencHdls_;
    vector<list<Surface *>> vencSurfaces_;
    static int OnVencBufferAvailble(UINTPTR hComponent, UINTPTR dataIn, OutputInfo *buffer);
    int32_t streamIdNum_[MAX_STREAM_NUM] = {-1, -1, -1, -1, -1};
#else
    static int OnVencBufferAvailble(UINTPTR userData, CodecBuffer *outBuf, int32_t *acquireFd);
    int32_t SetFrameConfigEnd(int32_t result);
    vector<CodecDesc> codecInfo_;
    int32_t streamIdNum_[RECORDER_MAX_NUM] = {-1, -1};
#endif
    static CodecCallback callbackH264CodecCb_;
};

class CameraDevice {
public:
    CameraDevice();
#ifdef MEDIA_INTERFACE_V1_0
    CameraDevice(string cameraId, HalCameraManager *halCameraDev);
#else
    explicit CameraDevice(uint32_t cameraId);
#endif
    virtual ~CameraDevice();

    int32_t Initialize();
    int32_t UnInitialize();
    int32_t SetCameraConfig(const char *dataBuff, uint32_t len);
    int32_t TriggerLoopingCapture(FrameConfig &fc, uint32_t *streamId);
    void StopLoopingCapture(int32_t type);
    int32_t TriggerSingleCapture(FrameConfig &fc, uint32_t *streamId);
    uint32_t GetCameraId();
    int32_t UpdataCameraSetting(const char *dataBuff, uint32_t len);
private:
    int32_t GetAssistantByFrameConfig(FrameConfig &fc, DeviceAssistant *&assistant);
#ifdef MEDIA_INTERFACE_V1_0
    string cameraId_;
#else
    uint32_t cameraId_;
#endif
    RecordAssistant recordAssistant_;
    PreviewAssistant previewAssistant_;
    CaptureAssistant captureAssistant_;
    CallbackAssistant callbackAssistant_;
    CallbackH264Assistant callbackH264Assistant_;
#ifdef MEDIA_INTERFACE_V1_0
    HalCameraManager *halCameraDev_ = nullptr;
#endif
};
} // namespace Media
} // namespace OHOS
#endif // OHOS_CAMERA_DEVICE_H