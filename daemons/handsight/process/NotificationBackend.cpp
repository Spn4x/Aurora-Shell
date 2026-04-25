#include "NotificationBackend.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QDBusError>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <csignal>
#include <cstdlib>

NotificationBackend::NotificationBackend(QObject *parent) : QObject(parent) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.registerService("com.meismeric.auranotify.UI");
    bus.registerObject("/com/meismeric/auranotify/UI", this, QDBusConnection::ExportAllSlots);

    setupThemeWatcher();
    reloadTheme();
}

void NotificationBackend::setupThemeWatcher() {
    m_watcher = new QFileSystemWatcher(this);
    QString path = QDir::homePath() + "/.config/aurora-shell/aurora-colors.css";
    QString dirPath = QFileInfo(path).absolutePath();
    
    QDir().mkpath(dirPath);
    if (!QFile::exists(path)) {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("/* Default Fallbacks */\n@define-color gnome-bg #28282C;\n@define-color gnome-fg #D7E0BB;\n@define-color aurora_accent #6D6D6C;\n");
            file.close();
        }
    }
    
    m_watcher->addPath(path);
    m_watcher->addPath(dirPath);

    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &NotificationBackend::reloadTheme);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this, path]() {
        if (QFile::exists(path) && !m_watcher->files().contains(path)) {
            m_watcher->addPath(path); 
            reloadTheme();
        }
    });
}

void NotificationBackend::reloadTheme() {
    QString path = QDir::homePath() + "/.config/aurora-shell/aurora-colors.css";
    QFile file(path);
    QVariantMap newTheme;

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QRegularExpression re("@define-color\\s+([\\w_-]+)\\s+([^;]+);");
        
        while (!in.atEnd()) {
            QString line = in.readLine();
            QRegularExpressionMatch match = re.match(line);
            if (match.hasMatch()) {
                QString key = match.captured(1);
                QString val = match.captured(2).trimmed();
                newTheme[key] = val;
            }
        }
        file.close();
    }
    m_themeData = newTheme;
    emit themeChanged();
}

void NotificationBackend::ShowNotification(uint id, const QString &icon, const QString &summary, const QString &body, const QStringList &actions) {
    NotificationData data;
    data.id = id;
    data.icon = icon;
    data.summary = summary;
    data.body = body;

    for (int i = 0; i < actions.size() - 1; i += 2) {
        QString actionId = actions[i];
        QString actionLabel = actions[i+1];
        if (actionId != "default") {
            data.actions.append(QVariantMap{{"id", actionId}, {"label", actionLabel}});
        }
    }

    m_queue.enqueue(data);
    emit queueChanged();
    
    if (!m_isShowingNotif) processNext();
}

void NotificationBackend::SetPrivacyStatus(const QString &payload) {
    QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
    if (!doc.isArray()) return;
    
    QJsonArray arr = doc.array();

    // 1. Prune ignored PIDs that have stopped streaming
    for (int i = m_ignoredPids.size() - 1; i >= 0; --i) {
        uint igPid = m_ignoredPids[i];
        bool stillRunning = false;
        for (int j = 0; j < arr.size(); ++j) {
            // FIXED: Used .toInt() instead of .toUInt() for QJsonValueRef
            if (arr[j].toObject()["pid"].toInt() == (int)igPid) {
                stillRunning = true; break;
            }
        }
        if (!stillRunning) m_ignoredPids.removeAt(i);
    }

    // 2. Prune ignored Names that have stopped streaming
    for (int i = m_ignoredNames.size() - 1; i >= 0; --i) {
        QString igName = m_ignoredNames[i];
        bool stillRunning = false;
        for (int j = 0; j < arr.size(); ++j) {
            QJsonObject obj = arr[j].toObject();
            // FIXED: Used .toInt() instead of .toUInt() for QJsonValueRef
            if (obj["pid"].toInt() == 0 && obj["name"].toString() == igName) {
                stillRunning = true; break;
            }
        }
        if (!stillRunning) m_ignoredNames.removeAt(i);
    }
    
    QVariantList apps;
    bool globalHasMic = false, globalHasCam = false;

    // 3. Process incoming apps
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject obj = arr[i].toObject();
        uint pid = obj["pid"].toInt();
        QString name = obj["name"].toString();
        int type = obj["type"].toInt();
        
        // Skip apps that are currently in our active ignore lists
        if (pid > 0 && m_ignoredPids.contains(pid)) continue;
        if (pid == 0 && m_ignoredNames.contains(name)) continue;

        if (type == 0) globalHasMic = true;
        if (type == 1) globalHasCam = true;

        bool found = false;
        for (int j = 0; j < apps.size(); ++j) {
            QVariantMap existing = apps[j].toMap();
            if ((pid > 0 && existing["pid"].toUInt() == pid) || (pid == 0 && existing["name"].toString() == name)) {
                if (type == 0) existing["hasMic"] = true;
                if (type == 1) existing["hasCam"] = true;
                apps[j] = existing;
                found = true;
                break;
            }
        }

        if (!found) {
            QVariantMap app;
            app["pid"] = pid;
            app["name"] = name;
            app["hasMic"] = (type == 0);
            app["hasCam"] = (type == 1);
            apps.append(app);
        }
    }

    m_privacyApps = apps;
    m_privacyHasMic = globalHasMic;
    m_privacyHasCam = globalHasCam;

    if (apps.isEmpty()) m_privacySummary = "";
    else if (apps.size() == 1) m_privacySummary = apps.first().toMap()["name"].toString() + " is active";
    else m_privacySummary = QString::number(apps.size()) + " Apps active";

    emit privacyChanged();
    updateDisplayMode();
}

