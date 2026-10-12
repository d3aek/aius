// openapi.test.js: the Aius OpenAPI contract. The document ships on the SD web
// tree (firmware/sd/www/openapi.json), so the device itself serves it at
// /sd/www/openapi.json; these assertions hold it to the firmware's serializers.

import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const firmwareRoot = resolve(repoRoot, "firmware");
const documentPath = resolve(firmwareRoot, "sd/www/openapi.json");

// ---------------------------------------------------------------------------
// Served-key extractor - the firmware is the ground truth
//
// Every JSON document the device serves is assembled from printf format-string
// literals in the firmware. The extractor below reads those literals out of the
// C++ source and rebuilds the key tree the device actually emits, so the schemas
// are checked against the device rather than against this document's older self:
// a field added to firmware without being documented here, and a documented
// field the firmware stopped serving, both fail this suite.
//
// It is deliberately strict - every step throws rather than quietly extracting a
// subset, because a subset would silently weaken the gate:
//
//   - comments are stripped by a scanner that respects string and char literals,
//     because firmware comments quote key names verbatim (optical_main.cpp's
//     status comment contains the text ",\"model_origin\":");
//   - a format argument that is not a run of adjacent string literals throws;
//   - a call that hands the response buffer (an argument named `capacity`) to a
//     function the emitter table does not model throws, so a new sub-object
//     serializer cannot join a document unnoticed;
//   - the assembled skeleton must parse with balanced braces and brackets, no
//     duplicate key at one path, no unrecognised value shape, and no content
//     after the document closes;
//   - each declared conditional-branch literal must be found exactly once per
//     invocation of the function that owns it.
// ---------------------------------------------------------------------------

/** Zero-based index of the printf format argument of each bounded append primitive. */
const APPEND_PRIMITIVES = new Map([
  ["snprintf", 2], // snprintf((char *)body, capacity, format, ...)
  ["http_json_append", 3], // http_json_append(body, capacity, length, format, ...)
  ["diag_append", 3], // aius_runtime_storage.cpp's own bounded append
  ["dsp_json_append", 3], // aius_optical_app.cpp's dsp.json serialiser append
  ["status_append", 3], // Phase 4f: the status render's append (http_json_append plus a yield point)
]);

/** Statement keywords a bare `identifier (` scan would otherwise read as calls. */
const CONTROL_KEYWORDS = new Set(["if", "for", "while", "switch", "return", "sizeof", "do", "catch", "defined"]);

/**
 * Every firmware function that appends to a response body, plus whatever the
 * extractor needs beyond a plain sequence of appends. A function reached through
 * the response buffer and absent from this table is an error, not a skip.
 */
const EMITTERS = {
  // GET /api/status. Since Phase 4f the head literal leaves the top-level
  // object open (no rewind) and the render's last append closes it.
  http_status_json: { file: "src/optical_main.cpp" },
  // GET /api/status/brief (Phase 4f F3), one snprintf.
  http_status_brief_json: { file: "src/optical_main.cpp" },
  http_net_diag_json: { file: "src/optical_main.cpp" },
  http_net_diag_generation: {
    file: "src/optical_main.cpp",
    // The object's key is a parameter, supplied as a literal by both call sites
    // ("first" and "current").
    keyFromArgument: 3,
    branchLiterals: [
      // `if (diag == nullptr) return ... ",\"%s\":null"` - the generation record
      // is served as null when it does not exist. The object branch is the one
      // that carries keys; NULLABLE_OBJECT_PATHS asserts the schema says null.
      ',"%s":null',
    ],
  },
  aius_runtime_storage_diag_json: {
    file: "src/aius_runtime_storage.cpp",
    // `if (failure->sequence == 0u) return ... "null}"` - no failure has been
    // latched, so last_failure is null and that literal also closes sd_diag.
    branchLiterals: ["null}"],
  },
  http_audio_diag_json: { file: "src/optical_main.cpp" },
  http_clock_json: { file: "src/optical_main.cpp" },
  http_clock_regs_json: { file: "src/optical_main.cpp" },
  // GET /api/status's input_diag object (pop diagnosis 2026-08-16): the SIS
  // error latch, production-gap watermark and update-ISR cadence watermark.
  http_input_diag_json: { file: "src/optical_main.cpp" },
  // GET /api/status's prev_boot object: the retained pv=/fa= forensics,
  // HTTP-readable since the 2026-08-16 slot-trial fallback.
  http_prev_boot_json: { file: "src/optical_main.cpp" },
  // GET /api/status's fault_record object: the core's retained crash record,
  // null when no valid record stands.
  http_fault_record_json: {
    file: "src/optical_main.cpp",
    branchLiterals: [',"fault_record":null'],
  },
  http_poll_probe_json: { file: "src/optical_main.cpp" },
  http_pipeline_diag_json: { file: "src/optical_main.cpp" },
  http_stall_diag_json: { file: "src/optical_main.cpp" },
  http_boot_fuse_json: { file: "src/optical_main.cpp" },
  // GET /api/status's ota_diag object (docs/plans/aius-ota-upgrade.md slice 3).
  // Behind #if AIUS_OTA in the firmware; the extractor is source-based and blind
  // to the preprocessor, so the schema documents it as the optical build serves
  // it (that env sets AIUS_OTA).
  http_ota_diag_json: { file: "src/optical_main.cpp" },
  // GET /api/status's ota_dark object (aius-ota-resilience.md Phase 1): the
  // retained dark-window forensic stamp, null when the retained slot carries
  // no valid stamp. Behind #if AIUS_OTA like ota_diag.
  http_ota_dark_json: {
    file: "src/optical_main.cpp",
    branchLiterals: [',"ota_dark":null'],
  },
  // GET /api/status's ab object (aius-ota-resilience.md Phase 3): the A/B
  // slot state, null on the single-image build. The non-null arm is behind
  // #if AIUS_AB_SLOT; the extractor is preprocessor-blind and documents the
  // slot builds' surface.
  http_ab_json: {
    file: "src/optical_main.cpp",
    branchLiterals: [',"ab":null'],
  },
  // GET /api/recorder. newest_frozen_slot is int|null across two appends
  // rather than one `? "%lu" : "null"` literal (a JSON null is not a printf
  // conversion), so the null arm is declared here and the schema is the only
  // place that null can be stated - the recorder test asserts it does.
  app_write_recorder: {
    file: "src/optical_main.cpp",
    branchLiterals: ['"newest_frozen_slot":null,'],
  },
  // The `slots` array within GET /api/recorder (aius-recorder-rolling-buffer.md
  // Phase C) - one static call site inside a 4-iteration loop, which is what
  // this extractor needs to see to validate the item shape (it does not model
  // the loop's runtime iteration count, same as http_dsp_eq_array below).
  recorder_slot_array: { file: "src/optical_main.cpp" },
  // One slot's `rate_change_frames` array - the same one-static-call-site-in-a-
  // loop shape as recorder_slot_array itself, so the item type is validated
  // once rather than the runtime position count.
  recorder_rate_change_array: { file: "src/optical_main.cpp" },
  // GET /api/dsp/config. Phase 4h: http_dsp_config_json hands the response buffer
  // to dsp_config_json, which hands it to the serialiser in aius_optical_app.cpp
  // (also the Q2 re-put's file form, which adds base_generation).
  http_dsp_config_json: { file: "src/optical_main.cpp" },
  dsp_config_json: { file: "src/optical_main.cpp" },
  aius_optical_dsp_config_write: {
    file: "src/aius_optical_app.cpp",
    // The file form only: the GET never serves base_generation, so the extractor
    // drops this branch and models the GET document.
    branchLiterals: [',"base_generation":%lu'],
  },
  dsp_eq_array: { file: "src/aius_optical_app.cpp" },
};

/** The three documents this gate covers, and the schema each one answers to. */
const DOCUMENTS = {
  status: { emitter: "http_status_json", schema: "Status" },
  statusBrief: { emitter: "http_status_brief_json", schema: "StatusBrief" },
  recorder: { emitter: "app_write_recorder", schema: "Recorder" },
  dspConfig: {
    emitter: "http_dsp_config_json",
    schema: "DspConfig",
    // http_dsp_config_json emits one loop iteration whose key is a `%s` filled
    // from its own channel_names table; the extractor reads that table out of
    // the firmware and applies the parsed entry shape to each of the six names.
    dynamicKeys: () => ({
      input_channels: firmwareStringArray("src/aius_optical_app.cpp", "aius_optical_dsp_config_write", "channel_names", 6),
    }),
  },
};

/**
 * Object paths the firmware serves as `null` on one branch (the branch literals
 * above). The extractor keeps the object branch, so the schema is the only place
 * the null can be stated - and it must state it.
 */
const NULLABLE_OBJECT_PATHS = [
  "net_diag.first",
  "net_diag.current",
  "sd_diag.last_failure",
  "ota_dark",
  "ab",
  "fault_record",
];

/**
 * Schema properties kept although the serializer does not emit them. Each entry
 * names the document path and why it is exempt (a genuinely conditional field, a
 * value the firmware only emits on a branch the extractor cannot reach, ...).
 * An empty table is the healthy state: the document says exactly what the device
 * says. Nothing in the three covered documents is conditional today.
 */
const SCHEMA_ONLY_EXCEPTIONS = new Map();

/** JSON type each printf conversion produces, by conversion character. */
const CONVERSION_TYPES = new Map([
  ["d", "integer"],
  ["i", "integer"],
  ["u", "integer"],
  ["o", "integer"],
  ["x", "integer"],
  ["X", "integer"],
  ["e", "number"],
  ["E", "number"],
  ["f", "number"],
  ["F", "number"],
  ["g", "number"],
  ["G", "number"],
  ["a", "number"],
  ["A", "number"],
  // A bare %s in value position is the firmware's boolean idiom
  // (`flag ? "true" : "false"`); a string value is written "%s", quoted.
  ["s", "boolean"],
]);

const CONVERSION = /^%[-+ #0]*[0-9*]*(?:\.[0-9*]+)?(?:hh|h|ll|l|L|j|z|t)?[diouxXeEfFgGaAsp]/;

const sources = new Map();

/** Read one firmware source with its comments stripped. */
function firmwareSource(relativePath) {
  if (!sources.has(relativePath)) {
    const text = readFileSync(resolve(firmwareRoot, relativePath), "utf8");
    sources.set(relativePath, stripComments(text, relativePath));
  }
  return sources.get(relativePath);
}

/** Index just past the closing quote of the string literal starting at `start`. */
function endOfStringLiteral(text, start, label) {
  let index = start + 1;
  while (index < text.length) {
    const character = text[index];
    if (character === "\\") {
      index += 2;
      continue;
    }
    if (character === '"') return index + 1;
    if (character === "\n") break;
    index++;
  }
  throw new Error(`${label}: unterminated string literal at offset ${start}`);
}

/** Strip C and C++ comments, preserving string and char literals verbatim. */
function stripComments(text, label) {
  let out = "";
  let index = 0;
  while (index < text.length) {
    const character = text[index];
    if (character === '"') {
      const end = endOfStringLiteral(text, index, label);
      out += text.slice(index, end);
      index = end;
      continue;
    }
    if (character === "'") {
      // A real char literal; anything else (a digit separator, say) is code.
      const literal = /^'(?:\\.|[^'\\])'/.exec(text.slice(index));
      if (literal) {
        out += literal[0];
        index += literal[0].length;
        continue;
      }
    }
    if (character === "/" && text[index + 1] === "/") {
      while (index < text.length && text[index] !== "\n") index++;
      continue;
    }
    if (character === "/" && text[index + 1] === "*") {
      const end = text.indexOf("*/", index + 2);
      if (end < 0) throw new Error(`${label}: unterminated block comment at offset ${index}`);
      for (const inner of text.slice(index, end + 2)) if (inner === "\n") out += "\n";
      index = end + 2;
      continue;
    }
    out += character;
    index++;
  }
  return out;
}

