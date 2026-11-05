// The texts wait for a legal review; until then nobody should contribute. Its text is the window's.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.InlineMessage {
    visible: backend.textsDraft
    position: Kirigami.InlineMessage.Position.Header
    type: Kirigami.MessageType.Warning
}