void NotificationBackend::ShowOSD(const QString &icon, double level) { 
    m_osdIcon = icon;
    m_osdLevel = level;
    m_isShowingOsd = true;
    
    emit osdChanged();
    updateDisplayMode();
}

void NotificationBackend::processNext() {
    if (!m_queue.isEmpty() && !m_isShowingNotif) {
        m_current = m_queue.dequeue();
        m_isShowingNotif = true;
        emit queueChanged();
        emit notificationChanged();
    }
    updateDisplayMode();
}

void NotificationBackend::readyForNext() {
    if (m_displayMode == "osd") {
        m_isShowingOsd = false;
    } else if (m_displayMode == "notification") {
        m_isShowingNotif = false;
    }
    
    processNext();
}

void NotificationBackend::updateDisplayMode() {
    QString oldMode = m_displayMode;

    if (m_isShowingOsd) {
        m_displayMode = "osd";
    } else if (m_isShowingNotif) {
        m_displayMode = "notification";
    } else if (!m_privacyApps.isEmpty()) {
        m_displayMode = "privacy";
    } else {
        m_displayMode = "idle";
    }

    if (m_displayMode != oldMode) {
        emit displayModeChanged();
        if (m_displayMode == "idle") {
            emit requestHide();
        } else {
            emit requestShow(); 
        }
    } else if (m_displayMode == "notification" || m_displayMode == "osd") {
        emit requestShow(); 
    }
}

void NotificationBackend::invokeAction(const QString& actionId) {
    QDBusMessage msg = QDBusMessage::createMethodCall("org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "InvokeAction");
    msg << m_current.id << actionId;
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void NotificationBackend::killPrivacyApp(uint pid, const QString& name) {
    if (pid > 0) kill(pid, SIGTERM);
    else if (!name.isEmpty()) {
        QString safeName = name.split(" ").first();
        QString cmd = QString("pkill -i '%1'").arg(safeName);
        system(cmd.toUtf8().constData());
    }
    ignorePrivacyApp(pid, name); 
}

void NotificationBackend::killAllPrivacyApps() {
    for (const QVariant& v : m_privacyApps) {
        QVariantMap map = v.toMap();
        uint pid = map["pid"].toUInt();
        QString name = map["name"].toString();
        
        if (pid > 0) kill(pid, SIGTERM);
        else if (!name.isEmpty()) {
            QString safeName = name.split(" ").first();
            QString cmd = QString("pkill -i '%1'").arg(safeName);
            system(cmd.toUtf8().constData());
        }
    }
    m_privacyApps.clear();
    m_privacySummary = "";
    m_privacyHasMic = false;
    m_privacyHasCam = false;
    emit privacyChanged();
    updateDisplayMode();
}

void NotificationBackend::ignorePrivacyApp(uint pid, const QString& name) {
    if (pid > 0) m_ignoredPids.append(pid);
    else if (!name.isEmpty()) m_ignoredNames.append(name);
    
    QVariantList filtered;
    bool globalHasMic = false, globalHasCam = false;
    for (const QVariant& v : m_privacyApps) {
        QVariantMap map = v.toMap();
        uint mPid = map["pid"].toUInt();
        QString mName = map["name"].toString();
        
        if (mPid > 0 && m_ignoredPids.contains(mPid)) continue;
        if (mPid == 0 && m_ignoredNames.contains(mName)) continue;
        
        if (map["hasMic"].toBool()) globalHasMic = true;
        if (map["hasCam"].toBool()) globalHasCam = true;
        filtered.append(map);
    }
    
    m_privacyApps = filtered;
    m_privacyHasMic = globalHasMic;
    m_privacyHasCam = globalHasCam;
    
    if (filtered.isEmpty()) m_privacySummary = "";
    else if (filtered.size() == 1) m_privacySummary = filtered.first().toMap()["name"].toString() + " is active";
    else m_privacySummary = QString::number(filtered.size()) + " Apps active";

    emit privacyChanged();
    updateDisplayMode();
}