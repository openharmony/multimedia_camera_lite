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
#ifndef OHOS_CAMERA_SERVICE_H
#define OHOS_CAMERA_SERVICE_H

#include "camera_device.h"
#include "camera_service_callback.h"
#include "camera_ability.h"
#include "camera_info_impl.h"

namespace OHOS {
namespace Media {
class CameraService {
public:
    ~CameraService();
    static CameraService *GetInstance();

    void Initialize();
    CameraAbility *GetCameraAbility(std::string &cameraId);
    CameraInfo *GetCameraInfo(std::string &cameraId);
    CameraDevice *GetCameraDevice(std::string &cameraId);
    int32_t CreateCamera(std::string &cameraId);
    int32_t CloseCamera(std::string &cameraId);
    list<std::string> GetCameraIdList();
    uint8_t GetCameraModeNum();
    int32_t SetCameraMode(uint8_t modeIndex);
    void RegCameraServiceCallback(CameraServiceCallback *callback);
    void CameraStatusChange(std::string &cameraId, CameraStatus status);
    void SetCameraFormatRanges(CameraAbility *ability, const std::string &cameraId);
private:
    CameraService();
#ifdef MEDIA_INTERFACE_V1_0
    std::pair<CameraDevice*, HalCameraManager*> GetCameraDeviceInfo(std::string &cameraId);
    HalCameraManager *GetHalCameraDevice(const std::string &cameraId);
#endif
#ifdef MEDIA_INTERFACE_V1_0
    std::map<std::string, std::pair<CameraDevice*, HalCameraManager*>> deviceMap_;
#else
    std::map<std::string, CameraDevice*> deviceMap_;
#endif
    std::map<std::string, CameraAbility*> deviceAbilityMap_;
    std::map<std::string, CameraInfo*> deviceInfoMap_;
    CameraServiceCallback *cameraServiceCb_ = nullptr;
#ifdef MEDIA_INTERFACE_V1_0
    HalCameraManager *localHalCameraDev_ = nullptr;
#endif
    bool inited_ = false;
};
} // namespace Media
} // namespace OHOS

#endif // OHOS_CAMERA_SERVICE_H