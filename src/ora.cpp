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

const char* const kNs = "urn:mydraw";

QString num(double v) { return QString::number(v, 'g', 17); }

// The outline as SVG path data; every subpath of a filled shape is closed.
QString pathData(const QPainterPath& p) {
    QString d;
    for (int i = 0; i < p.elementCount(); i++) {
        auto e = p.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) d += QString(d.isEmpty() ? "M%1 %2" : "ZM%1 %2").arg(num(e.x), num(e.y));
        else if (e.type == QPainterPath::LineToElement) d += QString("L%1 %2").arg(num(e.x), num(e.y));
        else if (e.type == QPainterPath::CurveToElement) d += QString("C%1 %2").arg(num(e.x), num(e.y));
        else d += QString(" %1 %2").arg(num(e.x), num(e.y));
    }
    return d + "Z";
}

// data/layerN.svg: one <path> per Vector Path that any viewer draws: d = its outline in the stroke color when the mode strokes,
// else its curve in the fill color. The rest is mydraw's own attributes.
QByteArray svg(const Document& doc, int layer) {
    QByteArray out;
    QXmlStreamWriter w(&out);
    w.writeStartDocument();
    w.writeStartElement("svg");
    w.writeDefaultNamespace("http://www.w3.org/2000/svg");
    w.writeNamespace(kNs, "mydraw");
    w.writeAttribute("width", QString::number(doc.image(0).width()));
    w.writeAttribute("height", QString::number(doc.image(0).height()));
    for (const Document::VPath& v : doc.paths(layer)) {
        w.writeEmptyElement("path");
        bool fill = v.mode == Document::PaintMode::Fill;
        QColor c = fill ? v.fillColor : v.color;
        w.writeAttribute("d", pathData(fill ? Document::curve(v) : Document::outline(v)));
        w.writeAttribute("fill", c.name());
        w.writeAttribute("fill-opacity", num(c.alphaF()));
        QStringList nodes, knots;
        for (const SpiroNode& n : v.nodes) nodes << QString("%1,%2,%3").arg(num(n.x), num(n.y), QChar(char(n.type)));
        for (const WidthKnot& k : v.knots) knots << QString("%1,%2").arg(num(k.t), num(k.width));
        w.writeAttribute(kNs, "nodes", nodes.join(' '));
        w.writeAttribute(kNs, "closed", v.closed ? "1" : "0");
        w.writeAttribute(kNs, "width", num(v.width));
        w.writeAttribute(kNs, "knots", knots.join(' '));
        w.writeAttribute(kNs, "kind", v.kind == Document::PathKind::Bezier ? "bezier" : "spiro");
        w.writeAttribute(kNs, "mode", v.mode == Document::PaintMode::Both ? "both" : v.mode == Document::PaintMode::Fill ? "fill" : "stroke");
        w.writeAttribute(kNs, "fillcolor", v.fillColor.name(QColor::HexArgb));
    }
    w.writeEndDocument();
    return out;
}

}  // namespace

std::vector<Document::VPath> readVectorSvg(const QByteArray& data) {
    std::vector<Document::VPath> paths;
    QXmlStreamReader r(data);
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement() || r.name() != u"path") continue;
        auto a = r.attributes();
        Document::VPath v{{}, a.value(kNs, "closed") == u"1", QColor(a.value("fill").toString()), a.value(kNs, "width").toDouble(), {}};
        v.color.setAlphaF(a.value("fill-opacity").toDouble());
        for (const auto& n : a.value(kNs, "nodes").split(' ', Qt::SkipEmptyParts)) {
            auto f = n.split(',');
            v.nodes.push_back({f[0].toDouble(), f[1].toDouble(), SpiroType(f[2].at(0).toLatin1())});
        }
        for (const auto& k : a.value(kNs, "knots").split(' ', Qt::SkipEmptyParts)) {
            auto f = k.split(',');
            v.knots.push_back({f[0].toDouble(), f[1].toDouble()});
        }
        // Files from before #296 lack these: Spiro, Stroke.
        if (a.value(kNs, "kind") == u"bezier") v.kind = Document::PathKind::Bezier;
        if (a.value(kNs, "mode") == u"fill") v.mode = Document::PaintMode::Fill;
        if (a.value(kNs, "mode") == u"both") v.mode = Document::PaintMode::Both;
        if (a.hasAttribute(kNs, "fillcolor")) v.fillColor = QColor(a.value(kNs, "fillcolor").toString());
        paths.push_back(v);
    }
    return paths;
}


bool writeOra(const Document& doc, const QString& path) {
    QByteArray xml;
    QXmlStreamWriter w(&xml);
    w.writeStartDocument();
    w.writeStartElement("image");
    w.writeNamespace(kNs, "mydraw");
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
        if (doc.vector(i)) w.writeAttribute(kNs, "svg", QString("data/layer%1.svg").arg(i));
    }
    w.writeEndDocument();

    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 0)) return false;
    bool ok = add(z, "mimetype", "image/openraster", MZ_NO_COMPRESSION) && add(z, "stack.xml", xml, MZ_DEFAULT_COMPRESSION);
    for (int i = 0; ok && i < doc.layerCount(); i++)
        ok = add(z, QString("data/layer%1.png").arg(i).toUtf8().constData(), png(doc.image(i)), MZ_DEFAULT_COMPRESSION)
          && (!doc.vector(i) || add(z, QString("data/layer%1.svg").arg(i).toUtf8().constData(), svg(doc, i), MZ_DEFAULT_COMPRESSION));
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
    struct L { QString name, src, svg; bool visible; double opacity; };
    std::vector<L> layers;  // top first, as in stack.xml
    QSize size;
    QXmlStreamReader r(entry("stack.xml"));
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement()) continue;
        auto a = r.attributes();
        if (r.name() == u"image") size = QSize(a.value("w").toInt(), a.value("h").toInt());
        else if (r.name() == u"layer") layers.push_back({a.value("name").toString(), a.value("src").toString(), a.value(kNs, "svg").toString(), a.value("visibility") != u"hidden", a.value("opacity").isEmpty() ? 1.0 : a.value("opacity").toDouble()});
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
            if (!l.svg.isEmpty()) doc->restorePaths(i, readVectorSvg(entry(l.svg)));
        }
    }
    mz_zip_reader_end(&z);
    return doc;
}

bool exportPng(const Document& doc, const QString& path) { return doc.render().save(path, "PNG"); }
