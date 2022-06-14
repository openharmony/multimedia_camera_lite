/*
 * Copyright (c) 2020-2021 Huawei Device Co., Ltd.
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

#include "camera_device_client.h"
#include "serializer.h"
#include "camera_type.h"
#include "media_log.h"
#include "camera_config.h"
#include "frame_config.h"
#include "surface.h"
#include "surface_impl.h"
#include "camera_impl.h"

#include <list>

using namespace std;
namespace OHOS {
namespace Media {
static IpcObjectStub objectStub_;

CameraDeviceClient *CameraDeviceClient::GetInstance()
{
    static CameraDeviceClient client;
    return &client;
}

CameraDeviceClient::CameraDeviceClient()
{
    cameraClient_ = CameraClient::GetInstance();
    if (cameraClient_->InitCameraClient()) {
        proxy_ = cameraClient_->GetIClientProxy();
    }
}

CameraDeviceClient::~CameraDeviceClient()
{
    if (para_ != nullptr) {
        delete para_;
        para_ = nullptr;
    }
}

void CameraDeviceClient::SetCameraId(string &cameraId)
{
    cameraId_ = cameraId;
}

void CameraDeviceClient::SetCameraImpl(CameraImpl *cameraImpl)
{
    cameraImpl_ = cameraImpl;
}

int CameraDeviceClient::Callback(void* owner, int code, IpcIo *reply)
{
    if (code != 0) {
        MEDIA_ERR_LOG("Callback error. (code=%d)", code);
        return -1;
    }
    if (owner == nullptr) {
        return -1;
    }
    CallBackPara* para = (CallBackPara*)owner;
    switch (para->funcId) {
        case CAMERA_SERVER_SET_CAMERA_CALLBACK:
            MEDIA_INFO_LOG("Camera server set callback success.");
            break;
        default:
            break;
    }
    return 0;
}

int32_t CameraDeviceClient::SetCameraConfig(CameraConfig &cc)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    if (cameraId_.empty()) {
        MEDIA_ERR_LOG("no camera exist.");
        return MEDIA_ERR;
    }
    WriteString(&io, cameraId_.c_str());
    para_->data = this;
    para_->cameraConfig = &cc;
    CallBackPara para = {};
    para.funcId = CAMERA_SERVER_SET_CAMERA_CONFIG;
    para.data = this;
    uint32_t ret = proxy_->Invoke(proxy_, CAMERA_SERVER_SET_CAMERA_CONFIG, &io, &para, Callback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Set camera config ipc transmission failed. (ret=%d)", ret);
        return MEDIA_ERR;
    }
    return MEDIA_OK;
}

int32_t SerilizeFrameConfig(IpcIo &io, FrameConfig &fc, uint32_t maxSurfaceNum)
{
    WriteInt32(&io, fc.GetFrameConfigType());
    list<Surface*> surfaceList = fc.GetSurfaces();
    if (maxSurfaceNum < surfaceList.size()) {
        MEDIA_ERR_LOG("Too many surfaces. (maxSurfaceNum=%u, sufaceNum=%d)", maxSurfaceNum, surfaceList.size());
        return MEDIA_ERR;
    }
    WriteUint32(&io, surfaceList.size());
    for (auto &surface : surfaceList) {
        dynamic_cast<SurfaceImpl *>(surface)->WriteIoIpcIo(io);
        MEDIA_DEBUG_LOG("Add surface");
    }
    int32_t qfactor = -1;
    fc.GetParameter(PARAM_KEY_IMAGE_ENCODE_QFACTOR, qfactor);
    WriteInt32(&io, qfactor);

    int32_t streamFps = 0;
    fc.GetParameter(CAM_FRAME_FPS, streamFps);
    WriteInt32(&io, streamFps);

    int32_t invertMode = 0;
    fc.GetParameter(CAM_IMAGE_INVERT_MODE, invertMode);
    WriteInt32(&io, invertMode);

    CameraRect streamCrop;
    fc.GetParameter(CAM_IMAGE_CROP_RECT, streamCrop);
    WriteInt32(&io, streamCrop.x);
    WriteInt32(&io, streamCrop.y);
    WriteInt32(&io, streamCrop.w);
    WriteInt32(&io, streamCrop.h);

    int32_t format = -1;
    fc.GetParameter(CAM_IMAGE_FORMAT, format);
    WriteInt32(&io, format);
    if (fc.GetFrameConfigType() != FRAME_CONFIG_RECORD) {
        uint8_t data[PRIVATE_TAG_LEN];
        fc.GetVendorParameter(data, sizeof(data));
        WriteUint32(&io, (uint32_t)sizeof(data));
        WriteBuffer(&io, (void *)data, sizeof(data));
    }

    return MEDIA_OK;
}


int32_t CameraDeviceClient::TriggerLoopingCapture(FrameConfig &fc)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    constexpr uint32_t maxSurfaceNum = 2; // 2 surfaces at most
    /* serilize parameters */
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, maxSurfaceNum);
    if (cameraId_.empty()) {
        MEDIA_ERR_LOG("no camera exist.");
        return MEDIA_ERR;
    }
    WriteString(&io, cameraId_.c_str());
    if (SerilizeFrameConfig(io, fc, maxSurfaceNum) != MEDIA_OK) {
        MEDIA_ERR_LOG("Serilize the frameconfig failed.");
        return MEDIA_ERR;
    }
    para_->data = this;
    CallBackPara para = {};
    para.funcId = CAMERA_SERVER_TRIGGER_LOOPING_CAPTURE;
    para.data = this;
    uint32_t ret = proxy_->Invoke(proxy_, CAMERA_SERVER_TRIGGER_LOOPING_CAPTURE, &io, &para, Callback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Trigger looping capture ipc  transmission failed. (ret=%d)", ret);
        return MEDIA_ERR;
    }
    return ret_;
}

