// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
use crate::catalog::Catalog;
use std::fmt::Write;

fn escape(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

fn size(bytes: u64) -> String {
    if bytes >= 1024 * 1024 * 1024 {
        format!("{:.1} GiB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    } else if bytes >= 1024 * 1024 {
        format!("{:.1} MiB", bytes as f64 / (1024.0 * 1024.0))
    } else if bytes >= 1024 {
        format!("{:.1} KiB", bytes as f64 / 1024.0)
    } else {
        format!("{bytes} B")
    }
}

pub fn render(catalog: &Catalog, origin: &str, token: Option<&str>) -> String {
    let mut rows = String::new();
    let mut diff_count = 0;
    for release in &catalog.releases {
        // Count only deltas that can be selected by the Omaha endpoint.
        let usable: Vec<_> =
            release.differential.iter().filter(|diff| diff.size < release.full.size).collect();
        diff_count += usable.len();
        let diff = if usable.is_empty() {
            "全量升级".to_string()
        } else {
            format!("{} 个补丁", usable.len())
        };
        write!(rows,
            "<tr><td class=\"appid\">{}</td><td>macOS</td><td>{}</td><td class=\"version\">{}</td><td>{}</td><td>{}</td><td><a class=\"download\" href=\"/artifacts/{}\" download>下载 CRX</a></td></tr>",
            escape(&release.appid),
            if release.arch == "x86_64" { "x64" } else { "ARM64" }, escape(&release.version),
            size(release.full.size), diff, escape(&release.full.file)).unwrap();
    }
    if rows.is_empty() {
        rows.push_str("<tr><td colspan=\"7\" class=\"empty\"><strong>还没有发布版本</strong><p>在本机管理页面导入安装包后，刷新即可查看。</p></td></tr>");
    }
    // Insert catalog-controlled HTML last so values cannot become template tokens.
    include_str!("home.html")
        .replace("{{ADMIN_TOKEN}}", &escape(token.unwrap_or("")))
        .replace("{{ADMIN_ENABLED}}", if token.is_some() { "true" } else { "false" })
        .replace("{{ORIGIN}}", &escape(origin))
        .replace("{{ENDPOINT}}", &escape(&format!("{origin}/service/update2/json")))
        .replace("{{RELEASE_COUNT}}", &catalog.releases.len().to_string())
        .replace("{{DIFF_COUNT}}", &diff_count.to_string())
        .replace("{{RELEASE_ROWS}}", &rows)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn release_fields_are_escaped_and_only_usable_deltas_are_counted() {
        let (dir, _, _) = crate::catalog::tests::fixture();
        let mut catalog = crate::catalog::load(&dir.path().join("release.json")).unwrap();
        catalog.releases[0].appid = "<script>alert(1)</script>".into();
        let mut oversized = catalog.releases[0].differential[0].clone();
        oversized.size = catalog.releases[0].full.size;
        catalog.releases[0].differential.push(oversized);
        let html = render(&catalog, "http://127.0.0.1:9001", None);
        assert!(!html.contains("<script>"));
        assert!(html.contains("&lt;script&gt;alert(1)&lt;/script&gt;"));
        assert!(html.contains("1 个补丁"));
        assert!(html.contains("href=\"/artifacts/app.crx3\""));
        assert!(!html.contains("{{RELEASE_ROWS}}"));
    }
}
