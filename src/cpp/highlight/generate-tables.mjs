#!/usr/bin/env node
// Writes src/cpp/highlight/LanguageTables.cpp, the keyword lists and
// lexer-style maps the highlighter needs. No build runs this: the output is
// checked in, and `pnpm test:native` checks the vendored lexers and the tables
// still agree with the golden token fixtures.
//
//     node src/cpp/highlight/generate-tables.mjs --scite <SciTE src dir>
//
// SciTE's `.properties` files hold the keyword lists under the same license as
// Lexilla, so the lists come from there rather than being retyped. Languages
// SciTE has no list for get the hand-written lists below. A SciTE checkout
// comes from https://www.scintilla.org/SciTEDownload.html; only the
// `src/*.properties` files are read.

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const SCILEXER = join(HERE, '..', 'lexilla', 'SciLexer.h');

const arg = (name, fallback) => {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 && process.argv[index + 1] ? process.argv[index + 1] : fallback;
};

const sciteDir = arg('scite');
if (!sciteDir) {
  console.error('usage: generate-tables.mjs --scite <SciTE src dir>');
  process.exit(1);
}
const out = resolve(arg('out', join(HERE, 'LanguageTables.cpp')));

const cap = (text) => text.charAt(0).toUpperCase() + text.slice(1);
const cString = (text) => `"${text.replace(/[\\"]/g, '\\$&')}"`;

// ---------------------------------------------------------------- properties

/** Joins `\`-continued lines and drops comments, as SciTE's parser does. */
function logicalLines(text) {
  return text
    .replace(/\\\n[ \t]*/g, ' ')
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line !== '' && !line.startsWith('#'));
}

/** A SciTE properties file, with `$(name)` references expanded. */
function readProperties(file) {
  const raw = new Map();
  for (const line of logicalLines(readFileSync(file, 'utf8'))) {
    const equals = line.indexOf('=');
    if (equals < 0) {
      continue;
    }
    raw.set(line.slice(0, equals).trim(), line.slice(equals + 1).trim());
  }
  const expand = (value, depth = 0) => {
    if (depth > 8 || !value.includes('$(')) {
      return value;
    }
    return expand(
      value.replace(/\$\(([^)]+)\)/g, (match, name) => raw.get(name) ?? match),
      depth + 1
    );
  };
  for (const [key, value] of [...raw]) {
    raw.set(key, expand(value));
  }
  return { raw, expand };
}

const files = {};
for (const name of [
  'bash',
  'cpp',
  'css',
  'html',
  'json',
  'lua',
  'markdown',
  'python',
  'ruby',
  'rust',
  'sql',
  'yaml',
]) {
  files[name] = readProperties(join(sciteDir, `${name}.properties`));
}

/** The value of one properties key, matched after `$(name)` expansion. */
function list(file, key) {
  const { raw, expand } = files[file];
  const wanted = expand(key);
  for (const [name, value] of raw) {
    if (expand(name) === wanted) {
      return value;
    }
  }
  throw new Error(`${file}.properties has no key ${key}`);
}

// ------------------------------------------------------------------ word lists

// Word list 0 is the language's keywords. From SciTE where it has a list, and
// hand-written where it does not: TypeScript, Objective C, Dart, Scala, and
// Kotlin have no SciTE list of their own, and TOML has no properties file at
// all. Word list 1 is the types and builtins a language exposes, which SciTE
// keeps out of its keyword lists because its C++ lexer had no second list for
// them. A language with no second list leaves it empty.
const JS_KEYWORDS = list('cpp', 'keywords.$(file.patterns.js)');
const CPP_KEYWORDS = list('cpp', 'keywords.$(file.patterns.cpp)');
const C_KEYWORDS = 'auto break case char const continue default do double else enum extern ' +
  'float for goto if inline int long register return short signed sizeof static struct switch ' +
  'typedef union unsigned void volatile while _Alignas _Alignof _Atomic _Bool _Complex _Generic ' +
  '_Imaginary _Noreturn _Static_assert _Thread_local';
