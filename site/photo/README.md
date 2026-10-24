# The board photo

`board-source.png` is the uncropped shot, 1536×1024. It is kept here rather
than in `public/` so it is not shipped with the site — only the crop is.

`public/board.jpg` is derived from it, cropped so the **board's own centre is
the image centre**. That is what lets the page centre the board with nothing
but `place-items: center`, with no offset to tune:

```bash
sips -c 996 1494 --cropOffset 28 38 photo/board-source.png -s format jpeg -s formatOptions 82 --out public/board.jpg
```

The numbers come from measuring the board in the source: it spans x 260–1309
and y 205–847, so its centre is (784.5, 526). The crop is the largest 3:2 box
centred on that point.

**Replacing the photo** means redoing that measurement and then updating, in
order:

1. the crop above, so the board is centred in the new image;
2. `src/hotspots.ts` — `dot`, `chip` and `zone` for all six call-outs, as
   percentages of the new crop;
3. `--board-top` / `--board-bottom` in `src/style.css`, the band boundaries,
   which are where the board's top and bottom edges fall in percent;
4. `Y` in `src/ui/tour.ts` and the `viewBox` in `index.html`, but only if the
   aspect ratio stops being 3:2.
