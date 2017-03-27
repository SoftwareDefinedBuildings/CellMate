#include "lib/data/Label.h"
#include <cassert>

Label::Label(int roomId, const cv::Point3f &point3, const std::string &name)
    : _roomId(roomId), _point3(point3), _name(name) {}

int Label::getRoomId() const { return _roomId; }

const cv::Point3f &Label::getPoint3() const { return _point3; }

const std::string &Label::getName() const { return _name; }