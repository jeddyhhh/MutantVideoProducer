#pragma once
// Tools > Effect Builder: make a video effect from a graph of nodes (core/NodeGraph.h). The example video on the left,
// the graph in the middle (drag nodes in from the list, wire outputs to inputs), the result live on the right with
// the chosen node's settings under it. Saved, it becomes an effect in the User Created category.

#include "core/NodeGraph.h"
#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <functional>
#include <memory>

class FrameConverter;
class QComboBox;
class QGraphicsScene;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QTreeWidget;
class QVBoxLayout;
class QWidget;
class VideoDecoder;
class GpuCompositor;
class FrameRenderer;
class QOffscreenSurface;

namespace nodeui {
class GraphView;
class NodeItem;
}

class NodeEffectEditor : public QDialog {
    Q_OBJECT
public:
    // videos: the project's video files (name, path) to try the effect on; example: the one to start with (empty: a
    // pattern). projectWidth: the project's picture width (pixel sizes in the settings are of it).
    NodeEffectEditor(const std::vector<std::pair<QString, QString>>& videos, const QString& example, int projectWidth, QWidget* parent = nullptr);
    ~NodeEffectEditor() override;

    bool openFile(const QString& path);
    bool saveTo(const QString& path, QString* error = nullptr);
    // The Live tab's picture as the example (first in the list, and chosen): frame() gives it, null when nothing is
    // live (then the color bars show).
    void setLiveSource(std::function<QImage()> frame);

    // (also for the tests)
    const nodes::Graph& graph() const { return m_graph; }
    void setGraph(const nodes::Graph& g);
    int addNode(const QString& type, QPointF pos);
    bool connectNodes(int from, int to, int port);
    void setParam(int node, const QString& id, double value);
    void setExposed(int node, const QString& param, bool on);
    void step();              // shows the next frame (the timer does this while playing)
    QImage result() const { return m_result; }
    QString status() const;
    void undo();
    void redo();

signals:
    void saved(const QString& effectId);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void setExample(const QString& path);
    QImage exampleFrame();
    QImage patternFrame();
    void rebuildScene();
    void frameGraph();
    void rebuildLibrary(const QString& filter);
    void showParams(int node);
    void selectionChanged();
    void changed(const QString& key = {}); // before an edit: undo step (edits with the same key are one step)
    void edited();                        // after it: the scene and settings follow
    bool save(bool as);
    bool maybeSave();
    void tick();

    nodes::Graph m_graph;
    nodes::Evaluator m_eval; // (its outputs: the nodes' pictures, from the GPU too)
    // The preview on the GPU (Options > GPU Acceleration); null: the CPU's evaluator.
    std::unique_ptr<QOffscreenSurface> m_gpuSurface;
    std::unique_ptr<FrameRenderer> m_renderer;
    GpuCompositor* m_gpu = nullptr;
    QString m_path;
    bool m_modified = false;
    std::vector<QJsonObject> m_undo, m_redo;
    QString m_lastKey;
    int m_paramsNode = 0;

    // The example video.
    std::vector<std::pair<QString, QString>> m_videos;
    std::unique_ptr<VideoDecoder> m_decoder;
    std::unique_ptr<FrameConverter> m_converter;
    QString m_pattern;                   // the generator preset shown without a video
    std::function<QImage()> m_liveFrame; // setLiveSource
    bool m_live = false;
    TimeUs m_time = 0;
    TimeUs m_duration = 0;
    bool m_playing = true;
    int m_projectWidth = 1920;
    QImage m_result;
    std::map<int, bool> m_runsOn; // each node: ran on the GPU (true) or the CPU, in the last preview

    QLineEdit* m_name = nullptr;
    QLineEdit* m_description = nullptr;
    QComboBox* m_example = nullptr;
    QPushButton* m_play = nullptr;
    QSlider* m_scrub = nullptr;
    QLabel* m_sourceView = nullptr;
    QLabel* m_resultView = nullptr;
    QLabel* m_status = nullptr;
    QLineEdit* m_filter = nullptr;
    QTreeWidget* m_library = nullptr;
    nodeui::GraphView* m_view = nullptr;
    QGraphicsScene* m_scene = nullptr;
    QWidget* m_paramsHost = nullptr;
    QVBoxLayout* m_paramsLayout = nullptr;
    std::map<int, nodeui::NodeItem*> m_items;
    bool m_rebuilding = false;
};
