"""Host tests for the shared C++ Markdown engine.

Builds the vendored md4c and the render-model engine with the system C/C++
compiler, then checks final documents against fixtures/.

    python3 cpp/tests/test_engine.py
"""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "cpp"
FIXTURES = ROOT / "fixtures"

KIND = {"paragraph": 0, "heading": 1, "code": 2, "html": 3, "hr": 4, "table": 5}
LIST_ITEM = 1
TASK = {0: None, 1: "unchecked", 2: "checked"}
STRONG, EMPHASIS, STRIKE, CODE, LINK, IMAGE_ALT, FOOTNOTE = (1 << i for i in range(7))

BUILD_DIR = None
# Mirrors the prefixes in Downstream.podspec and android/src/main/jni/CMakeLists.txt.
SYMBOL_PREFIXES = [
    "-Dmd_parse=downstream_md_parse",
    "-Dentity_lookup=downstream_entity_lookup",
    "-Dmd_html=downstream_md_html",
]


def setUpModule():
    global BUILD_DIR
    BUILD_DIR = Path(tempfile.mkdtemp(prefix="downstream-engine-"))
    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    md4c = CPP / "md4c"
    objects = []
    for source in ("md4c.c", "entity.c", "md4c-html.c"):
        target = BUILD_DIR / (source + ".o")
        subprocess.run(
            [cc, "-std=c11", "-O1", "-DMD4C_USE_UTF8", *SYMBOL_PREFIXES, "-c", md4c / source, "-o", target],
            check=True,
        )
        objects.append(target)
    engine = [CPP / "downstream" / name for name in ("MarkdownParser.cpp", "Unicode.cpp", "LinkPolicy.cpp")]
    subprocess.run(
        [cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-DMD4C_USE_UTF8", *SYMBOL_PREFIXES,
         f"-I{md4c}", f"-I{CPP / 'downstream'}", CPP / "tests" / "dump_document.cpp", *engine,
         objects[0], objects[1], "-o", BUILD_DIR / "dump_document"],
        check=True,
    )
    subprocess.run(
        [cc, "-std=c11", "-O1", "-DMD4C_USE_UTF8", *SYMBOL_PREFIXES, f"-I{md4c}", CPP / "tests" / "oracle_html.c",
         *objects, "-o", BUILD_DIR / "oracle_html"],
        check=True,
    )


def tearDownModule():
    shutil.rmtree(BUILD_DIR, ignore_errors=True)


def parse(source):
    data = source.encode("utf-8") if isinstance(source, str) else source
    output = subprocess.run([BUILD_DIR / "dump_document"], input=data, capture_output=True, check=True).stdout
    return json.loads(output)


def visible_text(document):
    """Joins non-table blocks the way the fixture manifest describes."""
    return "\n".join(block["text"] for block in document["blocks"] if block["kind"] != KIND["table"])


def styles_of(block, substring):
    start = block["text"].index(substring)
    end = start + len(substring)
    return [run for run in block["runs"] if run[0] < end and run[1] > start]


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((FIXTURES / "manifest.json").read_text(encoding="utf-8"))

    def test_vendored_parser_matches_oracle_html(self):
        for fixture in self.manifest["fixtures"]:
            with self.subTest(fixture=fixture["id"]):
                source = (FIXTURES / fixture["source"]).read_bytes()
                html = subprocess.run(
                    [BUILD_DIR / "oracle_html"], input=source, capture_output=True, check=True
                ).stdout
                self.assertEqual(html, (FIXTURES / fixture["expectedHtml"]).read_bytes())

    def test_final_checkpoints(self):
        for fixture in self.manifest["fixtures"]:
            final = fixture["checkpoints"][-1]
            expected = final["expectedPreview"]
            with self.subTest(fixture=fixture["id"]):
                source = (FIXTURES / fixture["source"]).read_bytes()
                self.assertEqual("".join(fixture["chunks"]).encode("utf-8"), source)
                document = parse(source)
                blocks = document["blocks"]

                if "visibleText" in expected:
                    self.assertEqual(visible_text(document), expected["visibleText"])
                if "enabledLinks" in expected:
                    self.assertEqual([link["url"] for link in document["links"]], expected["enabledLinks"])
                if "imageRequests" in expected:
                    # The first release never fetches images.
                    self.assertEqual(expected["imageRequests"], [])
                if expected.get("syntaxHighlighting") is False:
                    self.assertTrue(all(block["kind"] == KIND["code"] for block in blocks))
                if "provisionalStyles" in expected:
                    # Completed documents have no provisional styles.
                    self.assertEqual(expected["provisionalStyles"], [])
                if "rawHtmlEscaped" in expected:
                    self.assertIn("<b>bold</b>", visible_text(document))
                if expected.get("mathKind") == "literal":
                    self.assertIn("$x^2$", visible_text(document))

                kind = expected.get("blockKind")
                if kind == "list":
                    self.assertTrue(all(any(c["kind"] == LIST_ITEM for c in b["containers"]) for b in blocks))
                elif kind == "table":
                    rows = [b for b in blocks if b["kind"] == KIND["table"]]
                    cells = [[b["text"][s:e] for s, e, _ in b["cells"]] for b in rows]
                    self.assertTrue(rows[0]["tableHeader"])
                    self.assertEqual(cells[0], expected["headers"])
                    self.assertEqual(cells[1:], expected["rows"])
                elif kind is not None:
                    self.assertEqual([b["kind"] for b in blocks], [KIND[kind]])
                if "headingLevel" in expected:
                    self.assertEqual(blocks[0]["headingLevel"], expected["headingLevel"])
                if "taskStates" in expected:
                    tasks = [
                        TASK[c["task"]]
                        for b in blocks
                        for c in b["containers"]
                        if c["kind"] == LIST_ITEM and c["showsMarker"] and c["task"]
                    ]
                    self.assertEqual(tasks, expected["taskStates"])


