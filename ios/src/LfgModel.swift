import Foundation

/// Wraps the portable p99lfg C core (core.c/nostr.c/sync.c/ws_client.c) for SwiftUI.
///
/// All LfgState/SyncState access funnels through `stateQueue` (serial) because
/// the C core is single-threaded state and the sync thread mutates it from
/// its callback while the main thread reads entries.
final class LfgModel: ObservableObject {
    static let shared = LfgModel()

    @Published private(set) var entries: [Entry] = []
    @Published private(set) var myPubkey: String = ""
    @Published private(set) var relayStatus: String = ""
    @Published private(set) var lastSync: Date?
    @Published private(set) var favourites: [String] = []
    @Published private(set) var conversations: [String: [ChatMessage]] = [:]

    struct Entry: Identifiable, Equatable {
        let id: String
        let name: String
        let zone: String
        let detail: String
        let source: Int32
        let lastSeen: Date
    }

    struct ChatMessage: Identifiable, Equatable {
        let id: String
        let fromMe: Bool
        let text: String
        let at: Date
    }

    private var state: OpaquePointer?
    private var syncState: UnsafeMutablePointer<SyncState>?
    private var favList: OpaquePointer?
    private var syncThread: Thread?
    private var running = false
    private var wrapper: CallbackWrapper?
    private var dmWrapper: DmCallbackWrapper?

    private static let relays = ["wss://nos.lol", "wss://relay.damus.io"]

    private let stateQueue = DispatchQueue(label: "p99lfg.state")
    private let syncQueue = DispatchQueue(label: "p99lfg.syncstate")
    private let publishQueue = DispatchQueue(label: "p99lfg.publish")

    private var keyPath: String {
        let dir = NSHomeDirectory() + "/Documents"
        return dir + "/p99lfg.key"
    }

    private var favPath: String {
        let dir = NSHomeDirectory() + "/Documents"
        return dir + "/p99lfg_favourites.txt"
    }

    private final class CallbackWrapper {
        let callback: SyncOnEvent
        init(_ cb: @escaping @convention(c) (UnsafePointer<SyncRemoteEntry>?, UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Void) {
            callback = cb
        }
    }

