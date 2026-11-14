use crate::json::c_escape;
use crate::keys;
use sha2::{Digest, Sha256};

pub const KIND_LFG: u32 = 30078;
pub const LFG_TAG: &str = "p99lfg";
pub const KINDS_REQ: [u32; 7] = [30078, 4, 9, 30079, 30080, 30081, 30082];

#[derive(Clone, Debug, Default)]
pub struct NostrEvent {
    pub id: String,
    pub pubkey: String,
    pub created_at: u64,
    pub kind: u32,
    pub content: String,
    pub sig: String,
    pub dtag: String,
    pub ptag: String,
}

pub fn serialize_for_id(ev: &NostrEvent) -> String {
    let esc = c_escape(&ev.content);
    if !ev.ptag.is_empty() {
        format!(
            "[0,\"{}\",{},{},[[\"p\",\"{}\"],[\"t\",\"{}\"]],\"{}\"]",
            ev.pubkey, ev.created_at, ev.kind, ev.ptag, LFG_TAG, esc
        )
    } else {
        format!(
            "[0,\"{}\",{},{},[[\"d\",\"{}\"],[\"t\",\"{}\"]],\"{}\"]",
            ev.pubkey, ev.created_at, ev.kind, ev.dtag, LFG_TAG, esc
        )
    }
}

pub fn event_message(ev: &NostrEvent) -> String {
    let esc = c_escape(&ev.content);
    let tags = if !ev.ptag.is_empty() {
        format!("[[\"p\",\"{}\"],[\"t\",\"{}\"]]", ev.ptag, LFG_TAG)
    } else {
        format!("[[\"d\",\"{}\"],[\"t\",\"{}\"]]", ev.dtag, LFG_TAG)
    };
    format!(
        "[\"EVENT\",{{\"id\":\"{}\",\"pubkey\":\"{}\",\"created_at\":{},\"kind\":{},\"tags\":{},\"content\":\"{}\",\"sig\":\"{}\"}}]",
        ev.id, ev.pubkey, ev.created_at, ev.kind, tags, esc, ev.sig
    )
}

fn kinds_csv() -> String {
    KINDS_REQ
        .iter()
        .map(|k| k.to_string())
        .collect::<Vec<_>>()
        .join(",")
}

pub fn req_message(subid: &str) -> String {
    format!(
        "[\"REQ\",\"{}\",{{\"kinds\":[{}],\"#t\":[\"{}\"]}}]",
        subid,
        kinds_csv(),
        LFG_TAG
    )
}

pub fn req_message_since(subid: &str, since: u64) -> String {
    format!(
        "[\"REQ\",\"{}\",{{\"kinds\":[{}],\"#t\":[\"{}\"],\"since\":{}}}]",
        subid,
        kinds_csv(),
        LFG_TAG,
        since
    )
}

pub fn build_signed_event(
    sk_hex: &str,
    kind: u32,
    dtag: &str,
    ptag: &str,
    content: &str,
    created_at: u64,
) -> Result<NostrEvent, String> {
    let sk = keys::secret_from_hex(sk_hex)?;
    let mut ev = NostrEvent {
        pubkey: keys::pubkey_xonly_hex(&sk),
        created_at,
        kind,
        content: content.to_string(),
        dtag: dtag.to_string(),
        ptag: ptag.to_string(),
        ..Default::default()
    };
    let serial = serialize_for_id(&ev);
    let digest = Sha256::digest(serial.as_bytes());
    let mut idb = [0u8; 32];
    idb.copy_from_slice(&digest);
    ev.id = hex::encode(idb);
    ev.sig = hex::encode(keys::sign_schnorr_aux0(&sk, &idb)?);
    Ok(ev)
}

#[derive(Clone, Debug, Default)]
pub struct ParsedEvent {
    pub id: String,
    pub pubkey: String,
    pub created_at: u64,
    pub kind: u32,
    pub content: String,
    pub sig: String,
    pub dtag: String,
    pub ptag: String,
}

pub fn parse_event_message(msg: &str) -> Option<ParsedEvent> {
    let v: serde_json::Value = serde_json::from_str(msg).ok()?;
    let arr = v.as_array()?;
    if arr.first()?.as_str()? != "EVENT" {
        return None;
    }
    let ev = arr.get(1)?;
    let mut dtag = String::new();
    let mut ptag = String::new();
    if let Some(tags) = ev.get("tags").and_then(|t| t.as_array()) {
        for t in tags {
            let ta = t.as_array()?;
            if ta.len() < 2 {
                continue;
            }
            match ta[0].as_str()? {
                "d" => dtag = ta[1].as_str()?.to_string(),
                "p" => ptag = ta[1].as_str()?.to_string(),
                _ => {}
            }
        }
    }
    Some(ParsedEvent {
        id: ev.get("id")?.as_str()?.to_string(),
        pubkey: ev.get("pubkey")?.as_str()?.to_string(),
        created_at: ev.get("created_at")?.as_u64()?,
        kind: ev.get("kind")?.as_u64()? as u32,
        content: ev.get("content")?.as_str()?.to_string(),
        sig: ev.get("sig")?.as_str()?.to_string(),
        dtag,
        ptag,
    })
}

pub fn is_state_kind(kind: u32) -> bool {
    matches!(kind, 30078 | 30081 | 30082)
}
