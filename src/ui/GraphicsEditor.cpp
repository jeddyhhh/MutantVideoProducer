#include "core/Brand.h"
#include "core/Effects.h"
#include "core/NodeGraph.h"
#include "ui/GraphicsEditor.h"
#include "core/Graphics.h"
#include "core/GraphicsWorkflow.h"
#include "core/MotionGraphics.h"
#include "core/Profiles.h"
#include "core/Project.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextDocument>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <atomic>
#include <cmath>
#include <future>

namespace {
QJsonValue readValue(QString text)
{
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(("[" + text + "]").toUtf8(), &error);
    return error.error == QJsonParseError::NoError && doc.isArray() && doc.array().size() == 1 ? doc.array().first() : QJsonValue(text);
}
QString writeValue(QJsonValue v)
{
    if (v.isString())
        return v.toString();
    auto bytes = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}
QJsonValue layerProperty(QJsonObject o, QString path)
{
    QJsonValue v = o;
    for (auto key : path.split('.'))
        v = v.toObject()[key];
    return v;
}
QJsonObject setLayerProperty(QJsonObject o, QStringList keys, QJsonValue v)
{
    auto key = keys.takeFirst();
    o[key] = keys.isEmpty() ? v : QJsonValue(setLayerProperty(o[key].toObject(), keys, v));
    return o;
}
QString newId() { return "layer-" + QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonValue shifted(QJsonValue value, double amount)
{
    if (value.isObject()) {
        auto spec = value.toObject();
        if (spec.contains("keyframes")) {
            auto keys = spec["keyframes"].toArray();
            for (int i = 0; i < keys.size(); ++i) {
                auto key = keys[i].toArray();
                key[1] = key[1].toDouble() + amount;
                keys[i] = key;
            }
            spec["keyframes"] = keys;
        } else if (spec.contains("audio"))
            spec["base"] = spec["base"].toDouble() + amount;
        else if (spec.contains("variable"))
            spec["offset"] = spec["offset"].toDouble() + amount;
        return spec;
    }
    return value.toDouble() + amount;
}
QJsonObject pickAnimationAudio(QWidget* parent, Project* project, double duration, double eventStart, QString* name);
class KeyStrip : public QWidget {
public:
    QJsonArray keys;
    double duration = 10, time = 0;
    int dragging = -1;
    std::function<void(double)> seek;
    std::function<void(double, double)> moveKey;
    KeyStrip()
    {
        setMinimumHeight(60);
        setObjectName("graphicsKeyTimeline");
    }
    double x(double t) const { return 18 + std::clamp(t / duration, 0., 1.) * std::max(1, width() - 36); }
    double seconds(double px) const { return std::clamp((px - 18) / std::max(1, width() - 36) * duration, 0., duration); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor("#152536"));
        p.setPen(QColor("#7293ae"));
        p.drawLine(18, 35, width() - 18, 35);
        for (int i = 0; i <= 10; ++i) {
            double t = duration * i / 10;
            p.drawLine(QPointF(x(t), 28), QPointF(x(t), 43));
            p.drawText(QRectF(x(t) - 20, 0, 40, 22), Qt::AlignCenter, QString::number(t, 'g', 3));
        }
        p.setPen(QColor("#53e3d0"));
        p.drawLine(QPointF(x(time), 21), QPointF(x(time), height()));
        for (int i = 0; i < keys.size(); ++i) {
            double px = x(keys[i].toArray()[0].toDouble());
            p.setBrush(QColor(i == dragging ? "#ffffff" : "#ffb35c"));
            p.setPen(Qt::NoPen);
            p.drawPolygon(QPolygonF{{px, 28}, {px + 7, 35}, {px, 42}, {px - 7, 35}});
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        dragging = -1;
        for (int i = 0; i < keys.size(); ++i)
            if (std::abs(x(keys[i].toArray()[0].toDouble()) - e->position().x()) < 9)
                dragging = i;
        if (seek)
            seek(seconds(e->position().x()));
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton) {
            if (dragging < 0) {
                if (seek)
                    seek(seconds(e->position().x()));
            } else {
                time = seconds(e->position().x());
                update();
            }
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        int i = dragging;
        dragging = -1;
        if (i >= 0 && i < keys.size() && moveKey)
            moveKey(keys[i].toArray()[0].toDouble(), seconds(e->position().x()));
        update();
    }
};
class Canvas : public QWidget {
public:
    QJsonObject scene;
    int selected = -1;
    std::function<void(int)> select;
    std::function<void(QPointF, bool)> drag;
    QPointF previous;
    bool resizing = false, pressed = false;
    bool bypassSnap = false;
    QStringList selectedIds;
    QVector<double> xGuides, yGuides;
    mutable QJsonObject boundsScene;
    mutable double boundsTime = -1;
    mutable QSizeF boundsSize;
    mutable QHash<QString, QRectF> cachedBounds;
    double time = 0;
    QSize frame{1280, 720}; // the preview picture, in the project's shape (4:3 and vertical too)
    bool editPath = false;
    int handle = -1;
    std::function<void(int, int, QPointF)> pathDrag;
    Canvas()
    {
        setMinimumSize(560, 340);
        setObjectName("graphicsCanvas");
        setFocusPolicy(Qt::StrongFocus);
    }
    QRectF picture() const
    {
        QSizeF s(frame);
        s.scale(QSizeF(width(), height()), Qt::KeepAspectRatio);
        return {(width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height()};
    }
    QRectF bounds(const QJsonObject& l) const
    {
        auto r = picture();
        if (scene["version"].toInt() == 2 && !l["id"].toString().isEmpty()) {
            if (boundsScene != scene || boundsTime != time || boundsSize != r.size()) {
                cachedBounds = motion::allBounds(scene, time, r.size());
                boundsScene = scene;
                boundsTime = time;
                boundsSize = r.size();
            }
            return cachedBounds.value(l["id"].toString()).translated(r.topLeft());
        }
        return {r.left() + motion::evaluate(l["x"], time, scene).toDouble() * r.width(),
                r.top() + motion::evaluate(l["y"], time, scene).toDouble() * r.height(),
                motion::evaluate(l["width"], time, scene).toDouble(.3) * r.width(),
                std::max(3., motion::evaluate(l["height"], time, scene).toDouble(.1) * r.height())};
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor("#161a22"));
        auto r = picture();
        for (int y = int(r.top()); y < r.bottom(); y += 18)
            for (int x = int(r.left()); x < r.right(); x += 18)
                p.fillRect(QRect(x, y, 18, 18), ((x / 18 + y / 18) % 2) ? QColor("#303744") : QColor("#222936"));
        QImage img(frame, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        graphics::drawScene(img, scene, time);
        p.drawImage(r, img);
        p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::DashLine));
        p.drawRect(r.adjusted(r.width() * .05, r.height() * .05, -r.width() * .05, -r.height() * .05));
        p.setPen(QPen(QColor("#ffb35c"), 1, Qt::DashLine));
        for (auto x : xGuides)
            p.drawLine(QPointF(r.left() + x * r.width(), r.top()), QPointF(r.left() + x * r.width(), r.bottom()));
        for (auto y : yGuides)
            p.drawLine(QPointF(r.left(), r.top() + y * r.height()), QPointF(r.right(), r.top() + y * r.height()));
        auto a = scene["layers"].toArray();
        if (selected >= 0 && selected < a.size()) {
            auto b = bounds(a[selected].toObject());
            p.setPen(QPen(QColor("#66dddf"), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(b);
            p.fillRect(QRectF(b.bottomRight() - QPointF(5, 5), QSizeF(10, 10)), QColor("#66dddf"));
            if (editPath) {
                auto hs = motion::handles(scene, a[selected].toObject()["id"].toString(), time, r.size());
                p.setPen(QPen(QColor("#ffb35c"), 1, Qt::DashLine));
                for (int i = 0; i < hs.size(); ++i) {
                    auto at = hs[i].position + r.topLeft();
                    if (i)
                        p.drawLine(hs[i - 1].position + r.topLeft(), at);
                    p.setBrush(QColor("#ffb35c"));
                    p.drawEllipse(at, 5, 5);
                }
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        setFocus();
        auto a = scene["layers"].toArray();
        previous = e->position();
        resizing = false;
        pressed = false;
        handle = -1;
        if (editPath && selected >= 0 && selected < a.size()
            && !graphics_workflow::locked(scene, a[selected].toObject()["id"].toString())) {
            auto hs = motion::handles(scene, a[selected].toObject()["id"].toString(), time, picture().size());
            for (int i = 0; i < hs.size(); ++i)
                if (QLineF(hs[i].position + picture().topLeft(), previous).length() < 10) {
                    handle = i;
                    pressed = true;
                    return;
                }
        }
        if (selected >= 0 && selected < a.size() && !graphics_workflow::locked(scene, a[selected].toObject()["id"].toString())
            && QRectF(bounds(a[selected].toObject()).bottomRight() - QPointF(8, 8), QSizeF(16, 16)).contains(previous)) {
            resizing = true;
            pressed = true;
            return;
        }
        for (int i = a.size() - 1; i >= 0; --i)
            if (a[i].toObject()["visible"].toBool(true) && !graphics_workflow::locked(scene, a[i].toObject()["id"].toString())
                && bounds(a[i].toObject()).contains(previous)) {
                if (select)
                    select(i);
                pressed = true;
                break;
            }
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        bypassSnap = e->modifiers().testFlag(Qt::AltModifier);
        if (!pressed || !(e->buttons() & Qt::LeftButton))
            return;
        auto r = picture();
        auto delta = e->position() - previous;
        previous = e->position();
        if (handle >= 0 && selected >= 0 && selected < scene["layers"].toArray().size()) {
            auto a = scene["layers"].toArray();
            auto id = a[selected].toObject()["id"].toString();
            auto hs = motion::handles(scene, id, time, r.size());
            if (handle < hs.size() && pathDrag)
                pathDrag(hs[handle].command, hs[handle].coordinate,
                         motion::editDelta(scene, id, time, r.size(), {delta.x() / r.width(), delta.y() / r.height()}, true));
            return;
        }
        if (drag)
            drag({delta.x() / r.width(), delta.y() / r.height()}, resizing);
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        pressed = false;
        xGuides.clear();
        yGuides.clear();
        update();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        QPointF delta;
        if (e->key() == Qt::Key_Left)
            delta.setX(-1);
        else if (e->key() == Qt::Key_Right)
            delta.setX(1);
        else if (e->key() == Qt::Key_Up)
            delta.setY(-1);
        else if (e->key() == Qt::Key_Down)
            delta.setY(1);
        else {
            QWidget::keyPressEvent(e);
            return;
        }
        if (e->modifiers().testFlag(Qt::ShiftModifier))
            delta *= 10;
        bypassSnap = true;
        if (drag)
            drag({delta.x() / frame.width(), delta.y() / frame.height()}, false);
        bypassSnap = false;
        e->accept();
    }
};
class Designer : public QDialog {
public:
    QJsonObject data;
    QListWidget* list;
    Canvas* canvas;
    QLineEdit *name, *path;
    QPlainTextEdit* words;
    QFontComboBox* font;
    QCheckBox *bold, *visible, *fixed;
    QComboBox* align;
    QPushButton *fill, *stroke;
    std::map<QString, QDoubleSpinBox*> spins;
    QCheckBox* sceneBlur = nullptr;
    QDoubleSpinBox *sceneShutter = nullptr, *sceneDepth = nullptr, *sceneSpeakerZoom = nullptr;
    QCheckBox* sceneSpeaker = nullptr;
    bool loading = false;
    Project* project = nullptr;
    double duration = 10, currentTime = 0, eventStart = -1; // (eventStart: where the event is on the timeline)
    QCheckBox *record = nullptr, *pathEditing = nullptr;
    QComboBox *propertyBox = nullptr, *easeBox = nullptr, *targetBox = nullptr, *parentBox = nullptr, *followBox = nullptr;
    QLineEdit *keyValue = nullptr, *bezier = nullptr;
    QTableWidget* keyTable = nullptr;
    KeyStrip* keyStrip = nullptr;
    // Follow audio: a property driven by a binding's sound (a mouth's scale_y by a voice track), without editing JSON.
    QComboBox *audioName = nullptr, *audioChannel = nullptr;
    QDoubleSpinBox *audioBase = nullptr, *audioAmount = nullptr, *audioAttack = nullptr, *audioRelease = nullptr;
    QLabel* audioState = nullptr;
    QPlainTextEdit *sceneJson = nullptr, *layerJson = nullptr;
    QDoubleSpinBox* seconds = nullptr;
    QSlider* scrub = nullptr;
    QTimer* playTimer = nullptr;
    std::vector<std::function<void(QJsonObject)>> refreshers;
    QString layerDraftId;
    QCheckBox* snapping = nullptr;
    QComboBox* layoutReference = nullptr;
    QComboBox* brandRole = nullptr;
    QWidget* propertyTabs = nullptr;
    bool animation = true; // (false: a Graphics Overlay, with only the text and graphics tools)
    Designer(QWidget* parent, QJsonObject scene, Project* project = nullptr, double duration = 10, double eventStart = -1,
             bool animation = true)
        : QDialog(parent), data(std::move(scene)), project(project), duration(std::clamp(duration, .1, 3600.)), eventStart(eventStart),
          animation(animation)
    {
        if (parent && parent->testAttribute(Qt::WA_DontShowOnScreen))
            setAttribute(Qt::WA_DontShowOnScreen);
        setWindowTitle(animation ? tr("Animation Composer") : tr("Graphics Overlay Composer"));
        setObjectName("graphicsComposer");
        resize(1380, 860);
        normalize();
        auto* root = new QVBoxLayout(this);
        auto* top = new QHBoxLayout;
        auto* presets = new QComboBox;
        presets->setObjectName("graphicsPresets");
        presets->addItem(tr("Choose a starting layout…"), QString());
        if (animation)
            presets->addItem("motion_blank", "motion_blank");
        for (auto id : graphics::presets())
            if (id.startsWith("motion_") == animation)
                presets->addItem(id, id);
        top->addWidget(new QLabel(tr("Preset:")));
        top->addWidget(presets);
        auto* openTemplate = new QPushButton(tr("Open template…"));
        auto* saveTemplate = new QPushButton(tr("Save template…"));
        top->addWidget(openTemplate);
        top->addWidget(saveTemplate);
        connect(openTemplate, &QPushButton::clicked, this, [this] { openScene(); });
        connect(saveTemplate, &QPushButton::clicked, this, [this] { saveScene(); });
        top->addWidget(new QLabel(tr("Drag to move. Drag the corner to resize. "
                                     "Dashed guides show title-safe margins.")),
                       1);
        root->addLayout(top);
        installWorkflow(root);
        auto* row = new QHBoxLayout;
        root->addLayout(row, 1);
        auto* left = new QVBoxLayout;
        left->addWidget(new QLabel(tr("Layers — bottom to top")));
        list = new QListWidget;
        list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        list->setObjectName("graphicsLayers");
        left->addWidget(list, 1);
        auto* add = new QComboBox;
        add->addItems({"text", "rect", "ellipse", "line", "image"});
        if (animation)
            add->addItems({"video", "noise", "flash", "path", "group", "chart", "particles", "repeater", "instance"});
        left->addWidget(add);
        auto* addButton = new QPushButton(tr("Add layer"));
        addButton->setObjectName("graphicsAddLayer");
        left->addWidget(addButton);
        auto* groupButton = new QPushButton(tr("Group selected layers"));
        groupButton->setObjectName("graphicsGroup");
        left->addWidget(groupButton);
        connect(groupButton, &QPushButton::clicked, this, [this] { groupSelection(); });
        auto* symbolButton = new QPushButton(tr("Make reusable object"));
        symbolButton->setObjectName("graphicsMakeSymbol");
        left->addWidget(symbolButton);
        connect(symbolButton, &QPushButton::clicked, this, [this] { makeSymbol(); });
        auto* characterButton = new QPushButton(tr("Add character..."));
        characterButton->setObjectName("graphicsAddCharacter");
        left->addWidget(characterButton);
        connect(characterButton, &QPushButton::clicked, this, [this] { addCharacter(); });
        auto* saveCharacterButton = new QPushButton(tr("Save as character..."));
        saveCharacterButton->setObjectName("graphicsSaveCharacter");
        left->addWidget(saveCharacterButton);
        connect(saveCharacterButton, &QPushButton::clicked, this, [this] { saveAsCharacter(); });
        auto* propButton = new QPushButton(tr("Add prop..."));
        propButton->setObjectName("graphicsAddProp");
        left->addWidget(propButton);
        connect(propButton, &QPushButton::clicked, this, [this] { addProp(); });
        auto* setButton = new QPushButton(tr("Add set..."));
        setButton->setObjectName("graphicsAddSet");
        left->addWidget(setButton);
        connect(setButton, &QPushButton::clicked, this, [this] { addSet(); });
        auto* effectButton = new QPushButton(tr("Effect on this layer..."));
        effectButton->setObjectName("graphicsLayerEffect");
        left->addWidget(effectButton);
        connect(effectButton, &QPushButton::clicked, this, [this] { layerEffect(); });
        for (auto label : {tr("Duplicate"), tr("Delete"), tr("Raise"), tr("Lower")}) {
            auto* button = new QPushButton(label);
            left->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, label] {
                if (!editableSelection())
                    return;
                int i = layerRow();
                auto a = data["layers"].toArray();
                if (i < 0)
                    return;
                QSet<QString> subtree;
                if (data["version"].toInt() == 2) {
                    subtree.insert(a[i].toObject()["id"].toString());
                    bool changed = true;
                    while (changed) {
                        changed = false;
                        for (auto item : a) {
                            auto node = item.toObject();
                            auto id = node["id"].toString();
                            if (subtree.contains(node["parent"].toString()) && !subtree.contains(id)) {
                                subtree.insert(id);
                                changed = true;
                            }
                        }
                    }
                }
                if (label == tr("Delete")) {
                    for (auto id : subtree)
                        if (graphics_workflow::locked(data, id)) {
                            QMessageBox::information(this, tr("Locked layers"), tr("Unlock the group's locked layers before deleting it."));
                            return;
                        }
                }
                if (label == tr("Duplicate")) {
                    if (a.size() + std::max(1, int(subtree.size())) > (data["version"].toInt() == 2 ? motion::kMaxLayers : 128))
                        return;
                    QHash<QString, QString> ids;
                    for (auto id : subtree)
                        ids[id] = newId();
                    QJsonArray copies;
                    for (int index = 0; index < a.size(); ++index) {
                        auto copy = a[index].toObject();
                        auto id = copy["id"].toString();
                        if (index != i && !subtree.contains(id))
                            continue;
                        if (ids.contains(id))
                            copy["id"] = ids[id];
                        if (ids.contains(copy["parent"].toString()))
                            copy["parent"] = ids[copy["parent"].toString()];
                        if (ids.contains(copy["mask"].toString()))
                            copy["mask"] = ids[copy["mask"].toString()];
                        auto follow = copy["follow"].toObject();
                        if (ids.contains(follow["path"].toString())) {
                            follow["path"] = ids[follow["path"].toString()];
                            copy["follow"] = follow;
                        }
                        if (index == i) {
                            copy["name"] = copy["name"].toString() + " copy";
                            QString x = copy.contains("follow") ? "follow.offset_x" : "x";
                            QString y = copy.contains("follow") ? "follow.offset_y" : "y";
                            copy = setLayerProperty(copy, x.split('.'), shifted(layerProperty(copy, x), .02));
                            copy = setLayerProperty(copy, y.split('.'), shifted(layerProperty(copy, y), .02));
                        }
                        copies.append(copy);
                    }
                    ++i;
                    for (int index = 0; index < copies.size(); ++index)
                        a.insert(i + index, copies[index]);
                } else if (label == tr("Delete")) {
                    if (subtree.isEmpty())
                        a.removeAt(i);
                    else {
                        QJsonArray kept;
                        for (auto item : a) {
                            auto node = item.toObject();
                            if (subtree.contains(node["id"].toString()))
                                continue;
                            if (subtree.contains(node["mask"].toString()))
                                node.remove("mask");
                            if (subtree.contains(node["follow"].toObject()["path"].toString()))
                                node.remove("follow");
                            kept.append(node);
                        }
                        a = kept;
                        auto camera = data["camera"].toObject();
                        if (subtree.contains(camera["follow"].toString())) {
                            camera.remove("follow");
                            data["camera"] = camera;
                        }
                    }
                    i = std::min(i, int(a.size()) - 1);
                } else {
                    int to = i + (label == tr("Raise") ? 1 : -1);
                    if (to < 0 || to >= a.size())
                        return;
                    auto v = a.takeAt(i);
                    a.insert(to, v);
                    i = to;
                }
                data["layers"] = a;
                refresh(i);
            });
        }
        row->addLayout(left, 0);
        canvas = new Canvas;
        if (project && project->settings().width > 0 && project->settings().height > 0)
            canvas->frame = QSize(project->settings().width, project->settings().height).scaled(1280, 1280, Qt::KeepAspectRatio);
        row->addWidget(canvas, 1);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setMinimumWidth(250);
        scroll->setMaximumWidth(320);
        auto* formWidget = new QWidget;
        auto* form = new QFormLayout(formWidget);
        scroll->setWidget(formWidget);
        auto* tabs = new QTabWidget;
        propertyTabs = tabs;
        tabs->setMinimumWidth(300);
        tabs->setMaximumWidth(410);
        tabs->addTab(scroll, tr("Layout"));
        row->addWidget(tabs);
        name = new QLineEdit;
        name->setObjectName("graphicsLayerName");
        form->addRow(tr("Name"), name);
        brandRole = new QComboBox;
        brandRole->setObjectName("graphicsBrandRole");
        brandRole->addItems({"None", "primary", "secondary", "accent", "background", "foreground"});
        brandRole->setToolTip(tr("Assign a colour role to use reusable brand kits. Animated fills keep their animation."));
        form->addRow(tr("Brand colour"), brandRole);
        connect(brandRole, &QComboBox::currentTextChanged, this,
                [this](QString role) { change("brand_role", role == "None" ? QString() : role); });
        visible = new QCheckBox(tr("Visible"));
        form->addRow(visible);
        fixed = new QCheckBox(tr("Ignores the camera"));
        fixed->setToolTip(tr("Stays where it is on the picture when the camera zooms, pans or cuts to a speaker (lyrics, captions, logos)"));
        form->addRow(fixed);
        words = new QPlainTextEdit;
        words->setMaximumHeight(105);
        words->setObjectName("graphicsLayerText");
        form->addRow(tr("Text"), words);
        font = new QFontComboBox;
        form->addRow(tr("Font"), font);
        bold = new QCheckBox(tr("Bold"));
        form->addRow(bold);
        align = new QComboBox;
        align->addItems({"left", "center", "right"});
        form->addRow(tr("Text alignment"), align);
        for (auto key : {"x", "y", "width", "height", "font_size", "opacity", "rotation", "radius", "stroke_width", "shading", "contact_shadow",
                         "blur", "depth"}) {
            auto* spin = new QDoubleSpinBox;
            spin->setObjectName("graphics_" + QString(key));
            spin->setDecimals(4);
            spin->setRange(-2, 4);
            spin->setSingleStep(.01);
            if (QString(key) == "rotation")
                spin->setRange(-360, 360);
            else if (QString(key) == "opacity" || QString(key) == "shading" || QString(key) == "contact_shadow" || QString(key) == "depth")
                spin->setRange(0, 1);
            else if (QString(key) == "blur")
                spin->setRange(0, .2);
            else if (QString(key) == "font_size")
                spin->setRange(.001, 1);
            else if (QString(key) == "width" || QString(key) == "height")
                spin->setRange(0, 4);
            spins[key] = spin;
            if (QString(key) == "shading")
                spin->setToolTip(tr("Soft light from the top left: a highlight fading into a shadow (0 to 1)"));
            else if (QString(key) == "contact_shadow")
                spin->setToolTip(tr("A soft shadow on the ground under the layer (0 to 1)"));
            else if (QString(key) == "blur")
                spin->setToolTip(tr("Softens the layer (a fraction of the picture's height)"));
            else if (QString(key) == "depth")
                spin->setToolTip(tr("How far back the layer is: with the camera's depth of field (Scene tab), far layers blur as it zooms in"));
            form->addRow(QString(key).replace('_', ' '), spin);
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, key](double v) { change(key, v); });
        }
        fill = new QPushButton(tr("Choose colour"));
        stroke = new QPushButton(tr("Choose colour"));
        form->addRow(tr("Fill / text"), fill);
        form->addRow(tr("Stroke"), stroke);
        for (auto pair : {std::pair{fill, "fill"}, std::pair{stroke, "stroke"}})
            connect(pair.first, &QPushButton::clicked, this, [this, pair] {
                auto o = current();
                auto c = QColorDialog::getColor(QColor(o[pair.second].toString("#ffffff")), this, tr("Layer colour"),
                                                QColorDialog::ShowAlphaChannel);
                if (c.isValid()) {
                    change(pair.second, c.name(QColor::HexArgb));
                    load();
                }
            });
        path = new QLineEdit;
        form->addRow(tr("Image / video"), path);
        auto* browse = new QPushButton(tr("Choose file…"));
        form->addRow(browse);
        connect(browse, &QPushButton::clicked, this, [this] {
            auto p = current()["type"].toString() == "video"
                         ? QFileDialog::getOpenFileName(this, tr("Video in the scene"), {}, tr("Videos (*.mp4 *.mov *.mkv *.webm *.avi *.m4v)"))
                         : QFileDialog::getOpenFileName(this, tr("Overlay image"), {}, tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.svg)"));
            if (!p.isEmpty())
                path->setText(p);
        });
        connect(name, &QLineEdit::textChanged, this, [this](QString t) { change("name", t); });
        connect(path, &QLineEdit::textChanged, this, [this](QString t) { change("path", t); });
        connect(words, &QPlainTextEdit::textChanged, this, [this] { change("text", words->toPlainText()); });
        connect(font, &QFontComboBox::currentFontChanged, this, [this](QFont f) { change("font", f.family()); });
        connect(bold, &QCheckBox::toggled, this, [this](bool b) { change("bold", b); });
        connect(visible, &QCheckBox::toggled, this, [this](bool b) { change("visible", b); });
        connect(fixed, &QCheckBox::toggled, this, [this](bool b) { change("fixed", b); });
        connect(align, &QComboBox::currentTextChanged, this, [this](QString a) { change("align", a); });
        connect(list, &QListWidget::currentRowChanged, this, [this] { load(); });
        canvas->select = [this](int i) {
            bool additive = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
            if (additive) {
                list->item(i)->setSelected(!list->item(i)->isSelected());
                list->setCurrentRow(i, QItemSelectionModel::NoUpdate);
            } else if (list->item(i)->isSelected())
                list->setCurrentRow(i, QItemSelectionModel::NoUpdate);
            else
                list->setCurrentRow(i);
            load();
        };
        connect(list, &QListWidget::itemSelectionChanged, this, [this] {
            if (!loading)
                load();
        });
        canvas->drag = [this](QPointF delta, bool resize) {
            auto o = current();
            if (o.isEmpty())
                return;
            if (graphics_workflow::locked(data, o["id"].toString()))
                return;
            if (!resize) {
                auto ids = selectionIds();
                auto requested = delta;
                if (snapping->isChecked() && !canvas->bypassSnap) {
                    delta = graphics_workflow::snap(data, ids, delta, currentTime, 8. / std::max(1., canvas->picture().width()),
                                                    &canvas->xGuides, &canvas->yGuides);
                    // Keep the unsnapped pointer offset so slow dragging can escape a guide.
                    canvas->previous += QPointF((delta.x() - requested.x()) * canvas->picture().width(),
                                                (delta.y() - requested.y()) * canvas->picture().height());
                }
                QString error;
                auto result = graphics_workflow::translate(data, ids, delta, currentTime, &error);
                if (result.isEmpty()) {
                    canvas->xGuides.clear();
                    canvas->yGuides.clear();
                    return;
                }
                useWorkflow(result, true);
                return;
            }
            if (data["version"].toInt() == 2)
                delta = motion::editDelta(data, o["id"].toString(), currentTime, canvas->picture().size(), delta);
            if (resize) {
                change("width", std::clamp(motion::evaluate(o["width"], currentTime, data).toDouble(.3) + delta.x(), .005, 4.));
                change("height", std::clamp(motion::evaluate(o["height"], currentTime, data).toDouble(.1) + delta.y(), .005, 4.));
            } else {
                QString x = o.contains("follow") ? "follow.offset_x" : "x", y = o.contains("follow") ? "follow.offset_y" : "y";
                change(x, motion::evaluate(layerProperty(o, x), currentTime, data).toDouble() + delta.x());
                change(y, motion::evaluate(layerProperty(o, y), currentTime, data).toDouble() + delta.y());
            }
            load();
        };
        canvas->pathDrag = [this](int command, int coordinate, QPointF delta) {
            auto a = motion::evaluate(current()["commands"], currentTime, data).toArray();
            if (command >= a.size())
                return;
            auto c = a[command].toArray();
            c[coordinate] = std::clamp(c[coordinate].toDouble() + delta.x(), -4., 4.);
            c[coordinate + 1] = std::clamp(c[coordinate + 1].toDouble() + delta.y(), -4., 4.);
            a[command] = c;
            change("commands", a);
            load();
        };
        connect(addButton, &QPushButton::clicked, this, [this, add] {
            auto a = data["layers"].toArray();
            if (a.size() >= (data["version"].toInt() == 2 ? motion::kMaxLayers : 128))
                return;
            QJsonObject o{{"type", add->currentText()},
                          {"name", add->currentText() + " " + QString::number(a.size() + 1)},
                          {"x", .1},
                          {"y", .1},
                          {"width", .5},
                          {"height", .15},
                          {"fill", "#66dddf"},
                          {"text", "Your text"},
                          {"font_size", .04}};
            o["id"] = newId();
            auto type = add->currentText();
            if (!QStringList{"text", "rect", "ellipse", "line", "image"}.contains(type))
                data["version"] = 2;
            if (type == "noise" || type == "flash") { // (static over the whole picture; a flash every second)
                o["x"] = 0;
                o["y"] = 0;
                o["width"] = 1;
                o["height"] = 1;
                o.remove("text");
                o.remove("fill");
                if (type == "noise")
                    o["style"] = "static";
                else
                    o["times"] = QJsonArray{0.5, 1.5, 2.5};
            }
            if (type == "video") { // (its file: Choose file)
                o["height"] = .5; // (a 16:9 picture in a 16:9 frame)
                o.remove("fill");
                o.remove("text");
                o["loop"] = true;
            }
            if (type == "group") {
                o["x"] = 0;
                o["y"] = 0;
                o["width"] = 1;
                o["height"] = 1;
            }
            if (type == "path") {
                o["height"] = .5;
                o["fill"] = "#00000000";
                o["stroke"] = "#53e3d0";
                o["stroke_width"] = .012;
                o["commands"] = QJsonArray{QJsonArray{"M", 0, .75}, QJsonArray{"C", .25, 0, .75, 1, 1, .25}};
            }
            if (type == "chart") {
                o["height"] = .5;
                o["data"] = QJsonArray{QJsonObject{{"label", "A"}, {"value", 40}}, QJsonObject{{"label", "B"}, {"value", 80}}};
            }
            if (type == "particles") {
                o["height"] = .8;
                o["count"] = 100;
                o["lifetime"] = 4;
                o["speed"] = .25;
            }
            if (type == "repeater") {
                o["width"] = .06;
                o["height"] = .08;
                o["count"] = 8;
                o["columns"] = 8;
                o["spacing_x"] = .1;
                o["children"] =
                    QJsonArray{QJsonObject{{"type", "ellipse"}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}, {"fill", "#53e3d0"}}};
            }
            if (type == "instance") {
                auto symbols = data["symbols"].toObject();
                if (symbols.isEmpty()) {
                    QMessageBox::information(this, tr("Reusable objects"), tr("Select a layer and choose Make reusable object first."));
                    return;
                }
                o["symbol"] = symbols.keys().first();
            }
            a.append(o);
            data["layers"] = a;
            refresh(a.size() - 1);
        });
        connect(presets, qOverload<int>(&QComboBox::activated), this, [this, presets](int i) {
            auto id = presets->itemData(i).toString();
            if (!id.isEmpty()) {
                data = graphics::scene(id);
                normalize();
                refresh(0); // (before seek: its load() read the old layer list's rows in the new, shorter scene, and crashed)
                seek(0);
            }
        });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
        root->addWidget(buttons);
        tabs->setTabVisible(tabs->addTab(motionControls(), tr("Motion")), animation);
        tabs->addTab(animationControls(), tr("Keys"));
        tabs->addTab(sceneControls(), tr("Scene"));
        connect(tabs, &QTabWidget::currentChanged, this, [this](int) { syncJson(); });
        installPlayback(root);
        if (!animation) { // (an overlay keeps to version 1 scenes: these tools make version 2 ones, an Animation's)
            for (auto name : {"graphicsGroup", "graphicsMakeSymbol", "graphicsAddCharacter", "graphicsSaveCharacter", "graphicsAddProp",
                              "graphicsAddSet", "graphicsLayerEffect", "graphicsFollowAudio", "graphicsMoves", "graphicsExpression",
                              "graphicsBindAudio", "graphicsLook", "graphicsImportChart", "graphicsRecordKeys"})
                if (auto* widget = findChild<QWidget*>(name))
                    widget->hide();
            while (targetBox->count() > 1) // (the camera and scene variables)
                targetBox->removeItem(1);
        }
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (!applyLayerJson() || !applyPendingJson())
                return;
            auto e = graphics::validateScene(data);
            if (!e.isEmpty())
                QMessageBox::warning(this, tr("Graphics"), e);
            else
                accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        refresh(0);
    }
    // The current and the selected rows of the layer list, only those the scene has: every lookup of a layer by its row
    // goes through these (the list is rebuilt after the scene changes, and until then a row can outlive its layer, #7).
    int layerRow() const
    {
        const int i = list->currentRow();
        return i >= 0 && i < data["layers"].toArray().size() ? i : -1;
    }
    QList<int> selectedLayerRows() const
    {
        QList<int> rows;
        const qsizetype count = data["layers"].toArray().size();
        for (auto* item : list->selectedItems())
            if (const int r = list->row(item); r >= 0 && r < count)
                rows.append(r);
        return rows;
    }
    QJsonObject current() const
    {
        const int i = layerRow();
        return i >= 0 ? data["layers"].toArray()[i].toObject() : QJsonObject{};
    }
    QStringList selectionIds() const
    {
        QStringList ids;
        const auto layers = data["layers"].toArray();
        for (int r : selectedLayerRows())
            ids.append(layers[r].toObject()["id"].toString());
        return ids;
    }
    bool editableSelection()
    {
        for (auto id : selectionIds())
            if (graphics_workflow::locked(data, id)) {
                QMessageBox::information(this, tr("Locked layers"), tr("Unlock the selected layer or its parent first."));
                return false;
            }
        return true;
    }
    void useWorkflow(QJsonObject result, bool geometry = false)
    {
        if (geometry && record && record->isChecked()) {
            auto keyed = data;
            keyed["version"] = 2;
            auto before = data["layers"].toArray(), after = result["layers"].toArray();
            for (int i = 0; i < before.size(); ++i)
                for (auto property : {"x", "y", "follow.offset_x", "follow.offset_y"}) {
                    auto old = layerProperty(before[i].toObject(), property), value = layerProperty(after[i].toObject(), property);
                    if (old == value)
                        continue;
                    QString error;
                    auto updated = motion::withKeyframe(keyed, before[i].toObject()["id"].toString(), property, currentTime,
                                                        motion::evaluate(value, currentTime, result), easing(), &error);
                    if (updated.isEmpty()) {
                        QMessageBox::warning(this, tr("Layout animation"), error);
                        return;
                    }
                    keyed = updated;
                }
            result = keyed;
        }
        data = result;
        load();
        syncJson();
    }
    void installWorkflow(QVBoxLayout* root)
    {
        ensureIds();
        auto* bar = new QHBoxLayout;
        snapping = new QCheckBox(tr("Snap"));
        snapping->setChecked(true);
        snapping->setObjectName("graphicsSnap");
        snapping->setToolTip(tr("Snap moves to canvas edges, centre, safe margins and other layers. Hold Alt to bypass. Arrow keys nudge; "
                                "Shift nudges 10 pixels."));
        bar->addWidget(snapping);
        layoutReference = new QComboBox;
        layoutReference->addItem(tr("Canvas"), "canvas");
        layoutReference->addItem(tr("Safe margins"), "safe");
        layoutReference->addItem(tr("Selection"), "selection");
        bar->addWidget(layoutReference);
        auto* operation = new QComboBox;
        for (auto pair : {std::pair{"Align left", "left"}, std::pair{"Centre horizontally", "hcenter"}, std::pair{"Align right", "right"},
                          std::pair{"Align top", "top"}, std::pair{"Centre vertically", "vcenter"}, std::pair{"Align bottom", "bottom"},
                          std::pair{"Distribute horizontally", "distribute_h"}, std::pair{"Distribute vertically", "distribute_v"}})
            operation->addItem(tr(pair.first), pair.second);
        operation->setObjectName("graphicsLayoutOperation");
        bar->addWidget(operation);
        auto* apply = new QPushButton(tr("Arrange"));
        apply->setObjectName("graphicsArrange");
        bar->addWidget(apply);
        connect(apply, &QPushButton::clicked, this, [this, operation] {
            QString error;
            auto result = graphics_workflow::layout(data, selectionIds(), operation->currentData().toString(),
                                                    layoutReference->currentData().toString(), currentTime, &error);
            if (result.isEmpty())
                QMessageBox::information(this, tr("Arrange layers"), error);
            else
                useWorkflow(result, true);
        });
        for (bool value : {true, false}) {
            auto* b = new QPushButton(value ? tr("Lock") : tr("Unlock"));
            b->setObjectName(value ? "graphicsLock" : "graphicsUnlock");
            bar->addWidget(b);
            connect(b, &QPushButton::clicked, this, [this, value] {
                auto selected = selectionIds();
                QString error;
                auto result = graphics_workflow::setLocked(data, selected, value, &error);
                if (result.isEmpty()) {
                    QMessageBox::information(this, tr("Layer locks"), error);
                    return;
                }
                data = result;
                int row = list->currentRow();
                refresh(row);
                loading = true;
                for (int i = 0; i < list->count(); ++i)
                    list->item(i)->setSelected(selected.contains(data["layers"].toArray()[i].toObject()["id"].toString()));
                loading = false;
                load();
            });
        }
        auto* brand = new QPushButton(tr("Brand kit…"));
        brand->setObjectName("graphicsBrandKit");
        bar->addWidget(brand);
        connect(brand, &QPushButton::clicked, this, [this] { editBrand(); });
        bar->addStretch();
        root->addLayout(bar);
    }
    void editBrand()
    {
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Reusable brand kit"));
        dialog.setObjectName("graphicsBrandDialog");
        if (testAttribute(Qt::WA_DontShowOnScreen))
            dialog.setAttribute(Qt::WA_DontShowOnScreen);
        auto* root = new QVBoxLayout(&dialog);
        auto* hint = new QLabel(tr("Assign Brand colour roles in Layout. A kit updates tagged static fills and unanimated text fonts; "
                                   "locked layers and animated styles are preserved."));
        hint->setWordWrap(true);
        root->addWidget(hint);
        auto* form = new QFormLayout;
        root->addLayout(form);
        auto* kitName = new QLineEdit;
        kitName->setObjectName("graphicsBrandName");
        form->addRow(tr("Name"), kitName);
        auto* kitFont = new QFontComboBox;
        form->addRow(tr("Font"), kitFont);
        std::map<QString, QPushButton*> colours;
        for (auto role : {"primary", "secondary", "accent", "background", "foreground"}) {
            auto* button = new QPushButton;
            colours[role] = button;
            form->addRow(role, button);
            connect(button, &QPushButton::clicked, &dialog, [&, button] {
                auto c = QColorDialog::getColor(QColor(button->property("colour").toString()), &dialog, tr("Brand colour"),
                                                QColorDialog::ShowAlphaChannel);
                if (c.isValid()) {
                    auto s = c.name(QColor::HexArgb);
                    button->setProperty("colour", s);
                    button->setText(s);
                    button->setStyleSheet("background:" + s);
                }
            });
        }
        auto populate = [&](QJsonObject kit) {
            kitName->setText(kit["name"].toString());
            kitFont->setCurrentFont(QFont(kit["font"].toString()));
            for (auto& [role, button] : colours) {
                auto s = kit[role].toString();
                button->setProperty("colour", s);
                button->setText(s);
                button->setStyleSheet("background:" + s);
            }
        };
        auto kit = [&] {
            QJsonObject result{{"version", 1}, {"name", kitName->text().trimmed()}, {"font", kitFont->currentFont().family()}};
            for (auto& [role, button] : colours)
                result[role] = button->property("colour").toString();
            return result;
        };
        auto initial = data["brand"].toObject();
        if (!graphics_workflow::validateBrand(initial).isEmpty())
            initial = {{"version", 1},           {"name", "My brand"},  {"font", "Segoe UI"},      {"primary", "#53e3d0"},
                       {"secondary", "#679fff"}, {"accent", "#ffb35c"}, {"background", "#08131f"}, {"foreground", "#eff7fc"}};
        populate(initial);
        auto* row = new QHBoxLayout;
        root->addLayout(row);
        auto* open = new QPushButton(tr("Open kit…"));
        auto* save = new QPushButton(tr("Save kit…"));
        row->addWidget(open);
        row->addWidget(save);
        QString folder = templateFolder() + "/Brands";
        connect(open, &QPushButton::clicked, &dialog, [&] {
            auto path = QFileDialog::getOpenFileName(&dialog, tr("Open brand kit"), folder, tr("Brand kits (*.json)"));
            if (path.isEmpty())
                return;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) {
                QMessageBox::warning(&dialog, tr("Brand kit"), tr("Cannot open this kit (maximum 64 KB)."));
                return;
            }
            QJsonParseError parse;
            auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
            auto error = graphics_workflow::validateBrand(doc.object());
            if (parse.error != QJsonParseError::NoError || !error.isEmpty()) {
                QMessageBox::warning(&dialog, tr("Brand kit"), error.isEmpty() ? tr("Invalid kit JSON.") : error);
                return;
            }
            populate(doc.object());
        });
        connect(save, &QPushButton::clicked, &dialog, [&] {
            auto error = graphics_workflow::validateBrand(kit());
            if (!error.isEmpty()) {
                QMessageBox::warning(&dialog, tr("Brand kit"), error);
                return;
            }
            QDir().mkpath(folder);
            auto path = QFileDialog::getSaveFileName(&dialog, tr("Save brand kit"), folder + "/Brand.json", tr("Brand kits (*.json)"));
            if (path.isEmpty())
                return;
            auto bytes = QJsonDocument(kit()).toJson();
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                QMessageBox::warning(&dialog, tr("Brand kit"), tr("Could not save this kit."));
        });
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply kit"));
        root->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted) {
            QString error;
            auto result = graphics_workflow::applyBrand(data, kit(), &error);
            if (result.isEmpty())
                QMessageBox::warning(this, tr("Brand kit"), error);
            else
                useWorkflow(result);
        }
    }
    void normalize()
    {
        if (data["version"].toInt() != 2)
            return;
        QJsonArray out;
        std::function<void(QJsonArray, QString, int)> visit = [&](QJsonArray a, QString parent, int depth) {
            if (depth > 16)
                return;
            for (auto v : a) {
                auto o = v.toObject();
                if (o.value("id").toString().isEmpty())
                    o["id"] = newId();
                if (!parent.isEmpty() && !o.contains("parent"))
                    o["parent"] = parent;
                auto children = o.value("children").toArray();
                if (o.value("type") == "group")
                    o.remove("children");
                out.append(o);
                if (o.value("type") == "group")
                    visit(children, o.value("id").toString(), depth + 1);
            }
        };
        visit(data["layers"].toArray(), {}, 0);
        data["layers"] = out;
    }
    void ensureIds()
    {
        auto a = data["layers"].toArray();
        for (int i = 0; i < a.size(); ++i) {
            auto o = a[i].toObject();
            if (o["id"].toString().isEmpty())
                o["id"] = newId();
            a[i] = o;
        }
        data["layers"] = a;
    }
    QString templateFolder() const
    {
        auto home = Profiles::instance().active().home;
        return home.isEmpty() ? brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Graphics"))
                              : home + "/Media/Graphics Templates";
    }
    bool applyPendingJson()
    {
        if (!sceneJson || !sceneJson->document()->isModified())
            return true;
        QJsonParseError parse;
        auto doc = QJsonDocument::fromJson(sceneJson->toPlainText().toUtf8(), &parse);
        auto error = parse.error != QJsonParseError::NoError || !doc.isObject() ? tr("Enter a scene JSON object.")
                                                                                : graphics::validateScene(doc.object());
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Scene data"), error);
            return false;
        }
        data = doc.object();
        normalize();
        sceneJson->document()->setModified(false);
        refresh(std::max(0, list->currentRow()));
        return true;
    }
    void syncJson()
    {
        if (sceneJson && !sceneJson->document()->isModified()) {
            sceneJson->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson()));
            sceneJson->document()->setModified(false);
        }
        if (layerJson && !layerJson->document()->isModified()) {
            ensureIds();
            layerDraftId = current()["id"].toString();
            layerJson->setPlainText(QString::fromUtf8(QJsonDocument(current()).toJson()));
            layerJson->document()->setModified(false);
        }
    }
    void openScene()
    {
        auto chosen = QFileDialog::getOpenFileName(this, tr("Open graphics template"), templateFolder(), tr("Graphics templates (*.json)"));
        if (chosen.isEmpty())
            return;
        QFile file(chosen);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
            QMessageBox::warning(this, tr("Template"), tr("Cannot open this template (maximum 4 MB)."));
            return;
        }
        QJsonParseError parse;
        auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
        auto error = parse.error != QJsonParseError::NoError || !doc.isObject() ? tr("Invalid JSON template.")
                                                                                : graphics::validateScene(doc.object());
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Template"), error);
            return;
        }
        data = doc.object();
        normalize();
        if (sceneJson)
            sceneJson->document()->setModified(false);
        refresh(0);
        seek(0);
    }
    void saveScene()
    {
        if (!applyLayerJson() || !applyPendingJson())
            return;
        auto error = graphics::validateScene(data);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Template"), error);
            return;
        }
        QDir().mkpath(templateFolder());
        auto chosen = QFileDialog::getSaveFileName(this, tr("Save reusable graphics template"), templateFolder() + "/Animation.json",
                                                 tr("Graphics templates (*.json)"));
        if (chosen.isEmpty())
            return;
        QSaveFile file(chosen);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(data).toJson()) < 0 || !file.commit())
            QMessageBox::warning(this, tr("Template"), tr("Could not save this template."));
    }
    void groupSelection()
    {
        if (!editableSelection())
            return;
        const QList<int> selected = selectedLayerRows();
        if (selected.isEmpty())
            return;
        ensureIds();
        auto a = data["layers"].toArray();
        QString parent = a[selected.first()].toObject()["parent"].toString();
        for (auto index : selected)
            if (a[index].toObject()["parent"].toString() != parent) {
                QMessageBox::information(this, tr("Group layers"), tr("Select layers with the same parent to group them."));
                return;
            }
        QJsonObject group{{"type", "group"}, {"id", newId()}, {"name", "Group"}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}};
        if (!parent.isEmpty())
            group["parent"] = parent;
        for (auto index : selected) {
            auto o = a[index].toObject();
            o["parent"] = group["id"];
            a[index] = o;
        }
        a.append(group);
        data["version"] = 2;
        data["layers"] = a;
        refresh(a.size() - 1);
    }
    // Adds a character from the library (the program's and the user's saved ones) under a name: its voice binding is
    // that name in lower case.
    void addCharacter()
    {
        const auto library = graphics::characterLibrary();
        if (library.empty()) {
            QMessageBox::information(this, tr("Add character"), tr("The character library is missing from this installation."));
            return;
        }
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Add Character"));
        dialog.setObjectName("graphicsCharacterDialog");
        auto* form = new QFormLayout(&dialog);
        auto* pick = new QListWidget;
        pick->setObjectName("graphicsCharacterList");
        for (const auto& c : library)
            pick->addItem(c.builtIn ? c.character["name"].toString() : tr("%1 (saved)").arg(c.character["name"].toString()));
        pick->setCurrentRow(0);
        form->addRow(pick);
        auto* nameEdit = new QLineEdit;
        nameEdit->setObjectName("graphicsCharacterName");
        nameEdit->setPlaceholderText(tr("Their name, e.g. Gus"));
        form->addRow(tr("Name"), nameEdit);
        auto* note = new QLabel(tr("Their mouth moves with the voice bound under their name (lower case): Scene > Bind audio for "
                                   "animation, from their voice track. With the track's words, the mouth takes their shapes."));
        note->setWordWrap(true);
        form->addRow(note);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted || pick->currentRow() < 0)
            return;
        const QJsonObject& character = library[size_t(pick->currentRow())].character;
        const QString who = nameEdit->text().trimmed().isEmpty() ? character["name"].toString() : nameEdit->text().trimmed();
        data["version"] = 2;
        ensureIds();
        const int before = int(data.value("layers").toArray().size());
        QString error;
        const QJsonObject added = motion::withCharacter(data, character, who, .5 - character["width"].toDouble(.14) / 2, .9, &error);
        if (added.isEmpty()) {
            QMessageBox::warning(this, tr("Add character"), error);
            return;
        }
        data = added;
        refresh(before);
        syncJson();
        const QString binding = data.value("layers").toArray()[before].toObject()["id"].toString();
        QMessageBox::information(this, tr("Add character"), tr("%1 is in the scene. Bind their voice track under the name \"%2\" "
                                                               "(Scene > Bind audio for animation) and they talk.").arg(who, binding));
    }
    // Puts a set from the library behind the scene (its front part in front), replacing the set added before.
    void addSet()
    {
        const auto library = graphics::setLibrary();
        if (library.empty()) {
            QMessageBox::information(this, tr("Add set"), tr("The set library is missing from this installation."));
            return;
        }
        QStringList names;
        for (const auto& c : library)
            names << (c.builtIn ? c.character["name"].toString() : tr("%1 (saved)").arg(c.character["name"].toString()));
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, tr("Add Set"), tr("Background (it replaces the set added before)"), names, 0, false, &ok);
        if (!ok)
            return;
        data["version"] = 2;
        ensureIds();
        QString error;
        const QJsonObject added = motion::withSet(data, library[size_t(names.indexOf(pick))].character, &error);
        if (added.isEmpty()) {
            QMessageBox::warning(this, tr("Add set"), error);
            return;
        }
        data = added;
        refresh(std::max(0, layerRow()));
        syncJson();
    }
    // A prop from the library in the selected character's hand: it hangs from the arm and moves with its gestures.
    void addProp()
    {
        const QString who = current()["id"].toString();
        const auto library = graphics::propLibrary();
        if (library.empty() || current()["type"].toString() != "group") {
            QMessageBox::information(this, tr("Add prop"), library.empty() ? tr("The prop library is missing from this installation.")
                                                                           : tr("Select a character's own group first."));
            return;
        }
        QStringList names;
        for (const auto& c : library)
            names << c.character["name"].toString();
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, tr("Add Prop"), tr("Prop for %1").arg(current()["name"].toString(who)), names, 0, false, &ok);
        if (!ok)
            return;
        const QString hand = QInputDialog::getItem(this, tr("Add Prop"), tr("Hand"), {tr("Right"), tr("Left")}, 0, false, &ok);
        if (!ok)
            return;
        QString error;
        const QJsonObject added = motion::withProp(data, library[size_t(names.indexOf(pick))].character, who, hand == tr("Left") ? "L" : "R", &error);
        if (added.isEmpty()) {
            QMessageBox::warning(this, tr("Add prop"), error);
            return;
        }
        data = added;
        refresh(layerRow());
        syncJson();
    }
    // A video effect on the selected layer (or group) only: its "effects", applied to its own picture.
    void layerEffect()
    {
        const int i = layerRow();
        if (i < 0 || graphics_workflow::locked(data, current()["id"].toString()))
            return;
        QStringList names{tr("(remove this layer's effects)")};
        QStringList ids{QString()};
        for (const EffectDef& e : videoEffects())
            if (e.apply) {
                names << e.name;
                ids << e.id;
            }
        for (const EffectDef* e : nodes::userEffects()) // (made in the Effect Builder)
            if (e->apply) {
                names << e->name;
                ids << e->id;
            }
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, tr("Effect on This Layer"),
                                                   tr("Applied to this layer's own picture (its settings: Layer data, \"effects\")"), names, 1,
                                                   false, &ok);
        if (!ok)
            return;
        const QString id = ids[names.indexOf(pick)];
        data["version"] = 2;
        ensureIds();
        auto a = data["layers"].toArray();
        QJsonObject o = a[i].toObject();
        if (id.isEmpty()) {
            o.remove("effects");
        } else {
            QJsonArray effects = o.value("effects").toArray();
            effects.append(QJsonObject{{"type", id}, {"params", QJsonObject{}}});
            o["effects"] = effects;
        }
        a[i] = o;
        data["layers"] = a;
        load();
        syncJson();
    }
    // Saves the selected group (a character) to the user's library.
    void saveAsCharacter()
    {
        const QJsonObject o = current();
        if (o["type"].toString() != "group") {
            QMessageBox::information(this, tr("Save as character"), tr("Select a character's group first."));
            return;
        }
        bool ok = false;
        const QString title = QInputDialog::getText(this, tr("Save as Character"), tr("Character name"), QLineEdit::Normal,
                                                    o["name"].toString(o["id"].toString()), &ok);
        if (!ok)
            return;
        QString error, saved;
        const QJsonObject character = motion::characterFrom(data, o["id"].toString(), title, &error);
        if (character.isEmpty() || !graphics::saveCharacter(character, &saved, &error)) {
            QMessageBox::warning(this, tr("Save as character"), error);
            return;
        }
        QMessageBox::information(this, tr("Save as character"), tr("Saved to your character library:\n%1").arg(QDir::toNativeSeparators(saved)));
    }
    void makeSymbol()
    {
        if (!editableSelection())
            return;
        if (layerRow() < 0)
            return;
        ensureIds();
        auto a = data["layers"].toArray();
        int i = layerRow();
        auto source = a[i].toObject();
        QString id = source["id"].toString(), symbol = "object-" + QString::number(data["symbols"].toObject().size() + 1);
        auto definitions = data["symbols"].toObject();
        while (definitions.contains(symbol))
            symbol += 'x';
        QJsonArray children;
        QSet<QString> taken{id};
        bool more = true;
        while (more) {
            more = false;
            for (auto v : a) {
                auto o = v.toObject();
                if (taken.contains(o["parent"].toString()) && !taken.contains(o["id"].toString())) {
                    taken.insert(o["id"].toString());
                    more = true;
                }
            }
        }
        auto local = source;
        local.remove("parent");
        local.remove("follow");
        local.remove("trail");
        local.remove("motion_blur");
        local["x"] = 0;
        local["y"] = 0;
        local["width"] = 1;
        local["height"] = 1;
        local["rotation"] = 0;
        local["opacity"] = 1;
        local["scale_x"] = 1;
        local["scale_y"] = 1;
        children.append(local);
        for (auto v : a)
            if (v.toObject()["id"] != id && taken.contains(v.toObject()["id"].toString()))
                children.append(v);
        definitions[symbol] = QJsonObject{{"layers", children}};
        data["symbols"] = definitions;
        QJsonObject instance = source;
        instance["type"] = "instance";
        instance["symbol"] = symbol;
        instance.remove("children");
        QJsonArray out;
        int row = 0;
        for (int j = 0; j < a.size(); ++j) {
            auto o = a[j].toObject();
            if (j == i) {
                row = out.size();
                out.append(instance);
            } else if (!taken.contains(o["id"].toString()))
                out.append(o);
        }
        data["version"] = 2;
        data["layers"] = out;
        refresh(row);
    }
    QJsonValue easing() const
    {
        return easeBox && easeBox->currentText() == "Cubic Bezier" ? readValue(bezier->text())
                                                                   : QJsonValue(easeBox ? easeBox->currentText() : "linear");
    }
    QString targetId()
    {
        if (targetBox && targetBox->currentIndex() == 1)
            return "$camera";
        if (targetBox && targetBox->currentIndex() == 2)
            return "$variables";
        ensureIds();
        return current()["id"].toString();
    }
    QJsonObject target() const
    {
        return targetBox && targetBox->currentIndex() == 1 ? data["camera"].toObject()
            : targetBox && targetBox->currentIndex() == 2  ? data["variables"].toObject()
                                                           : current();
    }
    void setKey()
    {
        auto property = propertyBox->currentText();
        auto value = readValue(keyValue->text());
        QString error;
        auto changed = motion::withKeyframe(data, targetId(), property, currentTime, value, easing(), &error);
        if (changed.isEmpty()) {
            QMessageBox::warning(this, tr("Animation key"), error);
            return;
        }
        data = changed;
        load();
    }
    void removeKey(double at = -1)
    {
        QString prop = propertyBox->currentText();
        auto old = layerProperty(target(), prop).toObject();
        auto keys = old["keyframes"].toArray();
        int row = keyTable->currentRow();
        if (at < 0 && row >= 0 && row < keys.size())
            at = keys[row].toArray()[0].toDouble();
        if (at < 0)
            at = currentTime;
        QJsonArray out;
        for (auto k : keys)
            if (std::abs(k.toArray()[0].toDouble() - at) > .000001)
                out.append(k);
        auto currentValue = motion::evaluate(old, currentTime, data);
        old["keyframes"] = out;
        QJsonValue value = out.isEmpty() ? currentValue : QJsonValue(old);
        if (targetBox->currentIndex() == 0) {
            auto a = data["layers"].toArray();
            const int i = layerRow();
            if (i < 0)
                return;
            a[i] = setLayerProperty(current(), prop.split('.'), value);
            data["layers"] = a;
        } else {
            QString key = targetBox->currentIndex() == 1 ? "camera" : "variables";
            data[key] = setLayerProperty(data[key].toObject(), prop.split('.'), value);
        }
        load();
    }
    // The selected layer's property follows the chosen sound (or stops: it keeps the value it has when silent).
    void followAudio(bool on)
    {
        const int i = layerRow();
        const QString prop = propertyBox->currentText();
        if (i < 0 || prop.isEmpty() || targetBox->currentIndex() != 0 || graphics_workflow::locked(data, current()["id"].toString()))
            return;
        if (on && audioName->currentData().toString().isEmpty()) {
            QMessageBox::information(this, tr("Follow audio"), tr("Bind a sound first: Scene > Bind audio for animation... (a voice track, "
                                                                  "for lip sync), then choose it here."));
            return;
        }
        const QJsonValue value = on ? QJsonValue(QJsonObject{{"audio", audioName->currentData().toString()},
                                                             {"channel", audioChannel->currentData().toString()},
                                                             {"base", audioBase->value()},
                                                             {"amount", audioAmount->value()},
                                                             {"attack", audioAttack->value()},
                                                             {"release", audioRelease->value()}})
                                    : QJsonValue(audioBase->value());
        data["version"] = 2;
        ensureIds();
        auto a = data["layers"].toArray();
        a[i] = setLayerProperty(a[i].toObject(), prop.split('.'), value);
        data["layers"] = a;
        load();
        syncJson();
    }
    void refreshAudio()
    {
        if (!audioName)
            return;
        QSignalBlocker blocker(audioName);
        const QString was = audioName->currentData().toString();
        audioName->clear();
        for (const QString& key : data["audio"].toObject().keys())
            audioName->addItem(key, key);
        if (audioName->count() == 0)
            audioName->addItem(tr("(none bound yet)"), QString());
        const QJsonObject spec = layerProperty(target(), propertyBox->currentText()).toObject();
        const QString bound = spec["audio"].toString();
        audioName->setCurrentIndex(std::max(0, audioName->findData(bound.isEmpty() ? was : bound)));
        if (!bound.isEmpty()) {
            audioChannel->setCurrentIndex(std::max(0, audioChannel->findData(spec["channel"].toString("peak"))));
            audioBase->setValue(spec["base"].toDouble());
            audioAmount->setValue(spec["amount"].toDouble(1));
            audioAttack->setValue(spec["attack"].toDouble(.03));
            audioRelease->setValue(spec["release"].toDouble(.12));
        }
        audioState->setText(bound.isEmpty() ? tr("%1 doesn't follow a sound. For a mouth: scale_y, silent 0.1, full volume 2.5, and the "
                                                 "layer's anchor_y at 0 so it opens downwards.")
                                                  .arg(propertyBox->currentText())
                                            : tr("%1 follows \"%2\".").arg(propertyBox->currentText(), bound));
    }
    void refreshKeys()
    {
        if (!propertyBox)
            return;
        refreshAudio();
        auto v = layerProperty(target(), propertyBox->currentText());
        auto evaluated = motion::evaluate(v, currentTime, data);
        if (evaluated.isUndefined() || evaluated.isNull())
            evaluated = 0;
        keyValue->setText(writeValue(evaluated));
        auto keys = v.toObject()["keyframes"].toArray();
        QSignalBlocker blocker(keyTable);
        keyTable->setRowCount(keys.size());
        for (int i = 0; i < keys.size(); ++i) {
            auto a = keys[i].toArray();
            keyTable->setItem(i, 0, new QTableWidgetItem(QString::number(a[0].toDouble(), 'f', 3)));
            keyTable->setItem(i, 1, new QTableWidgetItem(writeValue(a[1])));
            keyTable->setItem(i, 2, new QTableWidgetItem(a.size() > 2 ? writeValue(a[2]) : v.toObject()["easing"].toString("linear")));
        }
        keyStrip->keys = keys;
        keyStrip->time = currentTime;
        keyStrip->duration = duration;
        keyStrip->update();
    }
    void seek(double t)
    {
        currentTime = std::clamp(t, 0., duration);
        if (canvas) {
            canvas->time = currentTime;
            canvas->update();
        }
        if (seconds) {
            QSignalBlocker b(seconds);
            seconds->setValue(currentTime);
        }
        if (scrub) {
            QSignalBlocker b(scrub);
            scrub->setValue(int(currentTime / duration * 10000));
        }
        if (propertyBox)
            load();
    }
    void installPlayback(QVBoxLayout* root)
    {
        auto* bar = new QHBoxLayout;
        auto* play = new QPushButton(tr("Play preview"));
        play->setObjectName("graphicsPlayPreview");
        bar->addWidget(play);
        seconds = new QDoubleSpinBox;
        seconds->setObjectName("graphicsPreviewTime");
        seconds->setRange(0, duration);
        seconds->setDecimals(3);
        seconds->setSuffix(tr(" s"));
        bar->addWidget(seconds);
        scrub = new QSlider(Qt::Horizontal);
        scrub->setRange(0, 10000);
        scrub->setObjectName("graphicsPreviewScrub");
        bar->addWidget(scrub, 1);
        record = new QCheckBox(tr("Record keyframes"));
        record->setObjectName("graphicsRecordKeys");
        bar->addWidget(record);
        root->insertLayout(root->count() - 1, bar);
        connect(seconds, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double t) { seek(t); });
        connect(scrub, &QSlider::valueChanged, this, [this](int p) { seek(p / 10000. * duration); });
        playTimer = new QTimer(this);
        playTimer->setInterval(33);
        connect(playTimer, &QTimer::timeout, this, [this] { seek(currentTime + .033 >= duration ? 0 : currentTime + .033); });
        connect(play, &QPushButton::clicked, this, [this, play] {
            if (playTimer->isActive()) {
                playTimer->stop();
                play->setText(tr("Play preview"));
            } else {
                playTimer->start();
                play->setText(tr("Pause preview"));
            }
        });
    }
    QWidget* animationControls()
    {
        auto* w = new QWidget;
        auto* layout = new QVBoxLayout(w);
        auto* form = new QFormLayout;
        layout->addLayout(form);
        targetBox = new QComboBox;
        targetBox->addItems({tr("Selected layer"), tr("Scene camera"), tr("Scene variables")});
        form->addRow(tr("Animate"), targetBox);
        propertyBox = new QComboBox;
        propertyBox->setEditable(true);
        propertyBox->setObjectName("graphicsAnimationProperty");
        propertyBox->addItems({"x", "y", "width", "height", "scale_x", "scale_y", "rotation", "opacity", "fill", "stroke", "font_size",
                               "text", "trim_end", "follow.progress", "mask.width", "gradient.x1", "count", "speed"});
        form->addRow(tr("Property"), propertyBox);
        keyValue = new QLineEdit;
        keyValue->setObjectName("graphicsKeyValue");
        form->addRow(tr("Value"), keyValue);
        easeBox = new QComboBox;
        easeBox->setObjectName("graphicsKeyEasing");
        easeBox->addItems({"linear", "hold", "ease_in", "ease_out", "ease_in_out", "smooth", "bounce", "elastic", "Cubic Bezier"});
        form->addRow(tr("Easing to next key"), easeBox);
        bezier = new QLineEdit("[0.25,0.1,0.25,1]");
        form->addRow(tr("Bezier curve"), bezier);
        auto* add = new QPushButton(tr("Set key at playhead"));
        add->setObjectName("graphicsSetKey");
        layout->addWidget(add);
        connect(add, &QPushButton::clicked, this, [this] { setKey(); });
        auto* remove = new QPushButton(tr("Delete selected key"));
        layout->addWidget(remove);
        connect(remove, &QPushButton::clicked, this, [this] { removeKey(); });
        keyStrip = new KeyStrip;
        layout->addWidget(keyStrip);
        keyStrip->seek = [this](double t) { seek(t); };
        keyStrip->moveKey = [this](double old, double next) {
            auto keys = layerProperty(target(), propertyBox->currentText()).toObject()["keyframes"].toArray();
            for (auto k : keys) {
                auto a = k.toArray();
                if (std::abs(a[0].toDouble() - old) > .000001)
                    continue;
                QJsonObject before = data;
                removeKey(old);
                QString error;
                auto changed = motion::withKeyframe(data, targetId(), propertyBox->currentText(), next, a[1],
                                                    a.size() > 2 ? a[2] : QJsonValue("linear"), &error);
                if (!changed.isEmpty())
                    data = changed;
                else
                    data = before;
                seek(next);
                break;
            }
        };
        keyTable = new QTableWidget(0, 3);
        keyTable->setObjectName("graphicsKeyTable");
        keyTable->setHorizontalHeaderLabels({tr("Seconds"), tr("Value"), tr("Easing")});
        keyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        keyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
        keyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(keyTable, 1);
        connect(keyTable, &QTableWidget::cellClicked, this, [this](int row, int) {
            auto keys = layerProperty(target(), propertyBox->currentText()).toObject()["keyframes"].toArray();
            if (row < keys.size())
                seek(keys[row].toArray()[0].toDouble());
        });
        connect(propertyBox, &QComboBox::currentTextChanged, this, [this] {
            if (!loading)
                refreshKeys();
        });
        connect(targetBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            if (!loading)
                refreshKeys();
        });
        layout->addWidget(new QLabel(tr("Drag diamonds to retime keys. Colours and text are also "
                                        "animatable.\nAdvanced property paths and values are "
                                        "available in Scene data.")));
        auto* follow = new QGroupBox(tr("Follow audio"));
        follow->setObjectName("graphicsFollowAudio");
        auto* ff = new QFormLayout(follow);
        audioName = new QComboBox;
        audioName->setObjectName("graphicsAudioBinding");
        ff->addRow(tr("Sound"), audioName);
        audioChannel = new QComboBox;
        audioChannel->addItem(tr("Loudness (both channels)"), "peak");
        audioChannel->addItem(tr("Left channel"), "left");
        audioChannel->addItem(tr("Right channel"), "right");
        audioChannel->addItem(tr("Beats"), "beat");
        ff->addRow(tr("Follow"), audioChannel);
        auto spin = [](double lo, double hi, double v, int decimals) {
            auto* b = new QDoubleSpinBox;
            b->setRange(lo, hi);
            b->setDecimals(decimals);
            b->setValue(v);
            return b;
        };
        audioBase = spin(-10000, 10000, 0.1, 3);
        audioAmount = spin(-10000, 10000, 2.5, 3);
        audioAttack = spin(0.001, 2, 0.02, 3);
        audioRelease = spin(0.001, 5, 0.1, 3);
        audioBase->setObjectName("graphicsAudioBase");
        audioAmount->setObjectName("graphicsAudioAmount");
        ff->addRow(tr("Value when silent"), audioBase);
        ff->addRow(tr("Added at full volume"), audioAmount);
        ff->addRow(tr("Attack (s)"), audioAttack);
        ff->addRow(tr("Release (s)"), audioRelease);
        auto* followRow = new QHBoxLayout;
        auto* followOn = new QPushButton(tr("Follow"));
        followOn->setObjectName("graphicsAudioFollow");
        auto* followOff = new QPushButton(tr("Stop following"));
        followRow->addWidget(followOn);
        followRow->addWidget(followOff);
        ff->addRow(followRow);
        audioState = new QLabel;
        audioState->setWordWrap(true);
        ff->addRow(audioState);
        layout->addWidget(follow);
        auto* movesBox = new QGroupBox(tr("Moves"));
        movesBox->setObjectName("graphicsMoves");
        auto* mf = new QFormLayout(movesBox);
        auto* move = new QComboBox;
        move->setObjectName("graphicsMove");
        for (const QString& m : motion::moves())
            move->addItem(m, m);
        mf->addRow(tr("Move"), move);
        auto* moveLength = new QDoubleSpinBox;
        moveLength->setRange(0, 60);
        moveLength->setDecimals(2);
        moveLength->setSpecialValueText(tr("Natural"));
        mf->addRow(tr("Length (s)"), moveLength);
        auto* moveAmount = new QDoubleSpinBox;
        moveAmount->setRange(-10, 10);
        moveAmount->setValue(1);
        mf->addRow(tr("Amount"), moveAmount);
        auto* walkTo = new QDoubleSpinBox;
        walkTo->setRange(-1, 2);
        walkTo->setDecimals(3);
        walkTo->setValue(.6);
        mf->addRow(tr("Walk to x"), walkTo);
        auto* addMove = new QPushButton(tr("Add move at the playhead"));
        addMove->setObjectName("graphicsAddMove");
        mf->addRow(addMove);
        auto* moveHint = new QLabel(tr("On a character's own group (point, arms up and shrug turn its arms). Wave: on an arm's group (it turns at its anchor). Blink: on the eyes."));
        moveHint->setWordWrap(true);
        mf->addRow(moveHint);
        layout->addWidget(movesBox);
        auto* moodBox = new QGroupBox(tr("Expression"));
        moodBox->setObjectName("graphicsExpression");
        auto* moodForm = new QFormLayout(moodBox);
        auto* mood = new QComboBox;
        mood->setObjectName("graphicsMood");
        mood->addItems(motion::expressions());
        moodForm->addRow(tr("From the playhead"), mood);
        auto* setMood = new QPushButton(tr("Set expression"));
        setMood->setObjectName("graphicsSetExpression");
        moodForm->addRow(setMood);
        layout->addWidget(moodBox);
        connect(setMood, &QPushButton::clicked, this, [=] {
            if (layerRow() < 0)
                return;
            QString error;
            const QJsonObject changed = motion::withExpression(data, current()["id"].toString(), mood->currentText(), currentTime, &error);
            if (changed.isEmpty()) {
                QMessageBox::information(this, tr("Expression"), error);
                return;
            }
            data = changed;
            load();
            refreshKeys();
            syncJson();
        });
        connect(addMove, &QPushButton::clicked, this, [=] {
            const int i = layerRow();
            if (i < 0 || graphics_workflow::locked(data, current()["id"].toString()))
                return;
            data["version"] = 2;
            ensureIds();
            QString error;
            const QJsonObject moved = motion::withMove(data, current()["id"].toString(), move->currentData().toString(), currentTime,
                                                       moveLength->value(), moveAmount->value(), walkTo->value(), &error);
            if (moved.isEmpty()) {
                QMessageBox::information(this, tr("Moves"), error);
                return;
            }
            data = moved;
            load();
            refreshKeys();
            syncJson();
        });
        connect(followOn, &QPushButton::clicked, this, [this] { followAudio(true); });
        connect(followOff, &QPushButton::clicked, this, [this] { followAudio(false); });
        return w;
    }
    QWidget* motionControls()
    {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto* w = new QWidget;
        auto* form = new QFormLayout(w);
        scroll->setWidget(w);
        parentBox = new QComboBox;
        parentBox->setObjectName("graphicsParentGroup");
        form->addRow(tr("Parent group"), parentBox);
        connect(parentBox, qOverload<int>(&QComboBox::activated), this, [this] {
            change("parent", parentBox->currentData().toString());
            refresh(list->currentRow());
        });
        followBox = new QComboBox;
        followBox->setObjectName("graphicsFollowPath");
        form->addRow(tr("Follow path"), followBox);
        connect(followBox, qOverload<int>(&QComboBox::activated), this, [this] {
            auto id = followBox->currentData().toString();
            change("follow",
                   id.isEmpty()
                       ? QJsonValue(QJsonValue::Undefined)
                       : QJsonValue(QJsonObject{
                             {"path", id},
                             {"orient", true},
                             {"progress",
                              QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 0, "linear"}, QJsonArray{duration, 1, "linear"}}}}}}));
            load();
        });
        pathEditing = new QCheckBox(tr("Edit curve points on canvas"));
        pathEditing->setObjectName("graphicsEditPath");
        form->addRow(pathEditing);
        connect(pathEditing, &QCheckBox::toggled, this, [this](bool on) {
            canvas->editPath = on;
            canvas->update();
        });
        auto* append = new QPushButton(tr("Append path point"));
        form->addRow(append);
        connect(append, &QPushButton::clicked, this, [this] {
            auto a = motion::evaluate(current()["commands"], currentTime, data).toArray();
            if (a.isEmpty())
                return;
            if (a.last().toArray().first() == "Z")
                a.removeLast();
            a.append(QJsonArray{"L", 1, 1});
            change("commands", a);
            load();
        });
        auto* curve = new QPushButton(tr("Curve last segment"));
        form->addRow(curve);
        connect(curve, &QPushButton::clicked, this, [this] {
            auto a = motion::evaluate(current()["commands"], currentTime, data).toArray();
            int i = a.size() - 1;
            if (i < 1)
                return;
            if (a[i].toArray().first() == "Z")
                --i;
            auto last = a[i].toArray(), prev = a[i - 1].toArray();
            if (last.first() != "L" || prev.size() < 3)
                return;
            double x = prev[prev.size() - 2].toDouble(), y = prev.last().toDouble(), ex = last[1].toDouble(), ey = last[2].toDouble();
            a[i] = QJsonArray{"C", x + (ex - x) / 3, y + (ey - y) / 3, x + 2 * (ex - x) / 3, y + 2 * (ey - y) / 3, ex, ey};
            change("commands", a);
            load();
        });
        auto* close = new QPushButton(tr("Open / close path"));
        form->addRow(close);
        connect(close, &QPushButton::clicked, this, [this] {
            auto a = motion::evaluate(current()["commands"], currentTime, data).toArray();
            if (a.isEmpty())
                return;
            if (a.last().toArray().first() == "Z")
                a.removeLast();
            else
                a.append(QJsonArray{"Z"});
            change("commands", a);
            load();
        });
        for (auto key : {"scale_x", "scale_y", "anchor_x", "anchor_y", "trim_start", "trim_end", "count", "seed", "speed", "gravity",
                         "spacing_x", "spacing_y"}) {
            auto* spin = new QDoubleSpinBox;
            spin->setDecimals(3);
            spin->setRange(QString(key) == "count" ? 0 : -100, QString(key) == "count" ? 2048 : 100);
            form->addRow(QString(key).replace('_', ' '), spin);
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, key](double v) { change(key, v); });
            refreshers.push_back([this, spin, key](QJsonObject o) {
                spin->setValue(motion::evaluate(o[key], currentTime, data)
                                   .toDouble(QString(key).startsWith("scale") || QString(key) == "trim_end" ? 1
                                                 : QString(key).startsWith("anchor")                        ? .5
                                                                                                            : 0));
            });
        }
        auto* unit = new QComboBox;
        unit->addItems({"character", "word", "line"});
        auto* style = new QComboBox;
        style->addItems({"none", "fade", "rise", "typewriter", "scale"});
        form->addRow(tr("Text animation"), style);
        form->addRow(tr("Text units"), unit);
        auto textStyle = [this, unit, style] {
            if (loading)
                return;
            change("text_animation",
                   style->currentText() == "none"
                       ? QJsonValue(QJsonValue::Undefined)
                       : QJsonValue(QJsonObject{
                             {"unit", unit->currentText()}, {"style", style->currentText()}, {"stagger", .05}, {"duration", .4}}));
            load();
        };
        connect(unit, qOverload<int>(&QComboBox::activated), this, [textStyle] { textStyle(); });
        connect(style, qOverload<int>(&QComboBox::activated), this, [textStyle] { textStyle(); });
        refreshers.push_back([unit, style](QJsonObject o) {
            auto a = o["text_animation"].toObject();
            unit->setCurrentText(a["unit"].toString("character"));
            style->setCurrentText(a["style"].toString("none"));
            unit->setEnabled(o["type"] == "text");
            style->setEnabled(o["type"] == "text");
        });
        auto* maskBox = new QComboBox;
        maskBox->addItems({tr("None"), "rect", "ellipse"});
        form->addRow(tr("Clip mask"), maskBox);
        connect(maskBox, qOverload<int>(&QComboBox::activated), this, [this, maskBox](int i) {
            change("mask",
                   i == 0 ? QJsonValue(QJsonValue::Undefined)
                          : QJsonValue(QJsonObject{{"type", maskBox->currentText()}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}));
        });
        refreshers.push_back([maskBox](QJsonObject o) {
            auto type = o["mask"].toObject()["type"].toString();
            maskBox->setCurrentIndex(type == "rect" ? 1 : type == "ellipse" ? 2 : 0);
        });
        for (auto key : {"gradient", "shadow", "glow", "trail", "motion_blur"}) {
            auto* check = new QCheckBox(QString(key).replace('_', ' '));
            form->addRow(check);
            connect(check, &QCheckBox::toggled, this, [this, key](bool enabled) {
                if (loading)
                    return;
                QJsonObject value;
                if (QString(key) == "gradient")
                    value = {{"type", "linear"}, {"stops", QJsonArray{QJsonArray{0, "#53e3d0"}, QJsonArray{1, "#679fff"}}}};
                else if (QString(key) == "shadow")
                    value = {{"color", "#000000"}, {"blur", .01}, {"y", .008}, {"opacity", .4}};
                else if (QString(key) == "glow")
                    value = {{"color", "#53e3d0"}, {"blur", .015}, {"opacity", .4}};
                else if (QString(key) == "trail")
                    value = {{"samples", 8}, {"duration", .3}, {"opacity", .3}};
                else
                    value = {{"samples", 5}, {"shutter", .03}};
                change(key, enabled ? QJsonValue(value) : QJsonValue(QJsonValue::Undefined));
                load();
            });
            refreshers.push_back([check, key](QJsonObject o) { check->setChecked(o.contains(key)); });
        }
        auto* chartStyle = new QComboBox;
        chartStyle->addItems({"bar", "line", "donut"});
        form->addRow(tr("Chart style"), chartStyle);
        connect(chartStyle, qOverload<int>(&QComboBox::activated), this,
                [this, chartStyle] { change("style", chartStyle->currentText()); });
        refreshers.push_back([chartStyle](QJsonObject o) {
            chartStyle->setEnabled(o["type"] == "chart");
            chartStyle->setCurrentText(o["style"].toString("bar"));
        });
        auto* cameraOn = new QCheckBox(tr("Enable scene camera"));
        form->addRow(cameraOn);
        connect(cameraOn, &QCheckBox::toggled, this, [this](bool on) {
            if (loading)
                return;
            data["version"] = 2;
            if (on)
                data["camera"] = QJsonObject{{"x", .5}, {"y", .5}, {"zoom", 1}, {"rotation", 0}};
            else
                data.remove("camera");
            load();
        });
        refreshers.push_back([this, cameraOn](QJsonObject) { cameraOn->setChecked(data.contains("camera")); });
        for (auto key : {"x", "y", "zoom", "rotation"}) {
            auto* spin = new QDoubleSpinBox;
            spin->setDecimals(3);
            spin->setRange(QString(key) == "zoom" ? .01 : -360, QString(key) == "zoom" ? 100 : 360);
            form->addRow(tr("Camera ") + key, spin);
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, key](double v) {
                if (loading)
                    return;
                data["version"] = 2;
                auto c = data["camera"].toObject();
                if ((record && record->isChecked()) || c[key].toObject().contains("keyframes")) {
                    QString error;
                    auto result = motion::withKeyframe(data, "$camera", key, currentTime, v, easing(), &error);
                    if (!result.isEmpty())
                        data = result;
                } else {
                    c[key] = v;
                    data["camera"] = c;
                }
                canvas->scene = data;
                canvas->update();
                refreshKeys();
            });
            refreshers.push_back([this, spin, key](QJsonObject) {
                spin->setEnabled(data.contains("camera"));
                spin->setValue(motion::evaluate(data["camera"].toObject()[key], currentTime, data)
                                   .toDouble(QString(key) == "zoom"           ? 1
                                                 : QString(key) == "rotation" ? 0
                                                                              : .5));
            });
        }
        auto* cameraFollow = new QComboBox;
        form->addRow(tr("Camera follows"), cameraFollow);
        connect(cameraFollow, qOverload<int>(&QComboBox::activated), this, [this, cameraFollow] {
            if (loading)
                return;
            auto c = data["camera"].toObject();
            c["follow"] = cameraFollow->currentData().toString();
            data["version"] = 2;
            data["camera"] = c;
            load();
        });
        refreshers.push_back([this, cameraFollow](QJsonObject) {
            cameraFollow->clear();
            cameraFollow->addItem(tr("Fixed view"), "");
            for (auto v : data["layers"].toArray()) {
                auto o = v.toObject();
                if (!o["id"].toString().isEmpty())
                    cameraFollow->addItem(o["name"].toString(o["id"].toString()), o["id"].toString());
            }
            cameraFollow->setCurrentIndex(std::max(0, cameraFollow->findData(data["camera"].toObject()["follow"].toString())));
        });
        return scroll;
    }
    bool applyLayerJson()
    {
        if (!layerJson || !layerJson->document()->isModified())
            return true;
        QJsonParseError parse;
        auto doc = QJsonDocument::fromJson(layerJson->toPlainText().toUtf8(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            QMessageBox::warning(this, tr("Layer data"), tr("Enter a layer JSON object."));
            return false;
        }
        auto candidate = data;
        auto a = data["layers"].toArray();
        int row = -1;
        for (int i = 0; i < a.size(); ++i)
            if (a[i].toObject()["id"] == layerDraftId)
                row = i;
        if (row < 0) {
            QMessageBox::warning(this, tr("Layer data"), tr("The edited layer was removed. Discard this layer draft."));
            return false;
        }
        a[row] = doc.object();
        candidate["layers"] = a;
        auto error = graphics::validateScene(candidate);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Layer data"), error);
            return false;
        }
        data = candidate;
        layerJson->document()->setModified(false);
        refresh(row);
        return true;
    }
    void importData()
    {
        auto file = QFileDialog::getOpenFileName(this, tr("Import chart data"), {}, tr("Data (*.json *.csv)"));
        if (file.isEmpty())
            return;
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
            return;
        auto bytes = f.readAll();
        QJsonArray rows;
        if (file.endsWith(".json", Qt::CaseInsensitive)) {
            auto doc = QJsonDocument::fromJson(bytes);
            rows = doc.isArray() ? doc.array() : doc.object()["data"].toArray();
        } else {
            QString input = QString::fromUtf8(bytes);
            QStringList fields;
            QString cell;
            bool quote = false;
            QList<QStringList> records;
            for (int i = 0; i < input.size(); ++i) {
                QChar ch = input[i];
                if (ch == '"') {
                    if (quote && i + 1 < input.size() && input[i + 1] == '"') {
                        cell += '"';
                        ++i;
                    } else
                        quote = !quote;
                } else if (!quote && (ch == ',' || ch == '\n')) {
                    fields.append(cell);
                    cell.clear();
                    if (ch == '\n') {
                        records.append(fields);
                        fields.clear();
                    }
                } else if (ch != '\r' || quote)
                    cell += ch;
            }
            if (!cell.isEmpty() || !fields.isEmpty()) {
                fields.append(cell);
                records.append(fields);
            }
            for (auto item : records) {
                if (item.size() < 2)
                    continue;
                bool ok = false;
                double value = item[1].trimmed().toDouble(&ok);
                if (!ok)
                    continue;
                QJsonObject row{{"label", item[0]}, {"value", value}};
                if (item.size() > 2)
                    row["color"] = item[2].trimmed();
                rows.append(row);
            }
        }
        if (rows.isEmpty() || rows.size() > 128) {
            QMessageBox::warning(this, tr("Chart data"),
                                 tr("Import 1–128 rows with label and numeric value "
                                    "(optional colour)."));
            return;
        }
        for (auto v : rows)
            if (!v.isObject() || !v.toObject()["value"].isDouble() || !std::isfinite(v.toObject()["value"].toDouble())) {
                QMessageBox::warning(this, tr("Chart data"), tr("Each row needs a numeric value."));
                return;
            }
        data["version"] = 2;
        if (current()["type"] == "chart")
            change("data", rows);
        else {
            auto sets = data["datasets"].toObject();
            sets["imported"] = rows;
            data["datasets"] = sets;
        }
        load();
        syncJson();
    }
    QWidget* sceneControls()
    {
        auto* w = new QWidget;
        auto* layout = new QVBoxLayout(w);
        auto* audio = new QPushButton(tr("Bind audio for animation…"));
        audio->setObjectName("graphicsBindAudio");
        layout->addWidget(audio);
        connect(audio, &QPushButton::clicked, this, [this] {
            QString name;
            auto binding = pickAnimationAudio(this, project, duration, eventStart, &name);
            if (binding.isEmpty())
                return;
            auto audio = data["audio"].toObject();
            audio[name] = binding;
            data["version"] = 2;
            data["audio"] = audio;
            load();
            refreshKeys();
            syncJson();
        });
        auto* look = new QGroupBox(tr("Look"));
        look->setObjectName("graphicsLook");
        auto* lf = new QFormLayout(look);
        sceneBlur = new QCheckBox(tr("Motion blur (fast moves and camera moves smear)"));
        sceneBlur->setObjectName("graphicsMotionBlur");
        lf->addRow(sceneBlur);
        sceneShutter = new QDoubleSpinBox;
        sceneShutter->setRange(.005, .2);
        sceneShutter->setDecimals(3);
        sceneShutter->setSingleStep(.005);
        sceneShutter->setValue(.02);
        lf->addRow(tr("Shutter (s)"), sceneShutter);
        sceneDepth = new QDoubleSpinBox;
        sceneDepth->setObjectName("graphicsDepthOfField");
        sceneDepth->setRange(0, 10);
        sceneDepth->setDecimals(2);
        sceneDepth->setToolTip(tr("As the camera zooms in, layers with a depth (Layout tab) blur: 0 for none"));
        lf->addRow(tr("Depth of field"), sceneDepth);
        sceneSpeaker = new QCheckBox(tr("Camera frames whoever is talking"));
        sceneSpeaker->setObjectName("graphicsSpeakerCamera");
        sceneSpeaker->setToolTip(tr("The camera closes in on the character whose voice binding is loudest (a group named like its binding)"));
        lf->addRow(sceneSpeaker);
        sceneSpeakerZoom = new QDoubleSpinBox;
        sceneSpeakerZoom->setRange(1, 6);
        sceneSpeakerZoom->setDecimals(2);
        sceneSpeakerZoom->setValue(1.7);
        lf->addRow(tr("Close-up zoom"), sceneSpeakerZoom);
        layout->addWidget(look);
        auto setLook = [this] {
            if (loading)
                return;
            data["version"] = 2;
            if (sceneBlur->isChecked())
                data["motion_blur"] = QJsonObject{{"samples", 5}, {"shutter", sceneShutter->value()}};
            else
                data.remove("motion_blur");
            auto camera = data["camera"].toObject();
            if (sceneDepth->value() > 0)
                camera["depth_of_field"] = sceneDepth->value();
            else
                camera.remove("depth_of_field");
            if (sceneSpeaker->isChecked())
                camera["speaker"] = QJsonObject{{"zoom", sceneSpeakerZoom->value()}};
            else
                camera.remove("speaker");
            data["camera"] = camera;
            load();
            syncJson();
        };
        connect(sceneBlur, &QCheckBox::toggled, this, setLook);
        connect(sceneShutter, qOverload<double>(&QDoubleSpinBox::valueChanged), this, setLook);
        connect(sceneDepth, qOverload<double>(&QDoubleSpinBox::valueChanged), this, setLook);
        connect(sceneSpeaker, &QCheckBox::toggled, this, setLook);
        connect(sceneSpeakerZoom, qOverload<double>(&QDoubleSpinBox::valueChanged), this, setLook);
        auto* import = new QPushButton(tr("Import chart data…"));
        import->setObjectName("graphicsImportChart");
        layout->addWidget(import);
        connect(import, &QPushButton::clicked, this, [this] { importData(); });
        auto* tabs = new QTabWidget;
        sceneJson = new QPlainTextEdit;
        sceneJson->setObjectName("graphicsSceneJson");
        layerJson = new QPlainTextEdit;
        layerJson->setObjectName("graphicsLayerJson");
        tabs->addTab(sceneJson, tr("Scene data"));
        tabs->addTab(layerJson, tr("Layer data"));
        layout->addWidget(tabs, 1);
        auto* apply = new QPushButton(tr("Apply data to preview"));
        layout->addWidget(apply);
        connect(apply, &QPushButton::clicked, this, [this] {
            if (applyLayerJson())
                applyPendingJson();
            syncJson();
        });
        auto* discard = new QPushButton(tr("Discard data drafts"));
        layout->addWidget(discard);
        connect(discard, &QPushButton::clicked, this, [this] {
            sceneJson->document()->setModified(false);
            layerJson->document()->setModified(false);
            syncJson();
        });
        auto* hint = new QLabel(tr("Templates and reusable objects retain their variables, data and "
                                   "animation.\nAudio bindings store real envelopes and beats; they do "
                                   "not add sound.\nAdvanced data exposes every scene property."));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        return w;
    }
    void change(QString key, QJsonValue value)
    {
        if (loading)
            return;
        int i = layerRow();
        if (i < 0)
            return;
        if (graphics_workflow::locked(data, current()["id"].toString()))
            return;
        auto a = data["layers"].toArray();
        auto o = a[i].toObject();
        auto old = layerProperty(o, key);
        if (key.contains('.')
            || QStringList{"parent",         "follow",   "mask",  "gradient", "shadow",    "glow",     "trail",    "motion_blur",
                           "text_animation", "commands", "data",  "style",    "scale_x",   "scale_y",  "anchor_x", "anchor_y",
                           "count",          "seed",     "speed", "gravity",  "spacing_x", "spacing_y"}
                   .contains(key)) {
            data["version"] = 2;
            ensureIds();
            o = current();
        }
        bool structural = QStringList{"id", "type", "parent", "symbol", "name", "path", "brand_role", "locked"}.contains(key);
        if (!structural
            && (((record && record->isChecked()) && (value.isDouble() || value.isString() || value.isBool() || key == "commands"))
                || old.toObject().contains("keyframes"))) {
            ensureIds();
            QString error;
            auto result = motion::withKeyframe(data, current()["id"].toString(), key, currentTime, value, easing(), &error);
            if (result.isEmpty()) {
                QMessageBox::warning(this, tr("Animation"), error);
                return;
            }
            data = result;
            o = current();
        } else {
            a = data["layers"].toArray();
            o = setLayerProperty(o, key.split('.'), value);
            a[i] = o;
            data["layers"] = a;
        }
        list->item(i)->setText(o["name"].toString(o["type"].toString()));
        canvas->scene = data;
        canvas->update();
        refreshKeys();
    }
    void refresh(int selection)
    {
        normalize();
        loading = true;
        list->clear();
        for (auto v : data["layers"].toArray()) {
            auto o = v.toObject();
            QString parent = o["parent"].toString();
            int depth = 0;
            auto a = data["layers"].toArray();
            while (!parent.isEmpty() && depth < 16) {
                ++depth;
                QString next;
                for (auto x : a)
                    if (x.toObject()["id"] == parent)
                        next = x.toObject()["parent"].toString();
                parent = next;
            }
            list->addItem(QString(depth * 2, ' ')
                          + (graphics_workflow::locked(data, o["id"].toString()) ? QStringLiteral("[locked] ") : QString())
                          + o["name"].toString(o["type"].toString()));
        }
        list->setCurrentRow(selection);
        loading = false;
        load();
    }
    void load()
    {
        loading = true;
        if (sceneBlur) { // (value(): reading a missing key with [] would add it)
            sceneBlur->setChecked(data.contains("motion_blur"));
            sceneShutter->setValue(data.value("motion_blur").toObject().value("shutter").toDouble(.02));
            sceneDepth->setValue(data.value("camera").toObject().value("depth_of_field").toDouble());
            sceneSpeaker->setChecked(data.value("camera").toObject().contains("speaker"));
            sceneSpeakerZoom->setValue(data.value("camera").toObject().value("speaker").toObject().value("zoom").toDouble(1.7));
        }
        auto o = current();
        propertyTabs->setEnabled(!graphics_workflow::locked(data, o["id"].toString()));
        brandRole->setCurrentText(o["brand_role"].toString().isEmpty() ? "None" : o["brand_role"].toString());
        const bool isText = o["type"].toString() == "text", isImage = o["type"].toString() == "image";
        words->setEnabled(isText);
        font->setEnabled(isText);
        bold->setEnabled(isText);
        align->setEnabled(isText);
        path->setEnabled(isImage || o["type"].toString() == "video");
        name->setText(o["name"].toString());
        words->setPlainText(motion::evaluate(o["text"], currentTime, data).toString());
        font->setCurrentFont(QFont(motion::evaluate(o["font"], currentTime, data).toString("Segoe UI")));
        bold->setChecked(motion::evaluate(o["bold"], currentTime, data).toBool());
        visible->setChecked(motion::evaluate(o["visible"], currentTime, data).toBool(true));
        fixed->setChecked(o["fixed"].toBool());
        align->setCurrentText(motion::evaluate(o["align"], currentTime, data).toString("left"));
        path->setText(motion::evaluate(o["path"], currentTime, data).toString());
        for (auto [key, spin] : spins) {
            double fallback = key == "opacity" ? 1 : key == "font_size" ? .035 : 0;
            spin->setValue(motion::evaluate(o[key], currentTime, data).toDouble(fallback));
        }
        fill->setStyleSheet("background:" + motion::evaluate(o["fill"], currentTime, data).toString("#ffffff"));
        stroke->setStyleSheet("background:" + motion::evaluate(o["stroke"], currentTime, data).toString("#ffffff"));
        canvas->selected = layerRow();
        canvas->scene = data;
        canvas->selectedIds = selectionIds();
        canvas->update();
        if (parentBox) {
            parentBox->clear();
            parentBox->addItem(tr("None"), "");
            followBox->clear();
            followBox->addItem(tr("None"), "");
            QSet<QString> descendants{o["id"].toString()};
            bool more = true;
            while (more) {
                more = false;
                for (auto v : data["layers"].toArray()) {
                    auto l = v.toObject();
                    if (descendants.contains(l["parent"].toString()) && !descendants.contains(l["id"].toString())) {
                        descendants.insert(l["id"].toString());
                        more = true;
                    }
                }
            }
            for (auto v : data["layers"].toArray()) {
                auto l = v.toObject();
                auto id = l["id"].toString();
                if (id.isEmpty())
                    continue;
                if (l["type"] == "group" && !descendants.contains(id))
                    parentBox->addItem(l["name"].toString(id), id);
                if (l["type"] == "path" && id != o["id"].toString())
                    followBox->addItem(l["name"].toString(id), id);
            }
            parentBox->setCurrentIndex(std::max(0, parentBox->findData(o["parent"].toString())));
            followBox->setCurrentIndex(std::max(0, followBox->findData(o["follow"].toObject()["path"].toString())));
            pathEditing->setEnabled(o["type"] == "path");
        }
        for (auto& refresh : refreshers)
            refresh(o);
        refreshKeys();
        loading = false;
    }
};
QJsonObject pickAnimationAudio(QWidget* parent, Project* project, double duration, double eventStart, QString* name)
{
    if (!project)
        return {};
    QDialog dialog(parent);
    dialog.setObjectName("animationAudioDialog");
    dialog.setWindowTitle("Animation Audio Source");
    if (parent->testAttribute(Qt::WA_DontShowOnScreen))
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
    auto* root = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    root->addLayout(form);
    auto* media = new QComboBox;
    media->setObjectName("animationAudioMedia");
    for (auto& [id, m] : project->media())
        if (m->info.hasAudio())
            media->addItem(m->displayName(), id);
    const Timeline tl = project->timeline();
    if (eventStart >= 0) // a whole track (a character's voice, for lip sync): every event on it during this one
        for (int i = 0; i < int(tl.tracks.size()); ++i)
            if (tl.tracks[size_t(i)].type == TrackType::Audio)
                media->addItem(QStringLiteral("Track: %1 (its events during this scene)").arg(tl.tracks[size_t(i)].name), -(i + 1));
    if (!media->count()) {
        QMessageBox::information(parent, "Audio animation", "Import an audio or video source first.");
        return {};
    }
    form->addRow("Audio / video source", media);
    auto* start = new QDoubleSpinBox;
    start->setRange(0, 86400);
    start->setDecimals(3);
    start->setObjectName("animationAudioStart");
    form->addRow("Source in (seconds)", start);
    auto* span = new QDoubleSpinBox;
    span->setRange(.01, 3600);
    span->setDecimals(3);
    span->setValue(duration);
    span->setObjectName("animationAudioSpan");
    form->addRow("Source span (seconds)", span);
    auto* bindingName = new QLineEdit(QStringLiteral("source"));
    bindingName->setObjectName("animationAudioName");
    form->addRow("Binding name", bindingName);
    const auto fromTrack = [media] { return media->currentData().toInt() < 0; };
    const auto showRange = [=] { start->setEnabled(!fromTrack()); span->setEnabled(!fromTrack()); };
    QObject::connect(media, &QComboBox::currentIndexChanged, &dialog, showRange);
    showRange();
    auto* label = new QLabel("Bind the real audio envelope and detected beats under the binding name.\nUse "
                             "audio-driven properties in a layer's data or start with Audio "
                             "Reactive.\nThis embeds analysis; it does not insert sound.");
    label->setWordWrap(true);
    root->addWidget(label);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return {};
    *name = bindingName->text().trimmed().isEmpty() ? QStringLiteral("source") : bindingName->text().trimmed();
    if (fromTrack()) {
        QString error;
        QJsonObject result = graphics::trackEnvelope(tl.tracks[size_t(-media->currentData().toInt() - 1)], project->snapshot()->media,
                                                     eventStart, duration, 4096, &error);
        if (result.isEmpty())
            QMessageBox::warning(parent, "Audio animation", error);
        return result;
    }
    auto item = project->mediaItem(media->currentData().toInt());
    if (!item || !item->info.hasAudio())
        return {};
    double at = start->value(), length = std::min(span->value(), double(item->info.duration) / kUsPerSecond - at);
    if (length <= 0) {
        QMessageBox::warning(parent, "Audio animation", "Choose a range inside the source.");
        return {};
    }
    QProgressDialog progress("Analysing audio envelope and beats…", "Cancel", 0, 100, parent);
    if (parent->testAttribute(Qt::WA_DontShowOnScreen))
        progress.setAttribute(Qt::WA_DontShowOnScreen);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    std::atomic<double> done = 0;
    std::atomic<bool> cancel = false;
    QString error;
    auto future = std::async(std::launch::async, [&] {
        return graphics::buildWaveform(
            item->path, item->info.audio.front().index, at, length, 4096, &error,
            [&](double p) {
                done = p;
                return !cancel.load();
            },
            true);
    });
    QEventLoop loop;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        progress.setValue(int(done.load() * 100));
        if (progress.wasCanceled())
            cancel = true;
        if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            loop.quit();
    });
    timer.start(25);
    loop.exec();
    auto result = future.get();
    progress.close();
    if (result.isEmpty()) {
        if (error != "Cancelled")
            QMessageBox::warning(parent, "Audio animation", error);
        return {};
    }
    result["source_media_id"] = item->id;
    result["rate"] = 1;
    result["offset"] = 0;
    return result;
}
} // namespace
void composeGraphics(QWidget* parent, Project* project, int eventId)
{
    const auto* clip = project->timeline().findClip(eventId);
    if (!clip || !graphics::isScene(clip->generator.type))
        return;
    const bool animation = clip->generator.type == "animation" || graphics::sceneType(clip->generator.data) == "animation";
    Designer dialog(parent,
                    clip->generator.data.isEmpty() ? graphics::scene(animation ? "motion_blank" : "broadcast") : clip->generator.data, project,
                    double(clip->generatedLength()) / kUsPerSecond, double(clip->start - clip->sourceIn) / kUsPerSecond, animation);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto error = graphics::validateScene(dialog.data);
    if (!error.isEmpty()) {
        QMessageBox::warning(parent, "Graphics", error);
        return;
    }
    auto tl = project->timeline();
    if (auto* c = tl.findClip(eventId)) {
        c->generator.data = dialog.data;
        c->generator.type = graphics::sceneType(dialog.data); // (scene data edited into version 2 makes it an Animation)
        project->applyEdit(tl, animation ? "Compose Animation" : "Compose Graphics Overlay");
    }
}
void chooseWaveformAudio(QWidget* parent, Project* project, int eventId)
{
    const auto* clip = project->timeline().findClip(eventId);
    if (!clip || clip->generator.type != "audio_waveform")
        return;
    double eventLength = double(clip->length) / kUsPerSecond;
    auto previous = clip->generator.data;
    QDialog dialog(parent);
    if (parent && parent->testAttribute(Qt::WA_DontShowOnScreen))
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.setObjectName("waveformSourceDialog");
    dialog.setWindowTitle("Waveform Audio Source");
    auto* root = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    root->addLayout(form);
    auto* source = new QComboBox;
    source->setObjectName("waveformSource");
    for (const auto& [id, m] : project->media())
        if (m->info.hasAudio())
            source->addItem(m->displayName(), id);
    form->addRow("Audio / video file", source);
    if (source->count() == 0) {
        QMessageBox::information(parent, "Waveform", "Import an audio or video file first.");
        return;
    }
    auto* start = new QDoubleSpinBox;
    start->setObjectName("waveformStart");
    start->setRange(0, 86400);
    start->setDecimals(3);
    start->setValue(previous["source_in"].toDouble());
    auto* length = new QDoubleSpinBox;
    length->setObjectName("waveformSpan");
    length->setRange(.01, 3600);
    length->setDecimals(3);
    length->setValue(previous["duration"].toDouble(eventLength));
    form->addRow("Source in (seconds)", start);
    form->addRow("Source span (seconds)", length);
    int idx = source->findData(previous["source_media_id"].toInt());
    if (idx >= 0)
        source->setCurrentIndex(idx);
    root->addWidget(new QLabel("The waveform uses decoded source audio before timeline effects.\nPeaks "
                               "are stored in the project; this adds no audio."));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Build waveform");
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    int id = source->currentData().toInt();
    auto media = project->mediaItem(id);
    if (!media || !media->info.hasAudio())
        return;
    double at = start->value(), span = std::min(length->value(), double(media->info.duration) / kUsPerSecond - at);
    if (span <= 0) {
        QMessageBox::warning(parent, "Waveform", "The selected range is outside the audio file.");
        return;
    }
    QProgressDialog progress("Decoding audio and building the waveform…", "Cancel", 0, 100, parent);
    if (parent && parent->testAttribute(Qt::WA_DontShowOnScreen))
        progress.setAttribute(Qt::WA_DontShowOnScreen);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    std::atomic<double> done = 0;
    std::atomic<bool> cancel = false;
    QString error;
    auto future = std::async(std::launch::async, [&] {
        return graphics::buildWaveform(media->path, media->info.audio.front().index, at, span, 2048, &error, [&](double p) {
            done = p;
            return !cancel.load();
        });
    });
    QEventLoop loop;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        progress.setValue(int(done.load() * 100));
        if (progress.wasCanceled())
            cancel = true;
        if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            loop.quit();
    });
    timer.start(25);
    loop.exec();
    auto data = future.get();
    progress.close();
    if (data.isEmpty()) {
        if (error != "Cancelled")
            QMessageBox::warning(parent, "Waveform", error);
        return;
    }
    data["source_media_id"] = id;
    auto tl = project->timeline();
    if (auto* c = tl.findClip(eventId)) {
        c->generator.data = data;
        project->applyEdit(tl, "Build Audio Waveform Graphic");
    }
}
