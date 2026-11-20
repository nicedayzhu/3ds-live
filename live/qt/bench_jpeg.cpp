// Compare CPU decoder paths on captured JPEG pairs, including dimension
// validation and an owned RGB888 result in both timings.
#include "imaging.h"
#include <QtCore>
#include <QImageReader>
#include "live_jpeg_wire.h"
using namespace live;

static QImage qtDecode(const QByteArray &bytes, int w) {
    QBuffer input;
    input.setData(bytes);
    input.open(QIODevice::ReadOnly);
    QImageReader header(&input, "jpeg");
    if (header.size() != QSize(w,240)) return {};
    return header.read().convertToFormat(QImage::Format_RGB888);
}

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    if (argc != 3) return 2;
    configureOpenCV();
    QDir input(app.arguments()[1]);
    QFile log(app.arguments()[2]);
    if (!log.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text)) return 2;
    QTextStream rows(&log);
    rows << "screen_pair,qt_ms,opencv_ms,max_rgb_delta,mean_rgb_delta\n";
    const auto names = input.entryList({"*-400.jpg"},QDir::Files);
    if (names.empty()) return 2;
    for (auto &name : names) {
        std::array<QByteArray,2> data;
        std::array<QByteArray,2> packed;
        for (int i=0;i<2;++i) {
            QFile f(input.filePath(name.chopped(8)+(i?"-320.jpg":"-400.jpg")));
            if (!f.open(QIODevice::ReadOnly)) return 3;
            data[i]=f.readAll();
            if (data[i].size() < LIVE_JPEG_FULL_HEADER_SIZE+3) continue;
            packed[i].resize(data[i].size());
            size_t size=0;
            for (int q=1;q<=95 && !size;++q)
                size=live_jpeg_wire_pack(reinterpret_cast<uchar *>(packed[i].data()),packed[i].size(),
                    reinterpret_cast<const uchar *>(data[i].constData()),data[i].size(),i?320:400,q);
            packed[i].resize(size);
        }
        std::array<Images,2> outputs;
        std::array<double,2> total{};
        for (int run=0;run<6;++run)
            for (int index=0;index<2;++index) {
                int method=(run+index)%2;
                QElapsedTimer t;
                t.start();
                for (int i=0;i<2;++i)
                    outputs[method][i]=method?decode(data[i],i?320:400):qtDecode(data[i],i?320:400);
                if (run) total[method]+=t.nsecsElapsed()/1e6;
            }
        qint64 error=0, pixels=0;
        int peak=0;
        for (int i=0;i<2;++i) {
            if (outputs[0][i].isNull() || outputs[1][i].size()!=outputs[0][i].size()) return 4;
            if (!packed[i].isEmpty() && decode(packed[i],i?320:400)!=outputs[1][i]) return 5;
            for (int y=0;y<240;++y) {
                auto a=outputs[0][i].constScanLine(y), b=outputs[1][i].constScanLine(y);
                for (int x=0;x<3*outputs[0][i].width();++x) {
                    int d=std::abs(a[x]-b[x]);
                    peak=std::max(peak,d);
                    error+=d;
                    ++pixels;
                }
            }
        }
        rows << name << ',' << total[0]/5 << ',' << total[1]/5 << ',' << peak << ',' << double(error)/pixels << '\n';
    }
    return 0;
}