const OBJC_KEYWORDS =
  'auto break case char const continue default do double else enum extern float for goto if ' +
  'inline int long register return short signed sizeof static struct switch typedef union ' +
  'unsigned void volatile while BOOL Class NSArray NSCoder NSDictionary NSInteger NSObject ' +
  'NSString NSUInteger Protocol SEL id nil instancetype nonatomic nullable nonnull strong weak ' +
  'copy assign readonly inout bycopy byref oneway self super YES NO @interface @implementation ' +
  '@protocol @property @end @class @synthesize @dynamic @selector @encode @try @catch @finally ' +
  '@throw @autoreleasepool @import';
const DART_KEYWORDS =
  'abstract as assert async await base break case catch class const continue covariant default ' +
  'deferred do dynamic else enum export extends extension external factory final finally for ' +
  'get hide if implements import in interface is late library mixin new on operator part ' +
  'required rethrow return sealed set show static super switch sync this throw try typedef var ' +
  'void when while with yield';
const SCALA_KEYWORDS =
  'abstract case catch class def do else extends false final finally for forSome given if ' +
  'implicit import lazy match new null object override package private protected return sealed ' +
  'super this throw trait try true type using val var while with yield';
const PHP_KEYWORDS =
  'abstract and array as break callable case catch class clone const continue declare default ' +
  'do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum extends ' +
  'final finally fn for foreach function global goto if implements include include_once ' +
  'instanceof insteadof interface isset list match namespace new or print private protected ' +
  'public readonly require require_once return static switch throw trait try unset use var ' +
  'while xor yield';
const KOTLIN_KEYWORDS =
  'as break by catch class companion const constructor continue crossinline data delegate do ' +
  'dynamic else enum external false field file finally for fun get if import in infix init ' +
  'inline inner interface internal is lateinit noinline null object open operator out override ' +
  'package param private property protected public reified return sealed set super suspend ' +
  'tailrec this throw true try typealias typeof val var vararg when where while';

/** Word lists per language, in the lexer's own WordListSet order. */
const WORD_LISTS = {
  c: [C_KEYWORDS, ''],
  cpp: [CPP_KEYWORDS, ''],
  java: [list('cpp', 'keywords.$(file.patterns.java)'), ''],
  csharp: [list('cpp', 'keywords.$(file.patterns.cs)'), ''],
  go: [list('cpp', 'keywords.$(file.patterns.go)'), ''],
  swift: [list('cpp', 'keywords.$(file.patterns.swift)'), ''],
  js: [JS_KEYWORDS, ''],
  ts: [JS_KEYWORDS, ''],
  objc: [OBJC_KEYWORDS, ''],
  dart: [DART_KEYWORDS, ''],
  scala: [SCALA_KEYWORDS, ''],
  kotlin: [KOTLIN_KEYWORDS, ''],
  python: [list('python', 'keywords.$(file.patterns.py)'), ''],
  bash: [list('bash', 'keywords.$(file.patterns.bash)'), ''],
  json: [list('json', 'keywords.$(file.patterns.json)'), ''],
  php: ['', JS_KEYWORDS, '', '', PHP_KEYWORDS],
  // LexHTML colors an embedded <script> with its second word list, which
  // SciTE fills with JavaScript keywords.
  html: [list('html', 'keywords.$(file.patterns.html)'), JS_KEYWORDS],
  xml: ['', JS_KEYWORDS],
  vue: [list('html', 'keywords.$(file.patterns.html)'), JS_KEYWORDS],
  php: ['', JS_KEYWORDS, '', '', PHP_KEYWORDS],
  css: [
    list('css', 'keywords.*.css'),
    list('css', 'keywords2.*.css'),
    list('css', 'keywords3.*.css'),
  ],
  sql: [list('sql', 'keywords.$(file.patterns.sql)'), ''],
  rust: [list('rust', 'keywords.$(file.patterns.rust)'), list('rust', 'keywords2.$(file.patterns.rust)')],
  yaml: [list('yaml', 'keywords.$(file.patterns.yaml)'), ''],
  ruby: [list('ruby', 'keywords.$(file.patterns.rb)'), ''],
  lua: [list('lua', 'keywords.$(file.patterns.lua)'), ''],
  diff: ['', ''],
  markdown: ['', ''],
  toml: ['', ''],
};

