#pragma once
#include <QImage>
#include <QSize>

class Document {
public:
    explicit Document(QSize size);
    QImage render() const;

private:
    QSize size_;
};
