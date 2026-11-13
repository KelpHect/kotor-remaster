//! The news panel's content. News will come from a website API later; until then the only source is a
//! placeholder that says so. Replace [`PlaceholderNews`] with an API-backed [`NewsSource`]; the panel only
//! reads [`NewsItem`]s. No network access happens here.

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NewsItem {
    pub title: String,
    pub body: String,
    /// Where "view all" leads; `None` hides the link.
    pub link: Option<String>,
}

pub trait NewsSource {
    /// The items to show, newest first. Must not block the UI thread.
    fn items(&self) -> Vec<NewsItem>;
}

/// No news service exists yet.
pub struct PlaceholderNews;

impl NewsSource for PlaceholderNews {
    fn items(&self) -> Vec<NewsItem> {
        vec![NewsItem { title: "No news yet".into(), body: "Project news will appear here.".into(), link: None }]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn placeholder_has_no_link() {
        let items = PlaceholderNews.items();
        assert_eq!(items.len(), 1);
        assert!(items[0].link.is_none());
    }
}
