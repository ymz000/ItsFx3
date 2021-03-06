#ifndef CONVOLUTIONWIDGET_H
#define CONVOLUTIONWIDGET_H

#include <mutex>
#include <vector>
#include <QWidget>
#include <QColor>
#include "gcacorr/convresult.h"

class ConvolutionWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ConvolutionWidget(QWidget *parent = 0);
    void SetConvolution( ConvResult* convolution );
    void SetUnderThreshold( bool is_under_threshold );

private:
    std::mutex mtx;
    ConvResult* conv = nullptr;
    std::vector<QColor> colors;
    std::vector<std::vector<QColor>> conv_paint;
    QSize getFrameSize();
    QSize frameSize;

    std::vector<int> xtr;
    std::vector<int> ytr;
    int lastXSize = 0;
    int lastYSize = 0;
    int stepDeg = 1;
    float xscale = 1.0f;
    float yscale = 1.0f;
    QSize lastFrameSize;
    void recalcTransform();

    bool is_under_threshold;

signals:

public slots:

    // QWidget interface
protected:
    void paintEvent(QPaintEvent *event);
};

#endif // CONVOLUTIONWIDGET_H
