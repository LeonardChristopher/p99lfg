use crate::json::c_escape;
use serde::Deserialize;

fn default_true() -> bool {
    true
}

#[derive(Clone, Debug, Default, Deserialize)]
pub struct LfgEntry {
    #[serde(default)]
    pub v: u32,
    #[serde(default)]
    pub name: String,
    #[serde(default, rename = "class")]
    pub cls: String,
    #[serde(default)]
    pub level: i64,
    #[serde(default)]
    pub zone: String,
    #[serde(default)]
    pub detail: String,
    #[serde(default = "default_true")]
    pub lfg: bool,
    #[serde(default)]
    pub user: String,
}

pub fn build_lfg_content(
    name: &str,
    cls: &str,
    level: i64,
    zone: &str,
    lfg: bool,
    user: &str,
) -> String {
    format!(
        "{{\"v\":2,\"name\":\"{}\",\"class\":\"{}\",\"level\":{},\"zone\":\"{}\",\"lfg\":{},\"user\":\"{}\"}}",
        c_escape(name),
        c_escape(cls),
        level,
        c_escape(zone),
        if lfg { "true" } else { "false" },
        c_escape(user)
    )
}

pub fn build_lfg_content_v1(name: &str, zone: &str, detail: &str, lfg: bool) -> String {
    format!(
        "{{\"v\":1,\"name\":\"{}\",\"zone\":\"{}\",\"detail\":\"{}\",\"lfg\":{}}}",
        c_escape(name),
        c_escape(zone),
        c_escape(detail),
        if lfg { "true" } else { "false" }
    )
}

pub fn parse_lfg(content: &str) -> Option<LfgEntry> {
    let v: serde_json::Value = serde_json::from_str(content).ok()?;
    if !v.get("name").map(|n| n.is_string()).unwrap_or(false) {
        return None;
    }
    serde_json::from_value(v).ok()
}

pub fn lfg_dtag(server: &str, charname: &str) -> String {
    format!("{}/{}", server, charname)
}
