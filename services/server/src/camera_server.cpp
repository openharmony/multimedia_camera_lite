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

#include "camera_server.h"
#include <cstdio>
#include <list>
#include <string>
#include "camera_device.h"
#include "camera_service.h"
#include "camera_type.h"
#include "media_log.h"
#include "meta_data.h"
#include "surface.h"
#include "surface_impl.h"
#include "rpc_errno.h"

using namespace std;
namespace OHOS {
namespace Media {
CameraServer *CameraServer::GetInstance()
{
    static CameraServer server;
    return &server;
}

void CameraServer::CameraServerRequestHandle(int funcId, void *origin, IpcIo *req, IpcIo *reply)
{
    switch (funcId) {
        case CAMERA_SERVER_GET_CAMERA_ABILITY:
            CameraServer::GetInstance()->GetCameraAbility(req, reply);
            break;
        case CAMERA_SERVER_GET_CAMERA_INFO:
            CameraServer::GetInstance()->GetCameraInfo(req, reply);
            break;
        case CAMERA_SERVER_GET_CAMERAIDLIST:
            CameraServer::GetInstance()->GetCameraIdList(req, reply);
            break;
        case CAMERA_SERVER_CREATE_CAMERA:
            CameraServer::GetInstance()->CreateCamera(req, reply);
            break;
        case CAMERA_SERVER_CLOSE_CAMERA:
            CameraServer::GetInstance()->CloseCamera(req, reply);
            break;
        case CAMERA_SERVER_SET_CAMERA_CONFIG:
            CameraServer::GetInstance()->SetCameraConfig(req, reply);
            break;
        case CAMERA_SERVER_TRIGGER_LOOPING_CAPTURE:
            CameraServer::GetInstance()->TriggerLoopingCapture(req, reply);
            break;
        case CAMERA_SERVER_STOP_LOOPING_CAPTURE:
            CameraServer::GetInstance()->StopLoopingCapture(req, reply);
            break;
        case CAMERA_SERVER_TRIGGER_SINGLE_CAPTURE:
            CameraServer::GetInstance()->TriggerSingleCapture(req, reply);
            break;
        case CAMERA_SERVER_SET_CAMERA_CALLBACK:
            CameraServer::GetInstance()->SetCameraCallback(req, reply);
            break;
        case CAMERA_SERVER_SET_CAMERA_MODE_NUM:
            CameraServer::GetInstance()->SetCameraMode(req, reply);
            break;
        case CAMERA_SERVER_GET_CAMERA_MODE_NUM:
            CameraServer::GetInstance()->GetCameraModeNum(req, reply);
            break;
        default:
            MEDIA_ERR_LOG("code not support:%d!", funcId);
            break;
    }
}
void CameraServer::InitCameraServer()
{
    CameraService *service = CameraService::GetInstance();
    service->Initialize();
}

void CameraServer::GetCameraAbility(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char*)(ReadString(req, &sz)));
    CameraAbility *ability = CameraService::GetInstance()->GetCameraAbility(cameraId);
    if (ability == nullptr) {
        return;
    }
    list<CameraPicSize> supportSizeList = ability->GetSupportedSizes(CAM_FORMAT_YVU420);
    uint32_t supportProperties = 0;
    WriteUint32(reply, supportProperties);
    uint32_t listSize = supportSizeList.size();
    WriteUint32(reply, listSize);
    for (auto supportSizeItem : supportSizeList) {
        bool ret = WriteRawData(reply, &supportSizeItem, sizeof(CameraPicSize));
        if (!ret) {
            return;
        }
    }
    // af
    list<int32_t> afModeList = ability->GetSupportedAfModes();
    uint32_t afListSize = afModeList.size();
    WriteUint32(reply, afListSize);
    for (auto supportAfMode : afModeList) {
        WriteInt32(reply, supportAfMode);
    }
    // ae
    list<int32_t> aeModeList = ability->GetSupportedAeModes();
    uint32_t aeListSize = aeModeList.size();
    WriteUint32(reply, aeListSize);
    for (auto supportAeMode : aeModeList) {
        WriteInt32(reply, supportAeMode);
    }
}

void CameraServer::GetCameraInfo(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char*)(ReadString(req, &sz)));
    CameraInfo *info = CameraService::GetInstance()->GetCameraInfo(cameraId);
    if (info == nullptr) {
        return;
    }
    WriteInt32(reply, info->GetCameraType());
    WriteInt32(reply, info->GetCameraFacingType());
}

