#ifndef SEARCHPROTOCOL_H
#define SEARCHPROTOCOL_H

#include <stdint.h>
#include <QByteArray>
#include <QString>
#include <QtEndian>

// Protocol version for compatibility checking
#define SEARCH_PROTOCOL_VERSION 2

// Message types
enum SearchMessageType {
    MSG_HELLO = 0,
    MSG_CONFIG = 1,
    MSG_TASK_REQUEST = 2,
    MSG_TASK_ASSIGN = 3,
    MSG_RESULT = 4,
    MSG_RESULTS = 5,  // batch results
    MSG_PROGRESS = 6,
    MSG_DONE = 7,
    MSG_STOP = 8,
    MSG_ERROR = 9,
    MSG_PING = 10,
    MSG_PONG = 11
};

// Helper functions for serialization
static inline void writeUint32(QByteArray& data, uint32_t value) {
    data.append(reinterpret_cast<const char*>(&value), 4);
}

static inline void writeUint64(QByteArray& data, uint64_t value) {
    data.append(reinterpret_cast<const char*>(&value), 8);
}

static inline void writeInt32(QByteArray& data, int32_t value) {
    data.append(reinterpret_cast<const char*>(&value), 4);
}

static inline uint32_t readUint32(const QByteArray& data, int& offset) {
    if (offset + 4 > data.size()) return 0;
    uint32_t value;
    memcpy(&value, data.constData() + offset, 4);
    offset += 4;
    return value;
}

static inline uint64_t readUint64(const QByteArray& data, int& offset) {
    if (offset + 8 > data.size()) return 0;
    uint64_t value;
    memcpy(&value, data.constData() + offset, 8);
    offset += 8;
    return value;
}

static inline int32_t readInt32(const QByteArray& data, int& offset) {
    if (offset + 4 > data.size()) return 0;
    int32_t value;
    memcpy(&value, data.constData() + offset, 4);
    offset += 4;
    return value;
}

static inline void writeString(QByteArray& data, const QString& str) {
    QByteArray utf8 = str.toUtf8();
    writeUint32(data, utf8.size());
    data.append(utf8);
}

static inline QString readString(const QByteArray& data, int& offset) {
    uint32_t len = readUint32(data, offset);
    if (offset + (int)len > data.size()) return QString();
    QString str = QString::fromUtf8(data.constData() + offset, len);
    offset += len;
    return str;
}

#endif // SEARCHPROTOCOL_H