// Types and builtins, which a language's lexer renders in its second word list
// and Shiki renders as a type or a builtin.
const TYPES = {
  c: 'bool char double float int long short signed unsigned void size_t ssize_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t wchar_t char8_t char16_t char32_t FILE va_list',
  cpp: 'bool char char8_t char16_t char32_t double float int long short signed size_t ssize_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t unsigned void wchar_t string vector map unordered_map set unordered_set deque list array pair tuple optional variant function shared_ptr unique_ptr weak_ptr string_view ostringstream istringstream stringstream ostream istream fstream',
  csharp: 'bool byte char decimal double dynamic float int long object sbyte short string uint ulong ushort var void',
  dart: 'bool double dynamic int num Object String void List Map Set Iterable Future Stream',
  go: 'any bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr',
  java: 'boolean byte char double float int long short void String Object List Map Set Optional',
  js: 'Array ArrayBuffer BigInt Boolean Date Error Function JSON Map Math Number Object Promise Proxy RegExp Set String Symbol WeakMap WeakSet BigInt64Array BigUint64Array Float32Array Float64Array Int8Array Int16Array Int32Array Uint8Array Uint8ClampedArray Uint16Array Uint32Array console globalThis document window',
  objc: 'BOOL id Class SEL IMP nil YES NO instancetype NSInteger NSUInteger CGFloat NSRange NSString NSArray NSDictionary NSNumber NSData NSDate NSError NSURL NSBundle',
  scala: 'Any AnyRef AnyVal Boolean Byte Char Double Float Int List Long Map Nothing Option Seq Set Short String Symbol Unit Vector Array',
  swift: 'Any AnyObject Array Bool Character Dictionary Double Error Float Int Optional Set String UInt Void',
  ts: 'any bigint boolean never number object string symbol undefined unknown void Array AsyncIterable Awaited Boolean Iterable Iterator Number Object Promise ReadonlyArray ReadonlyMap ReadonlySet Record RegExp Set String Uint8Array Uint16Array Uint32Array Uint8ClampedArray ArrayBuffer BigInt64Array BigUint64Array Float32Array Float64Array Int8Array Int16Array Int32Array Map Partial Pick Required Readonly Omit Exclude Extract NonNullable ConstructorParameters',
  kotlin: 'Any Array Boolean Byte Char Double Float Int List Long Map Nothing Number Set Short String Unit',
  python: 'bool bytes complex dict float frozenset int list object set str tuple',
};

/** The word lists for a language name, with its types in slot one. */
function wordLists(language) {
  const lists = WORD_LISTS[language];
  if (lists === undefined) {
    throw new Error(`no word lists named ${language}`);
  }
  const types = TYPES[language] ?? '';
  return lists.length > 1 ? lists : [lists[0], types];
}

// ------------------------------------------------------------------- styles

// SCE style names to token classes, one entry per lexer. Names are resolved
// against the pinned SciLexer.h, so a renamed or renumbered style fails this
// script instead of silently miscoloring code. `id` is the style the post-pass
// promotes to Function, Property, and Type.
// LexHTML serves both the hypertext and xml lexers from one style set.
const MARKUP = {
  id: -1,
  map: `
      SCE_H_COMMENT=Comment SCE_H_XCCOMMENT=Comment SCE_H_TAG=Tag SCE_H_TAGUNKNOWN=Tag
      SCE_H_TAGEND=Tag SCE_H_XMLSTART=Tag SCE_H_XMLEND=Tag SCE_H_SCRIPT=Tag
      SCE_H_ATTRIBUTE=Attribute SCE_H_ATTRIBUTEUNKNOWN=Attribute SCE_H_ASPAT=Attribute
      SCE_H_NUMBER=Number SCE_H_DOUBLESTRING=String SCE_H_SINGLESTRING=String
      SCE_H_CDATA=String SCE_H_ENTITY=Constant SCE_H_QUESTION=Keyword SCE_H_ASP=Preprocessor`,
};

