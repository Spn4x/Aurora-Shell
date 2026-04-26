#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <LayerShellQt/Window>
#include <QIcon>
#include "LauncherBackend.h"

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "wayland");
    
    // THE FIX: Trick Qt into loading the system GTK Icon Theme so apps aren't gray squares!
    qputenv("XDG_CURRENT_DESKTOP", "GNOME");

    QGuiApplication app(argc, argv);
    
    LauncherBackend backend;
    qmlRegisterSingletonInstance("Aurora.Foresight", 1, 0, "Backend", &backend);

    QQmlApplicationEngine engine;
    engine.addImportPath("qrc:/ui");
    engine.load(QUrl(QStringLiteral("qrc:/ui/Main.qml")));
    
    if (engine.rootObjects().isEmpty()) return -1;

    QWindow *window = qobject_cast<QWindow *>(engine.rootObjects().first());

    if (window) {
        LayerShellQt::Window *lsWindow = LayerShellQt::Window::get(window);
        lsWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        
        lsWindow->setAnchors(static_cast<LayerShellQt::Window::Anchor>(
            LayerShellQt::Window::AnchorTop | 
            LayerShellQt::Window::AnchorBottom | 
            LayerShellQt::Window::AnchorLeft | 
            LayerShellQt::Window::AnchorRight
        ));
        
        lsWindow->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        window->setVisible(true);
    }

    return app.exec();
}