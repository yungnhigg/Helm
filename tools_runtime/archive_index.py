#!/usr/bin/env python3
"""Offline archive index for Helm.

Builds a SQLite FTS5 index over the sharded JSONL emitted by the ZIM converter,
then answers keyword queries against it. The index is a single file on disk; at
50 GB of source text expect roughly 30-40 GB of index and query times in the
tens of milliseconds off NVMe.

Why FTS5 rather than embeddings over the whole corpus: 50 GB is on the order of
25 million chunks, which is ~100 GB of fp32 vectors and days of GPU time to
produce. Wikipedia is unusually friendly to lexical search - real titles, clean
section structure, consistent vocabulary - so BM25 recalls well enough to be the
first stage, and dense reranking only ever runs on the couple of hundred rows
that survive it.

Chunk text is NOT duplicated into the index. Only what FTS needs is stored;
`text` comes back by seeking into the original shard at a recorded byte offset,
which keeps the index smaller and means the JSONL stays canonical.

    index   python archive_index.py index --input D:\\wiki_RAG --db D:\\wiki.db
    search  python archive_index.py search --db D:\\wiki.db --query "..." -n 20
    stats   python archive_index.py stats --db D:\\wiki.db
"""
from __future__ import annotations

import argparse
import json
import os
import sqlite3
import sys
import time
from pathlib import Path


def fail(message: str, code: int = 2) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(code)


# FTS5 treats these as operators. A user query is meant to be read as terms, not
# as a query language, so anything structural gets quoted away.
_FTS_SPECIAL = set('"()*:^-')


def sanitize_query(raw: str) -> str:
    terms = []
    for token in raw.split():
        cleaned = "".join(c for c in token if c not in _FTS_SPECIAL).strip()
        if cleaned:
            terms.append('"' + cleaned + '"')
    if not terms:
        fail("query contained no searchable terms")
    return " OR ".join(terms)


def connect(db_path: Path, write: bool = False) -> sqlite3.Connection:
    if not write and not db_path.is_file():
        fail(f"index not found: {db_path}. Run the index command first.")
    conn = sqlite3.connect(str(db_path))
    conn.execute("PRAGMA journal_mode=WAL")
    if write:
        # Bulk load: durability matters less than finishing this decade.
        conn.execute("PRAGMA synchronous=OFF")
        conn.execute("PRAGMA cache_size=-524288")  # 512 MB
        conn.execute("PRAGMA temp_store=MEMORY")
    return conn


