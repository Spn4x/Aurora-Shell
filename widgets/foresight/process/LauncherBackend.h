#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QDBusContext>
#include <QFileSystemWatcher>
#include <QMetaType>
#include <QDBusArgument>

struct SearchResult {
    uint type;
    QString title;
    QString desc;
    QString icon;
    QString payload;
    int score;
};
Q_DECLARE_METATYPE(SearchResult)

inline QDBusArgument &operator<<(QDBusArgument &argument, const SearchResult &res) {
    argument.beginStructure();
    argument << res.type << res.title << res.desc << res.icon << res.payload << res.score;
    argument.endStructure();
    return argument;
}

inline const QDBusArgument &operator>>(const QDBusArgument &argument, SearchResult &res) {
    argument.beginStructure();
    argument >> res.type >> res.title >> res.desc >> res.icon >> res.payload >> res.score;
    argument.endStructure();
    return argument;
}

class LauncherBackend : public QObject, protected QDBusContext {
    Q_OBJECT

    Q_PROPERTY(QVariantMap themeData READ themeData NOTIFY themeChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)
    Q_PROPERTY(int currentMode READ currentMode NOTIFY modeChanged)

public:
    explicit LauncherBackend(QObject *parent = nullptr);

    QVariantMap themeData() const { return m_themeData; }
    QVariantList results() const { return m_results; }
    int currentMode() const { return m_currentMode; }

    Q_INVOKABLE void query(const QString& text);
    Q_INVOKABLE void activateResult(int index);
    Q_INVOKABLE void setMode(int mode); 
    Q_INVOKABLE void deleteClipboardItem(int index);

public slots:
    void clearState();

signals:
    void themeChanged();
    void resultsChanged();
    void modeChanged();
    void requestShow();
    void requestHide();

private slots:
    void reloadTheme();

private:
    void setupThemeWatcher();
    void launchApp(const QString& desktopId);

    QVariantMap m_themeData;
    QVariantList m_results;
    int m_currentMode = 0; 
    QFileSystemWatcher *m_watcher;
};