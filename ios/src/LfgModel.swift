import Foundation

/// Wraps the portable p99lfg C core (core.c/nostr.c/sync.c/ws_client.c) for SwiftUI.
///
/// All LfgState/SyncState access funnels through serial queues because the C
/// core is single-threaded state and the sync thread mutates it from its
/// callbacks while the main thread reads entries.
final class LfgModel: ObservableObject {
    static let shared = LfgModel()

    @Published private(set) var entries: [Entry] = []
    @Published private(set) var myPubkey: String = ""
    @Published private(set) var relayStatus: String = ""
    @Published private(set) var lastSync: Date?
    @Published private(set) var favourites: [String] = []
    @Published private(set) var conversations: [String: [ChatMessage]] = [:]
    @Published private(set) var ratingSummaries: [String: RatingSummary] = [:]

    struct Entry: Identifiable, Equatable {
        let id: String
        let name: String
        let zone: String
        let detail: String
        let source: Int32
        let lastSeen: Date
    }

    struct RatingSummary: Equatable {
        let competent: Int
        let friendly: Int
        let listens: Int
        let count: Int
    }

    struct ChatMessage: Identifiable, Equatable {
        let id: String
        let fromMe: Bool
        let text: String
        let at: Date
    }

    private var state: OpaquePointer?                       /* LfgState*   */
    private var favList: OpaquePointer?                    /* FavList*    */
    private var ratingStore: UnsafeMutablePointer<RatingStore>?
    private var syncState: UnsafeMutablePointer<SyncState>?
    private var syncThread: Thread?
    private var running = false
    private var wrapper: CallbackWrapper?
    private var dmWrapper: DmCallbackWrapper?
    private var genericWrapper: GenericCallbackWrapper?

    private static let relays = ["wss://nos.lol", "wss://relay.damus.io"]

    private let stateQueue = DispatchQueue(label: "p99lfg.state")
    private let syncQueue = DispatchQueue(label: "p99lfg.syncstate")
    private let publishQueue = DispatchQueue(label: "p99lfg.publish")

    private var keyPath: String {
        NSHomeDirectory() + "/Documents/p99lfg.key"
    }

