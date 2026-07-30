# Translation catalogs

`*.po` here are the **sources**. `bin/resources/<locale>/cemu.mo` are the compiled catalogs
the app actually loads, and they are committed too because nothing in the build pipeline
requires `gettext` to be installed.

## Why these files exist

They did not, until the TesseraEmu rebrand needed them. Upstream Cemu keeps its `.po` sources
in a separate `Cemu-Language` repository and a bot compiled them into this tree — the last such
commit here is `40d9664`, 2024-12-07. That bot does not exist for this fork, so the `.mo` files
were the only surviving copy of ~13,000 translated strings and the next msgid change would have
been unrepairable. They were recovered with `msgunfmt` and committed.

## Editing

```sh
brew install gettext
# after changing any _() string in src/, refresh the template:
find src \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | xargs xgettext --from-code=utf-8 \
    -w 100 --keyword=_ --keyword=wxTRANSLATE --keyword="wxPLURAL:1,2" --keyword=_tr \
    --keyword=TR_NOOP --check=space-ellipsis --omit-header -o cemu.pot
msgmerge --update --backup=none translations/fr.po cemu.pot   # then translate the new entries
cmake --build build --target translations                     # recompile every .mo
```

CI checks that every committed `.mo` is exactly what its `.po` compiles to, so the two cannot
drift apart silently.

## The catalog is named `cemu`, deliberately

`CemuApp::LocalizeUI` loads the catalog domain `"cemu"` and every `.mo` file is named
`cemu.mo`. Renaming the domain would orphan all 19 catalogs for no benefit; the domain is an
implementation detail nobody sees.

## Two things that were broken here for years

- **Arabic never loaded at all.** Its file was committed as `‏‏cemu.mo` — two U+200F
  RIGHT-TO-LEFT MARK characters before the name. wxWidgets looks for the literal `cemu.mo`, so
  the language was never offered in the settings list despite 821 translated strings.
- **The Arabic `.mo` also had no header entry**, so it declared no charset. Every gettext tool
  rejected it with `invalid multibyte sequence` and dropped all 17,000 Arabic code units on
  read. The data was intact the whole time; only the metadata was missing. Both are fixed.

Because `msgunfmt` could not read it, Arabic's `.po` had to be extracted by reading the `.mo`
directly. Two things that costs you, both of which bit here before being fixed: `.mo` stores a
plural entry as `singular\0plural` → `form0\0form1`, so a naive extractor writes raw NUL bytes
into the `.po` and destroys the two plural entries; and gettext needs a `Plural-Forms:` header
to accept them back. The header this file now declares — `nplurals=2; plural=(n != 1)` — is
gettext's own default, which is what the catalog was silently running under when it had no
header at all.