const STYLES = {
  cpp: {
    id: 'SCE_C_IDENTIFIER',
    map: `
      SCE_C_COMMENT=Comment SCE_C_COMMENTLINE=Comment SCE_C_COMMENTDOC=Comment
      SCE_C_COMMENTLINEDOC=Comment SCE_C_PREPROCESSORCOMMENT=Comment
      SCE_C_PREPROCESSORCOMMENTDOC=Comment SCE_C_COMMENTDOCKEYWORD=Keyword
      SCE_C_COMMENTDOCKEYWORDERROR=Comment SCE_C_TASKMARKER=Comment
      SCE_C_NUMBER=Number SCE_C_UUID=Number SCE_C_WORD=Keyword SCE_C_WORD2=Type
      SCE_C_GLOBALCLASS=Type SCE_C_STRING=String SCE_C_CHARACTER=String
      SCE_C_STRINGEOL=String SCE_C_STRINGRAW=String SCE_C_TRIPLEVERBATIM=String
      SCE_C_HASHQUOTEDSTRING=String SCE_C_VERBATIM=String SCE_C_ESCAPESEQUENCE=String
      SCE_C_REGEX=Regex SCE_C_PREPROCESSOR=Preprocessor SCE_C_OPERATOR=Operator
      SCE_C_USERLITERAL=Constant`,
  },
  python: {
    id: 'SCE_P_IDENTIFIER',
    map: `
      SCE_P_COMMENTLINE=Comment SCE_P_COMMENTBLOCK=Comment SCE_P_NUMBER=Number
      SCE_P_STRING=String SCE_P_CHARACTER=String SCE_P_TRIPLE=String
      SCE_P_TRIPLEDOUBLE=String SCE_P_FSTRING=String SCE_P_FCHARACTER=String
      SCE_P_FTRIPLE=String SCE_P_FTRIPLEDOUBLE=String SCE_P_STRINGEOL=String
      SCE_P_WORD=Keyword SCE_P_WORD2=Type SCE_P_CLASSNAME=Type SCE_P_DEFNAME=Function
      SCE_P_ATTRIBUTE=Property SCE_P_DECORATOR=Preprocessor SCE_P_OPERATOR=Operator`,
  },
  bash: {
    id: 'SCE_SH_IDENTIFIER',
    map: `
      SCE_SH_COMMENTLINE=Comment SCE_SH_ERROR=Comment SCE_SH_NUMBER=Number
      SCE_SH_WORD=Keyword SCE_SH_STRING=String SCE_SH_CHARACTER=String
      SCE_SH_BACKTICKS=String SCE_SH_HERE_DELIM=String SCE_SH_HERE_Q=String
      SCE_SH_SCALAR=Constant SCE_SH_PARAM=Constant SCE_SH_OPERATOR=Operator`,
  },
  json: {
    id: -1,
    map: `
      SCE_JSON_LINECOMMENT=Comment SCE_JSON_BLOCKCOMMENT=Comment SCE_JSON_ERROR=Comment
      SCE_JSON_NUMBER=Number SCE_JSON_STRING=String SCE_JSON_STRINGEOL=String
      SCE_JSON_ESCAPESEQUENCE=String SCE_JSON_URI=String SCE_JSON_PROPERTYNAME=Property
      SCE_JSON_KEYWORD=Constant SCE_JSON_LDKEYWORD=Constant SCE_JSON_OPERATOR=Operator`,
  },
  hypertext: MARKUP,
  xml: MARKUP,
  css: {
    id: -1,
    map: `
      SCE_CSS_COMMENT=Comment SCE_CSS_TAG=Tag SCE_CSS_GROUP_RULE=Tag
      SCE_CSS_DIRECTIVE=Tag SCE_CSS_CLASS=Type SCE_CSS_ID=Type
      SCE_CSS_PSEUDOCLASS=Keyword SCE_CSS_UNKNOWN_PSEUDOCLASS=Keyword
      SCE_CSS_PSEUDOELEMENT=Keyword SCE_CSS_EXTENDED_PSEUDOCLASS=Keyword
      SCE_CSS_EXTENDED_PSEUDOELEMENT=Keyword SCE_CSS_IDENTIFIER=Attribute
      SCE_CSS_UNKNOWN_IDENTIFIER=Attribute SCE_CSS_ATTRIBUTE=Attribute
      SCE_CSS_IDENTIFIER2=Property SCE_CSS_IDENTIFIER3=Property
      SCE_CSS_EXTENDED_IDENTIFIER=Property SCE_CSS_VARIABLE=Constant
      SCE_CSS_IMPORTANT=Keyword SCE_CSS_DOUBLESTRING=String
      SCE_CSS_SINGLESTRING=String SCE_CSS_OPERATOR=Operator`,
  },
  sql: {
    id: 'SCE_SQL_IDENTIFIER',
    map: `
      SCE_SQL_COMMENT=Comment SCE_SQL_COMMENTLINE=Comment SCE_SQL_COMMENTDOC=Comment
      SCE_SQL_COMMENTLINEDOC=Comment SCE_SQL_SQLPLUS_COMMENT=Comment
      SCE_SQL_COMMENTDOCKEYWORD=Keyword SCE_SQL_COMMENTDOCKEYWORDERROR=Comment
      SCE_SQL_NUMBER=Number SCE_SQL_WORD=Keyword SCE_SQL_WORD2=Type SCE_SQL_STRING=String
      SCE_SQL_CHARACTER=String SCE_SQL_QUOTEDIDENTIFIER=String SCE_SQL_OPERATOR=Operator
      SCE_SQL_QOPERATOR=Operator SCE_SQL_SQLPLUS=Operator SCE_SQL_SQLPLUS_PROMPT=Operator
      SCE_SQL_USER1=Constant SCE_SQL_USER2=Constant SCE_SQL_USER3=Constant SCE_SQL_USER4=Constant`,
  },
  rust: {
    id: 'SCE_RUST_IDENTIFIER',
    map: `
      SCE_RUST_COMMENTBLOCK=Comment SCE_RUST_COMMENTLINE=Comment
      SCE_RUST_COMMENTBLOCKDOC=Comment SCE_RUST_COMMENTLINEDOC=Comment
      SCE_RUST_LEXERROR=Comment SCE_RUST_NUMBER=Number SCE_RUST_WORD=Keyword
      SCE_RUST_WORD2=Type SCE_RUST_WORD3=Type SCE_RUST_WORD4=Constant
      SCE_RUST_WORD5=Constant SCE_RUST_WORD6=Constant SCE_RUST_WORD7=Type
      SCE_RUST_LIFETIME=Type
      SCE_RUST_STRING=String SCE_RUST_STRINGR=String SCE_RUST_CHARACTER=String
      SCE_RUST_BYTESTRING=String SCE_RUST_BYTESTRINGR=String SCE_RUST_BYTECHARACTER=String
      SCE_RUST_CSTRING=String SCE_RUST_CSTRINGR=String SCE_RUST_MACRO=Preprocessor
      SCE_RUST_OPERATOR=Operator`,
  },
  yaml: {
    id: -1,
    map: `
      SCE_YAML_COMMENT=Comment SCE_YAML_ERROR=Comment SCE_YAML_IDENTIFIER=Property
      SCE_YAML_KEYWORD=Constant SCE_YAML_REFERENCE=Constant SCE_YAML_NUMBER=Number
      SCE_YAML_TEXT=String SCE_YAML_OPERATOR=Operator`,
  },
  ruby: {
    id: 'SCE_RB_IDENTIFIER',
    map: `
      SCE_RB_COMMENTLINE=Comment SCE_RB_POD=Comment SCE_RB_NUMBER=Number
      SCE_RB_WORD=Keyword SCE_RB_WORD_DEMOTED=Constant SCE_RB_STRING=String
      SCE_RB_CHARACTER=String SCE_RB_BACKTICKS=String SCE_RB_HERE_DELIM=String
      SCE_RB_HERE_Q=String SCE_RB_HERE_QQ=String SCE_RB_HERE_QX=String
      SCE_RB_STRING_Q=String SCE_RB_STRING_QQ=String SCE_RB_STRING_QX=String
      SCE_RB_STRING_QR=String SCE_RB_STRING_QW=String SCE_RB_STRING_W=String
      SCE_RB_STRING_I=String SCE_RB_STRING_QI=String SCE_RB_STRING_QS=String
      SCE_RB_CLASSNAME=Type SCE_RB_MODULE_NAME=Type SCE_RB_DEFNAME=Function
      SCE_RB_INSTANCE_VAR=Property SCE_RB_CLASS_VAR=Property SCE_RB_REGEX=Regex
      SCE_RB_GLOBAL=Constant SCE_RB_SYMBOL=Constant SCE_RB_DATASECTION=Constant
      SCE_RB_STDIN=Constant SCE_RB_STDOUT=Constant SCE_RB_STDERR=Constant
      SCE_RB_UPPER_BOUND=Constant SCE_RB_OPERATOR=Operator`,
  },
  diff: {
    id: -1,
    map: `
      SCE_DIFF_COMMENT=Comment SCE_DIFF_COMMAND=Preprocessor SCE_DIFF_HEADER=Heading
      SCE_DIFF_POSITION=Constant SCE_DIFF_DELETED=Deleted SCE_DIFF_ADDED=Inserted
      SCE_DIFF_CHANGED=Inserted SCE_DIFF_PATCH_ADD=Inserted SCE_DIFF_PATCH_DELETE=Deleted
      SCE_DIFF_REMOVED_PATCH_ADD=Inserted SCE_DIFF_REMOVED_PATCH_DELETE=Deleted`,
  },
  markdown: {
    id: -1,
    map: `
      SCE_MARKDOWN_PRECHAR=Comment SCE_MARKDOWN_STRIKEOUT=Comment
      SCE_MARKDOWN_HRULE=Comment SCE_MARKDOWN_STRONG1=Keyword
      SCE_MARKDOWN_STRONG2=Keyword SCE_MARKDOWN_EM1=Keyword SCE_MARKDOWN_EM2=Keyword
      SCE_MARKDOWN_HEADER1=Heading SCE_MARKDOWN_HEADER2=Heading
      SCE_MARKDOWN_HEADER3=Heading SCE_MARKDOWN_HEADER4=Heading
      SCE_MARKDOWN_HEADER5=Heading SCE_MARKDOWN_HEADER6=Heading
      SCE_MARKDOWN_ULIST_ITEM=Operator SCE_MARKDOWN_OLIST_ITEM=Operator
      SCE_MARKDOWN_BLOCKQUOTE=Operator SCE_MARKDOWN_LINK=Tag`,
  },
  lua: {
    id: 'SCE_LUA_IDENTIFIER',
    map: `
      SCE_LUA_COMMENT=Comment SCE_LUA_COMMENTLINE=Comment SCE_LUA_COMMENTDOC=Comment
      SCE_LUA_NUMBER=Number SCE_LUA_WORD=Keyword SCE_LUA_WORD2=Type SCE_LUA_WORD3=Constant
      SCE_LUA_WORD4=Type SCE_LUA_WORD5=Type SCE_LUA_WORD6=Type SCE_LUA_WORD7=Type
      SCE_LUA_WORD8=Type SCE_LUA_STRING=String SCE_LUA_CHARACTER=String
      SCE_LUA_LITERALSTRING=String SCE_LUA_STRINGEOL=String SCE_LUA_LABEL=Constant
      SCE_LUA_PREPROCESSOR=Preprocessor SCE_LUA_OPERATOR=Operator`,
  },
  toml: {
    id: -1,
    map: `
      SCE_TOML_COMMENT=Comment SCE_TOML_ERROR=Comment SCE_TOML_IDENTIFIER=Property
      SCE_TOML_KEY=Property SCE_TOML_TABLE=Type SCE_TOML_KEYWORD=Constant
      SCE_TOML_NUMBER=Number SCE_TOML_DATETIME=Number SCE_TOML_STRING_SQ=String
      SCE_TOML_STRING_DQ=String SCE_TOML_TRIPLE_STRING_SQ=String
      SCE_TOML_TRIPLE_STRING_DQ=String SCE_TOML_ESCAPECHAR=String
      SCE_TOML_STRINGEOL=String SCE_TOML_OPERATOR=Operator`,
  },
};

