// 1. Include GIO FIRST before any Qt headers can define the 'signals' macro
#include <gio/gio.h>

// 2. Now include the Qt headers
#include "LauncherBackend.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusMetaType>
#include <QProcess>
#include <QGuiApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>

LauncherBackend::LauncherBackend(const QString& widgetName, QObject *parent) : QObject(parent) {
    qDBusRegisterMetaType<SearchResult>();
    qDBusRegisterMetaType<QList<SearchResult>>();

    // Register our D-Bus interface so Aurora Shell can command us
    QString busName = "com.meismeric.aurora.widgets." + widgetName;
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.registerService(busName);
    bus.registerObject("/com/meismeric/aurora/widget", this, QDBusConnection::ExportAllSlots);

    setupThemeWatcher();
    reloadTheme();
}

// --- THE FIX: Proper Window Lifecycle Management ---

void LauncherBackend::Show() { 
    m_isVisible = true; 
    emit windowNeedsShow(); // Map the window immediately to grab keyboard focus
    emit requestShow();     // Tell QML to fade in
}

void LauncherBackend::Hide() { 
    m_isVisible = false; 
    emit requestHide();     // Tell QML to fade out
}

void LauncherBackend::notifyHidden() { 
    m_isVisible = false; 
    emit windowNeedsHide(); // Unmap the window so it drops the keyboard/mouse lock!
}

// ---------------------------------------------------

void LauncherBackend::clearState() {
    m_results.clear();
    emit resultsChanged();
}

void LauncherBackend::setMode(int mode) {
    if (m_currentMode != mode) {
        m_currentMode = mode;
        emit modeChanged();
    }
}

void LauncherBackend::query(const QString& text) {
    if (text.trimmed().isEmpty() && m_currentMode != 2) {
        m_results.clear();
        emit resultsChanged();
        return;
    }

    QString method = (m_currentMode == 2) ? "QueryClipboard" : "Query";
    QString payloadText = text;
    if (m_currentMode == 1) payloadText = "> " + text; 

    QDBusMessage msg = QDBusMessage::createMethodCall(
        "com.meismeric.auroralauncher", 
        "/com/meismeric/auroralauncher", 
        "com.meismeric.auroralauncher.Search", 
        method
    );
    msg << payloadText;

    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(msg);
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(call, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *watcher) {
        QDBusPendingReply<QList<SearchResult>> reply = *watcher;
        if (!reply.isError()) {
            QVariantList newResults;
            QList<SearchResult> list = reply.value();
            for (const auto& item : list) {
                QVariantMap map;
                map["type"] = item.type;
                map["title"] = item.title;
                map["desc"] = item.desc;
                map["icon"] = item.icon;
                map["payload"] = item.payload;
                newResults.append(map);
            }
            m_results = newResults;
            emit resultsChanged();
        } else {
            qWarning() << "D-Bus Query Error:" << reply.error().message();
        }
        watcher->deleteLater();
    });
}

void LauncherBackend::launchApp(const QString& desktopId) {
    GList *all_apps = g_app_info_get_all();
    GAppInfo *target_app = nullptr;

    for (GList *l = all_apps; l != nullptr; l = l->next) {
        if (g_strcmp0(g_app_info_get_id(G_APP_INFO(l->data)), desktopId.toUtf8().constData()) == 0) {
            target_app = G_APP_INFO(l->data);
            break;
        }
    }

    if (target_app) {
        g_app_info_launch(target_app, nullptr, nullptr, nullptr);
    }
    g_list_free_full(all_apps, g_object_unref);
}

void LauncherBackend::activateResult(int index) {
    if (index < 0 || index >= m_results.size()) return;

    QVariantMap res = m_results[index].toMap();
    uint type = res["type"].toUInt();
    QString payload = res["payload"].toString();

    if (type == 0) { 
        launchApp(payload);
    } else if (type == 1) { 
        QGuiApplication::clipboard()->setText(payload);
    } else if (type == 2) { 
        QProcess::startDetached("sh", QStringList() << "-c" << payload);
    } else if (type == 3) { 
        QDBusMessage msg = QDBusMessage::createMethodCall("com.meismeric.auroralauncher", "/com/meismeric/auroralauncher", "com.meismeric.auroralauncher.Search", "SetClipboardItem");
        msg << payload;
        QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
    }

    Hide();
}

void LauncherBackend::deleteClipboardItem(int index) {
    if (index < 0 || index >= m_results.size() || m_currentMode != 2) return;
    QString payload = m_results[index].toMap()["payload"].toString();
    
    QDBusMessage msg = QDBusMessage::createMethodCall("com.meismeric.auroralauncher", "/com/meismeric/auroralauncher", "com.meismeric.auroralauncher.Search", "DeleteClipboardItem");
    msg << payload;
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void LauncherBackend::setupThemeWatcher() {
    m_watcher = new QFileSystemWatcher(this);
    QString path = QDir::homePath() + "/.config/aurora-shell/aurora-colors.css";
    QString dirPath = QFileInfo(path).absolutePath();
    QDir().mkpath(dirPath);
    if (!QFile::exists(path)) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("/* Default */\n@define-color gnome-bg #28282C;\n@define-color gnome-fg #D7E0BB;\n@define-color aurora_accent #6D6D6C;\n");
            file.close();
        }
    }
    m_watcher->addPath(path);
    m_watcher->addPath(dirPath);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &LauncherBackend::reloadTheme);
}

void LauncherBackend::reloadTheme() {
    QString path = QDir::homePath() + "/.config/aurora-shell/aurora-colors.css";
    QFile file(path);
    QVariantMap newTheme;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QRegularExpression re("@define-color\\s+([\\w_-]+)\\s+([^;]+);");
        while (!in.atEnd()) {
            QRegularExpressionMatch match = re.match(in.readLine());
            if (match.hasMatch()) newTheme[match.captured(1)] = match.captured(2).trimmed();
        }
        file.close();
    }
    m_themeData = newTheme;
    emit themeChanged();
}