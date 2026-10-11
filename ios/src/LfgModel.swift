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

    struct Entry: Identifiable, Equatable {
        let id: String
        let name: String
        let zone: String
        let detail: String
        let source: Int32
        let lastSeen: Date
    }

    private var state: OpaquePointer?
    private var syncState: UnsafeMutablePointer<SyncState>?
    private var syncThread: Thread?
    private var running = false
    private var wrapper: CallbackWrapper?

    private static let relays = ["wss://nos.lol", "wss://relay.damus.io"]

    private let stateQueue = DispatchQueue(label: "p99lfg.state")
    private let syncQueue = DispatchQueue(label: "p99lfg.syncstate")
    private let publishQueue = DispatchQueue(label: "p99lfg.publish")

    private var keyPath: String {
        let dir = NSHomeDirectory() + "/Documents"
        return dir + "/p99lfg.key"
    }

    private final class CallbackWrapper {
        let callback: SyncOnEvent
        init(_ cb: @escaping @convention(c) (UnsafePointer<SyncRemoteEntry>?, UnsafePointer<CChar>?, UnsafeMutableRawPointer?) -> Void) {
            callback = cb
        }
    }

    init() {
        let keyPath = self.keyPath

        state = lfg_new()
        lfg_set_stale_minutes(state, 30)

        var pubkey = [CChar](repeating: 0, count: 65)
        nostr_identity_load(keyPath, &pubkey)
        myPubkey = String(cString: pubkey)

        syncState = UnsafeMutablePointer<SyncState>.allocate(capacity: 1)
        syncState.initialize(to: SyncState())

        wrapper = CallbackWrapper { entry, _, userdata in
            guard let entry, let userdata else { return }
            let model = Unmanaged<LfgModel>.fromOpaque(userdata).takeUnretainedValue()
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

        sync_init(syncState, keyPath, "green", wrapper!.callback,
                  Unmanaged.passUnretained(self).toOpaque())
        for r in Self.relays {
            r.withCString { sync_add_relay(syncState, $0) }
        }
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
        publishAsync(sp: sp, name: name, zone: zone, detail: detail, lfg: true)
    }

    func remove(name: String) {
        guard let state else { return }
        stateQueue.sync {
            _ = name.withCString { lfg_remove(state, $0) }
        }
        refresh()
        if let sp = syncState {
            publishAsync(sp: sp, name: name, zone: "", detail: "", lfg: false)
        }
    }

    private func publishAsync(sp: UnsafeMutablePointer<SyncState>,
                             name: String, zone: String, detail: String, lfg: Bool) {
        publishQueue.async {
            var content = [CChar](repeating: 0, count: 512)
            _ = sync_build_content(&content, content.count, name, zone, detail, lfg ? 1 : 0)
            self.syncQueue.sync {
                _ = sync_publish(sp, &content)
            }
        }
    }
}
