use std::ffi::{CStr, CString};
use std::os::raw::c_char;

unsafe fn cstr<'a>(p: *const c_char) -> Result<&'a str, String> {
    if p.is_null() {
        return Err("null pointer".to_string());
    }
    unsafe { CStr::from_ptr(p) }.to_str().map_err(|e| e.to_string())
}

fn out(s: String) -> *mut c_char {
    CString::new(s).unwrap_or_default().into_raw()
}

fn null() -> *mut c_char {
    std::ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn p99lfg_free_str(p: *mut c_char) {
    if !p.is_null() {
        unsafe {
            drop(CString::from_raw(p));
        }
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_generate_key() -> *mut c_char {
    out(crate::keys::generate_secret_hex())
}

#[no_mangle]
pub extern "C" fn p99lfg_pubkey_of(sk: *const c_char) -> *mut c_char {
    let r = unsafe { cstr(sk) }
        .and_then(|s| crate::keys::secret_from_hex(s))
        .map(|k| crate::keys::pubkey_xonly_hex(&k));
    match r {
        Ok(p) => out(p),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_is_valid_secret(sk: *const c_char) -> i32 {
    match unsafe { cstr(sk) }.and_then(|s| crate::keys::secret_from_hex(s)) {
        Ok(_) => 1,
        Err(_) => 0,
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_build_signed_event(
    sk: *const c_char,
    kind: u32,
    dtag: *const c_char,
    ptag: *const c_char,
    content: *const c_char,
    created_at: u64,
) -> *mut c_char {
    let r = unsafe { cstr(sk) }.and_then(|sk| {
        let d = unsafe { cstr(dtag) }?;
        let p = unsafe { cstr(ptag) }?;
        let c = unsafe { cstr(content) }?;
        crate::event::build_signed_event(sk, kind, d, p, c, created_at)
    });
    match r {
        Ok(ev) => out(
            serde_json::json!({
                "id": ev.id,
                "pubkey": ev.pubkey,
                "created_at": ev.created_at,
                "kind": ev.kind,
                "sig": ev.sig,
                "message": crate::event::event_message(&ev),
            })
            .to_string(),
        ),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_req(subid: *const c_char, since: i64) -> *mut c_char {
    match unsafe { cstr(subid) } {
        Ok(s) => {
            if since >= 0 {
                out(crate::event::req_message_since(s, since as u64))
            } else {
                out(crate::event::req_message(s))
            }
        }
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_parse_event(msg: *const c_char) -> *mut c_char {
    match unsafe { cstr(msg) }.and_then(|m| {
        crate::event::parse_event_message(m).ok_or_else(|| "not an EVENT".to_string())
    }) {
        Ok(ev) => out(
            serde_json::json!({
                "id": ev.id,
                "pubkey": ev.pubkey,
                "created_at": ev.created_at,
                "kind": ev.kind,
                "content": ev.content,
                "sig": ev.sig,
                "dtag": ev.dtag,
                "ptag": ev.ptag,
            })
            .to_string(),
        ),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_encrypt_dm(
    sk: *const c_char,
    peer: *const c_char,
    msg: *const c_char,
) -> *mut c_char {
    let r = unsafe { cstr(sk) }.and_then(|sk| {
        let peer = unsafe { cstr(peer) }?;
        let msg = unsafe { cstr(msg) }?;
        let key = crate::nip04::derive_shared_key(sk, peer)?;
        crate::nip04::encrypt_content(&key, msg)
    });
    match r {
        Ok(b) => out(b),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_decrypt_dm(
    sk: *const c_char,
    peer: *const c_char,
    content: *const c_char,
) -> *mut c_char {
    let r = unsafe { cstr(sk) }.and_then(|sk| {
        let peer = unsafe { cstr(peer) }?;
        let content = unsafe { cstr(content) }?;
        let key = crate::nip04::derive_shared_key(sk, peer)?;
        crate::nip04::decrypt_content(&key, content)
    });
    match r {
        Ok(p) => out(p),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_lfg_content(
    name: *const c_char,
    cls: *const c_char,
    level: i64,
    zone: *const c_char,
    lfg: i32,
    user: *const c_char,
) -> *mut c_char {
    let r = unsafe { cstr(name) }.and_then(|n| {
        let c = unsafe { cstr(cls) }?;
        let z = unsafe { cstr(zone) }?;
        let u = unsafe { cstr(user) }?;
        Ok(crate::lfg::build_lfg_content(n, c, level, z, lfg != 0, u))
    });
    match r {
        Ok(s) => out(s),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_lfg_dtag(server: *const c_char, charname: *const c_char) -> *mut c_char {
    let r = unsafe { cstr(server) }
        .and_then(|s| Ok(crate::lfg::lfg_dtag(s, unsafe { cstr(charname) }?)));
    match r {
        Ok(s) => out(s),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_parse_lfg(content: *const c_char) -> *mut c_char {
    match unsafe { cstr(content) }.and_then(|c| {
        crate::lfg::parse_lfg(c).ok_or_else(|| "not lfg".to_string())
    }) {
        Ok(e) => out(
            serde_json::json!({
                "v": e.v, "name": e.name, "class": e.cls, "level": e.level,
                "zone": e.zone, "detail": e.detail, "lfg": e.lfg, "user": e.user,
            })
            .to_string(),
        ),
        Err(_) => null(),
    }
}

#[no_mangle]
pub extern "C" fn p99lfg_rating_content(
    competent: i64,
    friendly: i64,
    listens: i64,
) -> *mut c_char {
    out(crate::rating::build_rating_content(competent, friendly, listens))
}

#[no_mangle]
pub extern "C" fn p99lfg_vote_content(agree: i32) -> *mut c_char {
    out(crate::rating::build_vote_content(agree != 0))
}

#[no_mangle]
pub extern "C" fn p99lfg_rating_event_id(
    rater: *const c_char,
    target: *const c_char,
    content: *const c_char,
    created_at: u64,
) -> *mut c_char {
    let r = unsafe { cstr(rater) }.and_then(|rater| {
        let t = unsafe { cstr(target) }?;
        let c = unsafe { cstr(content) }?;
        Ok(crate::rating::rating_event_id(rater, t, c, created_at))
    });
    match r {
        Ok(s) => out(s),
        Err(_) => null(),
    }
}