/** Index of the delimiter closing the one at `start`, skipping literals. */
function matchDelimiter(text, start, open, close, label) {
  let depth = 0;
  let index = start;
  while (index < text.length) {
    const character = text[index];
    if (character === '"') {
      index = endOfStringLiteral(text, index, label);
      continue;
    }
    if (character === "'") {
      const literal = /^'(?:\\.|[^'\\])'/.exec(text.slice(index));
      if (literal) {
        index += literal[0].length;
        continue;
      }
    }
    if (character === open) depth++;
    else if (character === close) {
      depth--;
      if (depth === 0) return index;
    }
    index++;
  }
  throw new Error(`${label}: unbalanced ${open}${close} from offset ${start}`);
}

/** The body of the single definition of `name`, without its outer braces. */
function functionBody(source, name, label) {
  const pattern = new RegExp(`\\b${name}\\s*\\(`, "g");
  const bodies = [];
  let match;
  while ((match = pattern.exec(source)) !== null) {
    const openParen = match.index + match[0].length - 1;
    const closeParen = matchDelimiter(source, openParen, "(", ")", label);
    let index = closeParen + 1;
    while (index < source.length && /\s/.test(source[index])) index++;
    if (source[index] !== "{") continue; // a call or a forward declaration
    const closeBrace = matchDelimiter(source, index, "{", "}", label);
    bodies.push(source.slice(index + 1, closeBrace));
  }
  if (bodies.length !== 1)
    throw new Error(`${label}: expected exactly one definition of ${name}(), found ${bodies.length}`);
  return bodies[0];
}

/** Split one argument list on its top-level commas. */
function splitArguments(text, label) {
  const args = [];
  let depth = 0;
  let start = 0;
  let index = 0;
  while (index < text.length) {
    const character = text[index];
    if (character === '"') {
      index = endOfStringLiteral(text, index, label);
      continue;
    }
    if (character === "'") {
      const literal = /^'(?:\\.|[^'\\])'/.exec(text.slice(index));
      if (literal) {
        index += literal[0].length;
        continue;
      }
    }
    if ("([{".includes(character)) depth++;
    else if (")]}".includes(character)) depth--;
    else if (character === "," && depth === 0) {
      args.push(text.slice(start, index));
      start = index + 1;
    }
    index++;
  }
  if (text.trim().length > 0) args.push(text.slice(start));
  return args;
}

/** Every `identifier(` call site in a function body, in source order. */
function callSites(body, label) {
  const calls = [];
  const pattern = /\b([A-Za-z_][A-Za-z0-9_]*)\s*\(/g;
  let match;
  while ((match = pattern.exec(body)) !== null) {
    if (CONTROL_KEYWORDS.has(match[1])) continue;
    const openParen = match.index + match[0].length - 1;
    const closeParen = matchDelimiter(body, openParen, "(", ")", label);
    calls.push({ name: match[1], args: splitArguments(body.slice(openParen + 1, closeParen), label) });
  }
  return calls;
}

/** Decode one C string literal's escapes; an unknown escape is an error. */
function decodeStringLiteral(literal, label) {
  const escapes = new Map([
    ['"', '"'],
    ["\\", "\\"],
    ["'", "'"],
    ["n", "\n"],
    ["t", "\t"],
    ["r", "\r"],
    ["0", "\0"],
  ]);
  let out = "";
  for (let index = 1; index < literal.length - 1; index++) {
    if (literal[index] !== "\\") {
      out += literal[index];
      continue;
    }
    index++;
    const decoded = escapes.get(literal[index]);
    if (decoded === undefined) throw new Error(`${label}: unsupported escape \\${literal[index]} in a format literal`);
    out += decoded;
  }
  return out;
}

/** The value of a run of adjacent string literals, or null if it is not one. */
function stringLiteralValue(text, label) {
  if (text === undefined) return null;
  let index = 0;
  let value = "";
  let literals = 0;
  for (;;) {
    while (index < text.length && /\s/.test(text[index])) index++;
    if (text[index] !== '"') break;
    const end = endOfStringLiteral(text, index, label);
    value += decodeStringLiteral(text.slice(index, end), label);
    literals++;
    index = end;
  }
  while (index < text.length && /\s/.test(text[index])) index++;
  return literals > 0 && index === text.length ? value : null;
}

/**
 * A format argument given by name rather than as a literal: the value of a
 * `static const char <name>[] PROGMEM = "..." "...";` declaration in the same
 * function body. PROGMEM keeps a large format string in flash instead of the
 * DTCM .data the framework copies every .rodata into (http_input_diag_json, the
 * dark-interval instrument). Anything else - a parameter, a table lookup, a
 * declaration without PROGMEM or outside the function - stays an error, so the
 * strictness of the literal rule is kept: every format is still a compile-time
 * string the extractor can read.
 */
function namedFormatLiteral(body, argument, label) {
  const name = /^\s*([A-Za-z_][A-Za-z0-9_]*)\s*$/.exec(argument ?? "");
  if (name === null) return null;
  const declaration = new RegExp(
    `\\bstatic\\s+const\\s+char\\s+${name[1]}\\s*\\[\\s*\\]\\s*PROGMEM\\s*=\\s*((?:"(?:\\\\.|[^"\\\\])*"\\s*)+);`,
  ).exec(body);
  if (declaration === null) return null;
  return stringLiteralValue(declaration[1], label);
}

/** A firmware `static const char *const name[n] = {...}` table, as strings. */
function firmwareStringArray(file, owner, name, expected) {
  const body = functionBody(firmwareSource(file), owner, file);
  const declaration = new RegExp(`\\b${name}\\s*\\[\\s*\\d*\\s*\\]\\s*=\\s*\\{([^}]*)\\}`).exec(body);
  if (declaration === null) throw new Error(`${file}: ${owner}() declares no ${name}[] table`);
  const values = [];
  const literal = /"(?:\\.|[^"\\])*"/g;
  let match;
  while ((match = literal.exec(declaration[1])) !== null) values.push(decodeStringLiteral(match[0], file));
  if (values.length !== expected)
    throw new Error(`${file}: ${name}[] holds ${values.length} entries, expected ${expected}`);
  return values;
}

/**
 * The JSON skeleton one emitter appends, assembled from its format literals in
 * source order and from the emitters it calls.
 */
function emittedText(name, call) {
  const spec = EMITTERS[name];
  if (spec === undefined) throw new Error(`no emitter table entry for ${name}()`);
  const label = spec.file;
  const body = functionBody(firmwareSource(spec.file), name, label);
  const chunks = [];
  for (const site of callSites(body, label)) {
    const formatIndex = APPEND_PRIMITIVES.get(site.name);
    if (formatIndex !== undefined) {
      const format =
        stringLiteralValue(site.args[formatIndex], label) ?? namedFormatLiteral(body, site.args[formatIndex], label);
      if (format === null)
        throw new Error(
          `${label}: ${name}() calls ${site.name}() with a format argument that is neither a string literal ` +
            "nor a `static const char <name>[] PROGMEM` literal declared in the same function",
        );
      chunks.push({ text: format, literal: true });
      continue;
    }
    if (Object.hasOwn(EMITTERS, site.name)) {
      chunks.push({ text: emittedText(site.name, site), literal: false });
      continue;
    }
    if (site.args.some((argument) => /\bcapacity\b/.test(argument)))
      throw new Error(
        `${label}: ${name}() passes the response buffer to ${site.name}(), which the emitter table does not model`,
      );
  }

  const declared = spec.branchLiterals ?? [];
  const matches = declared.map(() => 0);
  const kept = [];
  for (const chunk of chunks) {
    const branch = chunk.literal ? declared.indexOf(chunk.text) : -1;
    if (branch >= 0) {
      matches[branch]++;
      continue;
    }
    kept.push(chunk.text);
  }
  declared.forEach((literal, index) => {
    if (matches[index] !== 1)
      throw new Error(
        `${label}: ${name}() emits the declared branch literal ${JSON.stringify(literal)} ` +
          `${matches[index]} times, expected once`,
      );
  });

  if (spec.rewindFirstChunkTrailing !== undefined) {
    const trailing = spec.rewindFirstChunkTrailing;
    if (kept.length === 0 || !kept[0].endsWith(trailing))
      throw new Error(
        `${label}: ${name}()'s first literal no longer ends in ${JSON.stringify(trailing)}, ` +
          "so the modelled rewind is wrong",
      );
    kept[0] = kept[0].slice(0, -trailing.length);
  }

  let text = kept.join("");
  if (spec.keyFromArgument !== undefined) {
    const key = stringLiteralValue(call?.args[spec.keyFromArgument], label);
    if (key === null) throw new Error(`${label}: ${name}() was called with a non-literal key argument`);
    const placeholders = text.split('"%s":').length - 1;
    if (placeholders !== 1)
      throw new Error(`${label}: ${name}() has ${placeholders} dynamic key placeholders, expected exactly one`);
    text = text.replace('"%s":', `${JSON.stringify(key)}:`);
  }
  return text;
}

