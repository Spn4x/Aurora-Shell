#pragma once

#include <QObject>
#include <QVariantList>
#include <QStringList>
#include <QString>
#include <QQueue>
#include <QDBusContext>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QList>
#include <QFileSystemWatcher>

struct NotificationData {
    uint id;
    QString icon;
    QString summary;
    QString body;
    QVariantList actions;
};

class NotificationBackend : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.meismeric.auranotify.UI")

    Q_PROPERTY(QString displayMode READ displayMode NOTIFY displayModeChanged) 
    Q_PROPERTY(QVariantMap themeData READ themeData NOTIFY themeChanged)

    // Notification Properties
    Q_PROPERTY(QString summary READ summary NOTIFY notificationChanged)
    Q_PROPERTY(QString body READ body NOTIFY notificationChanged)
    Q_PROPERTY(QString icon READ icon NOTIFY notificationChanged)
    Q_PROPERTY(QVariantList actions READ actions NOTIFY notificationChanged)
    Q_PROPERTY(bool hasActions READ hasActions NOTIFY notificationChanged)
    Q_PROPERTY(int pendingNotifications READ pendingNotifications NOTIFY queueChanged)

    // Privacy Properties
    Q_PROPERTY(QVariantList privacyApps READ privacyApps NOTIFY privacyChanged)
    Q_PROPERTY(QString privacySummary READ privacySummary NOTIFY privacyChanged)
    Q_PROPERTY(bool privacyHasMic READ privacyHasMic NOTIFY privacyChanged)
    Q_PROPERTY(bool privacyHasCam READ privacyHasCam NOTIFY privacyChanged)

    // OSD Properties
    Q_PROPERTY(QString osdIcon READ osdIcon NOTIFY osdChanged)
    Q_PROPERTY(double osdLevel READ osdLevel NOTIFY osdChanged)

public:
    explicit NotificationBackend(QObject *parent = nullptr);

    QString displayMode() const { return m_displayMode; }
    QVariantMap themeData() const { return m_themeData; }

    QString summary() const { return m_current.summary; }
    QString body() const { return m_current.body; }
    QString icon() const { return m_current.icon; }
    QVariantList actions() const { return m_current.actions; }
    bool hasActions() const { return !m_current.actions.isEmpty(); }
    int pendingNotifications() const { return m_queue.size(); }

    QVariantList privacyApps() const { return m_privacyApps; }
    QString privacySummary() const { return m_privacySummary; }
    bool privacyHasMic() const { return m_privacyHasMic; }
    bool privacyHasCam() const { return m_privacyHasCam; }

    QString osdIcon() const { return m_osdIcon; }
    double osdLevel() const { return m_osdLevel; }

    Q_INVOKABLE void invokeAction(const QString& actionId);
    Q_INVOKABLE void readyForNext();
    
    Q_INVOKABLE void killPrivacyApp(uint pid, const QString& name);
    Q_INVOKABLE void ignorePrivacyApp(uint pid, const QString& name);
    Q_INVOKABLE void killAllPrivacyApps(); 

public slots: 
    void ShowNotification(uint id, const QString &icon, const QString &summary, const QString &body, const QStringList &actions);
    void SetPrivacyStatus(const QString &payload);
    void ShowOSD(const QString &icon, double level); 

signals:
    void displayModeChanged();
    void themeChanged();
    void notificationChanged();
    void queueChanged();
    void privacyChanged();
    void osdChanged();
    void requestShow(); 
    void requestHide(); 

private slots:
    void reloadTheme();

private:
    void processNext();
    void updateDisplayMode();
    void setupThemeWatcher();

    QString m_displayMode = "idle";
    QQueue<NotificationData> m_queue;
    NotificationData m_current;
    
    // State Tracking
    bool m_isShowingNotif = false;
    bool m_isShowingOsd = false;

    // Privacy Data
    QVariantList m_privacyApps;
    QString m_privacySummary;
    bool m_privacyHasMic = false;
    bool m_privacyHasCam = false;
    QList<uint> m_ignoredPids;
    QStringList m_ignoredNames;

    // OSD Data
    QString m_osdIcon;
    double m_osdLevel = 0.0;

    QFileSystemWatcher *m_watcher;
    QVariantMap m_themeData;
};