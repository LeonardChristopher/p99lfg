import SwiftUI

/// P99 classes (Vanilla + Kunark era naming).
enum P99Class: String, CaseIterable, Identifiable {
    case bard, beastlord, cleric, druid, enchanter, magician
    case monk, necromancer, paladin, ranger, rogue, shadowknight
    case shaman, warrior, wizard

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .shadowknight: return "Shadow Knight"
        default: return rawValue.capitalized
        }
    }
}

/// Manual LFG entry: the game never runs on iOS, so this is the sole input
/// path — pick name, class, level (and optionally zone), and the listing is
/// published to the same Nostr relays the Windows app reads.
struct AddCharacterView: View {
    var onAdd: (String, String, String) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var zone = ""
    @State private var charClass: P99Class = .warrior
    @State private var level = 60

    var body: some View {
        NavigationView {
            Form {
                Section {
                    TextField("Character name", text: $name)
                        .textInputAutocapitalization(.never)
                        .disableAutocorrection(true)
                } header: {
                    Text("Character")
                }

                Section {
                    Picker("Class", selection: $charClass) {
                        ForEach(P99Class.allCases) { c in
                            Text(c.displayName).tag(c)
                        }
                    }
                    Stepper(value: $level, in: 1...65) {
                        Text("Level \(level)")
                    }
                    TextField("Zone", text: $zone)
                        .textInputAutocapitalization(.never)
                        .disableAutocorrection(true)
                } header: {
                    Text("Details")
                } footer: {
                    Text("Optional. Zone defaults to \"unrest\" style shorthand, e.g. sebilis, oasis.")
                }
            }
            .navigationTitle("Mark LFG")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Add") {
                        let detail = "\(level) \(charClass.displayName)"
                        onAdd(name.trimmingCharacters(in: .whitespaces),
                              zone.trimmingCharacters(in: .whitespaces),
                              detail)
                        dismiss()
                    }
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }
}
