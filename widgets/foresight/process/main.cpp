#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <LayerShellQt/Window>
#include <QIcon>
#include <QCommandLineParser>
#include "LauncherBackend.h"

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "wayland");
    qputenv("XDG_CURRENT_DESKTOP", "GNOME");

    QGuiApplication app(argc, argv);
    
    // Parse Arguments Sent by Aurora Shell
    QCommandLineParser parser;
    QCommandLineOption nameOpt("name", "Widget name", "name", "foresight");
    QCommandLineOption layerOpt("layer", "Layer", "layer", "overlay");
    QCommandLineOption anchorOpt("anchor", "Anchor", "anchor", "center");
    QCommandLineOption exclusiveOpt("exclusive", "Exclusive zone", "exclusive", "false");
    QCommandLineOption marginOpt("margin", "Margins t,b,l,r", "margin", "0,0,0,0");

    parser.addOptions({nameOpt, layerOpt, anchorOpt, exclusiveOpt, marginOpt});
    parser.process(app);

    // Initialize Backend with its Name
    LauncherBackend backend(parser.value(nameOpt));
    qmlRegisterSingletonInstance("Aurora.Foresight", 1, 0, "Backend", &backend);

    QQmlApplicationEngine engine;
    engine.addImportPath("qrc:/ui");
    engine.load(QUrl(QStringLiteral("qrc:/ui/Main.qml")));
    
    if (engine.rootObjects().isEmpty()) return -1;

    QWindow *window = qobject_cast<QWindow *>(engine.rootObjects().first());

    if (window) {
        // --- THE FIX: Connect backend signals to actual Window Mapping ---
        QObject::connect(&backend, &LauncherBackend::windowNeedsShow, window, [window]() {
            window->setVisible(true);
            window->requestActivate();
        });

        QObject::connect(&backend, &LauncherBackend::windowNeedsHide, window, [window]() {
            window->setVisible(false); // Unmaps window, releasing keyboard and mouse grabs!
        });

        LayerShellQt::Window *lsWindow = LayerShellQt::Window::get(window);
        
        // 1. Apply Layer
        QString layerStr = parser.value(layerOpt);
        if (layerStr == "background") lsWindow->setLayer(LayerShellQt::Window::LayerBackground);
        else if (layerStr == "bottom") lsWindow->setLayer(LayerShellQt::Window::LayerBottom);
        else if (layerStr == "top") lsWindow->setLayer(LayerShellQt::Window::LayerTop);
        else lsWindow->setLayer(LayerShellQt::Window::LayerOverlay);

        // 2. Apply Anchor
        QString anchorStr = parser.value(anchorOpt);
        int anchors = 0;
        
        if (anchorStr.contains("top")) anchors |= LayerShellQt::Window::AnchorTop;
        if (anchorStr.contains("bottom")) anchors |= LayerShellQt::Window::AnchorBottom;
        if (anchorStr.contains("left")) anchors |= LayerShellQt::Window::AnchorLeft;
        if (anchorStr.contains("right")) anchors |= LayerShellQt::Window::AnchorRight;
        
        if (anchorStr.contains("fill")) {
            anchors = LayerShellQt::Window::AnchorTop | 
                      LayerShellQt::Window::AnchorBottom | 
                      LayerShellQt::Window::AnchorLeft | 
                      LayerShellQt::Window::AnchorRight;
        }
        
        lsWindow->setAnchors(static_cast<LayerShellQt::Window::Anchor>(anchors));

        // 3. Apply Exclusivity
        if (parser.value(exclusiveOpt) == "true") {
            lsWindow->setExclusiveZone(1); // Auto-exclusive
        }

        // 4. Apply Margins
        QStringList m = parser.value(marginOpt).split(",");
        if (m.size() == 4) {
            lsWindow->setMargins(QMargins(m[2].toInt(), m[0].toInt(), m[3].toInt(), m[1].toInt())); 
        }

        lsWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        
        // Start completely hidden to avoid softlocking on boot!
        window->setVisible(false);
    }

    return app.exec();
}