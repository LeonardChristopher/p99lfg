import SwiftUI

/// Rate a player: competent / friendly / listens, 1-5 each. Published as a
/// replaceable kind-30079 event so re-rating overwrites.
struct RateView: View {
    let name: String
    var onRate: (Int, Int, Int) -> Void

    @Environment(\.dismiss) private var dismiss
    @State private var competent = 3
    @State private var friendly = 3
    @State private var listens = 3

    var body: some View {
        NavigationView {
            Form {
                Section {
                    Stepper(value: $competent, in: 1...5) {
                        HStack {
                            Text("Competent")
                            Spacer()
                            stars(competent)
                        }
                    }
                    Stepper(value: $friendly, in: 1...5) {
                        HStack {
                            Text("Friendly")
                            Spacer()
                            stars(friendly)
                        }
                    }
                    Stepper(value: $listens, in: 1...5) {
                        HStack {
                            Text("Listens")
                            Spacer()
                            stars(listens)
                        }
                    }
                } header: {
                    Text("Rate \(name)")
                } footer: {
                    Text("Ratings are public on the relays. Your rating can be voted down by consensus if it looks malicious.")
                }
            }
            .navigationTitle("Rate player")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Publish") {
                        onRate(competent, friendly, listens)
                        dismiss()
                    }
                }
            }
        }
    }

    private func stars(_ n: Int) -> some View {
        HStack(spacing: 1) {
            ForEach(1...5, id: \.self) { i in
                Image(systemName: i <= n ? "star.fill" : "star")
                    .font(.caption)
                    .foregroundColor(.yellow)
            }
        }
    }
}

/// Compact rating badge shown on list rows.
struct RatingBadge: View {
    let summary: LfgModel.RatingSummary?

    var body: some View {
        if let s = summary {
            VStack(alignment: .trailing, spacing: 1) {
                Text("★ \(s.competent)")
                    .font(.caption2.monospacedDigit())
                    .foregroundColor(.orange)
                Text("\(s.count) rating\(s.count == 1 ? "" : "s")")
                    .font(.system(size: 8))
                    .foregroundColor(.secondary)
            }
        } else {
            EmptyView()
        }
    }
}
