pub fn c_escape(input: &str) -> String {
    let mut out: Vec<u8> = Vec::with_capacity(input.len() + 8);
    for &b in input.as_bytes() {
        match b {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            c if c < 0x20 => {
                let hex = format!("\\u{:04x}", c);
                out.extend_from_slice(hex.as_bytes());
            }
            c => out.push(c),
        }
    }
    String::from_utf8(out).unwrap_or_default()
}
