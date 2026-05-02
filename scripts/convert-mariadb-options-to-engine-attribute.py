#!/usr/bin/env python3
"""
Convert MariaDB-style table options grammar to MySQL's ENGINE_ATTRIBUTE JSON
in MTR .test files.

Handles patterns like:
    ENGINE=TIDESDB COMPRESSION='LZ4' BLOOM_FILTER=1
becomes:
    ENGINE=TIDESDB ENGINE_ATTRIBUTE='{"compression":"LZ4","bloom_filter":1}'

Run:
    python3 scripts/convert-mariadb-options-to-engine-attribute.py FILE.test [...]
"""
import re
import sys
import json

# Maps MariaDB option name (UPPER) -> JSON key (lower) and value transform.
# Value transform takes the raw MariaDB token (e.g. "'LZ4'" or "1" or "10485760")
# and returns a JSON-compatible Python value.
def _str_lower_unquoted(v):
    return v.strip().strip("'").strip('"')

def _bool_yn(v):
    s = _str_lower_unquoted(v).lower()
    return s in ("y", "yes", "1", "true", "on")

def _int(v):
    s = v.strip()
    # Strip M/K/G suffix and apply multiplier (MariaDB allows shorthand sizes).
    mult = 1
    if s and s[-1] in "kKmMgG":
        mult = {"k":1024, "K":1024, "m":1024*1024, "M":1024*1024,
                "g":1024*1024*1024, "G":1024*1024*1024}[s[-1]]
        s = s[:-1]
    return int(s) * mult

OPTIONS = {
    "COMPRESSION":             ("compression",       _str_lower_unquoted),
    "BLOOM_FILTER":            ("bloom_filter",      _bool_yn),
    "WRITE_BUFFER_SIZE":       ("write_buffer_size", _int),
    "MIN_DISK_SPACE":          ("min_disk_space",    _int),
    "BLOOM_FPR":               ("bloom_fpr",         _int),
    "TTL":                     ("ttl",               _int),
    "ENCRYPTED":               ("encrypted",         _bool_yn),
    "ENCRYPTION_KEY_ID":       ("encryption_key_id", _int),
    "BLOCK_INDEXES":           ("block_indexes",     _bool_yn),
    "TOMBSTONE_DENSITY_TRIGGER":      ("tombstone_density_trigger",       _int),
    "TOMBSTONE_DENSITY_MIN_ENTRIES":  ("tombstone_density_min_entries",   _int),
    "USE_BTREE":               ("use_btree",         _bool_yn),
    "KLOG_VALUE_THRESHOLD":    ("klog_value_threshold", _int),
    "SYNC_MODE":               ("sync_mode",         _str_lower_unquoted),
    "SYNC_INTERVAL_US":        ("sync_interval_us",  _int),
    "ISOLATION_LEVEL":         ("isolation_level",   _str_lower_unquoted),
    "INDEX_SAMPLE_RATIO":      ("index_sample_ratio", _int),
    "BLOCK_INDEX_PREFIX_LEN":  ("block_index_prefix_len", _int),
    "LEVEL_SIZE_RATIO":        ("level_size_ratio",  _int),
    "MIN_LEVELS":              ("min_levels",        _int),
    "DIVIDING_LEVEL_OFFSET":   ("dividing_level_offset", _int),
    "SKIP_LIST_MAX_LEVEL":     ("skip_list_max_level", _int),
    "SKIP_LIST_PROBABILITY":   ("skip_list_probability", _int),
    "L1_FILE_COUNT_TRIGGER":   ("l1_file_count_trigger", _int),
    "L0_QUEUE_STALL_THRESHOLD":("l0_queue_stall_threshold", _int),
    "OBJECT_LAZY_COMPACTION":  ("object_lazy_compaction", _bool_yn),
    "OBJECT_PREFETCH_COMPACTION": ("object_prefetch_compaction", _bool_yn),
}

# Match `ENGINE = TIDESDB` followed by zero or more `KEY=VALUE` or `KEY='VAL'` pairs,
# until end-of-statement (`;`) or a SQL keyword we don't want to absorb.
# We anchor on ENGINE=TIDESDB and then greedily consume the option pairs.
ENGINE_RE = re.compile(
    r'(\bENGINE\s*=\s*(?:TIDESDB|TidesDB))'                       # group 1: ENGINE clause
    r'((?:\s+`?[A-Z][A-Z0-9_]*`?\s*=\s*(?:\'[^\']*\'|"[^"]*"|[A-Za-z0-9_]+))*)',  # group 2: options w/ optional backticks
    re.IGNORECASE,
)

# A single OPTION=VALUE pair (key may be backticked).
OPT_RE = re.compile(
    r'\s+`?([A-Z][A-Z0-9_]*)`?\s*=\s*'
    r'(\'[^\']*\'|"[^"]*"|[A-Za-z0-9_]+)',
    re.IGNORECASE,
)


