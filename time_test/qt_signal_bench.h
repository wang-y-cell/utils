#pragma once

#include <QObject>
#include <cstdint>

class QtCounter : public QObject {
    Q_OBJECT
public:
    std::uint64_t hits = 0;

public slots:
    void on_hit() { ++hits; }
    void on_hit_arg(int v) {
        hits += static_cast<std::uint64_t>(v);
    }
};

class QtEmitter : public QObject {
    Q_OBJECT
public:
signals:
    void ping();
    void ping_int(int v);
};
