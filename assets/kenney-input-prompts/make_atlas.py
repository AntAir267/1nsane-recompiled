"""Builds xbox_prompts.png, the button icons the port shows in the game's texts
(runtime/prompts.c), from Kenney's Input Prompts pack (CC0, see License.txt):

    make_atlas.py <kenney_input_prompts folder> [out.png]

8 x 4 cells of 64 px in the order of ICONS (cell k = icon k), made from the 128 px icons and
filtered down (the game shows them about 12-40 px high and its texture has no mipmaps). Each
icon gets the look of the game's menu font: a dark outline and its cut-out letters filled dark,
so it reads over any background. Needs Pillow."""
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter

ICONS = [  # font codes 0x80.. (0x8c is skipped: the French texts use it), Xbox Series/Double
    'xbox_button_color_a', 'xbox_button_color_b', 'xbox_button_color_x', 'xbox_button_color_y',
    'xbox_lb', 'xbox_rb', 'xbox_lt', 'xbox_rt',
    'xbox_button_view', 'xbox_button_menu', 'xbox_ls', 'xbox_rs',
    'xbox_dpad_up', 'xbox_dpad_down', 'xbox_dpad_left', 'xbox_dpad_right',
    'xbox_dpad', 'xbox_dpad_vertical', 'xbox_dpad_horizontal',
    'xbox_stick_l', 'xbox_stick_l_horizontal', 'xbox_stick_l_vertical',
    'xbox_stick_r', 'xbox_stick_r_horizontal', 'xbox_stick_r_vertical',
    'xbox_guide', 'xbox_button_share',
]
CELL, COLS, ROWS = 128, 8, 4
OUT = 64                            # cell size in the atlas
OUTLINE = 5                         # px at 128: about the font's outline at its glyph height
DARK = (16, 16, 16, 255)


def styled(im):
    alpha = im.getchannel('A').point(lambda a: 255 if a > 96 else 0)
    # silhouette: everything the outside cannot reach (fills the letters cut out of a button)
    outside = Image.new('L', (CELL + 2, CELL + 2), 0)
    outside.paste(alpha, (1, 1))
    ImageDraw.floodfill(outside, (0, 0), 128)
    sil = outside.point(lambda v: 0 if v == 128 else 255).crop((1, 1, CELL + 1, CELL + 1))
    ring = sil.filter(ImageFilter.MaxFilter(2 * OUTLINE + 1)).filter(ImageFilter.GaussianBlur(0.8))
    out = Image.new('RGBA', (CELL, CELL), (0, 0, 0, 0))
    dark = Image.new('RGBA', (CELL, CELL), DARK)
    out.paste(dark, (0, 0), ImageChops.lighter(ring, sil))
    out.alpha_composite(im)
    return out


def main():
    src = sys.argv[1].rstrip('/') + '/Xbox Series/Double/'
    out = sys.argv[2] if len(sys.argv) > 2 else 'xbox_prompts.png'
    atlas = Image.new('RGBA', (OUT * COLS, OUT * ROWS), (0, 0, 0, 0))
    for k, name in enumerate(ICONS):
        im = Image.open(src + name + '.png').convert('RGBA')
        assert im.size == (CELL, CELL), (name, im.size)
        small = styled(im).convert('RGBa').resize((OUT, OUT), Image.LANCZOS).convert('RGBA')
        atlas.paste(small, ((k % COLS) * OUT, (k // COLS) * OUT))
    atlas.save(out, optimize=True)
    print(out, len(ICONS), 'icons')


if __name__ == '__main__':
    main()
