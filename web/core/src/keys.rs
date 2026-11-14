use k256::elliptic_curve::sec1::ToEncodedPoint;
use k256::schnorr::SigningKey;
use k256::{ProjectivePoint, PublicKey, SecretKey};
use rand::RngCore;

pub fn is_hex64(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

pub fn generate_secret_hex() -> String {
    loop {
        let mut b = [0u8; 32];
        rand::rngs::OsRng.fill_bytes(&mut b);
        if let Ok(sk) = SecretKey::from_slice(&b) {
            return hex::encode(sk.to_bytes());
        }
    }
}

pub fn secret_from_hex(s: &str) -> Result<SecretKey, String> {
    let b = hex::decode(s).map_err(|e| e.to_string())?;
    if b.len() != 32 {
        return Err("secret key must be 32 bytes".to_string());
    }
    SecretKey::from_slice(&b).map_err(|e| e.to_string())
}

pub fn pubkey_xonly_hex(sk: &SecretKey) -> String {
    let enc = sk.public_key().to_encoded_point(true);
    hex::encode(&enc.as_bytes()[1..33])
}

pub fn public_key_from_xonly_hex(s: &str) -> Result<PublicKey, String> {
    let x = hex::decode(s).map_err(|e| e.to_string())?;
    if x.len() != 32 {
        return Err("pubkey must be 32 bytes".to_string());
    }
    let mut comp = [0u8; 33];
    comp[0] = 0x02;
    comp[1..].copy_from_slice(&x);
    PublicKey::from_sec1_bytes(&comp).map_err(|e| e.to_string())
}

pub fn sign_schnorr_aux0(sk: &SecretKey, msg32: &[u8; 32]) -> Result<[u8; 64], String> {
    let signing = SigningKey::from(sk);
    let sig = signing
        .sign_raw(msg32, &[0u8; 32])
        .map_err(|e| e.to_string())?;
    Ok(sig.to_bytes())
}

pub fn ecdh_shared_x(sk: &SecretKey, peer_xonly_hex: &str) -> Result<[u8; 32], String> {
    let peer = public_key_from_xonly_hex(peer_xonly_hex)?;
    let scalar = *sk.to_nonzero_scalar();
    let shared = (ProjectivePoint::from(*peer.as_affine()) * scalar).to_affine();
    let enc = shared.to_encoded_point(false);
    let bytes = enc.as_bytes();
    let mut x = [0u8; 32];
    x.copy_from_slice(&bytes[1..33]);
    Ok(x)
}
