#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QWindow>
#include <QSurfaceFormat>
#include <LayerShellQt/Window>
#include <QQuickItem>
#include <QQmlEngine>
#include "NotificationBackend.h"

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "wayland");

    QSurfaceFormat format;
    format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);

    QGuiApplication app(argc, argv);
    
    NotificationBackend backend;
    qmlRegisterSingletonInstance("Aurora.Shell", 1, 0, "Backend", &backend);

    QQmlApplicationEngine engine;
    
    // Tell QML to look in our resources folder for the qmldir singleton definition
    engine.addImportPath("qrc:/ui");
    
    // Load directly from the compiled QRC resources
    engine.load(QUrl(QStringLiteral("qrc:/ui/Main.qml")));
    
    if (engine.rootObjects().isEmpty()) return -1;

    QObject *rootObject = engine.rootObjects().first();
    QWindow *window = qobject_cast<QWindow *>(rootObject);

    QObject* pill_obj = rootObject->findChild<QObject*>("dynamicPill");
    QQuickItem* pill = qobject_cast<QQuickItem*>(pill_obj);

    if (window && pill) {
        auto updateInputMask = [window, pill]() {
            QRectF rect = pill->mapRectToScene(QRectF(0, 0, pill->width(), pill->height()));
            if (rect.width() <= 0 || rect.height() <= 0) {
                window->setMask(QRegion(0, 0, 1, 1));
            } else {
                window->setMask(QRegion(rect.x(), rect.y(), rect.width(), rect.height()));
            }
        };

        QObject::connect(pill, &QQuickItem::xChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::yChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::widthChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::heightChanged, pill, updateInputMask);
        
        updateInputMask();

        LayerShellQt::Window *lsWindow = LayerShellQt::Window::get(window);
        lsWindow->setLayer(LayerShellQt::Window::LayerOverlay);
        lsWindow->setAnchors(LayerShellQt::Window::AnchorTop);
        lsWindow->setExclusiveZone(0);
        lsWindow->setMargins(QMargins(0, 0, 0, 0)); 
        
        window->setVisible(true);
    }

    return app.exec();
}