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
    engine.addImportPath("qrc:/ui");
    engine.load(QUrl(QStringLiteral("qrc:/ui/Main.qml")));
    
    if (engine.rootObjects().isEmpty()) return -1;

    QObject *rootObject = engine.rootObjects().first();
    QWindow *window = qobject_cast<QWindow *>(rootObject);

    QObject* pill_obj = rootObject->findChild<QObject*>("dynamicPill");
    QQuickItem* pill = qobject_cast<QQuickItem*>(pill_obj);

    if (window && pill) {
        auto updateInputMask = [window, pill]() {
            // THE FIX: If the pill is invisible, drop the mask to a 1x1 pixel so clicks pass to the desktop
            if (pill->opacity() <= 0.01) {
                window->setMask(QRegion(0, 0, 1, 1));
                return;
            }

            // mapRectToScene automatically calculates the true size including the 'scale' property!
            QRectF rect = pill->mapRectToScene(QRectF(0, 0, pill->width(), pill->height()));
            
            if (rect.width() <= 0 || rect.height() <= 0) {
                window->setMask(QRegion(0, 0, 1, 1));
            } else {
                window->setMask(QRegion(rect.x(), rect.y(), rect.width(), rect.height()));
            }
        };

        // THE FIX: Connect opacity and scale to ensure the Wayland mask shrinks dynamically with the animations
        QObject::connect(pill, &QQuickItem::xChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::yChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::widthChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::heightChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::scaleChanged, pill, updateInputMask);
        QObject::connect(pill, &QQuickItem::opacityChanged, pill, updateInputMask);
        
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