def convert_match(m):
    engine_part = m.group(1)
    opts_part = m.group(2) or ""
    if not opts_part.strip():
        return engine_part
    json_obj = {}
    consumed_keys = []
    for opt_m in OPT_RE.finditer(opts_part):
        key = opt_m.group(1).upper()
        val = opt_m.group(2)
        if key not in OPTIONS:
            # Leave unknown options alone — let the parser report them as errors
            # rather than silently dropping.
            return m.group(0)
        json_key, transform = OPTIONS[key]
        json_obj[json_key] = transform(val)
        consumed_keys.append(key)
    if not json_obj:
        return engine_part
    json_str = json.dumps(json_obj, separators=(",", ":"))
    return f"{engine_part} ENGINE_ATTRIBUTE='{json_str}'"


# Per-index option syntax: `KEY idx_name (cols) USE_BTREE=1[, ...]`
# Convert to: `KEY idx_name (cols) ENGINE_ATTRIBUTE='{"use_btree":true}'`
KEY_RE = re.compile(
    r'(\b(?:KEY|INDEX|UNIQUE\s+KEY|UNIQUE\s+INDEX|FULLTEXT\s+KEY|FULLTEXT\s+INDEX|SPATIAL\s+KEY|SPATIAL\s+INDEX)'
    r'\s+\w+\s*\([^)]*\))'                                            # group 1: KEY name (cols)
    r'((?:\s+`?[A-Z][A-Z0-9_]*`?\s*=\s*(?:\'[^\']*\'|"[^"]*"|[A-Za-z0-9_]+))+)',  # group 2: trailing options
    re.IGNORECASE,
)

def convert_key_match(m):
    head = m.group(1)
    opts = m.group(2)
    json_obj = {}
    for opt_m in OPT_RE.finditer(opts):
        key = opt_m.group(1).upper()
        val = opt_m.group(2)
        if key not in OPTIONS:
            return m.group(0)  # leave alone if unknown option
        json_key, transform = OPTIONS[key]
        json_obj[json_key] = transform(val)
    if not json_obj:
        return head
    return f"{head} ENGINE_ATTRIBUTE='{json.dumps(json_obj, separators=(',',':'))}'"

# Per-column option (MariaDB backtick syntax): `expire_secs INT \`TTL\`=1`
# Convert to: `expire_secs INT ENGINE_ATTRIBUTE='{"ttl":1}'`
COL_RE = re.compile(
    r'(\b\w+\s+(?:TINYINT|SMALLINT|MEDIUMINT|INT|INTEGER|BIGINT|FLOAT|DOUBLE|DECIMAL|NUMERIC|'
    r'CHAR|VARCHAR|BINARY|VARBINARY|TINYBLOB|BLOB|MEDIUMBLOB|LONGBLOB|TINYTEXT|TEXT|MEDIUMTEXT|LONGTEXT|'
    r'DATE|DATETIME|TIME|TIMESTAMP|YEAR|JSON|ENUM|SET)'
    r'(?:\s*\([^)]*\))?'                                              # optional (size)
    r'(?:\s+(?:UNSIGNED|ZEROFILL|NOT\s+NULL|NULL|DEFAULT\s+\S+))*'   # optional modifiers
    r')'                                                              # group 1
    r'\s+`([A-Z][A-Z0-9_]*)`\s*=\s*(\'[^\']*\'|"[^"]*"|[A-Za-z0-9_]+)',  # group 2: opt name (backticked), group 3: value
    re.IGNORECASE,
)

def convert_col_match(m):
    head = m.group(1)
    key = m.group(2).upper()
    val = m.group(3)
    if key not in OPTIONS:
        return m.group(0)
    json_key, transform = OPTIONS[key]
    obj = {json_key: transform(val)}
    return f"{head} ENGINE_ATTRIBUTE='{json.dumps(obj, separators=(',',':'))}'"


def convert_text(text):
    text = ENGINE_RE.sub(convert_match, text)        # table-level options
    text = KEY_RE.sub(convert_key_match, text)       # per-index options
    text = COL_RE.sub(convert_col_match, text)       # per-column options
    # Generated column storage modifier: MariaDB `PERSISTENT` → MySQL `STORED`.
    text = re.sub(r'\bPERSISTENT\b', 'STORED', text, flags=re.IGNORECASE)
    return text


def main():
    if len(sys.argv) < 2:
        print("usage: convert-mariadb-options-to-engine-attribute.py FILE.test [...]",
              file=sys.stderr)
        sys.exit(2)
    n_changed = 0
    for path in sys.argv[1:]:
        with open(path) as f:
            src = f.read()
        new = convert_text(src)
        if new != src:
            with open(path, "w") as f:
                f.write(new)
            print(f"  converted: {path}")
            n_changed += 1
        else:
            print(f"  unchanged: {path}")
    print(f"\n{n_changed} of {len(sys.argv)-1} files changed")


if __name__ == "__main__":
    main()
