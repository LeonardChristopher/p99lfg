import SwiftUI

/// Chat with one LFG player over NIP-04 direct messages.
struct ChatView: View {
    let name: String
    @ObservedObject private var model = LfgModel.shared

    @State private var draft = ""

    var body: some View {
        VStack(spacing: 0) {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(spacing: 6) {
                        ForEach(model.conversations[name] ?? []) { msg in
                            HStack {
                                if msg.fromMe { Spacer(minLength: 48) }
                                Text(msg.text)
                                    .padding(.horizontal, 12)
                                    .padding(.vertical, 8)
                                    .background(
                                        msg.fromMe
                                            ? Color.blue.opacity(0.85)
                                            : Color(.secondarySystemBackground)
                                    )
                                    .foregroundColor(msg.fromMe ? .white : .primary)
                                    .clipShape(RoundedRectangle(cornerRadius: 14))
                                if !msg.fromMe { Spacer(minLength: 48) }
                            }
                            .id(msg.id)
                        }
                    }
                    .padding()
                }
                .onChange(of: model.conversations[name]?.count) { _ in
                    if let last = model.conversations[name]?.last {
                        withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                    }
                }
                .onAppear {
                    if let last = model.conversations[name]?.last {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
            Divider()
            HStack(spacing: 8) {
                TextField("Message…", text: $draft)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(send)
                Button(action: send) {
                    Image(systemName: "paperplane.fill")
                }
                .disabled(draft.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            .padding()
        }
        .navigationTitle(name)
        .navigationBarTitleDisplayMode(.inline)
    }

    private func send() {
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        model.sendChat(to: name, text: text)
        draft = ""
    }
}
