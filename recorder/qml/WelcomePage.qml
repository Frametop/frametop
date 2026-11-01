// The recorders' Welcome page: the consent text, the three checkboxes and "Agree and continue",
// or, once agreed, the contributor id and "Continue". Both windows use it; what differs comes in
// as properties, and a window's own consent fields (the Hand Recorder's handedness) go inside it.
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: welcome
    title: "Welcome"
    // The window's draft banner text.
    property string draftText
    // Whether to say the text changed since it was agreed to.
    property bool changedSinceAgreed: false
    // Records the agreement: accept(adult, region, agree), the window's backend.acceptConsent.
    property var accept
    // Where Continue (and an accepted agreement) goes.
    property var next
    // The window's own consent fields, shown with the checkboxes.
    default property alias extra: extras.data

    header: DraftBanner { text: welcome.draftText }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        MarkdownText {
            Layout.fillWidth: true
            markdown: backend.consentText
        }

        Kirigami.Separator { Layout.fillWidth: true }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !backend.needsConsent
            type: Kirigami.MessageType.Positive
            text: "You agreed to this text (" + backend.consentAccepted + "). Your contributor id is "
                  + backend.contributor + ": keep it if you might want to withdraw later."
        }
        Controls.Button {
            visible: !backend.needsConsent
            text: "Copy contributor id"
            icon.name: "edit-copy"
            onClicked: backend.copy(backend.contributor)
        }
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: backend.needsConsent && welcome.changedSinceAgreed
            type: Kirigami.MessageType.Information
            text: "This text changed since you last agreed to it. Please read it again; your contributor id stays the same."
        }

        Controls.CheckBox {
            id: adult
            visible: backend.needsConsent
            text: "I'm 18 or older"
        }
        Controls.CheckBox {
            id: region
            visible: backend.needsConsent
            text: "I don't live in Illinois, Texas or Washington (USA)"
        }
        Controls.CheckBox {
            id: agree
            visible: backend.needsConsent
            text: "I agree to the text above"
        }
        ColumnLayout {
            id: extras
            visible: backend.needsConsent && children.length > 0
            spacing: Kirigami.Units.largeSpacing
        }
        Controls.Button {
            visible: backend.needsConsent
            enabled: adult.checked && region.checked && agree.checked
            text: "Agree and continue"
            icon.name: "go-next"
            onClicked: {
                welcome.accept(adult.checked, region.checked, agree.checked)
                if (!backend.needsConsent)
                    welcome.next()
            }
        }
        Controls.Button {
            visible: !backend.needsConsent
            text: "Continue"
            icon.name: "go-next"
            onClicked: welcome.next()
        }
    }
}
