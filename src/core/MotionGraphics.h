#pragma once
#include <QImage>
#include <QHash>
#include <QJsonObject>
#include <QOpenGLPaintDevice>
#include <QPainterPath>
#include <QVector>
class QPainter;
struct EffectDef;
class Params;
struct RenderContext;

namespace motion {
// Scene v2: normalized coordinates, event-local seconds, deterministic seeking.
QJsonValue evaluate(const QJsonValue &value, double time,
                    const QJsonObject &scene = {});
double ease(double progress, const QJsonValue &curve);
QString validate(const QJsonObject &scene);
bool animating(const QJsonValue &value);
void render(QImage &image, const QJsonObject &scene, double time);
void paint(QPainter &painter, QSizeF size, const QJsonObject &scene, double time);
QJsonObject preset(const QString &name);
QStringList presets();
// Two cut-out characters whose mouths, arms and bodies follow the bindings voice_a and voice_b.
QJsonObject charactersScene();
QJsonObject capabilities();
QJsonObject withKeyframe(const QJsonObject &scene, const QString &layerId,
                         const QString &property, double time,
                         const QJsonValue &value, const QJsonValue &easing,
                         QString *error);
QPainterPath path(const QJsonArray &commands);
// Ready-made moves: keyframes for a layer around `time` (event-local seconds) that land back on its own animation.
// hop, jump, shake, nod, wave (for an arm's group: it turns at its anchor), spin, walk (to x = `to`, with a bob),
// pop (scales in), squash, blink (for an eyes group); point, arms up, shrug (on a character's own group: its arms). duration 0: the move's natural length; amount scales it.
// A character (a library file, or one saved with characterFrom) added to a scene: its layers' ids start with its name,
// its colour settings become scene variables name_<colour>, its voice binding is called by its name (lower case, the
// name to bind its voice track under), its feet stand on `ground` with its left edge at x. Two of the same name get
// numbered.
QJsonObject withCharacter(const QJsonObject &scene, const QJsonObject &character, const QString &name, double x, double ground,
                          QString *error);
// A set (library background) behind everything in the scene, its front part in front of everything, replacing the set
// added before. The back is one group, "set", with depth 1: the camera's depth of field softens it in close-ups.
QJsonObject withSet(const QJsonObject &scene, const QJsonObject &set, QString *error);
// A scene's group (and the layers in it) as a character: its fill colours become settings, its most used audio binding
// its voice.
QJsonObject characterFrom(const QJsonObject &scene, const QString &groupId, const QString &title, QString *error);
// Preview quality for this thread's renders (fewer motion blur moments, faster video frames). Returns the previous one.
bool setDraft(bool draft);
// The GPU's help while a scene paints through OpenGL: the scene's own pictures (a motion blur moment, a layer to soften
// or give effects, a group seen through as one) kept as framebuffers instead of QImages. The GPU compositor sets it
// for its thread around a generator's paint; without it (or painting into a QImage) the pictures are made on the CPU.
class GpuPictures {
public:
  struct Picture {
    unsigned fbo = 0, tex = 0;
    QSize size;
    int handle = -1;
    explicit operator bool() const { return handle >= 0; }
  };
  virtual ~GpuPictures() = default;
  // A cleared picture (with a stencil: QPainter clips with it); precise: float, for sums.
  virtual Picture acquire(QSize size, bool precise) = 0;
  virtual void release(const Picture &picture) = 0;
  // The part with anything in it (empty: nothing drawn), found in blocks of `block` pixels (read back small).
  virtual QRect drawnBounds(const Picture &picture, int block) = 0;
  // A new picture of that part of it.
  virtual Picture crop(const Picture &picture, const QRect &region) = 0;
  // A soft blur of `radius` pixels (as on the CPU).
  virtual void blur(Picture &picture, double radius) = 0;
  // One of a layer's own effects.
  virtual void effect(Picture &picture, const EffectDef &def, const Params &params, const RenderContext &context) = 0;
};
void setGpuPictures(GpuPictures *pictures);
// A framebuffer as a paint device (row 0 at the top) that binds its framebuffer again whenever its painter goes on after
// another painted: a scene paints its pictures in between.
class FramebufferPaintDevice : public QOpenGLPaintDevice {
public:
  FramebufferPaintDevice(QSize size, unsigned fbo);
  void ensureActiveTarget() override;

private:
  unsigned m_fbo;
};
// Pictures this thread made on the CPU while painting through OpenGL with GpuPictures set (0: all on the GPU).
int cpuPictures();
QStringList moves();
// A character's expression from `time` on (their <id>_mood variable, held): neutral, angry, sad, surprised, smug.
QStringList expressions();
QJsonObject withExpression(const QJsonObject &scene, const QString &characterId, const QString &mood, double time, QString *error);
// Small changes in one step: each {layer_id, property, value} sets a layer's property (a dotted path such as
// "shadow.blur"), or with remove: true takes it away; layer_id may also be $camera, $variables or $scene (the scene's own
// keys). Structural properties (id, type, parent, children) stay as they are. Empty on error.
QJsonObject withPatch(const QJsonObject &scene, const QJsonArray &operations, QString *error);
// A prop (a library prop: a group "root" of layers, its width and height on the picture and its grip, the point held)
// in a character's hand ("R" or "L"): it hangs from the arm and moves with every gesture. Empty on error.
QJsonObject withProp(const QJsonObject &scene, const QJsonObject &prop, const QString &characterId, const QString &hand, QString *error);
// The most layers a version 2 scene stores, reusable definitions included.
constexpr int kMaxLayers = 1024;
QJsonObject withMove(const QJsonObject &scene, const QString &layerId, const QString &move, double time, double duration,
                     double amount, double to, QString *error);
struct PathHandle {
  int command = 0, coordinate = 1;
  QPointF position;
};
QRectF bounds(const QJsonObject &scene, const QString &id, double time,
              QSizeF size);
QHash<QString,QRectF> allBounds(const QJsonObject& scene, double time, QSizeF size);
QVector<PathHandle> handles(const QJsonObject &scene, const QString &id,
                            double time, QSizeF size);
QPointF editDelta(const QJsonObject &scene, const QString &id, double time,
                  QSizeF size, QPointF delta, bool pathPoint = false);
} // namespace motion