void CameraServer::GetCameraIdList(IpcIo *req, IpcIo *reply)
{
    list<string> cameraIdList = CameraService::GetInstance()->GetCameraIdList();
    WriteUint32(reply, cameraIdList.size());
    for (string cameraId : cameraIdList) {
        WriteString(reply, cameraId.c_str());
    }
}

void CameraServer::GetCameraModeNum(IpcIo *req, IpcIo *reply)
{
    uint8_t num = CameraService::GetInstance()->GetCameraModeNum();
    WriteUint8(reply, num);
}

void CameraServer::CreateCamera(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char*)(ReadString(req, &sz)));
    int32_t cameraStatus = CameraService::GetInstance()->CreateCamera(cameraId);
    SvcIdentity sid;
    bool ret = ReadRemoteObject(req, &sid);
    if (!ret) {
        MEDIA_ERR_LOG("sid is null, failed.");
        return;
    }
    OnCameraStatusChange(cameraStatus, &sid);
}

void CameraServer::CloseCamera(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char*)(ReadString(req, &sz)));
    int32_t cameraStatus = CameraService::GetInstance()->CloseCamera(cameraId);
    SvcIdentity sid;
    bool ret = ReadRemoteObject(req, &sid);
    if (!ret) {
        MEDIA_ERR_LOG("sid is null, failed.");
        return;
    }
    OnCameraStatusChange(cameraStatus, &sid);
}

void CameraServer::SetCameraConfig(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char*)(ReadString(req, &sz)));
    CameraDevice *device_ = CameraService::GetInstance()->GetCameraDevice(cameraId);
    int32_t setStatus = device_->SetCameraConfig();
    WriteInt32(reply, setStatus);
    OnCameraConfigured(setStatus);
}

void CameraServer::SetCameraCallback(IpcIo *req, IpcIo *reply)
{
    SvcIdentity sid;
    bool ret = ReadRemoteObject(req, &sid);
    if (!ret) {
        MEDIA_ERR_LOG("sid is null, failed.");
        return;
    }
    sid_ = sid;
}

FrameConfig *DeserializeFrameConfig(IpcIo &io)
{
    int32_t type;
    ReadInt32(&io, &type);
    auto fc = new FrameConfig(type);

    uint32_t surfaceNum;
    ReadUint32(&io, &surfaceNum);
    for (uint32_t i = 0; i < surfaceNum; i++) {
        Surface *surface = SurfaceImpl::GenericSurfaceByIpcIo(io);
        if (surface == nullptr) {
            MEDIA_ERR_LOG("Camera server receive null surface.");
            delete fc;
            return nullptr;
        }
        fc->AddSurface(*surface);
    }

    int32_t qfactor;
    ReadInt32(&io, &qfactor);
    if (qfactor >= 0) {
        fc->SetParameter(PARAM_KEY_IMAGE_ENCODE_QFACTOR, qfactor);
    }
    int32_t streamFps = 0;
    ReadInt32(&io, &streamFps);
    fc->SetParameter(CAM_FRAME_FPS, streamFps);
    int32_t invertMode = 0;
    ReadInt32(&io, &invertMode);
    fc->SetParameter(CAM_IMAGE_INVERT_MODE, invertMode);
    CameraRect streamCrop;
    ReadInt32(&io, &streamCrop.x);
    ReadInt32(&io, &streamCrop.y);
    ReadInt32(&io, &streamCrop.w);
    ReadInt32(&io, &streamCrop.h);
    fc->SetParameter(CAM_IMAGE_CROP_RECT, streamCrop);
    int32_t streamFormat;
    ReadInt32(&io, &streamFormat);
    fc->SetParameter(CAM_IMAGE_FORMAT, streamFormat);
    MEDIA_INFO_LOG("streamFormat is %d", streamFormat);

    if (type != FRAME_CONFIG_RECORD) {
        uint32_t len;
        ReadUint32(&io, &len);
        uint8_t *dataBuff = (uint8_t *)ReadBuffer(&io, len);
        fc->SetVendorParameter(dataBuff, len);
    }
    return fc;
}