int32_t CameraDeviceClient::TriggerSingleCapture(FrameConfig &fc)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    constexpr uint32_t maxSurfaceNum = 2; // 2 surfaces at most
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, maxSurfaceNum);

    if (cameraId_.empty()) {
        MEDIA_ERR_LOG("no camera exist.");
        return MEDIA_ERR;
    }
    WriteString(&io, cameraId_.c_str());
    if (SerilizeFrameConfig(io, fc, maxSurfaceNum) != MEDIA_OK) {
        MEDIA_ERR_LOG("Serilize the frameconfig failed.");
        return MEDIA_ERR;
    }
    para_->frameConfig = &fc;
    para_->data = this;
    CallBackPara para = {};
    para.funcId = CAMERA_SERVER_TRIGGER_SINGLE_CAPTURE;
    para.data = this;
    uint32_t ret = proxy_->Invoke(proxy_, CAMERA_SERVER_TRIGGER_SINGLE_CAPTURE, &io, &para, Callback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Trigger single capture ipc  transmission failed. (ret=%d)", ret);
        return MEDIA_ERR;
    }
    return ret_;
}

void CameraDeviceClient::StopLoopingCapture(int32_t type)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    if (cameraId_.empty()) {
        MEDIA_ERR_LOG("no camera exist.");
        return;
    }
    WriteString(&io, cameraId_.c_str());
    WriteInt32(&io, type);
    CallBackPara para = {};
    para.funcId = CAMERA_SERVER_STOP_LOOPING_CAPTURE;
    para.data = this;
    uint32_t ret = proxy_->Invoke(proxy_, CAMERA_SERVER_STOP_LOOPING_CAPTURE, &io, &para, Callback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Stop Looping capture ipc  transmission failed. (ret=%d)", ret);
    }
}

