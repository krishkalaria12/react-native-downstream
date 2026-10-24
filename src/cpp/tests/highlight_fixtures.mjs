// Samples the golden token fixtures are taken from.
//
// One case per alias group, plus the cases a lexer update is most likely to
// change: template literals, preprocessor regions, a block that spans lines,
// and languages embedded in one another. The expected runs live in
// engine.test.mjs.snapshot, which `node --test --test-update-snapshots` writes.
//
//   node --test --test-update-snapshots src/cpp/tests/engine.test.mjs
//
// Read the resulting diff. Every change is either a deliberate improvement to
// record or a regression to fix, and a run's class is the only thing that
// decides its color, so a diff here is a diff in what a reader sees.

/** @type {{id: string, info: string, source: string}[]} */
export const highlightCases = [
  {
    id: 'ts-types',
    info: 'ts',
    source: [
      'export async function loadUser(id: number): Promise<User | null> {',
      '  const res = await fetch(`/api/users/${id}?v=${VERSION}`);',
      '  if (!res.ok) {',
      "    throw new Error('failed: ' + res.status);",
      '  }',
      '  return res.json();',
      '}',
      '',
    ].join('\n'),
  },
  {
    id: 'ts-template',
    info: 'ts',
    source: 'const greeting = `hello ${name}, you are ${age} years old`;\n',
  },
  {
    id: 'ts-regex-and-numbers',
    info: 'ts',
    source:
      'const pattern = /^[a-z]+\\d{2,}$/gi;\nconst sizes = [0x10, 1e6, 0b1010, 12_345];\n',
  },
  {
    id: 'ts-imports',
    info: 'ts',
    source:
      "import { readFile } from 'node:fs/promises';\nimport type { Foo } from './foo';\n",
  },
  {
    id: 'ts-jsx',
    // Tags, attributes, and an expression inside a type-checked file.
    info: 'tsx',
    source:
      'export const App = () => <View style={s}><Text>hi</Text></View>;\n',
  },
  {
    id: 'c-preprocessor',
    info: 'c',
    source: [
      '#include <stdio.h>',
      '#define MAX 10',
      '#if defined(UNIX) && MAX > 1',
      'int main(void) { return 0; }',
      '#else',
      'int main(void) { return 1; }',
      '#endif',
      '',
    ].join('\n'),
  },
  {
    id: 'java',
    info: 'java',
    source: [
      'package com.example;',
      '',
      'public final class Greeter {',
      '  public static String greet(String name) {',
      '    return "hello " + name;',
      '  }',
      '}',
      '',
    ].join('\n'),
  },
  {
    id: 'go',
    info: 'go',
    source: [
      'package main',
      '',
      'import "fmt"',
      '',
      'func main() {',
      '\tdefer fmt.Println("done")',
      '\tvar total int64',
      '\tfor i := 0; i < 10; i++ {',
      '\t\ttotal += int64(i)',
      '\t}',
      '\tfmt.Printf("%d\\n", total)',
      '}',
      '',
    ].join('\n'),
  },
  {
    id: 'go-raw-string',
    info: 'go',
    source: 'var query = `SELECT *\nFROM users`\nvar n = 1\n',
  },
  {
    id: 'swift',
    info: 'swift',
    source: [
      'struct Point { let x: Int; let y: Int }',
      '',
      'func distance(from a: Point, to b: Point) -> Double {',
      '  let dx = Double(b.x - a.x)',
      '  return (dx * dx).squareRoot()',
      '}',
      '',
    ].join('\n'),
  },
  {
    id: 'kotlin',
    info: 'kt',
    source: [
      'data class User(val id: Int, val name: String)',
      '',
      'fun greet(user: User): String {',
      '  val prefix = "hi"',
      '  return "$prefix, ${user.name}"',
      '}',
      '',
    ].join('\n'),
  },
  {
    id: 'python',
    info: 'py',
    source: [
      'from dataclasses import dataclass',
      '',
      '@dataclass',
      'class User:',
      '    """A user."""',
      '',
      '    def greet(self, name: str) -> str:',
      '        return f"hello {name}"',
      '',
    ].join('\n'),
  },
  {
    id: 'python-triple-quoted',
    info: 'python',
    source: "text = '''\nline one\nline two\n'''\nprint(text)\n",
  },
  {
    id: 'bash',
    info: 'bash',
    source: [
      '#!/usr/bin/env bash',
      'set -euo pipefail',
      '',
      'for file in *.txt; do',
      '  if [ -f "$file" ]; then',
      '    echo "found $file"',
      '  fi',
      'done',
      '',
    ].join('\n'),
  },
  {
    id: 'json',
    info: 'json',
    source:
      '{\n  "name": "downstream",\n  "count": 3,\n  "ok": true,\n  "tags": null\n}\n',
  },
  {
    id: 'html',
    info: 'html',
    source: [
      '<section class="wrap" id="main">',
      '  <!-- a comment -->',
      '  <p title="hi">text &amp; more</p>',
      '  <script>const a = 1;</script>',
      '</section>',
      '',
    ].join('\n'),
  },
  {
    id: 'xml',
    info: 'xml',
    source: '<?xml version="1.0"?>\n<root attr="1"><child/></root>\n',
  },
  {
    id: 'css',
    info: 'css',
    source:
      '.card > .title, #id {\n  color: #ff0000;\n  margin: 0 auto;\n}\n@media print { a { display: none; } }\n',
  },
  {
    id: 'sql',
    info: 'sql',
    source:
      '-- pick the newest\nSELECT id, name\nFROM users\nWHERE active = 1\nORDER BY created DESC;\n',
  },
  {
    id: 'rust',
    info: 'rs',
    source: [
      'use std::collections::HashMap;',
      '',
      'pub fn count(map: &HashMap<String, u32>) -> usize {',
      '    map.values().filter(|v| **v > 0).count()',
      '}',
      '',
    ].join('\n'),
  },
  {
    id: 'yaml',
    info: 'yaml',
    source: 'name: downstream\nversion: 1\nitems:\n  - one\n  - two\n',
  },
  {
    id: 'toml',
    info: 'toml',
    source: '[package]\nname = "downstream"\nversion = "0.1.0"\n',
  },
  {
    id: 'ruby',
    info: 'rb',
    source:
      'class Greeter\n  def greet(name)\n    puts "hi #{name}"\n  end\nend\n',
  },
  {
    id: 'lua',
    info: 'lua',
    source:
      'local M = {}\n\nfunction M.add(a, b)\n  return a + b\nend\n\nreturn M\n',
  },
  {
    id: 'diff',
    info: 'diff',
    source:
      '--- a/old.txt\n+++ b/new.txt\n@@ -1,3 +1,3 @@\n context\n-removed\n+added\n',
  },
  {
    id: 'markdown',
    info: 'md',
    source: '# Title\n\nSome *emphasis* and `code`.\n\n- one\n- two\n',
  },
  {
    id: 'dart',
    info: 'dart',
    source: 'class Greeter {\n  String greet(String name) => "hi $name";\n}\n',
  },
  {
    id: 'scala',
    info: 'scala',
    source:
      'object Main {\n  def main(args: Array[String]): Unit = println("hi")\n}\n',
  },
  {
    id: 'objc',
    info: 'm',
    source:
      '@implementation Greeter\n- (NSString *)greet:(NSString *)name {\n  return [@"hi " stringByAppendingString:name];\n}\n@end\n',
  },
  {
    id: 'csharp',
    info: 'cs',
    source:
      'public sealed class Greeter {\n  public static string Greet(string name) => $"hi {name}";\n}\n',
  },
  {
    id: 'php',
    info: 'php',
    source:
      '<?php\nfunction greet(string $name): string {\n  return "hi {$name}";\n}\n',
  },
  {
    id: 'haskell',
    info: 'hs',
    source:
      'module Main where\n\n-- | Greets.\ngreet :: String -> String\ngreet name = "hi " ++ name\n',
  },
  {
    id: 'elixir',
    info: 'elixir',
    source:
      'defmodule Greeter do\n  def greet(name), do: "hi #{name}" |> String.trim()\nend\n',
  },
  {
    id: 'zig',
    info: 'zig',
    source:
      'const std = @import("std");\npub fn main() !void {\n    std.debug.print("{d}\\n", .{42});\n}\n',
  },
  {
    id: 'graphql',
    info: 'graphql',
    source:
      'query User($id: ID!) {\n  user(id: $id) { name @include(if: true) }\n}\n',
  },
  {
    id: 'dockerfile',
    info: 'dockerfile',
    source:
      'FROM node:22 AS build\n# Install\nRUN npm ci && npm run build\nENV PORT=3000\n',
  },
  {
    id: 'vue',
    // A template, a script, and a style in one file, each lexed as its own
    // language.
    info: 'vue',
    source: [
      '<template>',
      '  <p :class="cls">{{ msg }}</p>',
      '</template>',
      '<script setup lang="ts">',
      'const msg: string = "hi";',
      '</script>',
      '<style>',
      'p { color: red; }',
      '</style>',
      '',
    ].join('\n'),
  },
  {
    id: 'scss',
    info: 'scss',
    source: '$gap: 4px;\n.card {\n  &:hover { margin: $gap * 2; }\n}\n',
  },
  {
    id: 'powershell',
    info: 'ps1',
    source: '$name = "world"\nWrite-Host "hi $name" -ForegroundColor Green\n',
  },
  {
    id: 'terraform',
    info: 'tf',
    source:
      'resource "aws_s3_bucket" "logs" {\n  bucket = "logs-${var.env}"\n  count  = 2\n}\n',
  },
  {
    id: 'makefile',
    info: 'makefile',
    source: 'CC ?= cc\nall: main.o\n\t$(CC) -o app $^\n',
  },
  {
    id: 'wide-characters',
    // Emoji, CJK, and Hebrew in one block: every run must still land on a code
    // point boundary, or a platform attribute would split a surrogate pair.
    info: 'ts',
    source: 'const 👩 = "漢字";\nconst ש = 1; // שלום\n',
  },
  {
    id: 'unknown-language',
    info: 'brainfuck',
    source: '+++++[>++++<-].\n',
  },
  {
    id: 'no-language',
    info: '',
    source: 'const a = 1;\n',
  },
];

/** Fenced documents whose parse must produce the same tokens dump_tokens does. */
export const fencedCases = [
  {
    id: 'fenced-ts',
    source: '```ts\nconst a: number = 1;\n```\n',
  },
  {
    id: 'fenced-two-languages',
    source: '```py\nx = 1\n```\n\n```sh\necho hi\n```\n',
  },
  {
    id: 'fenced-unknown',
    source: '```brainfuck\n+++++\n```\n',
  },
  {
    id: 'fenced-meta-string',
    source: '```ts title="a.ts"\nconst a = 1;\n```\n',
  },
  {
    id: 'indented-code',
    source: '    const a = 1;\n',
  },
  {
    id: 'prose-only',
    source:
      '# Title\n\nProse with `inline code`, a [link](https://x.dev), and *emphasis*.\n',
  },
];
