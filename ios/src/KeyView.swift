import SwiftUI

/// Share the Nostr identity between the Windows app and this phone.
/// The secret key *is* the identity: import the Windows key here, or copy
/// this device's key into the Windows app, and both publish/own the same
/// LFG listings.
struct KeyView: View {
    @ObservedObject private var model = LfgModel.shared

    @Environment(\.dismiss) private var dismiss
    @State private var importText = ""
    @State private var exportedKey: String?
    @State private var message: String?
    @State private var showError = false

    var body: some View {
        NavigationView {
            Form {
                Section {
                    Text(model.myPubkey)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                } header: {
                    Text("Your Nostr pubkey")
                } footer: {
                    Text("This is who owns your LFG listings on the relays.")
                }

                Section {
                    SecureField("64-character secret key", text: $importText)
                        .textInputAutocapitalization(.never)
                        .disableAutocorrection(true)
                    Button("Import key") {
                        if let pub = model.importKey(hex: importText) {
                            message = "Identity imported. Now publishing as \(pub.prefix(8))…"
                            showError = false
                            importText = ""
                        } else {
                            message = "That doesn't look like a valid key (need 64 hex characters)."
                            showError = true
                        }
                    }
                    .disabled(importText.trimmingCharacters(in: .whitespacesAndNewlines).count != 64)
                } header: {
                    Text("Import from Windows")
                } footer: {
                    Text("In the Windows app press \"Copy key\", then paste it here. Both devices will then share the same LFG identity.")
                }

                Section {
                    Button("Show my key") {
                        exportedKey = model.exportKeyHex()
                    }
                    if let key = exportedKey {
                        Text(key)
                            .font(.caption.monospaced())
                            .textSelection(.enabled)
                        Button {
                            UIPasteboard.general.string = key
                            message = "Copied. Paste it into the Windows app with \"Paste key\"."
                            showError = false
                        } label: {
                            Label("Copy", systemImage: "doc.on.doc")
                        }
                    }
                } header: {
                    Text("Export to another device")
                } footer: {
                    Text("Anyone with this key can publish as you — share it only with your own devices.")
                }

                if let message {
                    Text(message)
                        .foregroundColor(showError ? .red : .green)
                        .font(.footnote)
                }
            }
            .navigationTitle("Nostr Key")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