const styleNumbers = new Map();
for (const line of readFileSync(SCILEXER, 'utf8').split('\n')) {
  const match = /^#define\s+(SCE_\w+)\s+(\d+)\s*$/.exec(line.trim());
  if (match && !styleNumbers.has(match[1])) {
    styleNumbers.set(match[1], Number(match[2]));
  }
}

function resolveStyle(name, lexer) {
  const style = styleNumbers.get(name);
  if (style === undefined) {
    throw new Error(`${lexer}: ${name} is not defined in SciLexer.h`);
  }
  return style;
}

const styleMaps = {};
for (const [lexer, { id, map }] of Object.entries(STYLES)) {
  const pairs = [];
  for (const entry of map.split(/\s+/).filter(Boolean)) {
    const [name, token] = entry.split('=');
    if (!token) {
      throw new Error(`${lexer}: malformed style entry ${entry}`);
    }
    pairs.push([resolveStyle(name, lexer), token]);
  }
  pairs.sort((a, b) => a[0] - b[0]);
  for (let index = 1; index < pairs.length; index++) {
    if (pairs[index][0] === pairs[index - 1][0]) {
      throw new Error(`${lexer}: two entries share style ${pairs[index][0]}`);
    }
  }
  styleMaps[lexer] = {
    pairs,
    identifier: id === -1 ? -1 : resolveStyle(id, lexer),
  };
}

