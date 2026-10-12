import SwiftUI

struct RateTarget: Identifiable {
    let name: String
    var id: String { name }
}

struct ContentView: View {
    @ObservedObject private var model = LfgModel.shared

    @State private var showAdd = false
    @State private var showKey = false
    @State private var rateTarget: RateTarget?
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

                Section("Favourites") {
                    if favouriteEntries.isEmpty {
                        Text("Star a player to pin them here.")
                            .foregroundColor(.secondary)
                            .font(.footnote)
                    }
                    ForEach(favouriteEntries) { e in
                        row(e)
                    }
                }

                Section {
                    if model.entries.isEmpty {
                        Text("No one is LFG right now.\nMark yourself LFG or wait for sync.")
                            .foregroundColor(.secondary)
                            .font(.footnote)
                    }
                    ForEach(otherEntries) { e in
                        row(e)
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
                    HStack {
                        Button {
                            showKey = true
                        } label: {
                            Image(systemName: "key")
                        }
                        Button {
                            showAdd = true
                        } label: {
                            Image(systemName: "person.badge.plus")
                        }
                    }
                }
            }
            .sheet(isPresented: $showAdd) {
                AddCharacterView { name, zone, detail in
                    model.markLfg(name: name, zone: zone, detail: detail)
                }
            }
            .sheet(isPresented: $showKey) {
                KeyView()
            }
            .sheet(item: $rateTarget) { target in
                RateView(name: target.name) { c, f, l in
                    model.rate(name: target.name, competent: c, friendly: f, listens: l)
                }
            }
            .onAppear { model.start() }
            .onChange(of: server) { newValue in
                model.setServer(newValue)
            }
        }
    }

    private var favouriteEntries: [LfgModel.Entry] {
        model.entries.filter { model.isFavourite($0.name) }
    }

    private var otherEntries: [LfgModel.Entry] {
        model.entries.filter { !model.isFavourite($0.name) }
    }

    @ViewBuilder
    private func row(_ e: LfgModel.Entry) -> some View {
        HStack(spacing: 8) {
            Button {
                model.toggleFavourite(e.name)
            } label: {
                Image(systemName: model.isFavourite(e.name) ? "star.fill" : "star")
                    .foregroundColor(model.isFavourite(e.name) ? .yellow : .secondary)
            }
            .buttonStyle(.plain)
            VStack(alignment: .leading, spacing: 2) {
                Text(e.name)
                    .font(.body.weight(.semibold))
                Text(rowSubtitle(e))
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
            Spacer()
            RatingBadge(summary: model.ratingSummaries[e.name])
            NavigationLink {
                ChatView(name: e.name)
            } label: {
                Image(systemName: "bubble.right")
                    .foregroundColor(.blue)
            }
            .buttonStyle(.plain)
        }
        .swipeActions {
            Button(role: .destructive) {
                model.remove(name: e.name)
            } label: {
                Label("Remove", systemImage: "person.badge.minus")
            }
            Button {
                rateTarget = RateTarget(name: e.name)
            } label: {
                Label("Rate", systemImage: "star.square")
            }
            .tint(.orange)
        }
    }

    private func rowSubtitle(_ e: LfgModel.Entry) -> String {
        var parts = [e.detail]
        if !e.zone.isEmpty { parts.append(e.zone) }
        return parts.filter { !$0.isEmpty }.joined(separator: " · ")
    }
}
