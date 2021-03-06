#pragma once

#include "lib/adapter/rtabmap/RTABMapAdapter.h"
#include <QWidget>
#include <opencv2/core/core.hpp>
#include <pcl/point_types.h>
#include <rtabmap/core/DBDriver.h>
#include <rtabmap/core/Memory.h>
#include <sqlite3.h>

namespace Ui {
class Widget;
}

class Widget : public QWidget {
  Q_OBJECT

public:
  explicit Widget(QWidget *parent = 0);
  ~Widget();

  bool init(std::string path);

  QString getLabel() const;

private slots:
  void setSliderValue(int);
  void saveLabel();
  void mousePressEvent(QMouseEvent *);

private:
  void showImage(int imageId);
  void setLabel(const QString &name);
  void showLabel(int x, int y, std::string label);
  void projectPoints(void);
  bool getLabels(std::vector<cv::Point3f> &points,
                 std::vector<std::string> &labels);
  bool getPoint3World(int imageId, int x, int y, pcl::PointXYZ &pWorld);

private:
  std::unique_ptr<Ui::Widget> _ui;

  std::string _path;
  int numImages;
  RTABMapAdapter _adapter;
};
