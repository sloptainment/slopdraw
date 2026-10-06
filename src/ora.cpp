#include "ora.h"
#include <functional>
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
        // A Bezier node is "x,y,ix,iy,ox,oy,type", type c(usp), s(mooth) or y (symmetric) (#307).
        for (const BezierNode& n : v.bezier)
            nodes << QStringList{num(n.p.x()), num(n.p.y()), num(n.in.x()), num(n.in.y()), num(n.out.x()), num(n.out.y()),
                                 QString("csy"[int(n.type)])}.join(',');
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

// mydraw/references.xml (#538): one <reference> per Reference, bottom first; src is its PNG beside it, transform the 9 QTransform
// entries row by row, crop x y w h in image pixels.
QByteArray referencesXml(const Document& doc) {
    QByteArray out;
    QXmlStreamWriter w(&out);
    w.writeStartDocument();
    w.writeStartElement("references");
    for (int i = 0; i < int(doc.references().size()); i++) {
        const Reference& r = doc.references()[i];
        const QTransform& t = r.transform;
        w.writeEmptyElement("reference");
        w.writeAttribute("src", QString("reference%1.png").arg(i));
        w.writeAttribute("transform", QStringList{num(t.m11()), num(t.m12()), num(t.m13()), num(t.m21()), num(t.m22()), num(t.m23()),
                                                  num(t.m31()), num(t.m32()), num(t.m33())}.join(' '));
        w.writeAttribute("crop", QStringList{num(r.crop.x()), num(r.crop.y()), num(r.crop.width()), num(r.crop.height())}.join(' '));
        w.writeAttribute("opacity", num(r.opacity));
    }
    w.writeEndDocument();
    return out;
}

// The items at depth d from index i down, top first; a group is a <stack> holding the deeper run below it (#456).
void stackItems(QXmlStreamWriter& w, const Document& doc, int& i, int d) {
    while (i >= 0 && doc.depth(i) == d) {
        int k = i--;
        w.writeStartElement(doc.isGroup(k) ? "stack" : "layer");
        w.writeAttribute("name", doc.name(k));
        if (!doc.isGroup(k)) w.writeAttribute("src", QString("data/layer%1.png").arg(k));
        else w.writeAttribute("isolation", "isolate");
        w.writeAttribute("visibility", doc.visible(k) ? "visible" : "hidden");
        w.writeAttribute("opacity", QString::number(doc.opacity(k), 'g', 17));
        if (!doc.isGroup(k)) {
            w.writeAttribute("x", "0");
            w.writeAttribute("y", "0");
            if (doc.vector(k)) w.writeAttribute(kNs, "svg", QString("data/layer%1.svg").arg(k));
            if (doc.alphaLock(k)) w.writeAttribute(kNs, "alpha-lock", "true");
            if (doc.inheritAlpha(k)) w.writeAttribute(kNs, "inherit-alpha", "true");
        } else stackItems(w, doc, i, d + 1);
        w.writeEndElement();
    }
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
            if (f.size() == 7) {
                v.bezier.push_back({{f[0].toDouble(), f[1].toDouble()}, {f[2].toDouble(), f[3].toDouble()},
                                    {f[4].toDouble(), f[5].toDouble()}, BezierType(QString("csy").indexOf(f[6]))});
                continue;
            }
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
    int top = doc.layerCount() - 1;
    stackItems(w, doc, top, 0);
    w.writeEndDocument();

    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 0)) return false;
    bool ok = add(z, "mimetype", "image/openraster", MZ_NO_COMPRESSION) && add(z, "stack.xml", xml, MZ_DEFAULT_COMPRESSION);
    for (int i = 0; ok && i < doc.layerCount(); i++)
        ok = doc.isGroup(i) || (add(z, QString("data/layer%1.png").arg(i).toUtf8().constData(), png(doc.image(i)), MZ_DEFAULT_COMPRESSION)
          && (!doc.vector(i) || add(z, QString("data/layer%1.svg").arg(i).toUtf8().constData(), svg(doc, i), MZ_DEFAULT_COMPRESSION)));
    for (int i = 0; ok && i < int(doc.references().size()); i++)
        ok = add(z, QString("mydraw/reference%1.png").arg(i).toUtf8().constData(), png(doc.references()[i].image), MZ_DEFAULT_COMPRESSION);
    if (ok && !doc.references().empty()) ok = add(z, "mydraw/references.xml", referencesXml(doc), MZ_DEFAULT_COMPRESSION);
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

