import AVFoundation
import SwiftUI

/// Share the Nostr identity between the Windows app and this phone.
/// Primary flow: scan the QR shown by the Windows app's "Show key QR".
/// Fallback: a small manual-paste link for when a camera/QR isn't available.
struct KeyView: View {
    @ObservedObject private var model = LfgModel.shared

    @Environment(\.dismiss) private var dismiss
    @State private var showScanner = false
    @State private var showManual = false
    @State private var importText = ""
    @State private var message: String?
    @State private var showError = false
    @State private var cameraDenied = false

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
                    Button {
                        requestCamera()
                    } label: {
                        Label("Scan key QR", systemImage: "qrcode.viewfinder")
                    }
                    if cameraDenied {
                        Button {
                            if let url = URL(string: UIApplication.openSettingsURLString) {
                                UIApplication.shared.open(url)
                            }
                        } label: {
                            Label("Enable camera in Settings", systemImage: "gear")
                        }
                    }
                } header: {
                    Text("Import from Windows")
                } footer: {
                    Text("Press \"Show key QR\" in the Windows app and scan it with this phone.")
                }

                Section {
                    if showManual {
                        SecureField("64-character secret key", text: $importText)
                            .textInputAutocapitalization(.never)
                            .disableAutocorrection(true)
                        Button("Import pasted key") { importKey(importText) }
                            .disabled(importText.trimmingCharacters(in: .whitespacesAndNewlines).count != 64)
                    }
                    Button(showManual ? "Hide manual paste" : "No QR? Paste key manually") {
                        withAnimation { showManual.toggle() }
                    }
                    .font(.footnote)
                } header: {
                    if showManual { Text("Manual paste (fallback)") }
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
            .fullScreenCover(isPresented: $showScanner) {
                ScannerContainer { scanned in
                    showScanner = false
                    importKey(scanned)
                } onFailure: { reason in
                    showScanner = false
                    if reason == "denied" {
                        cameraDenied = true
                        message = "Camera access was denied — use the manual paste option below."
                        showError = true
                    } else {
                        showManual = true
                        message = reason
                        showError = true
                    }
                }
            }
        }
    }

    private func requestCamera() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            showScanner = true
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { granted in
                DispatchQueue.main.async {
                    if granted { showScanner = true } else { cameraDenied = true }
                }
            }
        default:
            cameraDenied = true
        }
    }

    private func importKey(_ hex: String) {
        if let pub = model.importKey(hex: hex) {
            message = "Identity imported. Now publishing as \(pub.prefix(8))…"
            showError = false
            importText = ""
            showManual = false
        } else {
            message = "That doesn't look like a valid key (need 64 hex characters)."
            showError = true
        }
    }
}

/// Full-screen scanner with a close button; validates the payload looks like
/// a 64-hex key before accepting it.
struct ScannerContainer: View {
    var onScan: (String) -> Void
    var onFailure: (String) -> Void

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ZStack(alignment: .top) {
            QrScannerView(
                onScan: { value in
                    let hex = value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                    if hex.count == 64, hex.allSatisfy({ $0.isHexDigit }) {
                        onScan(hex)
                    } else {
                        onFailure("Scanned code isn't a p99lfg key.")
                    }
                },
                onError: { reason in
                    onFailure(reason)
                }
            )
            .ignoresSafeArea()
            HStack {
                Spacer()
                Button {
                    dismiss()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.title)
                        .foregroundColor(.white.opacity(0.8))
                }
                .padding()
            }
        }
    }
}
