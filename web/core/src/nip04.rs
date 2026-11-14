use crate::keys;
use aes::Aes256;
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use cbc::{Decryptor, Encryptor};
use cipher::block_padding::Pkcs7;
use cipher::{BlockDecryptMut, BlockEncryptMut, KeyIvInit};
use rand::RngCore;

type Aes256CbcEnc = Encryptor<Aes256>;
type Aes256CbcDec = Decryptor<Aes256>;

pub fn derive_shared_key(sk_hex: &str, peer_xonly_hex: &str) -> Result<[u8; 32], String> {
    let sk = keys::secret_from_hex(sk_hex)?;
    keys::ecdh_shared_x(&sk, peer_xonly_hex)
}

pub fn encrypt_content(key: &[u8; 32], plaintext: &str) -> Result<String, String> {
    let mut iv = [0u8; 16];
    rand::rngs::OsRng.fill_bytes(&mut iv);
    let ct = Aes256CbcEnc::new(key.into(), &iv.into())
        .encrypt_padded_vec_mut::<Pkcs7>(plaintext.as_bytes());
    Ok(format!("{}?iv={}", B64.encode(&ct), B64.encode(iv)))
}

pub fn decrypt_content(key: &[u8; 32], content: &str) -> Result<String, String> {
    let (ctb64, ivb64) = content.split_once("?iv=").ok_or("missing ?iv=")?;
    let ct = B64.decode(ctb64).map_err(|e| e.to_string())?;
    let ivb = B64.decode(ivb64).map_err(|e| e.to_string())?;
    if ct.is_empty() || ct.len() % 16 != 0 || ivb.len() != 16 {
        return Err("bad ciphertext shape".to_string());
    }
    let mut iv = [0u8; 16];
    iv.copy_from_slice(&ivb);
    let pt = Aes256CbcDec::new(key.into(), &iv.into())
        .decrypt_padded_vec_mut::<Pkcs7>(&ct)
        .map_err(|_| "bad padding".to_string())?;
    for &b in &pt {
        if b < 0x20 && b != b'\n' && b != b'\r' && b != b'\t' {
            return Err("non-printable plaintext".to_string());
        }
    }
    String::from_utf8(pt).map_err(|e| e.to_string())
}

pub fn dm_envelope(from_char: &str, msg: &str) -> String {
    serde_json::json!({ "from": from_char, "msg": msg }).to_string()
}

pub fn parse_dm_payload(plaintext: &str) -> (String, String) {
    if let Ok(v) = serde_json::from_str::<serde_json::Value>(plaintext) {
        if let (Some(f), Some(m)) = (
            v.get("from").and_then(|x| x.as_str()),
            v.get("msg").and_then(|x| x.as_str()),
        ) {
            return (f.to_string(), m.to_string());
        }
    }
    (String::new(), plaintext.to_string())
}
