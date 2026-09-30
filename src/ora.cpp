#include "ora.h"
#include <QBuffer>
#include <QFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include "miniz.h"

namespace {

QByteArray png(const QImage& img) {
    QByteArray out;
    QBuffer b(&out);
    b.open(QIODevice::WriteOnly);
    img.save(&b, "PNG");
    return out;
}

bool add(mz_zip_archive& z, const char* name, const QByteArray& data, mz_uint level) {
    return mz_zip_writer_add_mem(&z, name, data.constData(), size_t(data.size()), level);
}

}  // namespace

bool writeOra(const Document& doc, const QString& path) {
    QByteArray xml;
    QXmlStreamWriter w(&xml);
    w.writeStartDocument();
    w.writeStartElement("image");
    w.writeAttribute("version", "0.0.3");
    w.writeAttribute("w", QString::number(doc.image(0).width()));
    w.writeAttribute("h", QString::number(doc.image(0).height()));
    w.writeStartElement("stack");
    for (int i = doc.layerCount() - 1; i >= 0; i--) {  // stack.xml lists the top layer first
        w.writeEmptyElement("layer");
        w.writeAttribute("name", doc.name(i));
        w.writeAttribute("src", QString("data/layer%1.png").arg(i));
        w.writeAttribute("visibility", doc.visible(i) ? "visible" : "hidden");
        w.writeAttribute("opacity", QString::number(doc.opacity(i), 'g', 17));
        w.writeAttribute("x", "0");
        w.writeAttribute("y", "0");
    }
    w.writeEndDocument();

    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 0)) return false;
    bool ok = add(z, "mimetype", "image/openraster", MZ_NO_COMPRESSION) && add(z, "stack.xml", xml, MZ_DEFAULT_COMPRESSION);
    for (int i = 0; ok && i < doc.layerCount(); i++)
        ok = add(z, QString("data/layer%1.png").arg(i).toUtf8().constData(), png(doc.image(i)), MZ_DEFAULT_COMPRESSION);
    QImage merged = doc.render();
    ok = ok && add(z, "mergedimage.png", png(merged), MZ_DEFAULT_COMPRESSION)
            && add(z, "Thumbnails/thumbnail.png", png(merged.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation)), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
    if (ok) {
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly) && f.write(static_cast<const char*>(buf), qint64(size)) == qint64(size);
    }
    mz_zip_writer_end(&z);
    return ok;
}

std::optional<Document> readOra(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    QByteArray zip = f.readAll();
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, zip.constData(), size_t(zip.size()), 0)) return std::nullopt;
    auto entry = [&](const QString& name) {
        size_t n = 0;
        void* p = mz_zip_reader_extract_file_to_heap(&z, name.toUtf8().constData(), &n, 0);
        QByteArray out(static_cast<const char*>(p), qsizetype(n));
        mz_free(p);
        return out;
    };
    struct L { QString name, src; bool visible; double opacity; };
    std::vector<L> layers;  // top first, as in stack.xml
    QSize size;
    QXmlStreamReader r(entry("stack.xml"));
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement()) continue;
        auto a = r.attributes();
        if (r.name() == u"image") size = QSize(a.value("w").toInt(), a.value("h").toInt());
        else if (r.name() == u"layer") layers.push_back({a.value("name").toString(), a.value("src").toString(), a.value("visibility") != u"hidden", a.value("opacity").isEmpty() ? 1.0 : a.value("opacity").toDouble()});
    }
    std::optional<Document> doc;
    if (!r.hasError() && !layers.empty() && !size.isEmpty()) {
        doc.emplace(size);
        for (int i = 0; i < int(layers.size()); i++) {
            const L& l = layers[layers.size() - 1 - i];
            QImage img;
            if (!img.loadFromData(entry(l.src)) || img.size() != size) { doc.reset(); break; }
            if (i > 0) doc->addLayer();
            doc->restoreLayer(i, img, l.name, l.visible, l.opacity);
        }
    }
    mz_zip_reader_end(&z);
    return doc;
}

bool exportPng(const Document& doc, const QString& path) { return doc.render().save(path, "PNG"); }
