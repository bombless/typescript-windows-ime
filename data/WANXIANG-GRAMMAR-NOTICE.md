# Wanxiang grammar model attribution

The optional `wanxiang-lts-zh-hans.gram` model is published by **amzxyz** in
the [Rime Wanxiang data project](https://github.com/amzxyz/rime-wanxiang-data).

- Model: `wanxiang-lts-zh-hans.gram` (LTS, Simplified Chinese)
- License: CC BY 4.0; retain attribution to amzxyz / Rime Wanxiang data.
- Upstream runtime reference: [librime-octagram](https://github.com/lotem/librime-octagram),
  whose source is distributed under the BSD 3-Clause license.
- This project ports the `.gram` metadata, Darts lookup, Unicode key encoding,
  and Octagram collocation-scoring behavior into TypeScript. It does not embed
  the Rime engine or its native runtime.

The model is distributed separately because it is approximately 418 MB.
Run `scripts/download-wanxiang-grammar.ps1` to download the official LTS model.