/** Parse an assembled skeleton into a key tree; every surprise throws. */
function parseKeyTree(text, dynamicKeys) {
  let index = 0;
  const fail = (message) => {
    const window = text.slice(Math.max(0, index - 60), index + 60);
    throw new Error(`served-key extractor: ${message} (offset ${index}, near ...${window}...)`);
  };
  const skipSpace = () => {
    while (index < text.length && /\s/.test(text[index])) index++;
  };
  const readQuoted = () => {
    const end = endOfStringLiteral(text, index, "assembled document");
    const value = decodeStringLiteral(text.slice(index, end), "assembled document");
    index = end;
    return value;
  };

  const parseValue = (path) => {
    skipSpace();
    const character = text[index];
    if (character === "{") return parseObject(path);
    if (character === "[") return parseArray(path);
    if (character === '"') {
      readQuoted();
      return { kind: "string" };
    }
    if (text.startsWith("true", index) || text.startsWith("false", index)) {
      index += text.startsWith("true", index) ? 4 : 5;
      return { kind: "boolean" };
    }
    if (text.startsWith("null", index)) {
      index += 4;
      return { kind: "null" };
    }
    const conversion = CONVERSION.exec(text.slice(index));
    if (conversion) {
      index += conversion[0].length;
      const kind = CONVERSION_TYPES.get(conversion[0].at(-1));
      if (kind === undefined) fail(`unsupported conversion ${conversion[0]} for ${path}`);
      // `cond ? "%lu" : "null"` concatenates to `%lunull`: a nullable scalar.
      let nullable = false;
      if (text.startsWith("null", index)) {
        index += 4;
        nullable = true;
      }
      return { kind, nullable };
    }
    const number = /^-?\d+(?:\.\d+)?/.exec(text.slice(index));
    if (number) {
      index += number[0].length;
      return { kind: number[0].includes(".") ? "number" : "integer" };
    }
    return fail(`unrecognised value shape for ${path || "the document root"}`);
  };

  const parseObject = (path) => {
    index++;
    const properties = new Map();
    for (;;) {
      skipSpace();
      if (index >= text.length) fail(`unterminated object at ${path || "the document root"}`);
      const character = text[index];
      if (character === "}") {
        index++;
        return { kind: "object", properties };
      }
      if (character === ",") {
        index++;
        continue;
      }
      // The `index == 0 ? "" : ","` separator idiom, emitted as a bare %s.
      if (text.startsWith("%s", index)) {
        index += 2;
        continue;
      }
      if (character !== '"') fail(`unrecognised token in the object at ${path || "the document root"}`);
      const key = readQuoted();
      skipSpace();
      if (text[index] !== ":") fail(`expected ':' after the key "${key}"`);
      index++;
      let names = [key];
      if (key === "%s") {
        names = dynamicKeys[path];
        if (names === undefined) fail(`dynamic key at ${path || "the document root"} with no declared expansion`);
      }
      const child = parseValue(path ? `${path}.${names[0]}` : names[0]);
      for (const name of names) {
        if (properties.has(name)) fail(`duplicate key ${name} in the object at ${path || "the document root"}`);
        properties.set(name, child);
      }
    }
  };

  const parseArray = (path) => {
    index++;
    let items = null;
    for (;;) {
      skipSpace();
      if (index >= text.length) fail(`unterminated array at ${path}`);
      const character = text[index];
      if (character === "]") {
        index++;
        return { kind: "array", items };
      }
      if (character === "," || text.startsWith("%s", index)) {
        index += character === "," ? 1 : 2;
        continue;
      }
      const value = parseValue(`${path}[]`);
      if (items === null) items = value;
      else if (shapeSignature(items) !== shapeSignature(value))
        fail(`the array at ${path} emits two different item shapes`);
    }
  };

  const root = parseValue("");
  skipSpace();
  if (index !== text.length) fail("content after the document closed");
  if (root.kind !== "object") fail("the document root is not an object");
  return root;
}

/** A comparable signature of one parsed node's shape. */
function shapeSignature(node) {
  if (node.kind === "object")
    return `{${[...node.properties].map(([key, child]) => `${key}:${shapeSignature(child)}`).join(",")}}`;
  if (node.kind === "array") return `[${node.items === null ? "" : shapeSignature(node.items)}]`;
  return node.nullable ? `${node.kind}?` : node.kind;
}

/** The key tree one document actually serves. */
function servedDocument(name) {
  const spec = DOCUMENTS[name];
  return parseKeyTree(emittedText(spec.emitter), spec.dynamicKeys ? spec.dynamicKeys() : {});
}

/** Follow $ref / single-member allOf, collecting nullability on the way. */
function resolveSchema(document, node, path) {
  let current = node;
  let nullable = false;
  for (let hops = 0; hops < 8; hops++) {
    if (current === undefined) throw new Error(`swagger: no schema at ${path}`);
    if (current.nullable === true) nullable = true;
    if (typeof current.$ref === "string") {
      const name = current.$ref.replace("#/components/schemas/", "");
      current = document.components.schemas[name];
      continue;
    }
    if (Array.isArray(current.allOf) && current.allOf.length === 1) {
      current = current.allOf[0];
      continue;
    }
    return { schema: current, nullable };
  }
  throw new Error(`swagger: $ref cycle at ${path}`);
}

/** Compare one served key tree against its schema, both directions. */
function compareWithSchema(document, served, schemaName) {
  const servedButUndocumented = [];
  const documentedButNotServed = [];
  const typeMismatches = [];

  const walk = (node, schemaNode, path) => {
    const { schema, nullable } = resolveSchema(document, schemaNode, path || schemaName);
    if (node.kind === "object") {
      const properties = schema.properties ?? {};
      if (schema.properties === undefined)
        typeMismatches.push(`${path}: firmware serves an object, schema declares no properties`);
      for (const [key, child] of node.properties) {
        const childPath = path ? `${path}.${key}` : key;
        if (!Object.hasOwn(properties, key)) {
          servedButUndocumented.push(childPath);
          continue;
        }
        walk(child, properties[key], childPath);
      }
      for (const key of Object.keys(properties)) {
        const childPath = path ? `${path}.${key}` : key;
        if (!node.properties.has(key) && !SCHEMA_ONLY_EXCEPTIONS.has(childPath)) documentedButNotServed.push(childPath);
      }
      return;
    }
    if (node.kind === "array") {
      if (schema.type !== "array") typeMismatches.push(`${path}: firmware serves an array, schema says ${schema.type}`);
      if (node.items !== null && schema.items !== undefined) walk(node.items, schema.items, `${path}[]`);
      return;
    }
    const declared = Array.isArray(schema.type) ? schema.type : [schema.type];
    if (!declared.includes(node.kind))
      typeMismatches.push(`${path}: firmware emits ${node.kind}, schema says ${declared.join("|")}`);
    if (node.nullable && !nullable && !declared.includes("null"))
      typeMismatches.push(`${path}: firmware can emit null, schema is not nullable`);
  };

  walk(served, { $ref: `#/components/schemas/${schemaName}` }, "");
  return {
    servedButUndocumented: servedButUndocumented.sort(),
    documentedButNotServed: documentedButNotServed.sort(),
    typeMismatches: typeMismatches.sort(),
  };
}

const NO_DRIFT = { servedButUndocumented: [], documentedButNotServed: [], typeMismatches: [] };

/** The schema node at a dotted served-key path, for the nullability checks. */
function schemaAtPath(document, schemaName, path) {
  let node = { $ref: `#/components/schemas/${schemaName}` };
  let resolved = resolveSchema(document, node, schemaName);
  for (const key of path.split(".")) {
    node = resolved.schema.properties[key];
    if (node === undefined) throw new Error(`swagger: ${schemaName} has no property at ${path}`);
    resolved = resolveSchema(document, node, path);
  }
  return resolved;
}