    private final class DmCallbackWrapper {
        let callback: @convention(c) (UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Void
        init(_ cb: @escaping @convention(c) (UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Void) {
            callback = cb
        }
    }

    init() {
        let keyPath = self.keyPath

        state = lfg_new()
        lfg_set_stale_minutes(state, 30)
        favList = fav_load(favPath)

        var pubkey = [CChar](repeating: 0, count: 65)
        nostr_identity_load(keyPath, &pubkey)
        myPubkey = String(cString: pubkey)

        syncState = UnsafeMutablePointer<SyncState>.allocate(capacity: 1)
        syncState.initialize(to: SyncState())

        wrapper = CallbackWrapper { entry, pubkey, userdata in
            guard let entry, let userdata else { return }
            let model = Unmanaged<LfgModel>.fromOpaque(userdata).takeUnretainedValue()
            if entry.pointee.lfg != 0, let pubkey {
                model.recordPubkey(name: String(cString: entry.pointee.name),
                                   pubkey: String(cString: pubkey))
            }
            model.stateQueue.sync {
                if entry.pointee.lfg != 0 {
                    lfg_add_manual(model.state, entry.pointee.name,
                                   entry.pointee.zone, entry.pointee.detail)
                } else {
                    _ = lfg_remove(model.state, entry.pointee.name)
                }
            }
            DispatchQueue.main.async { model.refresh() }
        }

        dmWrapper = DmCallbackWrapper { sender, content, userdata in
            guard let sender, let content, let userdata else { return }
            let model = Unmanaged<LfgModel>.fromOpaque(userdata).takeUnretainedValue()
            model.handleIncomingDm(sender: String(cString: sender),
                                    content: String(cString: content))
        }

        sync_init(syncState, keyPath, "green", wrapper!.callback,
                  Unmanaged.passUnretained(self).toOpaque())
        sync_set_dm_callback(syncState, dmWrapper!.callback,
                             Unmanaged.passUnretained(self).toOpaque())
        for r in Self.relays {
            r.withCString { sync_add_relay(syncState, $0) }
        }
        refreshFavourites()
        refresh()
    }

    deinit {
        stop()
        stateQueue.sync {
            if let syncState {
                syncState.deallocate()
            }
            if let state {
                lfg_free(state)
            }
            if let favList {
                fav_free(favList)
            }
        }
    }

    func start() {
        guard !running else { return }
        running = true
        let thread = Thread { [weak self] in
            guard let self else { return }
            while self.running {
                autoreleasepool {
                    self.syncQueue.sync {
                        sync_poll(self.syncState)
                    }
                    var status = ""
                    self.syncQueue.sync {
                        if let sp = self.syncState {
                            for i in 0..<Int(sp.pointee.nrelays) where sp.pointee.relays[i].enabled != 0 {
                                let url = String(cString: sp.pointee.relays[i].url)
                                status += url + (sp.pointee.relays[i].connected != 0 ? " ✓" : " ✗") + "\n"
                            }
                        }
                    }
                    let now = Date()
                    DispatchQueue.main.async {
                        self.relayStatus = status
                        self.lastSync = now
                    }
                }
                Thread.sleep(forTimeInterval: 5)
            }
        }
        thread.name = "p99lfg-sync"
        syncThread = thread
        thread.start()
    }

    func stop() {
        running = false
    }

    func refresh() {
        guard let state else { return }
        var out: [Entry] = []
        stateQueue.sync {
            let n = lfg_count(state)
            for i in 0..<n {
                guard let e = lfg_get(state, i) else { continue }
                out.append(Entry(
                    id: String(cString: e.pointee.name),
                    name: String(cString: e.pointee.name),
                    zone: String(cString: e.pointee.zone),
                    detail: String(cString: e.pointee.detail),
                    source: e.pointee.source,
                    lastSeen: Date(timeIntervalSince1970: TimeInterval(e.pointee.last_seen))
                ))
            }
        }
        entries = out
    }

    // MARK: - Favourites

    private func refreshFavourites() {
        guard let favList else { return }
        var out: [String] = []
        stateQueue.sync {
            let n = fav_count(favList)
            for i in 0..<n {
                if let name = fav_get(favList, i) {
                    out.append(String(cString: name))
                }
            }
        }
        favourites = out
    }

    func isFavourite(_ name: String) -> Bool {
        favourites.contains(name)
    }

    func toggleFavourite(_ name: String) {
        guard let favList else { return }
        stateQueue.sync {
            _ = fav_toggle(favList, name)
        }
        refreshFavourites()
    }

    // MARK: - Chat

    func sendChat(to name: String, text: String) {
        guard let pub = pubkeyFor(name), let sp = syncState else { return }
        appendMessage(name: name, text: text, fromMe: true)
        publishQueue.async {
            pub.withCString { pp in
                text.withCString { tp in
                    _ = self.syncQueue.sync { chat_send_message(sp, pp, tp) }
                }
            }
        }
    }

    /// Map of character name -> nostr pubkey from recent LFG events.
    private var nameToPubkey: [String: String] = [:]

    func recordPubkey(name: String, pubkey: String) {
        nameToPubkey[name] = pubkey
    }

    func pubkeyFor(_ name: String) -> String? {
        nameToPubkey[name]
    }

    private func handleIncomingDm(sender: String, content: String) {
        guard let sp = syncState else { return }
        var plain = [CChar](repeating: 0, count: 600)
        sender.withCString { sptr in
            content.withCString { cptr in
                _ = chat_try_decrypt(sp.pointee.privkey, sptr, cptr,
                                     &plain, plain.count)
            }
        }
        let text = String(cString: plain)
        guard !text.isEmpty else { return }
        let name = nameToPubkey.first(where: { $0.value == sender })?.key ?? sender.prefix(8) + "…"
        DispatchQueue.main.async {
            self.appendMessage(name: name, text: text, fromMe: false)
        }
    }

    private func appendMessage(name: String, text: String, fromMe: Bool) {
        let msg = ChatMessage(
            id: UUID().uuidString,
            fromMe: fromMe,
            text: text,
            at: Date()
        )
        DispatchQueue.main.async {
            self.conversations[name, default: []].append(msg)
        }
    }
}