def create_schema(conn: sqlite3.Connection) -> None:
    # `text` is indexed but not stored; `content=''` makes this a contentless
    # FTS table, roughly halving index size. Retrieval reads the real text back
    # out of the shard.
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS chunks (
            rowid      INTEGER PRIMARY KEY,
            chunk_id   TEXT NOT NULL,
            title      TEXT,
            section    TEXT,
            path       TEXT,
            shard      INTEGER NOT NULL,
            offset     INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS shards (
            id   INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE
        );
        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value TEXT
        );
        CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
            title, section, text,
            content='',
            tokenize='porter unicode61'
        );
    """)


def do_index(args: argparse.Namespace) -> None:
    source = Path(args.input)
    if not source.is_dir():
        fail(f"input directory does not exist: {source}")
    shards = sorted(source.glob("*.jsonl"))
    if not shards:
        fail(f"no .jsonl shards found in {source}")

    db_path = Path(args.db)
    db_path.parent.mkdir(parents=True, exist_ok=True)
    conn = connect(db_path, write=True)
    create_schema(conn)

    done = {row[0] for row in conn.execute("SELECT name FROM shards")}
    if done and not args.rebuild:
        print(f"resuming; {len(done)} shard(s) already indexed", file=sys.stderr)
    if args.rebuild:
        conn.executescript("DELETE FROM chunks; DELETE FROM shards; DELETE FROM chunks_fts;")
        done = set()

    started = time.time()
    total_rows = conn.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]

    for shard_path in shards:
        if shard_path.name in done:
            continue
        shard_id = conn.execute(
            "INSERT INTO shards(name) VALUES(?)", (shard_path.name,)).lastrowid

        batch_meta, batch_fts = [], []
        rows_here = 0
        # Byte offsets have to come from the raw stream, so the file is read in
        # binary and decoded per line.
        with shard_path.open("rb") as handle:
            offset = 0
            for raw in handle:
                line_offset = offset
                offset += len(raw)
                stripped = raw.strip()
                if not stripped:
                    continue
                try:
                    rec = json.loads(stripped)
                except Exception:
                    continue
                text = rec.get("text") or ""
                if not text:
                    continue
                rowid = total_rows + rows_here + 1
                batch_meta.append((rowid, rec.get("id", ""), rec.get("title", ""),
                                   rec.get("section", ""), rec.get("path", ""),
                                   shard_id, line_offset))
                batch_fts.append((rowid, rec.get("title", ""), rec.get("section", ""), text))
                rows_here += 1

                if len(batch_meta) >= 50_000:
                    _flush(conn, batch_meta, batch_fts)
                    batch_meta, batch_fts = [], []

        _flush(conn, batch_meta, batch_fts)
        total_rows += rows_here
        conn.commit()
        elapsed = time.time() - started
        print(f"{shard_path.name}: {rows_here:,} chunks "
              f"({total_rows:,} total, {elapsed/60:.1f} min)", file=sys.stderr)

    conn.execute("INSERT OR REPLACE INTO meta(key,value) VALUES('source_dir',?)",
                 (str(source),))
    conn.commit()
    print(f"indexed {total_rows:,} chunks from {len(shards)} shard(s) "
          f"in {(time.time()-started)/60:.1f} min", file=sys.stderr)

    if args.optimize:
        print("optimizing (this takes a while and is not required)", file=sys.stderr)
        conn.execute("INSERT INTO chunks_fts(chunks_fts) VALUES('optimize')")
        conn.commit()
    conn.close()


def _flush(conn: sqlite3.Connection, meta: list, fts: list) -> None:
    if not meta:
        return
    conn.executemany(
        "INSERT INTO chunks(rowid,chunk_id,title,section,path,shard,offset) "
        "VALUES(?,?,?,?,?,?,?)", meta)
    conn.executemany(
        "INSERT INTO chunks_fts(rowid,title,section,text) VALUES(?,?,?,?)", fts)


def read_chunk_text(source_dir: Path, shard_name: str, offset: int) -> str:
    try:
        with (source_dir / shard_name).open("rb") as handle:
            handle.seek(offset)
            return json.loads(handle.readline().decode("utf-8")).get("text", "")
    except Exception:
        return ""


def do_search(args: argparse.Namespace) -> None:
    conn = connect(Path(args.db))
    row = conn.execute("SELECT value FROM meta WHERE key='source_dir'").fetchone()
    source_dir = Path(args.source or (row[0] if row else ""))

    limit = max(1, min(args.max_results, 100))
    started = time.time()
    # bm25() weights columns left to right; a title match is worth far more than
    # an incidental body mention. Lower is better, hence the ascending sort.
    sql = """
        SELECT c.chunk_id, c.title, c.section, c.path, s.name, c.offset,
               bm25(chunks_fts, 10.0, 3.0, 1.0) AS score
        FROM chunks_fts
        JOIN chunks c ON c.rowid = chunks_fts.rowid
        JOIN shards s ON s.id = c.shard
        WHERE chunks_fts MATCH ?
        ORDER BY score
        LIMIT ?
    """
    try:
        rows = conn.execute(sql, (sanitize_query(args.query), limit)).fetchall()
    except sqlite3.OperationalError as exc:
        fail(f"search failed: {exc}")

    results = []
    for chunk_id, title, section, path, shard, offset, score in rows:
        text = read_chunk_text(source_dir, shard, offset) if source_dir else ""
        if args.max_chars and len(text) > args.max_chars:
            text = text[:args.max_chars] + " …[truncated]"
        results.append({
            "id": chunk_id, "title": title, "section": section,
            "zim_path": path, "score": round(score, 3), "text": text,
        })

    print(json.dumps({
        "query": args.query,
        "results": results,
        "count": len(results),
        "elapsed_seconds": round(time.time() - started, 3),
        "note": ("Local offline Wikipedia archive. zim_path resolves to the full "
                 "article in the ZIM file; cite the article title, not the chunk."),
    }, ensure_ascii=False))
    conn.close()


def do_stats(args: argparse.Namespace) -> None:
    conn = connect(Path(args.db))
    chunks = conn.execute("SELECT COUNT(*) FROM chunks").fetchone()[0]
    shards = conn.execute("SELECT COUNT(*) FROM shards").fetchone()[0]
    titles = conn.execute("SELECT COUNT(DISTINCT title) FROM chunks").fetchone()[0]
    size = Path(args.db).stat().st_size
    print(json.dumps({
        "chunks": chunks, "shards": shards, "distinct_titles": titles,
        "index_bytes": size, "index_gib": round(size / 1024**3, 2),
    }, ensure_ascii=False, indent=2))
    conn.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="archive_index")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("index", help="build or resume the FTS index")
    p.add_argument("--input", required=True, help="directory holding the .jsonl shards")
    p.add_argument("--db", required=True)
    p.add_argument("--rebuild", action="store_true")
    p.add_argument("--optimize", action="store_true",
                   help="merge FTS b-trees; slow, improves query speed")
    p.set_defaults(func=do_index)

    p = sub.add_parser("search", help="query the index")
    p.add_argument("--db", required=True)
    p.add_argument("--query", required=True)
    p.add_argument("--max-results", "-n", type=int, default=8)
    p.add_argument("--max-chars", type=int, default=1200)
    p.add_argument("--source", default="", help="override the recorded shard directory")
    p.set_defaults(func=do_search)

    p = sub.add_parser("stats", help="index size and row counts")
    p.add_argument("--db", required=True)
    p.set_defaults(func=do_stats)
    return parser


def _force_utf8_streams() -> None:
    """Make stdout/stderr UTF-8 regardless of the Windows code page.

    The C++ host reads this script's stdout through a pipe. Python sees a pipe,
    not a console, so it picks the locale encoding - cp1252 on a US Windows box -
    and any non-ASCII character in fetched web text kills the process:

        UnicodeEncodeError: 'charmap' codec can't encode character '\u2010'

    U+2010 is an ordinary typographic hyphen. Wikipedia and half the web are full
    of them, so this is not an edge case; it is every other page. nlohmann::json
    on the receiving side expects UTF-8, which is what this produces.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass


def main() -> None:
    _force_utf8_streams()
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