void CameraServer::SetFrameConfig(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    ReadString(req, &sz);
    int32_t streamId;
    ReadInt32(req, &streamId);
    MEDIA_ERR_LOG("SetFrameConfig streamId(%d).", streamId);
    FrameConfig *fc = DeserializeFrameConfig(*req);
    if (fc == nullptr) {
        MEDIA_ERR_LOG("Deserialize frame config failed.");
        return;
    }
    delete fc;
}


void CameraServer::TriggerLoopingCapture(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char*)(ReadString(req, &sz)));
    CameraDevice *device_ = CameraService::GetInstance()->GetCameraDevice(cameraId);
    FrameConfig *fc = DeserializeFrameConfig(*req);
    if (fc == nullptr) {
        MEDIA_ERR_LOG("Deserialize frame config failed.");
        return;
    }
    uint32_t streamId = 0;
    int32_t loopingCaptureStatus = device_->TriggerLoopingCapture(*fc, &streamId);
    OnTriggerLoopingCaptureFinished(loopingCaptureStatus, streamId);
    delete fc;
}

void CameraServer::TriggerSingleCapture(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char *)(ReadString(req, &sz)));
    CameraDevice *device_ = CameraService::GetInstance()->GetCameraDevice(cameraId);
    FrameConfig *fc = DeserializeFrameConfig(*req);
    if (fc == nullptr) {
        MEDIA_ERR_LOG("Deserialize frame config failed.");
        return;
    }
    uint32_t streamId = 0;
    int32_t singleCaptureStatus = device_->TriggerSingleCapture(*fc, &streamId);
    OnTriggerSingleCaptureFinished(singleCaptureStatus);
    delete fc;
}

void CameraServer::StopLoopingCapture(IpcIo *req, IpcIo *reply)
{
    size_t sz;
    string cameraId((const char *)(ReadString(req, &sz)));
    CameraDevice *device_ = CameraService::GetInstance()->GetCameraDevice(cameraId);
    if (device_ == nullptr) {
        MEDIA_INFO_LOG("device_ is  null in camera_server.cpp!");
        return;
    }

    int32_t type;
    ReadInt32(req, &type);
    device_->StopLoopingCapture(type);
}

void CameraServer::OnCameraStatusChange(int32_t ret, SvcIdentity *sid)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, ret);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t result = SendRequest(*sid, ON_CAMERA_STATUS_CHANGE, &io, nullptr, option, nullptr);
    if (result != ERR_NONE) {
        MEDIA_ERR_LOG("Create camera callback : on camera status change failed.");
    }
}

void CameraServer::OnTriggerSingleCaptureFinished(int32_t ret)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, ret);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t result = SendRequest(sid_, ON_TRIGGER_SINGLE_CAPTURE_FINISHED, &io, nullptr, option, nullptr);
    if (result != ERR_NONE) {
        MEDIA_ERR_LOG("Trigger single capture callback : on trigger single capture frame finished failed.");
        return;
    }
}

void CameraServer::OnTriggerLoopingCaptureFinished(int32_t ret, int32_t streamId)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, ret);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t result = SendRequest(sid_, ON_TRIGGER_LOOPING_CAPTURE_FINISHED, &io, nullptr, option, nullptr);
    if (result != ERR_NONE) {
        MEDIA_ERR_LOG("Trigger looping capture callback : on trigger looping capture finished failed.");
    }
}

void CameraServer::OnCameraConfigured(int32_t ret)
{
    IpcIo io;
    uint8_t tmpData[DEFAULT_IPC_SIZE];
    IpcIoInit(&io, tmpData, DEFAULT_IPC_SIZE, 0);
    WriteInt32(&io, ret);
    MessageOption option;
    MessageOptionInit(&option);
    option.flags = TF_OP_ASYNC;
    int32_t result = SendRequest(sid_, ON_CAMERA_CONFIGURED, &io, nullptr, option, nullptr);
    if (result != ERR_NONE) {
        MEDIA_ERR_LOG("Camera config callback : on trigger looping capture finished failed.");
    }
}

void CameraServer::SetCameraMode(IpcIo *req, IpcIo *reply)
{
    uint8_t modeIndex;
    ReadUint8(req, &modeIndex);
    int32_t cameraStatus = CameraService::GetInstance()->SetCameraMode(modeIndex);
    WriteInt32(reply, cameraStatus);
}
} // namespace Media
} // namespace OHOS