import type { Candidate } from "../protocol/messages.js";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import OpenCC from "opencc-js";

interface Entry { text: string; code: string; weight: number; }
interface PartialCandidate { text: string; weight: number; }
const DICTIONARY_PATH = fileURLToPath(new URL("../../data/pinyin_simp.dict.yaml", import.meta.url));
const MICROSOFT_DICTIONARY_PATH = fileURLToPath(new URL("../../data/microsoft_pinyin.dict.yaml", import.meta.url));
// Keep enough results available for candidate paging; the UI displays 9 per page.
const MAX_CANDIDATES = 90;
const toSimplified = OpenCC.Converter({ from: "twp", to: "cn" });

function normalizeCode(value: string): string {
  return value.toLowerCase().replace(/[\s'’`-]+/g, "");
}

function toUserText(value: string): string {
  return String(toSimplified(value)).replaceAll("妳", "你");
}

function loadDictionary(path = DICTIONARY_PATH): Map<string, Entry[]> {
  const entries = new Map<string, Entry[]>();

  const addFile = (path: string): void => {
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
      const entry: Entry = { text, code, weight: Number.isFinite(weight) ? weight : 0 };
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
        if (initials !== code) addEntry(initials, { ...entry, code: initials });
      }
    }
  };

  addFile(path);

  for (const list of entries.values()) {
    list.sort((a, b) => b.weight - a.weight);
  }
  return entries;
}

let dictionary: Map<string, Entry[]> | undefined;
function getDictionary(): Map<string, Entry[]> { return dictionary ??= loadDictionary(); }
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
  if (exact.length > 0) {
    return exact.map((entry) => ({ ...entry, text: toUserText(entry.text) }))
      .filter((entry, index, list) => list.findIndex((candidate) => candidate.text === entry.text) === index)
      .slice(0, MAX_CANDIDATES)
      .map((entry, index) => ({ text: entry.text, index, annotation: entry.code }));
  }

  const combinations: PartialCandidate[][] = Array.from({ length: query.length + 1 }, () => []);
  combinations[0] = [{ text: "", weight: 0 }];
  const dictionaryEntries = sourceDictionary;
  for (let offset = 0; offset < query.length; offset++) {
    const current = combinations[offset];
    if (!current || current.length === 0) continue;
    for (const [code, entries] of dictionaryEntries) {
      if (!query.startsWith(code, offset)) continue;
      const next = offset + code.length;
      const topEntries = entries.slice(0, 4);
      const target = combinations[next];
      if (!target) continue;
      for (const prefix of current) {
        for (const entry of topEntries) {
          target.push({ text: prefix.text + entry.text, weight: prefix.weight + entry.weight - 100 });
        }
      }
      target.sort((a, b) => b.weight - a.weight);
      combinations[next] = target.slice(0, 24);
    }
  }
  const composed = combinations[query.length] ?? [];
  const results = composed.length > 0 ? composed : [...sourceDictionary.entries()]
    .filter(([code]) => code.startsWith(query))
    .sort((a, b) => (b[1][0]?.weight ?? 0) - (a[1][0]?.weight ?? 0))
    .flatMap(([, list]) => list);
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
  const primary = queryDictionary(composition, getDictionary());
  return primary.length > 0 ? primary : queryDictionary(composition, getMicrosoftDictionary());
}