// ---------------------------------------------------------------- languages

const CPP = 'MarkCalls|MarkProperties|MarkTypes';
const cppLexer = (backquoted, marks, extra = []) => ({
  lexer: 'cpp',
  properties: [
    // JavaScript, and the shell and Objective C dialects layered on C, all use
    // '$' in identifiers. A back-quoted string is a template literal in
    // JavaScript and a raw string in Go.
    ['lexer.cpp.allow.dollars', '1'],
    ['lexer.cpp.backquoted.strings', String(backquoted)],
    ...extra,
  ],
  marks,
});
const plain = (lexer, properties = [], marks = 'MarkNone') => ({ lexer, properties, marks });

const LANGUAGES = [
  // The C family shares one lexer, so each language carries its own lists.
  { aliases: ['js', 'jsx', 'mjs', 'cjs', 'javascript'], words: 'js', lex: cppLexer(2, CPP) },
  { aliases: ['ts', 'tsx', 'typescript'], words: 'ts', lex: cppLexer(2, CPP) },
  { aliases: ['java'], words: 'java', lex: cppLexer(0, CPP) },
  { aliases: ['c'], words: 'c', lex: cppLexer(0, CPP) },
  { aliases: ['cpp', 'c++', 'cc', 'hpp', 'cxx', 'hxx', 'h++', 'h'], words: 'cpp', lex: cppLexer(0, CPP) },
  { aliases: ['cs', 'csharp'], words: 'csharp', lex: cppLexer(0, CPP) },
  { aliases: ['go', 'golang'], words: 'go', lex: cppLexer(1, CPP) },
  { aliases: ['swift'], words: 'swift', lex: cppLexer(0, CPP) },
  { aliases: ['kotlin', 'kt', 'kts'], words: 'kotlin', lex: cppLexer(0, CPP) },
  { aliases: ['objc', 'objective-c', 'obj-c'], words: 'objc', lex: cppLexer(0, CPP, [['lexer.cpp.allow.hashes', '1']]) },
  { aliases: ['dart'], words: 'dart', lex: cppLexer(0, CPP) },
  { aliases: ['scala'], words: 'scala', lex: cppLexer(0, CPP) },

  { aliases: ['py', 'python', 'python3'], words: 'python', lex: plain('python', [['lexer.python.identifier.attributes', '1']], CPP) },
  {
    aliases: ['sh', 'bash', 'zsh', 'shell', 'console', 'shellscript', 'dockerfile'],
    words: 'bash',
    lex: plain('bash', [], 'MarkCalls'),
  },
  { aliases: ['json', 'jsonc', 'json5'], words: 'json', lex: plain('json', [['lexer.json.allow.comments', '1']]) },
  { aliases: ['html', 'htm'], words: 'html', lex: plain('hypertext') },
  { aliases: ['xml', 'svg'], words: 'xml', lex: plain('xml') },
  { aliases: ['vue'], words: 'vue', lex: plain('xml') },
  { aliases: ['php'], words: 'php', lex: plain('xml') },
  { aliases: ['css', 'scss', 'less'], words: 'css', lex: plain('css') },
  { aliases: ['sql', 'postgres', 'postgresql', 'mysql', 'sqlite'], words: 'sql', lex: plain('sql', [], CPP) },
  { aliases: ['rs', 'rust'], words: 'rust', lex: plain('rust', [], 'MarkCalls|MarkTypes') },
  { aliases: ['yaml', 'yml'], words: 'yaml', lex: plain('yaml') },
  { aliases: ['rb', 'ruby'], words: 'ruby', lex: plain('ruby', [], 'MarkCalls|MarkTypes') },
  { aliases: ['diff', 'patch'], words: 'diff', lex: plain('diff') },
  { aliases: ['md', 'markdown'], words: 'markdown', lex: plain('markdown') },
  { aliases: ['lua'], words: 'lua', lex: plain('lua', [], CPP) },
  { aliases: ['toml'], words: 'toml', lex: plain('toml') },
];