class RenderModelTests(unittest.TestCase):
    def test_inline_styles(self):
        block = parse("a **b _c_** ~~d~~ `e`\n")["blocks"][0]
        self.assertEqual(block["text"], "a b c d e")
        self.assertEqual(styles_of(block, "b ")[0][2], STRONG)
        self.assertEqual(styles_of(block, "c")[0][2], STRONG | EMPHASIS)
        self.assertEqual(styles_of(block, "d")[0][2], STRIKE)
        self.assertEqual(styles_of(block, "e")[0][2], CODE)

    def test_hard_and_soft_breaks(self):
        self.assertEqual(parse("a\nb  \nc\\\nd\n")["blocks"][0]["text"], "a\nb\nc\nd")

    def test_entities_and_null(self):
        self.assertEqual(parse("&copy; &#65; &#x42; &bogus; &#0; a\0b\n")["blocks"][0]["text"],
                         "\u00a9 A B &bogus; \ufffd a\ufffdb")

    def test_headings_code_and_rules(self):
        document = parse("# One\n\n###### Six\n\n---\n\n```py\nx\n\ny\n```\n\n    indented\n")
        kinds = [(b["kind"], b["headingLevel"]) for b in document["blocks"]]
        self.assertEqual(kinds, [(1, 1), (1, 6), (4, 0), (2, 0), (2, 0)])
        self.assertEqual(document["blocks"][3]["text"], "x\n\ny")
        self.assertEqual(document["blocks"][3]["language"], "py")
        self.assertEqual(document["blocks"][4]["text"], "indented")
        self.assertEqual(document["blocks"][0]["spacing"], 0)

    def test_html_block_is_literal(self):
        block = parse("<div>\n<script>x()</script>\n</div>\n")["blocks"][0]
        self.assertEqual(block["kind"], KIND["html"])
        self.assertEqual(block["text"], "<div>\n<script>x()</script>\n</div>")

    def test_lists_markers_and_spacing(self):
        document = parse("1. one\n2. two\n   - inner\n\n3) x\n")
        blocks = document["blocks"]
        self.assertEqual([b["text"] for b in blocks], ["one", "two", "inner", "x"])
        self.assertEqual(blocks[0]["containers"][0]["marker"], "1.")
        self.assertEqual(blocks[1]["containers"][0]["marker"], "2.")
        self.assertFalse(blocks[2]["containers"][0]["showsMarker"])
        self.assertEqual(blocks[2]["containers"][1]["marker"], "\u2022")
        self.assertEqual(blocks[3]["containers"][0]["marker"], "3)")
        self.assertEqual([b["spacing"] for b in blocks], [0, 1, 1, 2])

    def test_loose_list_and_multi_paragraph_item(self):
        blocks = parse("- a\n\n  more\n- b\n")["blocks"]
        self.assertEqual([b["spacing"] for b in blocks], [0, 2, 2])
        self.assertTrue(blocks[0]["containers"][0]["showsMarker"])
        self.assertFalse(blocks[1]["containers"][0]["showsMarker"])

    def test_nested_bullets_change_glyph(self):
        blocks = parse("- a\n  - b\n    - c\n      - d\n")["blocks"]
        self.assertEqual([b["containers"][-1]["marker"] for b in blocks], ["\u2022", "\u25e6", "\u2023", "\u2023"])

    def test_quotes_nest(self):
        blocks = parse("> a\n>> b\n")["blocks"]
        self.assertEqual([len(b["containers"]) for b in blocks], [1, 2])
        self.assertEqual(blocks[0]["containers"][0]["id"], blocks[1]["containers"][0]["id"])

    def test_separate_quotes_have_distinct_ids(self):
        blocks = parse("> a\n\n> b\n")["blocks"]
        self.assertNotEqual(blocks[0]["containers"][0]["id"], blocks[1]["containers"][0]["id"])

    def test_table_cells_and_alignment(self):
        blocks = parse("| a | b | c |\n|:--|:-:|--:|\n| 1 | **2** |\n")["blocks"]
        self.assertEqual(blocks[0]["text"], "a\tb\tc")
        self.assertEqual([cell[2] for cell in blocks[0]["cells"]], [1, 2, 3])
        self.assertEqual(blocks[1]["text"], "1\t2\t")
        self.assertEqual([b["spacing"] for b in blocks], [0, 1])

    def test_adjacent_tables_have_distinct_ids(self):
        blocks = parse("| a |\n| - |\n| 1 |\n\n| b |\n| - |\n\npara\n")["blocks"]
        self.assertEqual([b["table"] for b in blocks], [1, 1, 2, 0])

    def test_admonition_and_footnotes(self):
        blocks = parse("> [!WARNING]\n> careful\n\nA[^n].\n\n[^n]: *why*\n")["blocks"]
        self.assertEqual([b["text"] for b in blocks], ["Warning", "careful", "A1.", "", "why"])
        self.assertEqual(blocks[0]["containers"][0]["kind"], 2)
        self.assertEqual(blocks[2]["runs"], [[1, 2, FOOTNOTE, -1]])
        self.assertEqual(blocks[3]["kind"], KIND["hr"])
        self.assertEqual(blocks[4]["containers"][0]["marker"], "1.")

    def test_image_alt_text_without_request(self):
        document = parse("![a *b*](https://x/y.png)\n")
        self.assertEqual(document["blocks"][0]["text"], "a b")
        self.assertEqual(document["links"], [])
        self.assertTrue(all(run[2] & IMAGE_ALT for run in document["blocks"][0]["runs"]))

    def test_empty_and_whitespace_sources(self):
        self.assertEqual(parse("")["blocks"], [])
        self.assertEqual(parse("\n\n  \n")["blocks"], [])


