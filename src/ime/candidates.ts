import type { Candidate } from "../protocol/messages.js";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
interface Entry { text: string; code: string; weight: number; priority: number; isInitials?: boolean; }
interface PartialCandidate { text: string; weight: number; trace?: string[]; }
const CUSTOM_DICTIONARY_PATH = fileURLToPath(new URL("../../data/custom.dict.yaml", import.meta.url));
const DICTIONARY_PATH = fileURLToPath(new URL("../../data/pinyin_simp.dict.yaml", import.meta.url));
const MICROSOFT_DICTIONARY_PATH = fileURLToPath(new URL("../../data/microsoft_pinyin.dict.yaml", import.meta.url));
// Keep enough results available for candidate paging; the UI displays 9 per page.
const MAX_CANDIDATES = 90;

function normalizeCode(value: string): string {
  return value.toLowerCase().replace(/[\s'’`-]+/g, "");
}

function toUserText(value: string): string {
  return value.replaceAll("妳", "你");
}

function loadDictionary(path?: string): Map<string, Entry[]> {
  const entries = new Map<string, Entry[]>();

  const addFile = (path: string, priority: number): void => {
    const source = readFileSync(path, "utf8");
    let inTable = false;
    for (const line of source.split(/\r?\n/)) {
      if (line.trim() === "...") { inTable = true; continue; }
      if (!inTable || !line || line.startsWith("#")) continue;
      const columns = line.split("\t");
      const text = columns[0]?.trim() ?? "";
      if (!text) continue;
      const rawCode = columns[1]?.trim() ?? "";
      const code = normalizeCode(rawCode);
      if (!/^[a-z]+$/.test(code)) continue;
      const rawWeight = columns[2]?.trim() ?? "";
      const weight = rawWeight.endsWith("%") ? 0 : Number.parseFloat(rawWeight);
      const entry: Entry = { text, code, weight: Number.isFinite(weight) ? weight : 0, priority };
      const addEntry = (key: string, value: Entry): void => {
        const list = entries.get(key) ?? [];
        list.push(value);
        entries.set(key, list);
      };
      addEntry(code, entry);
      // Rime-style dictionaries preserve syllable boundaries with spaces. Add
      // an initials alias (e.g. "da jia" -> "dj") for abbreviated input.
      const syllables = rawCode.toLowerCase().split(/[\s'’`-]+/).filter(Boolean);
      if (syllables.length > 1 && syllables.every((syllable) => /^[a-z]+$/.test(syllable))) {
        const initials = syllables.map((syllable) => syllable[0]).join("");
        if (initials !== code) addEntry(initials, { ...entry, code: initials, isInitials: true });
      }
    }
  };

  if (path) {
    addFile(path, 0);
  } else {
    addFile(CUSTOM_DICTIONARY_PATH, 0);
    addFile(DICTIONARY_PATH, 1);
  }

  for (const list of entries.values()) {
    list.sort((a, b) => a.priority - b.priority || b.weight - a.weight);
  }
  return entries;
}

let customDictionary: Map<string, Entry[]> | undefined;
function getCustomDictionary(): Map<string, Entry[]> { return customDictionary ??= loadDictionary(CUSTOM_DICTIONARY_PATH); }
let dictionary: Map<string, Entry[]> | undefined;
function getDictionary(): Map<string, Entry[]> { return dictionary ??= loadDictionary(DICTIONARY_PATH); }
let microsoftDictionary: Map<string, Entry[]> | undefined;
function getMicrosoftDictionary(): Map<string, Entry[]> {
  if (microsoftDictionary) return microsoftDictionary;
  try {
    microsoftDictionary = loadDictionary(MICROSOFT_DICTIONARY_PATH);
  } catch (error) {
    // The Microsoft dictionary is optional until the .dat conversion script is run.
    if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    microsoftDictionary = new Map();
  }
  return microsoftDictionary;
}

function queryDictionary(composition: string, sourceDictionary: Map<string, Entry[]>): Candidate[] {
  const query = normalizeCode(composition);
  if (!query) return [];
  const exact = sourceDictionary.get(query) ?? [];
  // For a single-letter composition, prefer entries that are a single Han
  // character. Only broaden to multi-character words when no single-character
  // result exists for this code. This prevents initial-letter aliases and
  // short codes from crowding out the basic single-character candidates.
  if (query.length === 1) {
    const toCandidate = (entry: Entry, index: number): Candidate => ({
      text: toUserText(entry.text), index, annotation: entry.code,
    });
    const exactSingleCharacters = exact.filter((entry) => [...toUserText(entry.text)].length === 1);
    const singleCharacterEntries = exactSingleCharacters.length > 0
      ? exactSingleCharacters
      : [...sourceDictionary.entries()]
        .filter(([code]) => code.startsWith(query))
        .flatMap(([, entries]) => entries.filter((entry) => [...toUserText(entry.text)].length === 1));
    const seenSingleCharacters = new Set<string>();
    const singleCharacterCandidates = singleCharacterEntries
      .map(toCandidate)
      .filter((candidate) => !seenSingleCharacters.has(candidate.text) && (seenSingleCharacters.add(candidate.text), true))
      .slice(0, MAX_CANDIDATES)
      .map((candidate, index) => ({ ...candidate, index }));
    if (singleCharacterCandidates.length > 0) return singleCharacterCandidates;
    if (exact.length > 0) {
      return exact.map(toCandidate)
        .filter((entry, index, list) => list.findIndex((candidate) => candidate.text === entry.text) === index)
        .slice(0, MAX_CANDIDATES)
        .map((entry, index) => ({ ...entry, index }));
    }
  } else if (exact.length > 0) {
    return exact.map((entry) => ({ ...entry, text: toUserText(entry.text) }))
      .filter((entry, index, list) => list.findIndex((candidate) => candidate.text === entry.text) === index)
      .slice(0, MAX_CANDIDATES)
      .map((entry, index) => ({ text: entry.text, index, annotation: entry.code }));
  }

  const traceEnabled = process.env.IME_CANDIDATE_TRACE === "1";
  // A query can end in the middle of a syllable (e.g. "xiangm" while typing
  // "xiangmu"). Prefer real full-pinyin dictionary codes that start with the
  // whole query before attempting arbitrary character-by-character splits.
  // Otherwise a valid prefix like "xiangm" can be segmented as "xiang" + "m".
  const prefixMatches = [...sourceDictionary.entries()]
    .filter(([code]) => code.startsWith(query))
    .flatMap(([, entries]) => entries.filter((entry) => !entry.isInitials))
    .sort((a, b) => a.priority - b.priority || b.weight - a.weight);
  if (prefixMatches.length > 0) {
    if (traceEnabled) {
      console.log(`[ime-candidates] query=${query} resultMode=full-pinyin-prefix matches=${prefixMatches.length}`);
      for (const entry of prefixMatches.slice(0, 12)) {
        const displayText = toUserText(entry.text);
        console.log(`[ime-candidates] prefix raw=${JSON.stringify(entry.text)} display=${JSON.stringify(displayText)} fullCode=${entry.code} weight=${entry.weight}${displayText !== entry.text ? " converted=1" : ""}`);
      }
    }
    const seenPrefix = new Set<string>();
    return prefixMatches
      .map((entry) => ({ text: toUserText(entry.text), index: 0, annotation: entry.code }))
      .filter((candidate) => !seenPrefix.has(candidate.text) && (seenPrefix.add(candidate.text), true))
      .slice(0, MAX_CANDIDATES)
      .map((candidate, index) => ({ ...candidate, index }));
  }
  const combinations: PartialCandidate[][] = Array.from({ length: query.length + 1 }, () => []);
  combinations[0] = [{ text: "", weight: 0, ...(traceEnabled ? { trace: [] } : {}) }];
  const dictionaryEntries = sourceDictionary;
  for (let offset = 0; offset < query.length; offset++) {
    const current = combinations[offset];
    if (!current || current.length === 0) continue;
    for (const [code, entries] of dictionaryEntries) {
      if (!query.startsWith(code, offset)) continue;
      const next = offset + code.length;
      // Initials aliases are useful for abbreviated input, but must not be
      // treated as full syllables inside a longer pinyin composition (e.g.
      // "lianx" must not become "li" + "a" + "nx" -> "里啊那些").
      const topEntries = entries.filter((entry) => !entry.isInitials).slice(0, 4);
      if (topEntries.length === 0) continue;
      const target = combinations[next];
      if (!target) continue;
      for (const prefix of current) {
        for (const entry of topEntries) {
          target.push({
            text: prefix.text + entry.text,
            weight: prefix.weight + entry.weight - 100,
            ...(traceEnabled ? { trace: [...(prefix.trace ?? []), `${code} -> ${entry.text} (dict code: ${entry.code}, weight: ${entry.weight})`] } : {}),
          });
        }
      }
      target.sort((a, b) => b.weight - a.weight);
      combinations[next] = target.slice(0, 24);
    }
  }
  const composed = combinations[query.length] ?? [];
  if (traceEnabled) {
    console.log(`[ime-candidates] query=${query} exact=${(sourceDictionary.get(query) ?? []).length} composed=${composed.length}`);
    for (const candidate of composed.slice(0, 12)) {
      console.log(`[ime-candidates] candidate=${JSON.stringify(candidate.text)} weight=${candidate.weight} via=${(candidate.trace ?? []).join(" | ")}`);
    }
  }
  const results = composed.length > 0 ? composed : [...sourceDictionary.entries()]
    .filter(([code]) => code.startsWith(query))
    .sort((a, b) => (b[1][0]?.weight ?? 0) - (a[1][0]?.weight ?? 0))
    .flatMap(([, list]) => list);
  if (traceEnabled) {
    console.log(`[ime-candidates] resultMode=${composed.length > 0 ? "composition" : "prefix-fallback"} rawResults=${results.length}`);
    for (const candidate of results.slice(0, 12)) {
      console.log(`[ime-candidates] result=${JSON.stringify(candidate.text)} code=${"code" in candidate ? candidate.code : "composed"} weight=${candidate.weight}`);
    }
  }
  const seen = new Set<string>();
  return results.map((entry) => {
    const text = toUserText(entry.text);
    if ("code" in entry) return { text, index: 0, annotation: String(entry.code) };
    return { text, index: 0 };
  })
    .filter((entry) => !seen.has(entry.text) && (seen.add(entry.text), true))
    .slice(0, MAX_CANDIDATES)
    .map((entry, index) => ({ ...entry, index }));
}

export function getCandidates(composition: string): Candidate[] {
  const query = normalizeCode(composition);
  // Custom entries are exact matches only; never use them for composition or prefix suggestions.
  const custom = (getCustomDictionary().get(query) ?? [])
    .map((entry) => ({ text: toUserText(entry.text), index: 0, annotation: entry.code }))
    .filter((candidate, index, list) => list.findIndex((item) => item.text === candidate.text) === index)
    .slice(0, MAX_CANDIDATES);
  const primary = queryDictionary(composition, getDictionary());
  const combined = [...custom, ...primary];
  // Apply the one-letter rule across dictionary sources too: a custom multi-
  // character entry must not outrank available single-character candidates.
  const preferred = query.length === 1
    ? combined.filter((candidate) => [...candidate.text].length === 1)
    : combined;
  const candidates = preferred.length > 0 ? preferred : combined;
  const seen = new Set<string>();
  const deduplicated = candidates.filter((candidate) => {
    if (seen.has(candidate.text)) return false;
    seen.add(candidate.text);
    return true;
  });
  if (deduplicated.length > 0) {
    return deduplicated.slice(0, MAX_CANDIDATES).map((candidate, index) => ({ ...candidate, index }));
  }
  return queryDictionary(composition, getMicrosoftDictionary());
}