const rows = [];
for (const { aliases, words, lex } of LANGUAGES) {
  const lists = words === '' ? ['', ''] : wordLists(words);
  for (const alias of aliases) {
    if (rows.some((row) => row.alias === alias)) {
      throw new Error(`duplicate alias ${alias}`);
    }
    rows.push({ alias, ...lex, lists });
  }
}
rows.sort((a, b) => (a.alias < b.alias ? -1 : a.alias > b.alias ? 1 : 0));

// --------------------------------------------------------------------- emit

const lexers = [...new Set(rows.map((row) => row.lexer))];
// Word lists and properties are per language, not per lexer: the C family
// shares one lexer but each language has its own keywords and its own idea of
// what a back-quoted string is.
const propertySets = new Map();
for (const row of rows) {
  const key = row.properties.flat().join('\u0000');
  if (!propertySets.has(key)) {
    const name = `kProperties${propertySets.size}`;
    propertySets.set(key, { name, flat: row.properties.flat() });
  }
  row.propertySet = propertySets.get(key).name;
}
const wordSets = new Map();
for (const row of rows) {
  const key = row.lists.join('\u0000');
  if (!wordSets.has(key)) {
    const name = `kWords${wordSets.size}`;
    wordSets.set(key, { name, lists: row.lists });
  }
  row.wordSet = wordSets.get(key).name;
}

