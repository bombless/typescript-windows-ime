# Wanxiang Octagram grammar integration

This IME now contains a TypeScript port of the RIME Octagram .gram reader and
collocation scorer. It reads the original Darts double-array model directly;
it does not convert the model to JSON, text n-grams, or KenLM.

## Install the official Simplified Chinese model

From the repository root, run:

\`\`\`powershell
.\scripts\download-wanxiang-grammar.ps1
\`\`\`

The script downloads the official wanxiang-lts-zh-hans.gram LTS release and
verifies the published file size and SHA-256 digest. The model is about 418 MB
and is intentionally excluded from Git. Restart the TypeScript IME process
after installing it.

By default, the runtime looks for:

\`\`\`text
data/wanxiang-lts-zh-hans.gram
\`\`\`

Set WANXIANG_GRAMMAR_PATH to override the model path.

## Runtime behavior

- Parses the Rime::Grammar/1.0 mapped-file header and relative OffsetPtr.
- Traverses the Darts double-array trie using the same unit labels, offsets,
  leaf values, and common-prefix lookup behavior as librime-octagram.
- Uses the upstream Octagram compact Unicode encoding for model keys.
- Applies the upstream context-suffix / candidate-prefix lookup and collocation
  penalties during dictionary-path decoding.
- Keeps up to 32 committed Unicode characters per IME state as context;
  sentence punctuation clears the context window.

The default Wanxiang settings are:

| Setting | Value |
|---|---:|
| collocation_max_length | 6 |
| collocation_min_length | 3 |
| collocation_penalty | -14 |
| non_collocation_penalty | -6 |
| weak_collocation_penalty | -100 |
| rear_penalty | -20 |

The TypeScript candidate decoder applies the model's log-scale score to each
dictionary transition. If the model file is absent or invalid, candidate
generation falls back to the existing dictionary-only behavior.

## Tests

\`\`\`powershell
npm test
npm run build
\`\`\`

See data/WANXIANG-GRAMMAR-NOTICE.md and
licenses/librime-octagram-BSD-3-Clause.txt for attribution and license terms.
