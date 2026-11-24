#pragma once
#include <QByteArray>
#include <QtEndian>
#include <algorithm>
#include "live_jpeg_rows.h"
#include "lz4.h"

namespace row_fixture {
// Valid 4:2:0 entropy for constant luma/chroma. In the alternate row, the
// first Y DC difference raises the predictor by four until the next restart.
inline QByteArray compact(int width, int alternate = -1) {
    QByteArray panel("R8J1\x01\x50", 6);
    char le[2];
    qToLittleEndian(quint16(width), le);
    panel.append(le, 2);
    for (int i = 0; i < 15; ++i) {
        if (i == alternate) {
            panel += QByteArray::fromHex("928a28a0");
            panel += QByteArray::fromHex("028a28a0").repeated(width / 16 - 1);
            panel += char(0x0f);
        } else panel += QByteArray::fromHex("28a28a00").repeated(width / 16);
        panel += char(0xff);
        panel += char(0xd0 + i % 8);
    }
    panel += QByteArray::fromHex("ffd9");
    return panel;
}
inline QByteArray key(QByteArray compact) {
    compact.replace(0, 4, "R17K");
    return compact;
}
inline QByteArray delta(const QByteArray &base, const QByteArray &current, quint32 baseSeq) {
    const auto *src = reinterpret_cast<const uint8_t *>(current.constData());
    const auto *ref = reinterpret_cast<const uint8_t *>(base.constData());
    LiveJpegRows rows{}, refRows{};
    if (!live_jpeg_rows_parse(&rows, src, current.size()) || !live_jpeg_rows_parse(&refRows, ref, base.size())) return {};
    QByteArray out(current.size(), Qt::Uninitialized);
    auto size = live_jpeg_rows_pack(reinterpret_cast<uint8_t *>(out.data()), out.size(), src, current.size(), &rows,
                                    ref, base.size(), &refRows, baseSeq);
    out.truncate(qsizetype(size));
    return out;
}
inline QByteArray dictionary(const QByteArray &base, const QByteArray &inner, quint32 baseSeq) {
    LZ4_stream_t stream{};
    const int n = std::min<int>(32768, int(base.size()) - 8);
    LZ4_loadDict(&stream, base.constData() + base.size() - n, n);
    QByteArray envelope(20 + LZ4_compressBound(int(inner.size())), Qt::Uninitialized);
    memcpy(envelope.data(), "R20D", 4);
    memcpy(envelope.data() + 4, base.constData() + 4, 4);
    qToLittleEndian(baseSeq, envelope.data() + 8);
    qToLittleEndian(quint32(inner.size()), envelope.data() + 12);
    qToLittleEndian(quint32(0), envelope.data() + 16);
    int bytes = LZ4_compress_fast_continue(&stream, inner.constData(), envelope.data() + 20,
                                          int(inner.size()), int(envelope.size()) - 20, 4);
    if (!bytes) return {};
    envelope.truncate(20 + bytes);
    return envelope;
}
}