// The references in mydraw/references.xml, bottom first (#538); entry reads one zip entry. Files from before #538 have none.
static std::vector<Reference> readReferences(const std::function<QByteArray(const QString&)>& entry) {
    std::vector<Reference> refs;
    QXmlStreamReader rr(entry("mydraw/references.xml"));
    while (!rr.atEnd()) {
        rr.readNext();
        if (!rr.isStartElement() || rr.name() != u"reference") continue;
        auto a = rr.attributes();
        auto t = a.value("transform").split(' ');
        auto c = a.value("crop").split(' ');
        Reference ref;
        if (t.size() != 9 || c.size() != 4 || !ref.image.loadFromData(entry("mydraw/" + a.value("src").toString()))) continue;
        ref.transform = QTransform(t[0].toDouble(), t[1].toDouble(), t[2].toDouble(), t[3].toDouble(), t[4].toDouble(), t[5].toDouble(),
                                   t[6].toDouble(), t[7].toDouble(), t[8].toDouble());
        ref.crop = QRectF(c[0].toDouble(), c[1].toDouble(), c[2].toDouble(), c[3].toDouble());
        ref.opacity = a.value("opacity").toDouble();
        refs.push_back(ref);
    }
    return refs;
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
    struct L { QString name, src, svg; bool visible; double opacity; bool alphaLock, inheritAlpha, group; int depth; };
    std::vector<L> layers;  // top first, as in stack.xml
    QSize size;
    QXmlStreamReader r(entry("stack.xml"));
    int nest = 0;  // <stack>s open: the root is 1, so an item's depth is nest - 1
    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement() && r.name() == u"stack") nest--;
        if (!r.isStartElement()) continue;
        auto a = r.attributes();
        bool vis = a.value("visibility") != u"hidden";
        double op = a.value("opacity").isEmpty() ? 1.0 : a.value("opacity").toDouble();
        if (r.name() == u"image") size = QSize(a.value("w").toInt(), a.value("h").toInt());
        else if (r.name() == u"stack" && nest++ > 0) layers.push_back({a.value("name").toString(), {}, {}, vis, op, false, false, true, nest - 2});
        else if (r.name() == u"layer") layers.push_back({a.value("name").toString(), a.value("src").toString(), a.value(kNs, "svg").toString(), vis, op, a.value(kNs, "alpha-lock") == u"true", a.value(kNs, "inherit-alpha") == u"true", false, nest - 1});
    }
    std::optional<Document> doc;
    if (!r.hasError() && !layers.empty() && !size.isEmpty()) {
        doc.emplace(size);
        for (int i = 0; i < int(layers.size()); i++) {
            const L& l = layers[layers.size() - 1 - i];
            QImage img;
            if ((i == 0 && (l.group || l.depth)) || (!l.group && (!img.loadFromData(entry(l.src)) || img.size() != size))) { doc.reset(); break; }
            if (i > 0) doc->addLayer();
            doc->restoreLayer(i, img, l.name, l.visible, l.opacity, l.alphaLock, l.inheritAlpha, l.depth, l.group);
            if (!l.svg.isEmpty()) doc->restorePaths(i, readVectorSvg(entry(l.svg)));
        }
    }
    if (doc) doc->previewReferences(readReferences(entry));  // no undo step
    mz_zip_reader_end(&z);
    return doc;
}

bool exportPng(const Document& doc, const QString& path) { return doc.render().save(path, "PNG"); }
