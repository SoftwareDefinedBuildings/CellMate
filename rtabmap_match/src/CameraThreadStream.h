#pragma once

#include "rtabmap/core/RtabmapExp.h" // DLL export/import defines

#include <rtabmap/utilite/UThread.h>
#include <rtabmap/utilite/UEventsSender.h>

namespace rtabmap
{

class Camera;

/**
 * Class CameraThreadStream
 *
 */
class RTABMAP_EXP CameraThreadStream :
    public UThread,
    public UEventsSender
{
public:
    // ownership transferred
    CameraThreadStream(Camera * camera);
    virtual ~CameraThreadStream();

    //getters
    bool isPaused() const {return !this->isRunning();}
    bool isCapturing() const {return this->isRunning();}
    void setImageRate(float imageRate);

    Camera * camera() {return _camera;} // return null if not set, valid until CameraThreadStream is deleted

private:
    virtual void mainLoop();

private:
    Camera * _camera;
};

} // namespace rtabmap
