import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Aurora.Foresight

Rectangle {
    id: container
    
    ListModel { id: searchModel }

    Connections {
        target: Backend
        function onResultsChanged() {
            let newRes = Backend.results;
            
            for (let i = 0; i < newRes.length; i++) {
                if (i < searchModel.count) {
                    let existing = searchModel.get(i);
                    if (existing.modelPayload !== newRes[i].payload || existing.modelTitle !== newRes[i].title || existing.modelIcon !== newRes[i].icon) {
                        searchModel.setProperty(i, "modelType", newRes[i].type);
                        searchModel.setProperty(i, "modelTitle", newRes[i].title);
                        searchModel.setProperty(i, "modelDesc", newRes[i].desc);
                        searchModel.setProperty(i, "modelIcon", newRes[i].icon);
                        searchModel.setProperty(i, "modelPayload", newRes[i].payload);
                    }
                } else {
                    searchModel.append({
                        "modelType": newRes[i].type,
                        "modelTitle": newRes[i].title,
                        "modelDesc": newRes[i].desc,
                        "modelIcon": newRes[i].icon,
                        "modelPayload": newRes[i].payload
                    });
                }
            }
            
            while (searchModel.count > newRes.length) {
                searchModel.remove(searchModel.count - 1);
            }
            
            if (searchModel.count === 0) {
                listView.currentIndex = -1;
            } else if (listView.currentIndex >= searchModel.count) {
                listView.currentIndex = searchModel.count - 1;
            } else if (listView.currentIndex === -1) {
                listView.currentIndex = 0; 
            }
        }
    }

    property int maxRows: 8
    property int currentRows: Math.min(searchModel.count, maxRows)
    
    property int targetHeight: AppTheme.searchHeight + 10 + (currentRows > 0 ? (currentRows * AppTheme.itemHeight) + ((currentRows - 1) * 2) + 8 : 0)

    width: AppTheme.launcherWidth
    height: targetHeight
    radius: AppTheme.boxRadius
    color: AppTheme.bg
    border.color: AppTheme.borderAlpha
    border.width: 2
    clip: true

    Behavior on height { 
        id: heightAnim
        NumberAnimation { duration: 200; easing.type: Easing.OutCubic } 
    }
    
    opacity: 0
    scale: 0.95
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 150 } }
    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutBack } }

    function show() {
        Backend.clearState();
        searchModel.clear(); 
        inputField.text = "";
        opacity = 1;
        scale = 1.0;
        inputField.forceActiveFocus();
        if (Backend.currentMode === 2) Backend.query(""); 
    }

    function hide() {
        opacity = 0;
        scale = 0.95;
    }

    Keys.onEscapePressed: hide()

    component SystemIcon: Button {
        property string iconName: ""
        property color iconColor: "transparent" 
        property int size: 36 
        width: size; height: size
        icon.name: iconName
        icon.color: iconColor 
        icon.width: size; icon.height: size
        background: Item {} 
        focusPolicy: Qt.NoFocus; hoverEnabled: false; down: false
    }

    Timer {
        id: searchDebouncer
        interval: 50
        onTriggered: Backend.query(inputField.text)
    }

    Timer {
        id: reenableAnimTimer
        interval: 100
        onTriggered: heightAnim.enabled = true
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 5 
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: AppTheme.searchHeight
            Layout.bottomMargin: searchModel.count > 0 ? 8 : 0 
            color: AppTheme.surfaceAlpha
            radius: AppTheme.entryRadius
            border.color: inputField.activeFocus ? AppTheme.accent : "transparent"
            border.width: 2 

            Behavior on border.color { ColorAnimation { duration: 200 } }

            RowLayout {
                anchors.fill: parent
                anchors.margins: 8 
                spacing: 8

                SystemIcon {
                    Layout.alignment: Qt.AlignVCenter
                    iconColor: AppTheme.fg 
                    size: 20
                    iconName: {
                        if (Backend.currentMode === 1) return "utilities-terminal-symbolic"
                        if (Backend.currentMode === 2) return "edit-paste-symbolic"
                        return "system-search-symbolic"
                    }
                }

                TextField {
                    id: inputField
                    Layout.fillWidth: true
                    color: AppTheme.fg
                    font.pixelSize: 16 
                    font.bold: true
                    background: Item {} 
                    
                    placeholderText: {
                        if (Backend.currentMode === 1) return "Run Command..."
                        if (Backend.currentMode === 2) return "Search Clipboard..."
                        return "Search Apps or Math..."
                    }
                    placeholderTextColor: Qt.rgba(AppTheme.fg.r, AppTheme.fg.g, AppTheme.fg.b, 0.4)
                    
                    onTextEdited: searchDebouncer.restart()

                    Keys.onUpPressed: (event) => { listView.decrementCurrentIndex(); event.accepted = true }
                    Keys.onDownPressed: (event) => { listView.incrementCurrentIndex(); event.accepted = true }
                    
                    Keys.onTabPressed: (event) => {
                        heightAnim.enabled = false;
                        Backend.setMode((Backend.currentMode + 1) % 3);
                        Backend.query(text);
                        reenableAnimTimer.restart();
                        event.accepted = true;
                    }
                    
                    Keys.onReturnPressed: (event) => {
                        if (listView.currentIndex >= 0 && listView.currentIndex < searchModel.count) {
                            Backend.activateResult(listView.currentIndex)
                        }
                        event.accepted = true
                    }
                    Keys.onDeletePressed: (event) => {
                        if (Backend.currentMode === 2 && listView.currentIndex >= 0) {
                            Backend.deleteClipboardItem(listView.currentIndex)
                            Backend.query(text)
                        }
                    }
                }
            }
        }

        ListView {
            id: listView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: searchModel
            currentIndex: 0
            boundsBehavior: Flickable.StopAtBounds
            spacing: 2 

            delegate: Rectangle {
                width: listView.width
                height: AppTheme.itemHeight
                
                property bool isSelected: ListView.isCurrentItem
                radius: isSelected ? AppTheme.rowSelectedRadius : AppTheme.rowRadius

                color: isSelected ? AppTheme.accent : (mouseArea.containsMouse ? Qt.rgba(1, 1, 1, 0.05) : "transparent")
                Behavior on color { ColorAnimation { duration: 150 } }

                MouseArea {
                    id: mouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: listView.currentIndex = index
                    onClicked: Backend.activateResult(index)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 6 
                    spacing: 12

                    Item {
                        Layout.preferredWidth: 38
                        Layout.preferredHeight: 38

                        SystemIcon {
                            visible: !modelIcon.startsWith("/") 
                            anchors.centerIn: parent
                            iconName: modelIcon.startsWith("/") ? "" : modelIcon
                            size: 36 
                        }

                        Image {
                            visible: modelIcon.startsWith("/")
                            anchors.fill: parent
                            source: modelIcon.startsWith("/") ? "file://" + modelIcon : ""
                            fillMode: Image.PreserveAspectCrop
                            Rectangle { anchors.fill: parent; color: "transparent"; border.color: Qt.rgba(1, 1, 1, 0.1); border.width: 1; radius: 4 }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            Layout.fillWidth: true
                            text: modelTitle
                            color: isSelected ? AppTheme.selectedText : AppTheme.fg
                            font.pixelSize: 15
                            font.bold: true
                            elide: Text.ElideRight
                            // THE FIX: Sync text color animation with background animation
                            Behavior on color { ColorAnimation { duration: 150 } }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelDesc
                            color: isSelected ? Qt.rgba(AppTheme.selectedText.r, AppTheme.selectedText.g, AppTheme.selectedText.b, 0.8) : Qt.rgba(AppTheme.fg.r, AppTheme.fg.g, AppTheme.fg.b, 0.6)
                            font.pixelSize: 13 
                            elide: Text.ElideRight
                            // THE FIX: Sync text color animation with background animation
                            Behavior on color { ColorAnimation { duration: 150 } }
                        }
                    }
                }
            }
        }
    }
}