class LinkTests(unittest.TestCase):
    def assertLinkRanges(self, source, expected):
        document = parse(source)
        ranges = [source[slice(*link["range"])] for link in document["links"]]
        self.assertEqual(ranges, expected)
        return document

    def test_link_policy(self):
        source = (
            "[a](https://ok) [b](HTTP://ok) [c](mailto:x@y) [d](javascript:x) [e](/rel) "
            "[f](data:x) [g](https:) [h](ftp://x) [i](custom:x)\n"
        )
        document = parse(source)
        self.assertEqual([link["url"] for link in document["links"]], ["https://ok", "HTTP://ok", "mailto:x@y"])
        runs = document["blocks"][0]["runs"]
        self.assertEqual(len([run for run in runs if run[2] & LINK]), 3)

    def test_inline_link_ranges(self):
        self.assertLinkRanges(
            'x [a](https://a) [*b*](<https://b c> "t)") [c](https://c/(d)) y\n',
            ["[a](https://a)", '[*b*](<https://b c> "t)")', "[c](https://c/(d))"],
        )

    def test_reference_link_ranges(self):
        self.assertLinkRanges(
            "[full][r] [collapsed][] [r]\n\n[r]: https://r\n[collapsed]: https://c\n",
            ["[full][r]", "[collapsed][]", "[r]"],
        )

    def test_autolink_ranges(self):
        self.assertLinkRanges(
            "<https://a.b/c> www.example.com x@y.com\n",
            ["<https://a.b/c>", "www.example.com", "x@y.com"],
        )
        self.assertEqual(
            [link["url"] for link in parse("www.example.com x@y.com\n")["links"]],
            ["http://www.example.com", "mailto:x@y.com"],
        )

    def test_nested_image_and_escapes(self):
        self.assertLinkRanges(
            "[![i](s)](https://a) [\\]x\\[](https://b) [`]`](https://c) [](https://d) ![](z)[e](https://e)\n",
            ["[![i](s)](https://a)", "[\\]x\\[](https://b)", "[`]`](https://c)", "[](https://d)", "[e](https://e)"],
        )

    def test_ranges_use_utf16_offsets(self):
        source = "👩\u200d💻 \u00e9 [link](https://x) \u6f22\n"
        document = parse(source)
        start, end = document["links"][0]["range"]
        utf16 = source.encode("utf-16-le")
        self.assertEqual(utf16[start * 2 : end * 2].decode("utf-16-le"), "[link](https://x)")

    def test_entity_in_url(self):
        self.assertEqual(parse("[a](https://x?a=1&amp;b=2)\n")["links"][0]["url"], "https://x?a=1&b=2")

    def test_link_in_footnote_is_still_actionable(self):
        document = parse("a[^1]\n\n[^1]: see [docs](https://d)\n")
        self.assertEqual([link["url"] for link in document["links"]], ["https://d"])
        source = "a[^1]\n\n[^1]: see [docs](https://d)\n"
        self.assertEqual(source[slice(*document["links"][0]["range"])], "[docs](https://d)")


class InputTests(unittest.TestCase):
    def test_rejects_malformed_utf8(self):
        self.assertEqual(parse(b"a\xff")["error"], "invalid_input")
        self.assertEqual(parse(b"\xed\xa0\x80")["error"], "invalid_input")

    def test_rejects_oversized_source(self):
        self.assertEqual(parse(b"a" * (1024 * 1024 + 1))["error"], "input_limit")
        self.assertIn("blocks", parse(b"a" * (1024 * 1024)))

    def test_crlf_is_normalized_in_visible_text(self):
        self.assertEqual(visible_text(parse("a\r\nb\r\n\r\nc\r\n")), "a\nb\nc")


if __name__ == "__main__":
    unittest.main()