describe("Aius OpenAPI contract", () => {
  it("describes every device control route", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    expect(document.openapi).toBe("3.0.3");
    expect(Object.keys(document.paths).sort()).toEqual([
      "/api/clear",
      "/api/config/public",
      "/api/dsp/config",
      "/api/events",
      "/api/measure",
      "/api/measure/start",
      "/api/meters",
      "/api/ota",
      "/api/ota/abort",
      "/api/ota/activate",
      "/api/ota/arm",
      "/api/ota/image",
      "/api/ota/verify",
      "/api/reboot",
      "/api/recorder",
      "/api/recorder/config",
      "/api/recorder/data",
      "/api/recorder/freeze",
      "/api/recorder/thaw",
      "/api/sd",
      "/api/sd/mode",
      "/api/start",
      "/api/status",
      "/api/status/brief",
      "/api/stop",
      "/sd/{directory}/",
      "/sd/{resource}",
    ]);
    // The single-config model retires mode/mix selection entirely.
    expect(document.paths["/api/mode"]).toBeUndefined();
    expect(document.paths["/api/mix"]).toBeUndefined();
    expect(document.components.schemas.ModeRequest).toBeUndefined();
    expect(document.components.schemas.MixRequest).toBeUndefined();
    expect(document.components.schemas.DspProfile).toBeUndefined();
    expect(document.components.schemas.DspMix).toBeUndefined();
    expect(document.components.schemas.DspActive).toBeUndefined();

    // GET /api/status exposes every AiusRecoveryStatus field, the same set the
    // 99-byte UDP status payload and the USB ID line carry.
    const status = document.components.schemas.Status.properties;
    expect(
      Object.keys(status).filter((key) =>
        [
          "network_state",
          "recovery_safe",
          "network_last_fault",
          "last_checkpoint",
          "network_generation",
          "boot_generation",
          "reset_source",
          "watchdog_early_resets",
          "loop_epoch",
          "network_restarts",
        ].includes(key),
      ).length,
    ).toBe(10);
    // network_mode is a two-word vocabulary, and both words are reachable: the
    // firmware derives the string from byte 1 of the UDP status payload, whose
    // encoder refuses anything above 1, and static addressing was removed.
    expect(status.network_mode.enum).toEqual(["offline", "dhcp"]);
    expect(status.network_mode.description).toContain("static");
    // Wire-honest types (corrected 2026-07-27): the serializer emits integers,
    // 0|1 flags included; these were typed string/boolean and a schema-coded
    // client would have broken on first contact.
    for (const field of ["carrier", "sd_present", "config_valid", "link"]) {
      expect(status[field].enum).toEqual([0, 1]);
    }
    expect(status.state.enum).toEqual([2, 3, 4]);
    expect(status.format.type).toBe("integer");
    // The volume type comes from the SD probe, so "unknown" means an unnamed
    // mounted volume rather than "not produced".
    expect(status.sd_filesystem.enum).toEqual(["none", "fat12", "fat16", "fat32", "unknown"]);
    expect(status.sd_filesystem.description).toContain("mounted");

    // The domains the firmware's UDP encoder validates before it will emit a payload.
    expect(status.network_state.maximum).toBe(5);
    expect(status.network_last_fault.maximum).toBe(10);
    // 10, not 9: the audio checkpoint sits at 9 and only 10 means the epoch
    // completed (aius_boot.h, output-jitter Phase B). aius_net.c bounds the
    // encoded field against AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE = 10.
    expect(status.last_checkpoint.maximum).toBe(10);
    expect(status.watchdog_early_resets.maximum).toBe(255);
    expect(status.recovery_safe.enum).toEqual([0, 1]);
    // recovery_safe latches on the second early watchdog reset; last_checkpoint names the stall.
    expect(status.recovery_safe.description).toContain("two consecutive early watchdog resets");
    expect(status.last_checkpoint.description).toContain("9 audio, 10 epoch-complete");
    expect(status.watchdog_early_resets.description).toContain("recovery-safe");
    // Unbounded counters stay integers with a floor and no ceiling.
    for (const field of ["network_generation", "boot_generation", "reset_source", "loop_epoch", "network_restarts"]) {
      expect(status[field].type).toBe("integer");
      expect(status[field].minimum).toBe(0);
      expect(status[field].maximum).toBeUndefined();
    }

    // The optical transmit gate: latched decision + latched inputs, policy
    // open iff carrier_locked, with the boot-blip caveat stated where a client
    // will read it.
    expect(status.tx_gate).toEqual({ $ref: "#/components/schemas/TxGate" });
    const txGate = document.components.schemas.TxGate;
    expect(txGate.additionalProperties).toBe(false);
    expect(txGate.required).toEqual(["open", "carrier_locked"]);
    for (const field of ["open", "carrier_locked"]) {
      expect(txGate.properties[field].enum).toEqual([0, 1]);
    }
    expect(txGate.properties.replay_running).toBeUndefined();
    expect(txGate.description).toContain("latched");
    expect(txGate.description).toContain("blip");

    // The network measurement surface. Its whole purpose is to distinguish
    // readings that look alike, so the document has to keep those distinctions
    // legible: the cached link member beside a fresh read, the two ways a
    // generation's beginNoWait() can behave, and the frame counter that says
    // whether a DISCOVER ever reached the MAC.
    expect(status.net_diag).toEqual({ $ref: "#/components/schemas/NetDiag" });
    const netDiag = document.components.schemas.NetDiag;
    expect(netDiag.additionalProperties).toBe(false);
    expect(netDiag.required).toEqual([
      "link",
      "link_live",
      "dhcp_state",
      "frames_valid",
      "tx_frames",
      "rx_frames",
      "dhcp_schedule_valid",
      "dhcp_lease_s",
      "dhcp_t1_s",
      "dhcp_t2_s",
      "fnet_heap_valid",
      "fnet_heap_free_b",
      "fnet_heap_largest_b",
      "first",
      "current",
    ]);
    // The DHCP schedule is served from the client's own option values; the
    // reopened periodic-WDOG second death mode is why reconstruction from
    // cadence is not acceptable evidence (open-work register).
    expect(netDiag.properties.dhcp_schedule_valid.type).toBe("boolean");
    expect(netDiag.properties.dhcp_t1_s.description).toContain("T1");
    expect(netDiag.properties.dhcp_t2_s.description).toContain("T2");
    expect(netDiag.description).toContain("Diagnostic only");
    // link is the cached member; link_live asks the interface. Same encoding,
    // different evidence, and the document must say which is which.
    expect(netDiag.properties.link.enum).toEqual([0, 1, 2]);
    expect(netDiag.properties.link_live.enum).toEqual([0, 1, 2]);
    expect(netDiag.properties.link.description).toContain("cached");
    expect(netDiag.properties.link_live.description).toContain("fresh");
    // FNET's DHCP client states are a private enum in the pinned library, so
    // this document is where the mapping lives.
    expect(netDiag.properties.dhcp_state.enum).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 255]);
    expect(netDiag.properties.dhcp_state.description).toContain("selecting");
    expect(netDiag.properties.dhcp_state.description).toContain("255");
    // Both generation records are nullable: neither is guaranteed to exist.
    for (const field of ["first", "current"]) {
      expect(netDiag.properties[field].nullable).toBe(true);
      expect(netDiag.properties[field].allOf).toEqual([{ $ref: "#/components/schemas/NetDiagGeneration" }]);
    }
    expect(netDiag.properties.first.description).toContain("retained");

    const generation = document.components.schemas.NetDiagGeneration;
    expect(generation.additionalProperties).toBe(false);
    expect(Object.keys(generation.properties).sort()).toEqual([...generation.required].sort());
    // The three-way netif_init answer: performed, skipped, or not probed.
    expect(generation.properties.netif_init.enum).toEqual([0, 1, 2]);
    expect(generation.properties.netif_init.description).toContain("skipped");
    // beginNoWait() blocks on the generation that initialises the interface.
    expect(generation.properties.stack_start_blocking_ms.maximum).toBe(65535);
    expect(generation.properties.stack_start_blocking_ms.description).toContain("2000");
    // Four observers of link-up, only one of which is an actual observation.
    expect(generation.properties.link_up_source.enum).toEqual([0, 1, 2, 3]);
    expect(generation.properties.link_up_source.description).toContain("snapshot");
    expect(generation.properties.end_reason.enum).toEqual([0, 1, 2]);
    // A measurement that did not happen is null, never 0.
    for (const field of ["link_up_ms", "link_callback_up_ms", "dhcp_start_ms", "end_ms", "tx_frames", "rx_frames"]) {
      expect(generation.properties[field].type).toBe("integer");
      expect(generation.properties[field].minimum).toBe(0);
      expect(generation.properties[field].nullable).toBe(true);
    }
    expect(generation.description).toContain("null rather than 0");
    // tx_frames over the DHCP window is the frame-level discriminator.
    expect(generation.properties.tx_frames.description).toContain("no DISCOVER was ever handed to the MAC");

    expect(document.paths["/api/config/public"].put.requestBody).toBeTruthy();
    expect(document.components.parameters.SdAuth.schema.pattern).toBe("^[0-9a-fA-F]{16}$");
    expect(document.paths["/api/dsp/config"].get.responses["200"]).toBeTruthy();
    expect(document.components.schemas.DspConfig.required).toEqual([
      "source",
      "config_generation",
      "sample_rate_hz",
      "features",
      "input_channels",
      "output_eq",
      "matrix",
      "limiter",
    ]);
    expect(document.components.schemas.DspConfig.additionalProperties).toBe(false);
    // staged_generation and config_pending are served on every GET but stay out
    // of `required`, because DspConfigUpdate derives its key set from it and the
    // PUT parser answers UNKNOWN_KEY for any top-level key outside its nine.
    expect(document.components.schemas.DspConfig.properties.staged_generation.type).toBe("integer");
    expect(document.components.schemas.DspConfig.properties.config_pending.type).toBe("boolean");
    expect(document.components.schemas.DspConfig.properties.config_pending.description).toContain("STAGED");
    for (const key of ["staged_generation", "config_pending"]) {
      expect(document.components.schemas.DspConfigUpdate.properties[key]).toBeUndefined();
    }
    expect(document.components.schemas.DspConfig.properties.source.enum).toEqual(["default", "sd"]);
    expect(document.components.schemas.DspConfig.properties.output_eq.maxItems).toBe(2);
    expect(document.components.schemas.DspFeatures.required).toEqual([
      "input_eq",
      "matrix",
      "output_eq",
      "limiter",
      "meters",
    ]);
    expect(document.components.schemas.DspInputChannels.required).toEqual(["fl", "fr", "fc", "lfe", "sl", "sr"]);
    expect(document.components.schemas.DspInputChannel.properties.eq.maxItems).toBe(2);
    expect(document.components.schemas.DspInputChannel.properties.gain_db.minimum).toBe(-60);
    expect(document.components.schemas.DspInputChannel.properties.gain_db.maximum).toBe(12);
    expect(document.components.schemas.DspEqSection.properties.type.enum).toEqual(["bypass", "highpass", "peaking"]);
    expect(document.components.schemas.DspMatrix.required).toEqual([
      "center_gain_linear",
      "surround_gain_linear",
      "lfe_gain_linear",
      "lfe_crossover_hz",
      "headroom_linear",
      "clipfree",
    ]);
    expect(document.components.schemas.DspMatrix.properties.center_gain_linear.description).toContain("dialogue");
    expect(document.components.schemas.DspMatrix.properties.surround_gain_linear.description).toContain("rear-channel");
    for (const field of ["center_gain_linear", "surround_gain_linear", "lfe_gain_linear"]) {
      expect(document.components.schemas.DspMatrix.properties[field].minimum).toBe(0);
      expect(document.components.schemas.DspMatrix.properties[field].maximum).toBe(4);
    }
    expect(document.components.schemas.DspMatrix.properties.headroom_linear.exclusiveMinimum).toBe(true);
    // The LR4 fold-in corner; 0 is deliberately outside the range (no-LFE is lfe_gain_linear = 0).
    expect(document.components.schemas.DspMatrix.properties.lfe_crossover_hz.minimum).toBe(20);
    expect(document.components.schemas.DspMatrix.properties.lfe_crossover_hz.maximum).toBe(500);
    expect(document.components.schemas.DspLimiter.required).toEqual([
      "enabled",
      "threshold_dbfs",
      "attack_ms",
      "release_ms",
      "lookahead_ms",
    ]);
    expect(document.components.schemas.DspLimiter.properties.threshold_dbfs.minimum).toBe(-24);
    expect(document.components.schemas.DspLimiter.properties.threshold_dbfs.maximum).toBe(0);
    expect(document.components.schemas.DspLimiter.properties.lookahead_ms.maximum).toBe(5);

    const example = document.paths["/api/dsp/config"].get.responses["200"].content["application/json"].examples.current;
    expect(example).toBeTruthy();
    expect(example.value.source).toBe("default");
    expect(example.value.config_generation).toBe(0);
    expect(Object.keys(example.value.input_channels).sort()).toEqual(["fc", "fl", "fr", "lfe", "sl", "sr"]);
    // The example IS the factory model: pinned to the sd-editor default fixture,
    // which is itself pinned to firmware/config/optical_dsp.json. Closes the
    // third copy of the factory values that drifted silently before 5449fb6d9.
    const factoryFixture = JSON.parse(
      readFileSync(
        resolve(repoRoot, "test/sd-editor/fixtures/dsp-config-default.json"),
        "utf8",
      ),
    );
    expect(example.value).toEqual(factoryFixture);
    expect(document.paths["/api/reboot"].post.description).toContain("Acknowledges before reboot");

    // PUT /api/dsp/config: authenticated, bounded, optimistically concurrent.
    const put = document.paths["/api/dsp/config"].put;
    expect(put.operationId).toBe("putDspConfig");
    expect(put.parameters).toEqual([{ $ref: "#/components/parameters/SdAuth" }]);
    expect(put.requestBody).toEqual({ $ref: "#/components/requestBodies/DspConfigUpdate" });
    expect(put.description).toContain("base_generation");
    expect(put.description).toContain("next audio block boundary");

    const body = document.components.requestBodies.DspConfigUpdate;
    expect(body.required).toBe(true);
    // The parser reads the body out of the fixed request buffer, so the cap is a hard reject.
    expect(body["x-max-body-bytes"]).toBe(12288);
    expect(body.description).toContain("12,288 bytes");
    expect(body.content["application/json"].schema).toEqual({ $ref: "#/components/schemas/DspConfigUpdate" });

    // The request adds base_generation to the eight keys the GET returns; all nine are required.
    const update = document.components.schemas.DspConfigUpdate;
    expect(update.required).toEqual([...document.components.schemas.DspConfig.required, "base_generation"]);
    expect(update.additionalProperties).toBe(false);
    // source is narrowed to the SD override; a PUT never writes the factory default.
    expect(update.properties.source.enum).toEqual(["sd"]);
    expect(update.properties.base_generation).toEqual({
      type: "integer",
      minimum: 0,
      description:
        "Optimistic-concurrency token: the config_generation the edit was based on. " +
        "A value other than the device's current config_generation returns 409 and touches nothing.",
    });
    expect(update.properties.config_generation.description).toContain("not otherwise validated");
    expect(update.properties.sample_rate_hz.description).toContain("hertz");
    // The shared sub-schemas are reused, not duplicated.
    for (const field of ["features", "input_channels", "matrix", "limiter"]) {
      expect(update.properties[field]).toEqual(document.components.schemas.DspConfig.properties[field]);
    }
    expect(update.properties.output_eq.items).toEqual({ $ref: "#/components/schemas/DspEqSection" });
    expect(update.properties.output_eq.maxItems).toBe(2);
    // Firmware accepts an EQ section carrying only "type"; the schema is deliberately stricter.
    expect(update.description).toContain("bypass");

    const request = body.content["application/json"].examples.edit;
    expect(request.value.source).toBe("sd");
    expect(request.value.base_generation).toBe(request.value.config_generation);
    expect(Object.keys(request.value.input_channels).sort()).toEqual(["fc", "fl", "fr", "lfe", "sl", "sr"]);
    expect(request.value.output_eq).toHaveLength(1);
    expect(request.value.matrix.clipfree).toBe(true);
    // clipfree headroom is the resolved 1 / (1 + centre + surround + |lfe|).
    const { center_gain_linear: centre, surround_gain_linear: surround, lfe_gain_linear: lfe } = request.value.matrix;
    expect(request.value.matrix.headroom_linear).toBeCloseTo(1 / (1 + centre + surround + Math.abs(lfe)), 12);

    // Every status the route can actually produce, including the asynchronous SD-job failures.
    expect(Object.keys(put.responses).sort()).toEqual(["200", "400", "401", "409", "413", "500", "503", "507"]);
    expect(put.responses["200"].content["application/json"].schema).toEqual({
      $ref: "#/components/schemas/Ack",
    });
    expect(put.responses["200"].description).toContain("staged");
    const errorExamples = (status) => Object.keys(put.responses[status].content["application/json"].examples);
    expect(errorExamples("400")).toEqual(["dsp_config_invalid"]);
    expect(errorExamples("401")).toEqual(["sd_auth"]);
    expect(errorExamples("409")).toEqual(["stale_generation"]);
    expect(errorExamples("413")).toEqual(["request_too_large", "body_too_large"]);
    expect(errorExamples("500")).toEqual(["sd_commit"]);
    expect(errorExamples("503")).toEqual(["sd_unavailable", "sd_busy", "audio_busy"]);
    expect(errorExamples("507")).toEqual(["sd_write"]);
    for (const status of ["400", "401", "409", "413", "503"]) {
      expect(put.responses[status].content["application/json"].schema).toEqual({
        $ref: "#/components/schemas/Error",
      });
      for (const [name, sample] of Object.entries(put.responses[status].content["application/json"].examples)) {
        expect(sample.value).toEqual({ ok: false, error: name });
      }
    }
    // Both SD-job failures carry the byte counters the job reached.
    expect(document.components.schemas.SdJobError.required).toEqual(["ok", "error", "received", "written", "expected"]);
    expect(document.components.schemas.SdJobError.additionalProperties).toBe(false);
    expect(document.components.schemas.SdJobError.properties.error.enum).toEqual(["sd_write", "sd_commit"]);
    for (const status of ["500", "507"]) {
      expect(put.responses[status].content["application/json"].schema).toEqual({
        $ref: "#/components/schemas/SdJobError",
      });
      const [sample] = Object.values(put.responses[status].content["application/json"].examples);
      expect(Object.keys(sample.value).sort()).toEqual(["error", "expected", "ok", "received", "written"]);
    }
    // 409 is the stale-generation contract; 400 collapses every parser code to one string.
    expect(put.responses["409"].description).toContain("config_generation");
    expect(put.responses["400"].description).toContain("12,288 bytes");
    expect(put.responses["401"].description).toContain("X-Aius-SD-Auth");
  });

  it("describes the measurement loopback surface as the firmware serves it", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));

    // The public read: no auth parameter, no body, memory only.
    const get = document.paths["/api/measure"].get;
    expect(get.operationId).toBe("getMeasure");
    expect(get.parameters).toBeUndefined();
    expect(get.requestBody).toBeUndefined();
    expect(Object.keys(get.responses).sort()).toEqual(["200", "500"]);

    // The session schema pins the state machine's whole vocabulary and both
    // fixed geometries, so a firmware rename cannot slip past the contract.
    const measure = document.components.schemas.Measure;
    expect(measure.additionalProperties).toBe(false);
    expect(measure.properties.state.enum).toEqual([
      "idle",
      "armed",
      "capturing",
      "captured",
      "analysing",
      "done",
      "cancelling",
      "failed",
    ]);
    expect(measure.properties.fail_reason.enum).toEqual(["none", "generation", "overrun", "cancelled", "reclaimed"]);
    expect(measure.properties.capture_frames.const).toBe(16384);
    expect(measure.properties.window_frames.const).toBe(8192);
    // captured means buffer-complete, not figures-published.
    expect(measure.description).toContain("buffer-complete");

    // The floors are the 16-bit stimulus's, stated beside the figures, and the
    // -140 dB inter-stage claim stays host-proven (deliberately absent).
    const floors = measure.properties.floors;
    expect(floors.properties.thd_n_db.const).toBe(-96.0);
    expect(floors.properties.snr_db.const).toBe(96.0);
    expect(floors.properties.thd_dbc.const).toBe(-95.0);
    expect(floors.description).toContain("host-proven");

    // Per-channel figures: the fit/Goertzel split and the definitional
    // THD+N-vs-SNR difference are documented properties, not lore.
    const figures = document.components.schemas.MeasureFigures;
    expect(figures.description).toContain("1057");
    expect(figures.description).toContain("different numbers by definition");
    expect(figures.properties.harmonics_dbc.maxItems).toBe(9);
    expect(figures.properties.probes_dbfs.maxItems).toBe(8);

    // The arm: auth-gated (it joined the control-route gate), id required,
    // both fit frequencies defaulting to the corpus baseline, probes capped
    // at the analysis's own bin budget.
    const post = document.paths["/api/measure/start"].post;
    expect(post.operationId).toBe("startMeasure");
    expect(post.parameters).toEqual([{ $ref: "#/components/parameters/SdAuth" }]);
    const start = document.components.schemas.MeasureStartRequest;
    expect(start.required).toEqual(["id"]);
    expect(start.additionalProperties).toBe(false);
    expect(start.properties.f0_l.default).toBe(440);
    expect(start.properties.f0_r.default).toBe(440);
    expect(start.properties.probes.maxItems).toBe(8);
    expect(Object.keys(post.responses).sort()).toEqual(["200", "400", "401", "409"]);
    // Render-then-arm is the documented operator order (the U24 stimulus runs
    // first), with the epoch-side timeout bounds that make it safe.
    expect(post.description).toContain("U24");
    expect(post.description).toContain("10 s");
    expect(post.description).not.toMatch(/replay/i);
    expect(post.description).toContain("X-Aius-SD-Auth");
  });

  it("describes GET /api/meters with the domains the firmware actually emits", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const get = document.paths["/api/meters"].get;
    expect(get.operationId).toBe("getMeters");
    // A diagnostic read: no auth parameter, no request body, and only the
    // statuses http_meters_json() plus the dispatcher can produce, with the
    // Phase 4f admission shed (503 audio_busy).
    expect(get.parameters).toBeUndefined();
    expect(get.requestBody).toBeUndefined();
    expect(Object.keys(get.responses).sort()).toEqual(["200", "500", "503"]);
    expect(get.responses["200"].content["application/json"].schema).toEqual({
      $ref: "#/components/schemas/Meters",
    });
    expect(get.responses["500"].content["application/json"].schema).toEqual({
      $ref: "#/components/schemas/Error",
    });
    // The 500 is a contended seqlock read, never "meters unavailable".
    expect(Object.keys(get.responses["500"].content["application/json"].examples)).toEqual(["response_too_large"]);
    expect(get.responses["500"].description).toContain("torn");
    // sequence 0 is a valid 200, not a status-code special case.
    expect(get.responses["200"].description).toContain("sequence of 0 is a valid 200");

    const meters = document.components.schemas.Meters;
    expect(meters.additionalProperties).toBe(false);
    expect(meters.required).toEqual([
      "sequence",
      "sample_rate_hz",
      "input",
      "output",
      "limiter_gain_reduction_db",
      "waveform",
      "queue_frames",
      "estimated_latency_us",
      "dsp_cycles_last",
      "dsp_cycles_max",
    ]);
    expect(meters.properties.sequence.description).toContain("never published");

    // Six positional inputs in AIUS_CH_* order, with no per-entry channel key.
    expect(meters.properties.input.minItems).toBe(6);
    expect(meters.properties.input.maxItems).toBe(6);
    expect(meters.properties.input.items).toEqual({ $ref: "#/components/schemas/MeterLevel" });
    expect(meters.properties.input.description).toContain("fl, fr, fc, lfe, sl, sr");
    expect(document.components.schemas.MeterLevel.required).toEqual(["peak_dbfs", "rms_dbfs"]);
    expect(document.components.schemas.MeterLevel.additionalProperties).toBe(false);

    // Levels are dBFS floored at -120, so every emitted number is finite.
    for (const field of ["peak_dbfs", "rms_dbfs"]) {
      expect(document.components.schemas.MeterLevel.properties[field].type).toBe("number");
      expect(document.components.schemas.MeterLevel.properties[field].minimum).toBe(-120);
    }
    expect(meters.description).toContain("-120");

    // The stereo output keeps both channels as two-element [L, R] arrays.
    expect(meters.properties.output).toEqual({ $ref: "#/components/schemas/MeterOutput" });
    for (const field of ["peak_dbfs", "rms_dbfs"]) {
      const values = document.components.schemas.MeterOutput.properties[field];
      expect(values.type).toBe("array");
      expect(values.minItems).toBe(2);
      expect(values.maxItems).toBe(2);
      expect(values.items).toEqual({ type: "number", minimum: -120 });
    }

    // Waveform points are per-mille integers 0..1000, capped at 128 points.
    expect(meters.properties.waveform.maxItems).toBe(128);
    expect(meters.properties.waveform.items).toEqual({ type: "integer", minimum: 0, maximum: 1000 });
    expect(meters.properties.waveform.description).toContain("per-mille");

    // limiter_gain_reduction_db passes through unconverted, so it keeps its own domain.
    expect(meters.properties.limiter_gain_reduction_db.maximum).toBe(0);
    expect(meters.properties.limiter_gain_reduction_db.minimum).toBe(-1000);

    // The four target-adapter telemetry fields are unbounded non-negative counters.
    for (const field of ["queue_frames", "estimated_latency_us", "dsp_cycles_last", "dsp_cycles_max"]) {
      expect(meters.properties[field].type).toBe("integer");
      expect(meters.properties[field].minimum).toBe(0);
      expect(meters.properties[field].maximum).toBeUndefined();
    }

    // Both examples are valid against the shape they document.
    const examples = get.responses["200"].content["application/json"].examples;
    expect(Object.keys(examples).sort()).toEqual(["live", "never_published"]);
    for (const { value } of Object.values(examples)) {
      expect(Object.keys(value).sort()).toEqual([...meters.required].sort());
      expect(value.input).toHaveLength(6);
      expect(value.output.peak_dbfs).toHaveLength(2);
      expect(value.output.rms_dbfs).toHaveLength(2);
      expect(value.waveform.length).toBeLessThanOrEqual(128);
      for (const point of value.waveform) {
        expect(Number.isInteger(point)).toBe(true);
        expect(point).toBeGreaterThanOrEqual(0);
        expect(point).toBeLessThanOrEqual(1000);
      }
      for (const entry of value.input) {
        expect(entry.peak_dbfs).toBeGreaterThanOrEqual(-120);
        expect(entry.rms_dbfs).toBeGreaterThanOrEqual(-120);
        expect(Object.keys(entry).sort()).toEqual(["peak_dbfs", "rms_dbfs"]);
      }
      expect(value.limiter_gain_reduction_db).toBeLessThanOrEqual(0);
    }
    // The never-published example carries the sentinel and an empty envelope.
    expect(examples.never_published.value.sequence).toBe(0);
    expect(examples.never_published.value.waveform).toEqual([]);
    expect(examples.live.value.sequence).toBeGreaterThan(0);
  });

  it("describes the SD directory index and the bound it holds to", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const path = document.paths["/sd/{directory}/"];
    const get = path.get;
    expect(get.operationId).toBe("getSdIndex");
    // A public read, like GET /sd/{resource}: no auth parameter, no request body.
    expect(get.parameters).toBeUndefined();
    expect(get.requestBody).toBeUndefined();
    // The listing is the trailing-slash form of the same resource, so the file
    // route's pattern must exclude a trailing slash or the two would overlap.
    expect(document.paths["/sd/{resource}"].parameters[0].schema.pattern).toBe(
      "^www/[A-Za-z0-9._/-]{0,91}[A-Za-z0-9._-]$",
    );
    expect(path.parameters[0].schema.pattern).toBe("^www(/[A-Za-z0-9._-]{1,32}){0,7}$");
    // Other methods are refused, not silently routed to the file handler.
    expect(path.description).toContain("405 sd_directory");

    // The cap is the whole bounding argument, so the document has to state it
    // and say what happens past it.
    expect(get.description).toContain("capped at 64 entries");
    expect(get.description).toContain("not paginated");
    expect(get.description).toContain("truncated: yes");
    expect(get.responses["200"].description).toContain("truncated");

    // 200 and 500 are HTML; every refusal is the shared JSON error shape.
    expect(Object.keys(get.responses).sort()).toEqual(["200", "404", "500", "503"]);
    expect(Object.keys(get.responses["200"].content)).toEqual(["text/html"]);
    expect(Object.keys(get.responses["500"].content)).toEqual(["text/html"]);
    for (const status of ["404", "503"]) {
      expect(get.responses[status].content["application/json"].schema).toEqual({
        $ref: "#/components/schemas/Error",
      });
      for (const [name, sample] of Object.entries(get.responses[status].content["application/json"].examples)) {
        expect(sample.value).toEqual({ ok: false, error: name });
      }
    }
    // Every token the dispatcher can emit on this route, and no invented ones.
    expect(Object.keys(get.responses["404"].content["application/json"].examples)).toEqual([
      "sd_path",
      "sd_not_found",
      "sd_not_directory",
    ]);
    expect(Object.keys(get.responses["503"].content["application/json"].examples)).toEqual([
      "sd_unavailable",
      "sd_busy",
      "sd_index",
      "sd_index_unavailable",
    ]);
    expect(get.responses["503"].description).toContain("refused rather than queued");

    // Both HTML examples are the firmware's exact framing, footer included.
    const examples = get.responses["200"].content["text/html"].examples;
    expect(Object.keys(examples).sort()).toEqual(["listing", "truncated"]);
    for (const { value } of Object.values(examples)) {
      expect(value.startsWith('<!DOCTYPE html><html><head><meta charset="utf-8"><title>/sd/www/</title>')).toBe(true);
      expect(value.endsWith("Sizes in bytes; at most 64 entries per listing.</p></body></html>")).toBe(true);
      expect(value).toMatch(/<p>\d+ shown, \d+ hidden, truncated: (yes|no)\./);
      // Every link is relative, so a listing navigates within /sd/ wherever it
      // is served from.
      for (const [, href] of value.matchAll(/href="([^"]*)"/g)) {
        expect(href.startsWith("/")).toBe(false);
      }
    }
    expect(examples.listing.value).toContain("truncated: no");
    expect(examples.truncated.value).toContain("truncated: yes");

    // The file verbs, enumerated from the firmware dispatch rather than from
    // this document's older self (which knew only 200/404/507 for PUT and
    // 200/404 for DELETE). PUT has NO 404 arm: an invalid path is one of the
    // two meanings of 507 sd_upload, stated so a client never renders it as
    // "card full" (sd_capacity's meaning).
    const filePath = document.paths["/sd/{resource}"];
    expect(Object.keys(filePath.put.responses).sort()).toEqual(["200", "401", "403", "409", "500", "503", "507"]);
    // Phase 4l (S5): the preset cap is its own 409, and the 503 arms stay unchanged.
    expect(filePath.put.responses["409"].content["application/json"].schema).toEqual({
      $ref: "#/components/schemas/Error",
    });
    expect(filePath.put.responses["409"].content["application/json"].examples).toEqual({
      preset_limit: { value: { ok: false, error: "preset_limit" } },
    });
    expect(Object.keys(filePath.put.responses["503"].content["application/json"].examples)).toEqual([
      "sd_unavailable",
      "sd_busy",
      "audio_busy",
    ]);
    expect(Object.keys(filePath.put.responses["507"].content["application/json"].examples)).toEqual([
      "sd_upload",
      "sd_capacity",
    ]);
    expect(filePath.put.responses["507"].description).toContain("card full");
    expect(filePath.put.responses["500"].content["application/json"].schema).toEqual({
      $ref: "#/components/schemas/SdJobError",
    });
    // Content-Range has no meaning on this route; a preset/asset upload is one request.
    expect(filePath.put.description).toContain("Content-Range is ignored");
    // The config guard is a bare suffix match; both verbs document it.
    expect(filePath.put.responses["403"].description).toContain("suffix");
    expect(Object.keys(filePath.delete.responses).sort()).toEqual(["200", "401", "403", "404", "503"]);
    expect(Object.keys(filePath.delete.responses["404"].content["application/json"].examples)).toEqual([
      "sd_path",
      "sd_not_found",
    ]);
    expect(Object.keys(filePath.delete.responses["503"].content["application/json"].examples)).toEqual([
      "sd_unavailable",
      "sd_busy",
      "audio_busy",
    ]);
  });

  it("describes the flight-recorder surface as the firmware serves it", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));

    // Every recorder route is token-gated, the GETs included: the data read
    // carries recorded audio and the header reveals trigger state.
    const operations = [
      document.paths["/api/recorder"].get,
      document.paths["/api/recorder/freeze"].post,
      document.paths["/api/recorder/thaw"].post,
      document.paths["/api/recorder/config"].put,
      document.paths["/api/recorder/data"].get,
    ];
    for (const operation of operations) {
      const refs = operation.parameters.map((parameter) => parameter.$ref ?? parameter.name);
      expect(refs).toContain("#/components/parameters/SdAuth");
      expect(operation.responses["401"]).toBeDefined();
    }

    // The bank document pins board/constant facts + the uniform config once,
    // then the per-slot array (aius-recorder-rolling-buffer.md Phase C).
    const recorder = document.components.schemas.Recorder;
    expect(recorder.required).toEqual([
      "fitted_mb",
      "encoding",
      "significant_bits",
      "interleave",
      "frame_bytes",
      "boot_generation",
      "chunk_max_bytes",
      "active_slot",
      "slot_count",
      "slots_recycled",
      // The bank's own liveness record: a stalled round-robin (the documented
      // correctness-over-liveness trade) is otherwise indistinguishable from an
      // idle board, and lease_leaks should stay 0 forever.
      "deferred_epochs",
      "lease_skips",
      "lease_leaks",
      "empty_triggers_dropped",
      "config_pending",
      "newest_frozen_slot",
      "config",
      "slots",
    ]);
    expect(recorder.properties.encoding.const).toBe("s32le");
    expect(recorder.properties.significant_bits.const).toBe(24);
    expect(recorder.properties.frame_bytes.const).toBe(8);
    expect(recorder.properties.slot_count.const).toBe(4);
    expect(recorder.properties.slots.minItems).toBe(4);
    expect(recorder.properties.slots.maxItems).toBe(4);
    expect(recorder.properties.slots.items).toEqual({ $ref: "#/components/schemas/RecorderSlot" });

    // Each slot's own WAV-sufficient field list: geometry + extent + trigger
    // + generation, the fields that genuinely vary per capture.
    const slot = document.components.schemas.RecorderSlot;
    expect(slot.required).toEqual([
      "slot",
      "state",
      "sample_rate_hz",
      // rate_changes joined sample_rate_hz: one scalar cannot describe a ring
      // that spans a rate transition, so a non-zero count says the header is
      // true of only the newest frames.
      "rate_changes",
      "ring_frames",
      "valid_frames",
      "oldest_frame",
      "freeze_generation",
      "gap_count",
      "last_gap_frame",
      "suppressed_triggers",
      "postroll_captured",
      // The stream-to-WAV origin: every other stream-indexed field (the
      // rate-change positions, trigger.at_frame) is unusable without it.
      "stream_end_frame",
      "rate_change_count",
      "rate_change_frames",
      "trigger",
    ]);
    expect(slot.properties.state.enum).toEqual(["absent", "recording", "postroll", "draining", "frozen"]);
    // The core stops storing positions at four while the count keeps rising, so
    // the two are read together: a count above the array means the positions are
    // incomplete and the capture has to be rejected rather than cut.
    expect(slot.properties.rate_change_frames.maxItems).toBe(4);
    expect(slot.properties.rate_change_count.description).toContain("incomplete");
    expect(slot.properties.stream_end_frame.description).toContain(
      "wav_index = stream_index - (stream_end_frame - valid_frames)",
    );
    expect(slot.properties.trigger).toEqual({ $ref: "#/components/schemas/RecorderTrigger" });

    // Appended, never renumbered: the three producer-posted triggers sit
    // after `operator`, so every existing name keeps its position in the
    // firmware's own enum. `governor_trim` and `missed_fill` arrived with
    // glitch-timestamping 2026-09-07; `crc_reject` with the AC-3 syncframe
    // CRC the same day.
    expect(document.components.schemas.RecorderTrigger.properties.which.enum).toEqual([
      "none",
      "overrun",
      "underrun",
      "carrier_loss",
      "limiter_gr",
      "operator",
      "governor_trim",
      "missed_fill",
      "crc_reject",
    ]);

    // The config body requires all ten keys exactly - the DSP PUT's
    // anti-drift stance - and refuses unknown ones.
    const config = document.components.schemas.RecorderConfig;
    expect(config.additionalProperties).toBe(false);
    expect(config.required).toEqual([
      "overrun",
      "underrun",
      "carrier_loss",
      "limiter_gr",
      "governor_trim",
      "missed_fill",
      "crc_reject",
      "gr_threshold_db",
      "gr_hold_blocks",
      "postroll_blocks",
    ]);
    expect(config.properties.gr_threshold_db.maximum).toBe(0);
    expect(config.properties.postroll_blocks.maximum).toBe(938);

    // The data route's three fixed-order query parameters, byte-addressed and
    // frame-aligned, capped at the chunk bound.
    const data = document.paths["/api/recorder/data"].get;
    const queryNames = data.parameters.filter((parameter) => parameter.name).map((parameter) => parameter.name);
    expect(queryNames).toEqual(["offset", "length", "generation"]);
    const offset = data.parameters.find((parameter) => parameter.name === "offset");
    expect(offset.schema.multipleOf).toBe(8);
    const length = data.parameters.find((parameter) => parameter.name === "length");
    expect(length.schema.maximum).toBe(65536);
    expect(data.responses["200"].content["application/octet-stream"]).toBeDefined();
  });

  it("reaches the card through the whole-tree SD sync", () => {
    // The device serves the document because firmware/tools/aius-sd-sync.sh
    // uploads every file under the web tree. An allow-list in that tool would
    // silently strand the document on the host.
    const sync = readFileSync(resolve(firmwareRoot, "tools/aius-sd-sync.sh"), "utf8");
    expect(sync).toContain('find "$www_dir" -type f -print0');
  });

  // -------------------------------------------------------------------------
  // The drift gate. Each of the three documents is compared with its schema in
  // both directions, against the firmware's own serializer format strings.
  // -------------------------------------------------------------------------

  it("documents GET /api/status exactly as the firmware serializes it", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const served = servedDocument("status");
    // A silently-truncated extraction would make the comparison vacuous, so the
    // floor is asserted before the comparison rather than trusted.
    expect(served.properties.size).toBeGreaterThan(50);
    expect(compareWithSchema(document, served, "Status")).toEqual(NO_DRIFT);

    // The diagnostic objects the status body carries beside the scalars, each a
    // named schema rather than a free-form blob.
    for (const [key, schema] of [
      ["net_diag", "NetDiag"],
      ["tx_gate", "TxGate"],
      ["sd_diag", "SdDiag"],
      ["www_cache", "WwwCache"],
      ["audio_diag", "AudioDiag"],
      ["clock", "Clock"],
      ["pipeline_diag", "PipelineDiag"],
      ["stall_diag", "StallDiag"],
      ["boot_fuse_0x6e0", "BootFuse"],
    ]) {
      expect(document.components.schemas.Status.properties[key]).toEqual({
        $ref: `#/components/schemas/${schema}`,
      });
    }

    // Every path the firmware serves as null on one branch says so in the
    // schema; the extractor keeps the object branch, so this is the only place
    // the null can be stated.
    expect(NULLABLE_OBJECT_PATHS.filter((path) => !schemaAtPath(document, "Status", path).nullable)).toEqual([]);

    // model_origin is the third answer `source` cannot give.
    expect(document.components.schemas.Status.properties.model_origin.enum).toEqual(["default", "sd", "put"]);
    expect(document.components.schemas.Status.properties.model_origin.description).toContain("PUT /api/dsp/config");
    // source/model_origin are staged-aware, so the pair beside them says which
    // of the two models a reader is looking at. Served as 0|1 like this route's
    // other flags, not as a JSON boolean, and dsp_-prefixed because
    // config_valid/config_error here describe aius.json, not the DSP model.
    expect(document.components.schemas.Status.properties.dsp_config_pending.enum).toEqual([0, 1]);
    expect(document.components.schemas.Status.properties.dsp_config_pending.description).toContain("staged");
    expect(document.components.schemas.Status.properties.dsp_staged_generation.type).toBe("integer");
    expect(document.components.schemas.Status.properties.dsp_staged_generation.description).toContain(
      "staged_generation",
    );
    // ab.booted_slot is the other half of the slot-agnostic acceptance gate:
    // ledger_valid alone cannot say which chooser wrote the ledger.
    const abState = document.components.schemas.AbState;
    expect(abState.properties.booted_slot.maximum).toBe(2);
    expect(abState.properties.booted_slot.description).toContain("ledger_valid && booted_slot != 0");
    expect([...abState.required].sort()).toEqual(Object.keys(abState.properties).sort());
    // Two keys are served as a literal 0 by a firmware that has no producer for
    // them; documenting them as open counters would invite a client to plot noise.
    for (const key of ["dhcp_discovers", "http_server_rebinds"]) {
      expect(document.components.schemas.Status.properties[key].const).toBe(0);
    }
    // A temperature of 0 means "not measured" until cpu_temp_valid says otherwise.
    expect(document.components.schemas.Status.properties.cpu_temp_valid.enum).toEqual([0, 1]);
    expect(document.components.schemas.Status.properties.cpu_temp_c.description).toContain("cpu_temp_valid");

    // The clock object's vocabulary, in the firmware's own precedence order.
    const clock = document.components.schemas.Clock;
    expect(clock.properties.master.enum).toEqual(["input"]);
    expect(clock.properties.state.enum).toEqual([
      "fault",
      "unlocked",
      "unsupported",
      "tracking",
      "refused",
      "acquiring",
    ]);
    expect(clock.properties.state.description).toContain("First match wins");
    // Rates are millihertz where a ppm-scale figure would otherwise quantise away.
    expect(clock.properties.carrier_rate_millihz.description).toContain("millihertz");
    expect(clock.properties.last_clock_fault.enum).toEqual([0, 1, 2]);
    // invalid_windows is documented as the firmware behaves, not as it reads.
    expect(clock.properties.invalid_windows.description).toContain("Despite the name");

    // The boot fuse is a bit field served as hex, and unreadable off-target.
    expect(document.components.schemas.BootFuse.properties.word.pattern).toBe("^0x[0-9a-f]{8}$");
    expect(document.components.schemas.BootFuse.properties.read.description).toContain("host build");

    // The two failure-attribution objects keep the closed shape their siblings use.
    for (const name of ["SdDiag", "SdDiagFailure", "AudioDiag", "Clock", "PipelineDiag", "BootFuse"]) {
      const schema = document.components.schemas[name];
      expect(schema.additionalProperties).toBe(false);
      expect([...schema.required].sort()).toEqual(Object.keys(schema.properties).sort());
    }

    // The dark-interval instrument in input_diag (aius-input-recovery Phase
    // 1): the gate-closed companion to the production-gap watermark and the
    // commit-bounded interval record. Closed shape, every new field required,
    // and the two arrays carry the firmware's fixed geometry (6 gate-closed
    // phases, a ring of 8 intervals) so a reader can size its buffers.
    const inputDiag = document.components.schemas.InputDiag;
    expect(inputDiag.additionalProperties).toBe(false);
    expect([...inputDiag.required].sort()).toEqual(Object.keys(inputDiag.properties).sort());
    for (const field of [
      "dark_alert_ms",
      "dark_record_min_ms",
      "dark_phase",
      "dark_phase_ms",
      "dark_open",
      "dark_open_ms",
      "dark_open_start_ms",
      "dark_open_entry_phase",
      "dark_open_entry_class",
      "dark_open_entry_ready",
      "dark_open_entry_gate",
      "dark_interval_count",
      "dark_phases",
      "dark_intervals",
    ]) {
      expect(inputDiag.required).toContain(field);
    }
    expect(inputDiag.properties.dark_alert_ms.description).toContain("3000");
    expect(inputDiag.properties.dark_record_min_ms.description).toContain("500");
    // The ladder vocabulary: seven phases, six of which have a watermark row
    // (gate_open is the production gap's region), named in the description.
    const phaseEnum = [0, 1, 2, 3, 4, 5, 6];
    expect(inputDiag.properties.dark_phase.enum).toEqual(phaseEnum);
    for (const name of [
      "no_carrier",
      "rate_unsupported",
      "clock_acquiring",
      "plan_pending",
      "awaiting_block",
      "gate_armed",
      "gate_open",
    ]) {
      expect(inputDiag.properties.dark_phase.description).toContain(name);
    }
    expect(inputDiag.properties.dark_phases.minItems).toBe(6);
    expect(inputDiag.properties.dark_phases.maxItems).toBe(6);
    expect(inputDiag.properties.dark_phases.items).toEqual({ $ref: "#/components/schemas/InputDarkPhase" });
    expect(inputDiag.properties.dark_intervals.maxItems).toBe(8);
    expect(inputDiag.properties.dark_intervals.items).toEqual({ $ref: "#/components/schemas/InputDarkInterval" });
    const darkPhase = document.components.schemas.InputDarkPhase;
    expect(darkPhase.additionalProperties).toBe(false);
    expect(darkPhase.required).toEqual(["phase", "max_ms", "over_count", "last_ms", "last_at_ms"]);
    expect(darkPhase.properties.phase.enum).toEqual([0, 1, 2, 3, 4, 5]);
    const darkInterval = document.components.schemas.InputDarkInterval;
    expect(darkInterval.additionalProperties).toBe(false);
    expect([...darkInterval.required].sort()).toEqual(Object.keys(darkInterval.properties).sort());
    // The snapshot fields share one vocabulary at both ends of an interval.
    for (const end of ["entry", "exit"]) {
      expect(darkInterval.properties[`${end}_phase`].enum).toEqual(phaseEnum);
      expect(darkInterval.properties[`${end}_class`].enum).toEqual([0, 1, 2]);
      expect(darkInterval.properties[`${end}_ready`].enum).toEqual([0, 1]);
      expect(darkInterval.properties[`${end}_gate`].enum).toEqual([0, 1]);
    }
    expect(inputDiag.properties.dark_open_entry_class.enum).toEqual([0, 1, 2]);
    // The event ring is 32 deep and paged 8 at a time via `next`.
    const events = document.components.schemas.Events;
    expect(events.properties.events.maxItems).toBe(8);
    expect(events.description).toContain("32");
    expect(events.properties.next.description).toContain("full");
    expect(document.paths["/api/events"].get.description).toContain("next");
  });

  it("documents GET /api/recorder exactly as the firmware serializes it", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const served = servedDocument("recorder");
    // Most of the WAV-sufficient fields moved into the nested `slots` array
    // (Phase C), which this top-level .size count does not drill into; 17 is
    // the bank document's actual, complete top-level property count - enough to
    // catch a silently-truncated extraction.
    expect(served.properties.size).toBeGreaterThan(15);
    expect(compareWithSchema(document, served, "Recorder")).toEqual(NO_DRIFT);
    // The extractor keeps the %lu arm of newest_frozen_slot's two appends, so
    // the schema is the only place its null (nothing frozen, which is exactly
    // when a defaulted data read answers 409) can be stated.
    expect(schemaAtPath(document, "Recorder", "newest_frozen_slot").nullable).toBe(true);
    // The pair that makes a pulled WAV honest: the snapshot's own rate, and the
    // count that says whether one rate describes the whole ring.
    expect(document.components.schemas.RecorderSlot.properties.rate_changes.description).toContain("newest frames");
    expect(document.components.schemas.RecorderSlot.properties.sample_rate_hz.description).toContain("8.8%");
  });

  it("documents no replay route, schema, status field, clock state or fill_busy refusal (Phase 4i)", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    expect(Object.keys(document.paths).filter((path) => path.includes("replay"))).toEqual([]);
    for (const name of ["ReplayDiag", "ReplayList", "ReplayStatus"]) {
      expect(document.components.schemas[name], name).toBeUndefined();
    }
    const status = document.components.schemas.Status.properties;
    expect(status.replay_diag).toBeUndefined();
    expect(document.components.schemas.TxGate.properties.replay_running).toBeUndefined();
    expect(document.components.schemas.StatusBrief.properties.tx_gate.required).toEqual(["open"]);
    expect(document.components.schemas.Clock.properties.state.enum).not.toContain("replay");
    // Job kind 6 stays retired and is never reused, so CACHE_FILL keeps 7.
    const kinds = Object.values(document.components.schemas).flatMap((schema) =>
      Object.entries(schema.properties ?? {}).filter(([key]) => key === "job_kind"),
    );
    expect(kinds.some(([, field]) => field.description.includes("6 is retired"))).toBe(true);
    // The only mention of the word left in the contract is the retired event
    // type 3, which is documented as such.
    expect(JSON.stringify(document).match(/replay(?!ing)/gi) ?? []).toHaveLength(1);
    expect(JSON.stringify(document)).not.toContain("fill_busy");
  });

  it("documents GET /api/dsp/config exactly as the firmware serializes it", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const served = servedDocument("dspConfig");
    expect(compareWithSchema(document, served, "DspConfig")).toEqual(NO_DRIFT);
    // The six channel keys come from the firmware's own table, so a renamed
    // channel fails here rather than drifting.
    expect([...served.properties.get("input_channels").properties.keys()].sort()).toEqual([
      "fc",
      "fl",
      "fr",
      "lfe",
      "sl",
      "sr",
    ]);
    // layout_aware is emitted even though the PUT validator would accept its
    // absence, so the GET and PUT key sets match; the schema documents it.
    expect(served.properties.get("matrix").properties.has("layout_aware")).toBe(true);
  });

  it("documents GET /api/status/brief exactly as the firmware serializes it", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const served = servedDocument("statusBrief");
    expect(compareWithSchema(document, served, "StatusBrief")).toEqual(NO_DRIFT);
    // Exactly the keys the editor's flow ends read, under the full body's names
    // and nesting (Phase 4f F3); the ui-polling fixture is one such body.
    const fixture = JSON.parse(readFileSync(resolve(repoRoot, "test/sd-editor/fixtures/status-brief.json"), "utf8"));
    expect(Object.keys(fixture).sort()).toEqual([...served.properties.keys()].sort());
    const status = document.components.schemas.Status.properties;
    for (const key of ["carrier", "state", "sample_rate", "pipeline_diag", "tx_gate"]) {
      expect(status[key], key).toBeDefined();
    }
    const get = document.paths["/api/status/brief"].get;
    expect(get.responses["200"].content["application/json"].examples.live.value).toEqual(fixture);
  });

  it("documents the write-behind store's responses and sd_flush status (Phase 4h)", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const { paths, components } = document;
    // 409 sd_flush_pending on the routes that would discard an unflushed write.
    for (const [route, method] of [
      ["/api/reboot", "post"],
      ["/api/ota/activate", "post"],
    ]) {
      const conflict = paths[route][method].responses["409"];
      expect(conflict.content["application/json"].examples.sd_flush_pending.value, route).toEqual({
        ok: false,
        error: "sd_flush_pending",
      });
    }
    expect(paths["/api/ota/activate"].post.responses["409"].description).toContain("rehearse");
    // A full store answers 503 sd_busy with Retry-After on every accepted write.
    for (const [route, method] of [
      ["/api/dsp/config", "put"],
      ["/api/config/public", "put"],
      ["/sd/{resource}", "put"],
      ["/sd/{resource}", "delete"],
    ]) {
      const busy = paths[route][method].responses["503"];
      expect(busy.headers["Retry-After"], route).toEqual({ $ref: "#/components/headers/RetryAfter" });
      expect(busy.content["application/json"].examples.sd_busy.value, route).toEqual({ ok: false, error: "sd_busy" });
    }
    expect(paths["/sd/{resource}"].delete.description).toContain("even for a path the card does not hold");
    // The full sd_flush object is in /api/status; the brief carries a subset.
    expect(components.schemas.Status.properties.sd_flush).toEqual({ $ref: "#/components/schemas/SdFlush" });
    const full = Object.keys(components.schemas.SdFlush.properties).sort();
    expect(full).toEqual(
      [
        "crc_drops",
        "dirty",
        "drained",
        "failures",
        "flushed",
        "flushing",
        "held",
        "last_fault",
        "max_settings_latency_ms",
        "oldest_dirty_age_ms",
        "sync_drain_ms",
        "tombstones",
      ].sort(),
    );
    expect(components.schemas.SdFlush.required.sort()).toEqual(full);
    const brief = components.schemas.StatusBrief.properties.sd_flush.properties;
    for (const key of Object.keys(brief)) expect(components.schemas.SdFlush.properties[key], key).toBeDefined();
    expect(Object.keys(brief).sort()).toEqual(["dirty", "drained", "failures", "held", "last_fault"]);
    // Diagnostics for the mount restore and the refused activation reasons.
    const sdDiag = components.schemas.SdDiag;
    for (const key of ["settings_restores", "settings_restore_faults"]) {
      expect(sdDiag.properties[key], key).toBeDefined();
      expect(sdDiag.required).toContain(key);
    }
    expect(components.schemas.OtaDiag.properties.activation_fault.enum).toEqual(
      expect.arrayContaining(["ota_flush_pending", "ota_sd_busy"]),
    );
  });

  it("keeps the fill walk's progress readable from the status surface (Phase 4g)", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    expect(document.components.schemas.WwwCache.properties.fill_complete).toBeDefined();
    expect(document.components.schemas.WwwCache.properties.present).toBeDefined();
  });

  it("lists audio_busy with Retry-After on every sheddable route (Phase 4f)", () => {
    const document = JSON.parse(readFileSync(documentPath, "utf8"));
    const resolveResponse = (response) =>
      response.$ref ? document.components.responses[response.$ref.split("/").pop()] : response;
    // The admission classes (status, brief, meters, events, settings PUT parses,
    // directory listing) and every HTTP SD job start; OTA staging is exempt.
    const sheddable = [
      ["/api/status", "get", []],
      ["/api/status/brief", "get", []],
      ["/api/meters", "get", []],
      ["/api/events", "get", []],
      ["/api/dsp/config", "put", ["sd_unavailable", "sd_busy"]],
      ["/api/config/public", "put", ["sd_unavailable", "sd_busy"]],
      ["/sd/{resource}", "get", ["sd_unavailable", "sd_busy"]],
      ["/sd/{resource}", "put", ["sd_unavailable", "sd_busy"]],
      ["/sd/{resource}", "delete", ["sd_unavailable", "sd_busy"]],
    ];
    for (const [path, method, sdBodies] of sheddable) {
      const response = resolveResponse(document.paths[path][method].responses["503"]);
      const label = `${method.toUpperCase()} ${path}`;
      expect(response, label).toBeDefined();
      expect(Object.keys(response.content["application/json"].examples), label).toEqual([...sdBodies, "audio_busy"]);
      expect(response.headers["Retry-After"], label).toEqual({ $ref: "#/components/headers/RetryAfter" });
      expect(response.description, label).toContain("audio_busy");
      for (const [name, sample] of Object.entries(response.content["application/json"].examples)) {
        expect(sample.value).toEqual({ ok: false, error: name });
      }
    }
    // Control and OTA routes are never shed (F13): no audio_busy body on them.
    for (const [path, method] of [
      ["/api/start", "post"],
      ["/api/stop", "post"],
      ["/api/clear", "post"],
      ["/api/reboot", "post"],
      ["/api/ota/image", "put"],
      ["/api/dsp/config", "get"],
    ]) {
      expect(JSON.stringify(document.paths[path][method]), `${method} ${path}`).not.toContain("audio_busy");
    }
  });

  it("fails loudly rather than extracting a subset", () => {
    // Comments quote key names verbatim, so a comment-blind scan would invent
    // served keys; the scanner strips them and keeps string literals whole.
    const stripped = stripComments('int a; /* ",\\"ghost\\":" */ const char *f = "//not a comment";', "probe");
    expect(stripped).not.toContain("ghost");
    expect(stripped).toContain('"//not a comment"');
    // Brace imbalance, a duplicate key at one path, an unmodelled value shape
    // and content after the close are all errors, never a quiet partial tree.
    expect(() => parseKeyTree('{"a":%lu', {})).toThrow(/unterminated object/);
    expect(() => parseKeyTree('{"a":%lu,"a":%lu}', {})).toThrow(/duplicate key a/);
    expect(() => parseKeyTree('{"a":?}', {})).toThrow(/unrecognised value shape/);
    expect(() => parseKeyTree('{"a":%lu}{}', {})).toThrow(/content after the document closed/);
    expect(() => parseKeyTree('{"%s":%lu}', {})).toThrow(/no declared expansion/);
    // Adjacent literals concatenate; a computed format argument is refused
    // rather than treated as an empty contribution.
    expect(stringLiteralValue('"a" "b"', "probe")).toBe("ab");
    expect(stringLiteralValue("format", "probe")).toBeNull();
    // A named format resolves only to a PROGMEM literal declared in the same
    // body; a bare name, a non-PROGMEM declaration or a table stays unreadable.
    const progmemBody =
      'static const char fmt[] PROGMEM = "{\\"a\\":" "%lu}"; http_json_append(b, capacity, l, fmt, 1u);';
    expect(namedFormatLiteral(progmemBody, "fmt", "probe")).toBe('{"a":%lu}');
    expect(namedFormatLiteral(progmemBody, "other", "probe")).toBeNull();
    expect(namedFormatLiteral('static const char fmt[] = "{}";', "fmt", "probe")).toBeNull();
    expect(namedFormatLiteral(progmemBody, "table[0]", "probe")).toBeNull();
    // A renamed or duplicated serializer is an error, not a zero-key document.
    expect(() => functionBody("void other(void) {}", "missing", "probe")).toThrow(/expected exactly one definition/);
    // An emitter that is not in the table cannot be reached silently.
    expect(() => emittedText("http_meters_json")).toThrow(/no emitter table entry/);
  });
});