const lines = [];
const push = (...text) => lines.push(...text);

push('// GENERATED FILE. Written by src/cpp/highlight/generate-tables.mjs.');
push('//');
push("// Keyword lists come from SciTE's .properties files, which carry the same");
push('// license as Lexilla. The style maps name SciLexer.h constants rather than');
push('// numbers, so the generator fails if the pinned Lexilla renames one.');
push('//');
push('//   node src/cpp/highlight/generate-tables.mjs --scite <SciTE src dir>');
push('');
push('#include "LanguageRegistry.h"');
push('');
push('#include <cstddef>');
push('');
push('namespace downstream {');
push('namespace {');
push('');

for (const set of propertySets.values()) {
  // A null terminator, not an empty string: the reader steps by two, so an
  // odd-length array would read past the end.
  const flat = set.flat.map(cString).concat('nullptr');
  push(`const char *const ${set.name}[] = {${flat.join(', ')}};`);
}
push('');
for (const set of wordSets.values()) {
  const names = set.lists.map(cString).concat('nullptr').join(', ');
  push(`const char *const ${set.name}[] = {${names}};`);
}
push('');
for (const lexer of lexers) {
  const flat = styleMaps[lexer].pairs.flatMap(([style, token]) => [String(style), `static_cast<int32_t>(TokenClass::${token})`]);
  push(`const int32_t kStyles${cap(lexer)}[] = {${flat.join(', ')}, -1};`);
}
push('');
push('const LanguageTable kLanguages[] = {');
for (const row of rows) {
  push(
    `    {${cString(row.alias)}, ${cString(row.lexer)}, ${row.wordSet}, ` +
      `${row.propertySet}, kStyles${cap(row.lexer)}, ` +
      `${row.marks}, ${styleMaps[row.lexer].identifier}},`
  );
}
push('};');
push('');
push('} // namespace');
push('');
push('const LanguageTable *languageTables(size_t *count) {');
push('  *count = sizeof(kLanguages) / sizeof(kLanguages[0]);');
push('  return kLanguages;');
push('}');
push('');
push('} // namespace downstream');
push('');

writeFileSync(out, lines.join('\n'));
console.log(
  `wrote ${out}: ${rows.length} aliases, ${lexers.length} lexers, ${wordSets.size} word sets`
);