void CameraDeviceClient::Release()
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    if (cameraId_.empty()) {
        MEDIA_ERR_LOG("no camera exist.");
        return;
    }
    if (proxy_ == nullptr) {
        return;
    }
    WriteString(&io, cameraId_.c_str());
    bool writeRemote = WriteRemoteObject(&io, &sid_);
    if (!writeRemote) {
        return;
    }
    para_->data = this;
    CallBackPara para = {};
    para.funcId = CAMERA_SERVER_CLOSE_CAMERA;
    para.data = this;
    uint32_t ret = proxy_->Invoke(proxy_, CAMERA_SERVER_CLOSE_CAMERA, &io,  &para, Callback);
    if (ret != 0) {
        MEDIA_ERR_LOG("Stop Looping capture ipc  transmission failed. (ret=%d)", ret);
    }
}

void CameraDeviceClient::SetCameraCallback()
{
    para_ = new (nothrow) CallBackPara;
    if (para_ == nullptr) {
        MEDIA_ERR_LOG("para_ is null.");
        return;
    }
    objectStub_.func = CameraDeviceClient::DeviceClientCallback;
    objectStub_.args = para_;
    objectStub_.isRemote = false;
    sid_.handle = IPC_INVALID_HANDLE;
    sid_.token = SERVICE_TYPE_ANONYMOUS;
    sid_.cookie = reinterpret_cast<uintptr_t>(&objectStub_);

    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 1);
    bool writeRemote = WriteRemoteObject(&io, &sid_);
    if (!writeRemote) {
        return;
    }

    CallBackPara para = {};
    para.funcId = CAMERA_SERVER_SET_CAMERA_CALLBACK;
    uint32_t ans = proxy_->Invoke(proxy_, CAMERA_SERVER_SET_CAMERA_CALLBACK, &io, &para, Callback);
    if (ans != 0) {
        MEDIA_ERR_LOG("Stop Looping capture ipc  transmission failed. (ret=%d)", ans);
    }
}

int32_t CameraDeviceClient::DeviceClientCallback(uint32_t code, IpcIo *data, IpcIo *reply, MessageOption option)
{
    if (data == nullptr) {
        MEDIA_ERR_LOG("call back error, data is null\n");
        return MEDIA_ERR;
    }
    if (option.args == nullptr) {
        MEDIA_ERR_LOG("call back error, option.args is null\n");
        return MEDIA_ERR;
    }
    CallBackPara *para = static_cast<CallBackPara *>(option.args);
    CameraDeviceClient *client = static_cast<CameraDeviceClient*>(para->data);
    MEDIA_INFO_LOG("DeviceCallback, funcId=%d", code);
    switch (code) {
        case ON_CAMERA_CONFIGURED: {
            int32_t ret;
            ReadInt32(data, &ret);
            CameraConfig *cc = static_cast<CameraConfig*>(para->cameraConfig);
            client->cameraImpl_->OnConfigured(ret, *cc);
            break;
        }
        case ON_TRIGGER_SINGLE_CAPTURE_FINISHED: {
            int32_t ret;
            ReadInt32(data, &ret);
            FrameConfig *fc = static_cast<FrameConfig*>(para->frameConfig);
            client->cameraImpl_->OnFrameFinished(ret, *fc);
            client->ret_ = ret;
            break;
        }
        case ON_TRIGGER_LOOPING_CAPTURE_FINISHED: {
            int32_t ret;
            ReadInt32(data, &ret);
            int32_t streamId;
            ReadInt32(data, &streamId);
            MEDIA_INFO_LOG("ON_TRIGGER_LOOPING_CAPTURE_FINISHED : (ret=%d, streamId=%d).", ret, streamId);
            client->ret_ = ret;
            break;
        }
        case ON_CAMERA_STATUS_CHANGE: {
            int32_t ret;
            ReadInt32(data, &ret);
            MEDIA_INFO_LOG("ON_CAMERA_STATUS_CHANGE: ret=%d", ret);
            break;
        }
        default: {
            MEDIA_ERR_LOG("unsupport funId\n");
            break;
        }
    }
    return MEDIA_OK;
}
} // namespace Media
} // namespace OHOS
