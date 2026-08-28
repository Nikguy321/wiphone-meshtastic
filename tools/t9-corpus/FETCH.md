# T9 source corpus

Not committed — see `.gitignore`. Refetch it before regenerating `t9_dict.h`:

```bash
mkdir -p tools/t9-corpus
curl -sSL -o tools/t9-corpus/en_50k.txt \
  https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/en/en_50k.txt
shasum -a 256 tools/t9-corpus/en_50k.txt
# expect 5351ff405b1126ef555791dd4d9798a48e3e9a501a9fc481a9da957752cfb458
```

That hash is also recorded in the provenance banner of the generated
`WiPhone/src/assets/t9_dict.h`, so a rebuild can be proven to have used the same input.

**Source:** [hermitdave/FrequencyWords](https://github.com/hermitdave/FrequencyWords),
word frequencies derived from the OpenSubtitles corpus. **Licence: MIT.**

Only an *ordered list of ordinary English words* is compiled into the firmware — no counts,
no n-grams, no contiguous source text. Regenerate with:

```bash
python3 tools/gen_t9_dict.py --freq tools/t9-corpus/en_50k.txt --limit 25000 \
  --out WiPhone/src/assets/t9_dict.h \
  --freq-name "OpenSubtitles 2018, English 50k (hermitdave/FrequencyWords, content/2018/en/en_50k.txt)" \
  --freq-url "https://github.com/hermitdave/FrequencyWords" --freq-licence "MIT"
```
