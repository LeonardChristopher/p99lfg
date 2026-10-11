import SwiftUI

struct ContentView: View {
    @ObservedObject private var model = LfgModel.shared

    @State private var showAdd = false
    @State private var server = "green"

    var body: some View {
        NavigationView {
            List {
                Section {
                    Picker("Server", selection: $server) {
                        Text("Green").tag("green")
                        Text("Blue").tag("blue")
                    }
                    .pickerStyle(.segmented)
                } header: {
                    Text("Project 1999 server")
                }

                Section {
                    if model.entries.isEmpty {
                        Text("No one is LFG right now.\nMark yourself LFG or wait for sync.")
                            .foregroundColor(.secondary)
                            .font(.footnote)
                    }
                    ForEach(model.entries) { e in
                        HStack(spacing: 8) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(e.name)
                                    .font(.body.weight(.semibold))
                                if !e.zone.isEmpty || !e.detail.isEmpty {
                                    Text("\(e.zone) \(e.detail)")
                                        .font(.footnote)
                                        .foregroundColor(.secondary)
                                }
                            }
                            Spacer()
                            Text("LFG")
                                .font(.caption2.weight(.bold))
                                .foregroundColor(.white)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                                .background(Capsule().fill(Color.blue))
                        }
                        .swipeActions {
                            Button(role: .destructive) {
                                model.remove(name: e.name)
                            } label: {
                                Label("Remove", systemImage: "person.badge.minus")
                            }
                        }
                    }
                } header: {
                    Text("Looking for group")
                } footer: {
                    if let last = model.lastSync {
                        Text("Synced \(last, style: .relative) ago")
                    }
                }

                Section {
                    Text(model.relayStatus)
                        .font(.caption2.monospaced())
                        .foregroundColor(.secondary)
                        .textSelection(.enabled)
                } header: {
                    Text("Relays")
                }
            }
            .navigationTitle("p99lfg")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        showAdd = true
                    } label: {
                        Image(systemName: "person.badge.plus")
                    }
                }
            }
            .sheet(isPresented: $showAdd) {
                AddCharacterView { name, zone, detail in
                    model.markLfg(name: name, zone: zone, detail: detail)
                }
            }
            .onAppear { model.start() }
        }
    }
}

struct AddCharacterView: View {
    var onAdd: (String, String, String) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var zone = ""
    @State private var detail = ""

    var body: some View {
        NavigationView {
            Form {
                Section {
                    TextField("Character name", text: $name)
                } header: {
                    Text("Required")
                }
                Section {
                    TextField("Zone", text: $zone)
                    TextField("Class and level", text: $detail)
                } header: {
                    Text("Optional")
                } footer: {
                    Text("Stays listed until removed or stale (30 min).")
                }
            }
            .navigationTitle("Mark LFG")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Add") {
                        onAdd(name.trimmingCharacters(in: .whitespaces),
                              zone.trimmingCharacters(in: .whitespaces),
                              detail.trimmingCharacters(in: .whitespaces))
                        dismiss()
                    }
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }
}
