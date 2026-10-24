# Reference material

## The panel datasheet

`GDEY0213Z98-datasheet.pdf` is **not in this repository**: it is GoodDisplay's
document, and redistributing it is theirs to allow, not ours. It is also 9 MB,
which is most of what a clone would weigh.

Download it from GoodDisplay's product page for the **GDEY0213Z98** (2.13",
122x250, SSD1680) and drop it here — `.gitignore` already expects that name, so
it will not be committed by accident:

    docs/reference/GDEY0213Z98-datasheet.pdf

What the firmware actually relies on from it is quoted where it is used, so the
code reads without the PDF in hand:

- command `0x22` (Display Update Control 2) and `0x32` (Write LUT register) —
  [`esp32/src/DisplayDriver.cpp`](../../esp32/src/DisplayDriver.cpp)
- the two RAM planes `0x24` / `0x26` and how their meaning flips with the loaded
  waveform — [`../eink-gdey0213z98.md`](../eink-gdey0213z98.md)

## The bring-up sketch

`eink-hybrid-experiment.cpp` is the standalone sketch that produced the timings
in [`../eink-gdey0213z98.md`](../eink-gdey0213z98.md): tri-colour vs monochrome
refresh, and the softened LUT the firmware ships. It is kept verbatim, comments
in French, because it is the evidence behind those numbers — it is not built by
any environment in `platformio.ini`.
