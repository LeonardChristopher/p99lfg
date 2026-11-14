use crate::event::{serialize_for_id, NostrEvent};
use crate::keys::is_hex64;
use sha2::{Digest, Sha256};

pub const RATING_KIND: u32 = 30079;
pub const VOTE_KIND: u32 = 30080;
pub const HIDE_DISAGREES: usize = 2;
pub const RATING_MAX: usize = 512;
pub const VOTES_MAX: usize = 64;

#[derive(Clone, Debug, Default)]
pub struct Vote {
    pub voter: String,
    pub rating_id: String,
    pub agree: bool,
    pub created_at: u64,
}

#[derive(Clone, Debug, Default)]
pub struct Rating {
    pub rater: String,
    pub target: String,
    pub created_at: u64,
    pub content: String,
    pub competent: i64,
    pub friendly: i64,
    pub listens: i64,
    pub id: String,
    pub votes: Vec<Vote>,
    pub votes_dirty: bool,
}

#[derive(Default)]
pub struct RatingStore {
    pub ratings: Vec<Rating>,
    pub votes_pending: Vec<Vote>,
}

pub fn rating_event_id(rater: &str, target: &str, content: &str, created_at: u64) -> String {
    let ev = NostrEvent {
        pubkey: rater.to_string(),
        created_at,
        kind: RATING_KIND,
        content: content.to_string(),
        dtag: target.to_string(),
        ..Default::default()
    };
    hex::encode(Sha256::digest(serialize_for_id(&ev).as_bytes()))
}

pub fn build_rating_content(competent: i64, friendly: i64, listens: i64) -> String {
    format!(
        "{{\"competent\":{},\"friendly\":{},\"listens\":{}}}",
        competent, friendly, listens
    )
}

pub fn build_vote_content(agree: bool) -> String {
    format!("{{\"agree\":{}}}", if agree { "true" } else { "false" })
}

fn parse_scores(content: &str) -> (i64, i64, i64) {
    let v: serde_json::Value = serde_json::from_str(content).unwrap_or(serde_json::Value::Null);
    let get = |k: &str| v.get(k).and_then(|x| x.as_i64()).unwrap_or(0);
    (get("competent"), get("friendly"), get("listens"))
}

impl RatingStore {
    pub fn new() -> Self {
        Self::default()
    }

    fn find(&self, rater: &str, target: &str) -> Option<usize> {
        self.ratings
            .iter()
            .position(|r| r.rater == rater && r.target == target)
    }

    pub fn rater_is_active(&self, pubkey: &str) -> bool {
        self.ratings.iter().any(|r| r.rater == pubkey)
    }

    pub fn record(&mut self, rater: &str, target: &str, content: &str, created_at: u64) -> bool {
        if !is_hex64(rater) || !is_hex64(target) {
            return false;
        }
        let (competent, friendly, listens) = parse_scores(content);
        match self.find(rater, target) {
            Some(i) => {
                if created_at < self.ratings[i].created_at {
                    return false;
                }
                let r = &mut self.ratings[i];
                r.created_at = created_at;
                r.content = content.to_string();
                r.competent = competent;
                r.friendly = friendly;
                r.listens = listens;
                r.id = rating_event_id(rater, target, content, created_at);
                r.votes_dirty = true;
            }
            None => {
                if self.ratings.len() >= RATING_MAX {
                    return false;
                }
                self.ratings.push(Rating {
                    rater: rater.to_string(),
                    target: target.to_string(),
                    created_at,
                    content: content.to_string(),
                    competent,
                    friendly,
                    listens,
                    id: rating_event_id(rater, target, content, created_at),
                    votes: Vec::new(),
                    votes_dirty: true,
                });
            }
        }
        true
    }

    pub fn vote(&mut self, voter: &str, rating_id: &str, agree: bool, created_at: u64) -> bool {
        if !is_hex64(voter) || !is_hex64(rating_id) {
            return false;
        }
        for r in &mut self.ratings {
            if let Some(v) = r
                .votes
                .iter_mut()
                .find(|v| v.voter == voter && v.rating_id == rating_id)
            {
                if created_at < v.created_at {
                    return false;
                }
                v.agree = agree;
                v.created_at = created_at;
                return true;
            }
        }
        if self.votes_pending.len() >= RATING_MAX * 4 {
            return false;
        }
        self.votes_pending.push(Vote {
            voter: voter.to_string(),
            rating_id: rating_id.to_string(),
            agree,
            created_at,
        });
        true
    }

    pub fn finalize(&mut self) {
        for i in 0..self.ratings.len() {
            if !self.ratings[i].votes_dirty {
                continue;
            }
            self.ratings[i].votes_dirty = false;
            let id = self.ratings[i].id.clone();
            self.ratings[i].votes.clear();
            let pending: Vec<Vote> = self
                .votes_pending
                .iter()
                .filter(|v| v.rating_id == id)
                .cloned()
                .collect();
            for v in pending {
                if self.ratings[i].votes.len() < VOTES_MAX
                    && !self.ratings[i].votes.iter().any(|x| x.voter == v.voter)
                {
                    self.ratings[i].votes.push(v);
                }
            }
        }
        let pending: Vec<Vote> = self.votes_pending.clone();
        for v in pending {
            for r in &mut self.ratings {
                if r.id != v.rating_id {
                    continue;
                }
                if r.votes.len() < VOTES_MAX && !r.votes.iter().any(|x| x.voter == v.voter) {
                    r.votes.push(v.clone());
                }
            }
        }
    }

    pub fn is_hidden(&self, r: &Rating) -> bool {
        let mut active_disagrees = 0;
        for v in &r.votes {
            if !v.agree && self.rater_is_active(&v.voter) {
                active_disagrees += 1;
            }
        }
        active_disagrees >= HIDE_DISAGREES
    }

    pub fn summary(&self, target: &str) -> Option<(i64, i64, i64, i64)> {
        let mut c = 0i64;
        let mut f = 0i64;
        let mut l = 0i64;
        let mut cnt = 0i64;
        for r in &self.ratings {
            if r.target != target || self.is_hidden(r) {
                continue;
            }
            c += r.competent;
            f += r.friendly;
            l += r.listens;
            cnt += 1;
        }
        if cnt == 0 {
            return None;
        }
        Some(((c * 10 / cnt + 5) / 10, (f * 10 / cnt + 5) / 10, (l * 10 / cnt + 5) / 10, cnt))
    }

    pub fn find_id(&self, rater: &str, target: &str) -> Option<String> {
        self.find(rater, target).map(|i| self.ratings[i].id.clone())
    }
}
