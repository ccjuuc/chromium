"""Check duplicate keys in zh_comments.json (raw text scan, since json drops dups)."""
import re
from collections import Counter
from pathlib import Path

p = Path(__file__).resolve().parents[1] / "tools" / "zh_comments.json"
text = p.read_text(encoding="utf-8")
keys = re.findall(r'^  "([^"]+)":', text, flags=re.M)
c = Counter(keys)
dups = {k: v for k, v in c.items() if v > 1}
print("total raw keys:", len(keys))
print("dup keys:", dups)
