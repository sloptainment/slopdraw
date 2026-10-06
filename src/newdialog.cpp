#include "newdialog.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

std::optional<QSize> askNewSize(QWidget* parent) {  // take B of #380: preset list, size fields, orientation, preview
    struct SizeBox : QSpinBox {  // Enter on a bad size does nothing (#383); QSpinBox would restore the old value and let OK fire
        void keyPressEvent(QKeyEvent* e) override {
            if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !hasAcceptableInput()) return;
            QSpinBox::keyPressEvent(e);
        }
        QLineEdit* edit() { return lineEdit(); }  // its textChanged fires on typed edits; QSpinBox's only on valid text
    };
    struct Preset { const char* group; const char* name; int w, h; };
    static const Preset presets[] = {  // #384, #388; paper sizes at 300 ppi (/r/16)
        {"Screen", "720p", 1280, 720}, {"Screen", "1080p", 1920, 1080}, {"Screen", "1440p", 2560, 1440}, {"Screen", "4K UHD", 3840, 2160},
        {"Paper, 300 ppi", "A5", 1748, 2480}, {"Paper, 300 ppi", "A4", 2480, 3508}, {"Paper, 300 ppi", "US Letter", 2550, 3300},
        {"Square", "Square 1024", 1024, 1024}, {"Square", "Square 2048", 2048, 2048},
        {"Other", "1000 × 1500", 1000, 1500}, {"Other", "Golden ratio", 1618, 1000},
    };
    QDialog dlg(parent);
    dlg.setWindowTitle("New");
    auto* list = new QListWidget;
    list->setFixedWidth(220);
    list->setMinimumHeight(18 * 15 + 4);  // every row shows: 11 presets, 4 group headers
    QString group;
    for (auto& p : presets) {
        if (group != p.group) (new QListWidgetItem(group = p.group, list))->setFlags(Qt::NoItemFlags);  // a header row
        QString size = QString("%1 × %2").arg(p.w).arg(p.h);
        auto* it = new QListWidgetItem(size == p.name ? size : QString("%1   %2").arg(p.name, size));
        it->setData(Qt::UserRole, QSize(p.w, p.h));
        list->addItem(it);
    }
    auto* ws = new SizeBox; auto* hs = new SizeBox;
    for (auto* s : {ws, hs}) { s->setRange(1, 16384); s->setSuffix(" px"); }  // cap #383
    auto* swap = new QToolButton; swap->setText("⇄");
    auto* portrait = new QToolButton; portrait->setText("Portrait"); portrait->setCheckable(true);
    auto* landscape = new QToolButton; landscape->setText("Landscape"); landscape->setCheckable(true);
    auto* preview = new QLabel; preview->setFixedSize(160, 140); preview->setAlignment(Qt::AlignCenter);
    auto* info = new QLabel;
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* form = new QGridLayout;
    form->addWidget(new QLabel("Width"), 0, 0); form->addWidget(ws, 0, 1);
    form->addWidget(new QLabel("Height"), 1, 0); form->addWidget(hs, 1, 1); form->addWidget(swap, 1, 2);
    auto* orient = new QHBoxLayout; orient->addWidget(portrait); orient->addWidget(landscape);
    form->addWidget(new QLabel("Orientation"), 2, 0); form->addLayout(orient, 2, 1, 1, 2);
    form->addWidget(preview, 3, 0, 1, 3); form->addWidget(info, 4, 0, 1, 3);
    form->setRowStretch(5, 1);
    auto* body = new QHBoxLayout; body->addWidget(list); body->addLayout(form);
    auto* col = new QVBoxLayout(&dlg); col->addLayout(body); col->addWidget(btns);

    auto sync = [=] {  // fields changed: OK, row highlight, orientation, preview and memory line follow
        bool good = ws->hasAcceptableInput() && hs->hasAcceptableInput();
        btns->button(QDialogButtonBox::Ok)->setEnabled(good);
        QSize sz(ws->value(), hs->value());
        { QSignalBlocker b(list); list->setCurrentRow(-1);
          for (int i = 0; i < list->count(); ++i) if (good && list->item(i)->data(Qt::UserRole).toSize() == sz) list->setCurrentRow(i); }
        portrait->setChecked(good && sz.width() < sz.height()); landscape->setChecked(good && sz.width() >= sz.height());
        QPixmap pm(preview->size()); pm.fill(QColor(42, 42, 42));
        if (good) { QPainter p(&pm); QSize r = sz.scaled(140, 120, Qt::KeepAspectRatio).expandedTo({2, 2}); p.fillRect(QRect(QPoint(80 - r.width() / 2, 70 - r.height() / 2), r), Qt::white); }
        preview->setPixmap(pm);
        info->setText(good ? QString("%1 × %2 px · Background layer %3 MB").arg(sz.width()).arg(sz.height()).arg(sz.width() * double(sz.height()) * 4 / 1048576, 0, 'f', 1) : "");  // RGBA8888 (src/document.cpp:64)
    };
    for (auto* s : {ws, hs}) {  // typing reaches the lineEdit; setValue (a row, swap) only valueChanged
        QObject::connect(s->edit(), &QLineEdit::textChanged, sync);
        QObject::connect(s, &QSpinBox::valueChanged, sync);
    }
    QObject::connect(list, &QListWidget::currentRowChanged, [=](int row) {
        QSize sz = row < 0 ? QSize() : list->item(row)->data(Qt::UserRole).toSize();
        if (sz.isValid()) { ws->setValue(sz.width()); hs->setValue(sz.height()); }
    });
    auto flip = [=] { int w = ws->value(); ws->setValue(hs->value()); hs->setValue(w); };
    QObject::connect(swap, &QToolButton::clicked, flip);
    QObject::connect(portrait, &QToolButton::clicked, [=] { if (ws->value() >= hs->value()) flip(); sync(); });
    QObject::connect(landscape, &QToolButton::clicked, [=] { if (ws->value() < hs->value()) flip(); sync(); });
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QSize last = QSettings().value("newSize", QSize(1920, 1080)).toSize();  // the last size OK'd (#385)
    ws->setValue(last.width()); hs->setValue(last.height());
    sync();
    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    QSize sz(ws->value(), hs->value());
    QSettings().setValue("newSize", sz);
    return sz;
}
