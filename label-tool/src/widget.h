#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <opencv2/core/core.hpp>
#include <pcl/point_types.h>
#include <rtabmap/core/DBDriver.h>
#include <rtabmap/core/Memory.h>
#include <sqlite3.h>

namespace Ui {
class Widget;
}

class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = 0);
    ~Widget();

    void setDbPath(char *);
    bool openDatabase(void);
    void createLabelTable(void);
    bool setSliderRange(void);

    void setLabel(const QString &name);
    QString getLabel() const;

private slots:
    void setSliderValue(int);
    void saveLabel();
    void mousePressEvent(QMouseEvent*);

private:
    Ui::Widget *ui;

    sqlite3 *db;
    rtabmap::DBDriver *dbDriver;
    rtabmap::Memory memory;
    QString dbPath;
    int numImages;

    void showImage(int);
    void addDot(int, int);

    bool convertTo3D(int, int, int);
    bool convert(int imageId, int x, int y, rtabmap::Memory &memory, std::map<int, rtabmap::Transform> &poses, pcl::PointXYZ &pWorld);
    std::map<int, rtabmap::Transform> optimizeGraph(rtabmap::Memory &memory);
};

#endif // WIDGET_H
