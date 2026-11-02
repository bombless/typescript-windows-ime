# Wanxiang dictionary attribution

This project includes selected dictionary tables from **amzxyz/rime-wanxiang-data**
(branch `wanxiang`): `zi`, `duoyin`, `cuoyin`, `mingren`, and `yiren`.

- Author/project: amzxyz, [Rime Wanxiang data](https://github.com/amzxyz/rime-wanxiang-data)
- License: [Creative Commons Attribution 4.0 International (CC BY 4.0)](https://creativecommons.org/licenses/by/4.0/)
- Changes made for this project: tone marks are removed from pinyin codes to fit
  this IME's untoned full-pinyin input; `ü` is mapped to the conventional `v` key.
  Entry text and source weights are retained.

This is a **partial dictionary integration**, not the complete Wanxiang Base
dictionary. The large `jichu` table is not bundled, and the Rime Lua grammar
model, user database, and other Rime runtime features are not implemented here.