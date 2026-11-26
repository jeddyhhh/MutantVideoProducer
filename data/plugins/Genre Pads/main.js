// Genre Pads: eight named effect chains per style, onto eight Live pads.
// A chain is a list of {type: an effect id (list_effects kind=video), params: {setting: value}}, first to last.

const fx = (type, params) => ({ type: type, params: params || {} });

const GENRES = {
    ambient: [
        ["Haze", [fx("glow", { threshold: 0.3, intensity: 1.2 }), fx("gaussian_blur")]],
        ["Dusk", [fx("gradient_map", { palette: "Gold" }), fx("vignette")]],
        ["Ice", [fx("gradient_map", { palette: "Ice" }), fx("glow")]],
        ["Drift", [fx("trails")]],
        ["Mirror", [fx("mirror_symmetry"), fx("glow")]],
        ["Sepia", [fx("sepia"), fx("noise")]],
        ["Soft Tint", [fx("tint", { color: "#6a8cff", amount: 0.35 })]],
        ["Dream", [fx("glow", { threshold: 0.2, intensity: 1.6 }), fx("chromatic_aberration")]]
    ],
    techno: [
        ["Strobe Split", [fx("rgb_split", { jitter: 20 })]],
        ["Mirror", [fx("mirror_symmetry")]],
        ["Kaleido", [fx("kaleidoscope")]],
        ["Acid", [fx("gradient_map", { palette: "Acid" })]],
        ["Edges", [fx("neon_edges")]],
        ["Tiles", [fx("tile")]],
        ["Posterize", [fx("posterize"), fx("glow")]],
        ["Glitch", [fx("block_glitch"), fx("rgb_split")]]
    ],
    punk: [
        ["Xerox", [fx("halftone")]],
        ["Mono", [fx("black_white"), fx("posterize")]],
        ["VHS", [fx("vhs")]],
        ["Sort", [fx("pixel_sort")]],
        ["Old Film", [fx("old_film")]],
        ["Shake", [fx("rgb_split", { jitter: 40 })]],
        ["Dither", [fx("dither")]],
        ["Invert", [fx("invert"), fx("noise")]]
    ],
    retro: [
        ["VHS", [fx("vhs")]],
        ["CRT", [fx("crt")]],
        ["NES", [fx("vg_nes_palette")]],
        ["Game Boy", [fx("vg_gb_lines")]],
        ["CGA", [fx("vg_cga_palette")]],
        ["Glitch City", [fx("vg_gb_glitchcity")]],
        ["ASCII", [fx("ascii")]],
        ["Pixels", [fx("pixelate"), fx("dither")]]
    ],
    psychedelic: [
        ["Kaleido", [fx("kaleidoscope"), fx("glow")]],
        ["Melt", [fx("melt")]],
        ["Rainbow", [fx("gradient_map", { palette: "Rainbow" })]],
        ["Mirror", [fx("mirror_symmetry"), fx("chromatic_aberration")]],
        ["Datamosh", [fx("datamosh")]],
        ["Solarize", [fx("solarize"), fx("glow")]],
        ["Trails", [fx("trails"), fx("kaleidoscope")]],
        ["Thermal", [fx("gradient_map", { palette: "Thermal" })]]
    ]
};

const args = mvp.args;
const chains = GENRES[args.genre];
const first = Number(args.first);
let filled = 0;
for (let i = 0; i < chains.length; i++) {
    const [name, effects] = chains[i];
    mvp.call("set_pad", { pad: first + i, name: name, effects: effects, momentary: args.momentary });
    filled++;
}
mvp.log("Genre Pads: " + filled + " " + args.genre + " pads from pad " + first);
