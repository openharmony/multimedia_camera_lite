/*
 * Copyright (c) 2020-2022 Huawei Device Co., Ltd.
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
#include "camera_service.h"
#include <string>
#include <iostream>
#include <unistd.h>
#include "media_log.h"
#include "codec_interface.h"

const std::string CAMERA_ID_PREFIX = "Camera_";

using namespace std;

namespace OHOS {
namespace Media {
CameraService::CameraService() {}

CameraService::~CameraService()
{
#ifdef MEDIA_INTERFACE_V1_0
    auto iter = deviceMap_.begin();
    while (iter != deviceMap_.end()) {
        if (iter->second.first != nullptr) {
            CameraDevice *cameraDev = (CameraDevice *)iter->second.first;
            cameraDev->StopLoopingCapture(-1);
            int32_t ret = cameraDev->UnInitialize();
            if (ret != 0) {
                MEDIA_ERR_LOG("UnInitialize failed. ret(%d)", ret);
            }
            deviceMap_.erase(iter++);
            delete cameraDev;
        } else {
            ++iter;
        }
    }
    int32_t ret;
    if (localHalCameraDev_ != nullptr) {
        ret = localHalCameraDev_->HalCameraDeinit();
        if (ret != 0) {
            MEDIA_ERR_LOG("localHalCameraDev_ HiCameraDeInit return failed ret(%d).", ret);
        }
    }

    inited_ = false;
#else
    auto iter = deviceMap_.begin();
    while (iter != deviceMap_.end()) {
        if (iter->second != nullptr) {
            iter->second->StopLoopingCapture(-1);
            int32_t ret = HalCameraDeviceClose((uint32_t)std::atoi(iter->first.c_str()));
            if (ret != 0) {
                MEDIA_ERR_LOG("HalCameraDeviceClose failed. ret(%d)", ret);
            }
            deviceMap_.erase(iter++);
        } else {
            ++iter;
        }
    }
    HalCameraDeinit();
#endif
}

CameraService *CameraService::GetInstance()
{
    static CameraService instance;
    return &instance;
}

#ifdef MEDIA_INTERFACE_V1_0
void DistributedCameraDetectCb(const char *cameraId, CameraStatus status)
{
    MEDIA_INFO_LOG("cameraId :%s status:%d", cameraId, status);
    CameraService *cameraService = CameraService::GetInstance();
    std::string camId(cameraId);
    cameraService->CameraStatusChange(camId, status);
}
#endif

void CameraService::CameraStatusChange(std::string &cameraId, CameraStatus status)
{
    if (cameraServiceCb_ == nullptr) {
        MEDIA_ERR_LOG("callback cameraServiceCb_ is uninitialized");
        return;
    }
    switch (status) {
        case CAMERA_STATUS_NOT_PRESENT:
            MEDIA_INFO_LOG("callback cameraId:%s CAMERA_STATUS_NOT_PRESENT", cameraId.c_str());
            cameraServiceCb_->OnCameraStatusChange(cameraId, CameraServiceCallback::CAMERA_STATUS_UNAVAIL);
            break;
        case CAMERA_STATUS_PRESENT:
            MEDIA_INFO_LOG("callback cameraId:%s CAMERA_STATUS_PRESENT", cameraId.c_str());
            cameraServiceCb_->OnCameraStatusChange(cameraId, CameraServiceCallback::CAMERA_STATUS_AVAIL);
            break;
        default:
            break;
    }
}

void CameraService::Initialize()
{
#ifdef MEDIA_INTERFACE_V1_0
    if (inited_) {
        return;
    }
    int32_t ret;
    if (localHalCameraDev_ == nullptr) {
        localHalCameraDev_ = GetHalCameraFuncs();
        ret = localHalCameraDev_->HalCameraInit();
        MEDIA_INFO_LOG("localHalCameraDev Init ret(%d)", ret);
    }

    inited_ = true;
#else
    int32_t ret = HalCameraInit();
    if (ret != 0) {
        MEDIA_ERR_LOG("HiCameraInit failed. ret(%d)", ret);
    }
    inited_ = true;
#endif
}

#ifdef MEDIA_INTERFACE_V1_0
HalCameraManager *CameraService::GetHalCameraDevice(const std::string &cameraId)
{
    (void)cameraId;
    return localHalCameraDev_;
}
#endif

void CameraService::SetCameraFormatRanges(CameraAbility *ability, const std::string &cameraId)
{
    uint32_t streamCapNum = 0;
    StreamCap *streamCap = nullptr;
    int32_t ret;
#ifdef MEDIA_INTERFACE_V1_0
    HalCameraManager *halCameraDev = GetHalCameraDevice(cameraId);
    ret = halCameraDev->HalCameraGetStreamCapNum(cameraId.c_str(), &streamCapNum);
#else
    ret = HalCameraGetStreamCapNum(atoi(cameraId.c_str()), &streamCapNum);
#endif
    const uint32_t MAX_STREAM_CAP_NUM = 64;
    if (streamCapNum == 0 || streamCapNum > MAX_STREAM_CAP_NUM) {
        MEDIA_ERR_LOG("Invalid streamCapNum: %u.", streamCapNum);
        return;
    }
    streamCap = new StreamCap[streamCapNum];
    for (uint32_t pos = 0; pos < streamCapNum; pos++) {
        streamCap[pos].type = CAP_DESC_ENUM;
    }
#ifdef MEDIA_INTERFACE_V1_0
    ret = halCameraDev->HalCameraGetStreamCap(cameraId.c_str(), streamCap, streamCapNum);
#else
    ret = HalCameraGetStreamCap(atoi(cameraId.c_str()), streamCap, streamCapNum);
#endif
    list<CameraPicSize> range;
    for (int pos = 0; pos < streamCapNum; pos++) {
        CameraPicSize tmpSize = {.width = (uint32_t)streamCap[pos].u.formatEnum.width, .height =
            (uint32_t)streamCap[pos].u.formatEnum.height};
        range.emplace_back(tmpSize);
    }
    ability->SetParameterRange(CAM_FORMAT_YVU420, range);
    ability->SetParameterRange(CAM_FORMAT_JPEG, range);
    ability->SetParameterRange(CAM_FORMAT_H264, range);
    ability->SetParameterRange(CAM_FORMAT_H265, range);
    delete[] streamCap;
}

CameraAbility *CameraService::GetCameraAbility(std::string &cameraId)
{
    if (!inited_) {
        MEDIA_ERR_LOG("not inited  %s ", cameraId.c_str());
        return NULL;
    }
    std::map<string, CameraAbility*>::iterator iter = deviceAbilityMap_.find(cameraId);
    if (iter != deviceAbilityMap_.end()) {
        MEDIA_ERR_LOG("return GetCameraAbility  %s ", cameraId.c_str());
        return iter->second;
    }
    CameraAbility *ability = new (nothrow) CameraAbility;
    if (ability == nullptr) {
        return nullptr;
    }
    SetCameraFormatRanges(ability, cameraId);

    AbilityInfo cameraAbility = {};
#ifdef MEDIA_INTERFACE_V1_0
    HalCameraManager *halCameraDev = GetHalCameraDevice(cameraId);
    halCameraDev->HalCameraGetAbility(cameraId.c_str(), &cameraAbility);
#else
    HalCameraGetAbility(atoi(cameraId.c_str()), &cameraAbility);
#endif
    list<int32_t> afModes;
    for (int i = 0; i < cameraAbility.afModeNum; i++) {
        afModes.emplace_back(cameraAbility.afModes[i]);
    }
    ability->SetParameterRange(CAM_AF_MODE, afModes);

    list<int32_t> aeModes;
    for (int i = 0; i < cameraAbility.aeModeNum; i++) {
        aeModes.emplace_back(cameraAbility.aeModes[i]);
    }
    ability->SetParameterRange(CAM_AE_MODE, aeModes);
    deviceAbilityMap_.insert(pair<string, CameraAbility*>(cameraId, ability));
    return ability;
}

CameraInfo *CameraService::GetCameraInfo(std::string &cameraId)
{
    if (!inited_) {
        MEDIA_ERR_LOG("not inited  %s ", cameraId.c_str());
        return NULL;
    }
    std::map<string, CameraInfo*>::iterator iter = deviceInfoMap_.find(cameraId);
    if (iter != deviceInfoMap_.end()) {
        return iter->second;
    }
    AbilityInfo deviceAbility;
    int32_t ret;
#ifdef MEDIA_INTERFACE_V1_0
    HalCameraManager *halCameraDev = GetHalCameraDevice(cameraId);
    ret = halCameraDev->HalCameraGetAbility(cameraId.c_str(), &deviceAbility);
#else
    ret = HalCameraGetAbility((uint32_t)std::atoi(cameraId.c_str()), &deviceAbility);
#endif
    if (ret != MEDIA_OK) {
        MEDIA_ERR_LOG("HalCameraGetAbility failed. ret(%d)", ret);
        return nullptr;
    }
    CameraInfo *info = new (nothrow) CameraInfoImpl(deviceAbility.type, deviceAbility.orientation);
    if (info == nullptr) {
        return nullptr;
    }
    deviceInfoMap_.insert(pair<string, CameraInfo*>(cameraId, info));
    return info;
}

CameraDevice *CameraService::GetCameraDevice(std::string &cameraId)
{
#ifdef MEDIA_INTERFACE_V1_0
    std::map<std::string, std::pair<CameraDevice*, HalCameraManager*>>::iterator iter = deviceMap_.find(cameraId);
    if (iter != deviceMap_.end()) {
        return iter->second.first;
    }
    return nullptr;
#else
    std::map<string, CameraDevice*>::iterator iter = deviceMap_.find(cameraId);
    if (iter != deviceMap_.end()) {
        return iter->second;
    }
    return nullptr;
#endif
}

#ifdef MEDIA_INTERFACE_V1_0
std::pair<CameraDevice*, HalCameraManager*> CameraService::GetCameraDeviceInfo(std::string &cameraId)
{
    std::map<std::string, std::pair<CameraDevice*, HalCameraManager*>>::iterator iter = deviceMap_.find(cameraId);
    if (iter != deviceMap_.end()) {
        return iter->second;
    }
    return {};
}
#endif

list<string> CameraService::GetCameraIdList()
{
#ifdef MEDIA_INTERFACE_V1_0
    uint8_t camNum = 0;
    localHalCameraDev_->HalCameraGetDeviceNum(&camNum);
    char (*cameraList)[CAMERA_NAME_MAX_LEN] = new char[camNum][CAMERA_NAME_MAX_LEN];
    localHalCameraDev_->HalCameraGetDeviceList(cameraList, camNum);
    list<string> cameraStrList;
    MEDIA_INFO_LOG("Local camNum(%u)\n", camNum);
    for (uint32_t pos = 0; pos < camNum; pos++) {
        MEDIA_INFO_LOG("index(%u) name: %s \n", pos, cameraList[pos]);
        cameraStrList.push_back(cameraList[pos]);
    }
    delete[] cameraList;
    return cameraStrList;
#else
    uint8_t camNum = 0;
    HalCameraGetDeviceNum(&camNum);
    uint32_t *cameraList = new uint32_t[camNum];
    HalCameraGetDeviceList(cameraList, camNum);
    list<string> cameraStrList;
    for (uint32_t pos = 0; pos < camNum; pos++) {
        cameraStrList.push_back(to_string(cameraList[pos]));
    }
    delete[] cameraList;
    return cameraStrList;
#endif
}

uint8_t CameraService::GetCameraModeNum()
{
    uint8_t num;
    int32_t ret;
#ifdef MEDIA_INTERFACE_V1_0
    ret = localHalCameraDev_->HalCameraGetModeNum(&num);
#else
    ret = HalCameraGetModeNum(&num);
#endif
    if (ret == 0) {
        return num;
    }
    return 0;
}

int32_t CameraService::CreateCamera(std::string &cameraId)
{
    if (!inited_) {
        MEDIA_ERR_LOG("not inited  %s ", cameraId.c_str());
        return MEDIA_ERR;
    }
#ifdef MEDIA_INTERFACE_V1_0
    HalCameraManager *halCameraDev = GetHalCameraDevice(cameraId);
    CameraDevice *device = new (nothrow) CameraDevice(cameraId.c_str(), halCameraDev);
    if (device == nullptr) {
        MEDIA_FATAL_LOG("New device object failed.");
        return MEDIA_ERR;
    }
    if (device->Initialize() != MEDIA_OK) {
        MEDIA_FATAL_LOG("device Initialize failed.");
        delete device;
        return MEDIA_ERR;
    }
    deviceMap_.insert(pair<string, pair<CameraDevice*, HalCameraManager*>>(cameraId, {device, halCameraDev}));
    return CameraServiceCallback::CAMERA_STATUS_CREATED;
#else
    int32_t ret = HalCameraDeviceOpen((uint32_t)std::atoi(cameraId.c_str()));
    if (ret != 0) {
        MEDIA_ERR_LOG("HalCameraDeviceOpen failed. ret(%d)", ret);
        return CameraServiceCallback::CAMERA_STATUS_CREATE_FAILED;
    }
    CameraDevice *device = new (nothrow) CameraDevice((uint32_t)std::atoi(cameraId.c_str()));
    if (device == nullptr) {
        MEDIA_FATAL_LOG("New device object failed.");
        return MEDIA_ERR;
    }
    if (device->Initialize() != MEDIA_OK) {
        MEDIA_FATAL_LOG("device Initialize failed.");
        delete device;
        return MEDIA_ERR;
    }
    deviceMap_.insert(pair<string, CameraDevice*>(cameraId, device));
    return CameraServiceCallback::CAMERA_STATUS_CREATED;
#endif
}

int32_t CameraService::CloseCamera(std::string &cameraId)
{
#ifdef MEDIA_INTERFACE_V1_0
    CameraDevice *device = GetCameraDevice(cameraId);
    if (device != NULL) {
        device->StopLoopingCapture(-1);
        deviceMap_.erase(cameraId);
        int32_t ret = device->UnInitialize();
        if (ret != 0) {
            MEDIA_ERR_LOG("UnInitialize failed. ret(%d)", ret);
        }
    }
    MEDIA_INFO_LOG("CameraService:CloseCamera, success");
    return CameraServiceCallback::CAMERA_STATUS_CLOSE;
#else
    CameraDevice *device = GetCameraDevice(cameraId);
    if (device != NULL) {
        device->StopLoopingCapture(-1);
        deviceMap_.erase(cameraId);
    }
    int32_t ret = HalCameraDeviceClose((uint32_t)std::atoi(cameraId.c_str()));
    if (ret != 0) {
        MEDIA_ERR_LOG("HalCameraDeviceClose failed. ret(%d)", ret);
    }
    return CameraServiceCallback::CAMERA_STATUS_CLOSE;
#endif
}

int32_t CameraService::SetCameraMode(uint8_t modeIndex)
{
    CodecDeinit();
    int32_t ret;
#ifdef MEDIA_INTERFACE_V1_0
    ret = localHalCameraDev_->HalCameraSetMode(modeIndex);
#else
    ret = HalCameraSetMode(modeIndex);
#endif
    CodecInit();
    return ret;
}

void CameraService::RegCameraServiceCallback(CameraServiceCallback *callback)
{
    cameraServiceCb_ = callback;
}

} // namespace Media
} // namespace OHOS