    private var favPath: String {
        NSHomeDirectory() + "/Documents/p99lfg_favourites.txt"
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

    private final class GenericCallbackWrapper {
        let callback: @convention(c) (Int32, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UInt64, UnsafeMutableRawPointer?) -> Void
        init(_ cb: @escaping @convention(c) (Int32, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UnsafePointer<CChar>?, UInt64, UnsafeMutableRawPointer?) -> Void) {
            callback = cb
        }
    }

    init() {
        state = lfg_new()
        lfg_set_stale_minutes(state, 30)
        favList = fav_load(favPath)

        var pubkey = [CChar](repeating: 0, count: 65)
        nostr_identity_load(keyPath, &pubkey)
        myPubkey = String(cString: pubkey)

        let ss = UnsafeMutablePointer<SyncState>.allocate(capacity: 1)
        ss.initialize(to: SyncState())
        syncState = ss

        wrapper = CallbackWrapper { entry, pubkey, userdata in
            guard let entry, let userdata else { return }
            let model = Unmanaged<LfgModel>.fromOpaque(userdata).takeUnretainedValue()
            if entry.pointee.lfg != 0, let pubkey {
                model.recordPubkey(name: String(cString: sync_entry_name(entry)),
                                   pubkey: String(cString: pubkey))
            }
            model.stateQueue.sync {
                if entry.pointee.lfg != 0 {
                    lfg_add_manual(model.state,
                                   sync_entry_name(entry),
                                   sync_entry_zone(entry),
                                   sync_entry_detail(entry))
                } else {
                    _ = lfg_remove(model.state, sync_entry_name(entry))
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

        let rs = rating_store_new()
        ratingStore = rs
        genericWrapper = GenericCallbackWrapper { kind, pubkey, dtag, content, createdAt, userdata in
            guard let pubkey, let dtag, let content, let userdata else { return }
            let model = Unmanaged<LfgModel>.fromOpaque(userdata).takeUnretainedValue()
            model.stateQueue.sync {
                if kind == 30079 {
                    _ = rating_record(model.ratingStore, pubkey, dtag, content, createdAt)
                } else if kind == 30080 {
                    let agree: Int32 = String(cString: content).contains("\"agree\":true") ? 1 : 0
                    _ = vote_record(model.ratingStore, pubkey, dtag, agree, createdAt)
                }
            }
            DispatchQueue.main.async { model.refreshRatings() }
        }

        keyPath.withCString { kp in
            sync_init(ss, kp, "green", wrapper!.callback,
                      Unmanaged.passUnretained(self).toOpaque())
        }
        sync_set_dm_callback(ss, dmWrapper!.callback,
                             Unmanaged.passUnretained(self).toOpaque())
        sync_set_generic_callback(ss, genericWrapper!.callback,
                                  Unmanaged.passUnretained(self).toOpaque())
        for r in Self.relays {
            r.withCString { sync_add_relay(ss, $0) }
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
            if let ratingStore {
                rating_store_free(ratingStore)
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
                    guard let sp = self.syncState else { return }
                    self.syncQueue.sync {
                        sync_poll(sp)
                    }
                    var status = ""
                    self.syncQueue.sync {
                        let n = sync_relay_count(sp)
                        for i in 0..<n {
                            if let urlc = sync_relay_url(sp, i) {
                                let url = String(cString: urlc)
                                status += url + (sync_relay_connected(sp, i) != 0 ? " ✓" : " ✗") + "\n"
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

    func setServer(_ server: String) {
        guard let sp = syncState else { return }
        syncQueue.sync {
            server.withCString { sync_set_server(sp, $0) }
        }
    }

    func refresh() {
        guard let state else { return }
        var out: [Entry] = []
        stateQueue.sync {
            let n = lfg_count(state)
            for i in 0..<n {
                guard lfg_get(state, i) != nil else { continue }
                out.append(Entry(
                    id: String(cString: lfg_entry_name(state, i)),
                    name: String(cString: lfg_entry_name(state, i)),
                    zone: String(cString: lfg_entry_zone(state, i)),
                    detail: String(cString: lfg_entry_detail(state, i)),
                    source: lfg_entry_source(state, i),
                    lastSeen: Date(timeIntervalSince1970: TimeInterval(lfg_entry_last_seen(state, i)))
                ))
            }
        }
        entries = out
    }

    /// Import the Windows app's Nostr key so both devices share one identity.
    func importKey(hex: String) -> String? {
        let hexOnly = hex.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        var pubkey = [CChar](repeating: 0, count: 65)
        let ok = hexOnly.withCString { cstr -> Int32 in
            nostr_import_key(keyPath, cstr, &pubkey)
        }
        guard ok == 0, let sp = syncState else { return nil }
        syncQueue.sync {
            keyPath.withCString { kp in
                _ = sync_reload_key(sp, kp)
            }
        }
        let pub = String(cString: pubkey)
        DispatchQueue.main.async { self.myPubkey = pub }
        return pub
    }

    func exportKeyHex() -> String? {
        guard let secret = try? String(contentsOfFile: keyPath, encoding: .ascii) else {
            return nil
        }
        return secret.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    func markLfg(name: String, zone: String, detail: String) {
        guard let state, let sp = syncState else { return }
        stateQueue.sync {
            name.withCString { n in
                zone.withCString { z in
                    detail.withCString { d in
                        lfg_add_manual(state, n, z, d)
                    }
                }
            }
        }
        refresh()
        publishQueue.async {
            var content = [CChar](repeating: 0, count: 512)
            name.withCString { n in
                zone.withCString { z in
                    detail.withCString { d in
                        _ = sync_build_content(&content, content.count, n, z, d, 1)
                        _ = self.syncQueue.sync { sync_publish(sp, &content) }
                    }
                }
            }
        }
    }

    func remove(name: String) {
        guard let state else { return }
        stateQueue.sync {
            _ = name.withCString { lfg_remove(state, $0) }
        }
        refresh()
        guard let sp = syncState else { return }
        publishQueue.async {
            var content = [CChar](repeating: 0, count: 512)
            name.withCString { n in
                _ = sync_build_content(&content, content.count, n, "", "", 0)
                _ = self.syncQueue.sync { sync_publish(sp, &content) }
            }
        }
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
            _ = name.withCString { fav_toggle(favList, $0) }
        }
        refreshFavourites()
    }

    // MARK: - Ratings

    func refreshRatings() {
        guard let ratingStore else { return }
        stateQueue.sync {
            rating_store_finalize(ratingStore)
        }
        var out: [String: RatingSummary] = [:]
        stateQueue.sync {
            for (name, pub) in nameToPubkey {
                var c: Int32 = 0, f: Int32 = 0, l: Int32 = 0, n: Int32 = 0
                pub.withCString { pp in
                    if rating_summary(ratingStore, pp, &c, &f, &l, &n) == 0 {
                        out[name] = RatingSummary(competent: Int(c), friendly: Int(f),
                                                  listens: Int(l), count: Int(n))
                    }
                }
            }
        }
        ratingSummaries = out
    }

    func rate(name: String, competent: Int, friendly: Int, listens: Int) {
        guard let pub = pubkeyFor(name), let sp = syncState else { return }
        publishQueue.async {
            pub.withCString { pp in
                _ = self.syncQueue.sync {
                    rating_publish(sp, pp, Int32(competent), Int32(friendly), Int32(listens))
                }
            }
        }
    }

    func vote(raterName: String, aboutName: String, agree: Bool) {
        guard let raterPub = pubkeyFor(raterName),
              let aboutPub = pubkeyFor(aboutName),
              let sp = syncState, let ratingStore else { return }
        publishQueue.async {
            var ratingId: String?
            self.stateQueue.sync {
                rating_store_finalize(ratingStore)
                raterPub.withCString { rp in
                    aboutPub.withCString { ap in
                        if let idc = rating_find_id(ratingStore, rp, ap) {
                            ratingId = String(cString: idc)
                        }
                    }
                }
            }
            guard let ratingId else { return }
            ratingId.withCString { rid in
                _ = self.syncQueue.sync {
                    vote_publish(sp, rid, agree ? 1 : 0)
                }
            }
        }
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
                _ = sync_decrypt_dm(sp, sptr, cptr, &plain, plain.count)
            }
        }
        let text = String(cString: plain)
        guard !text.isEmpty else { return }
        let name = nameToPubkey.first(where: { $0.value == sender })?.key ?? String(sender.prefix(8)) + "…"
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
