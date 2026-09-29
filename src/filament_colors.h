#pragma once

struct FilamentColor {
    const char *name;
    const char *hex;      // "#RRGGBB", uppercase — must match exactly what every tag_parse_*() formats back
    uint32_t    rgb;
    bool        light_bg; // true = use black label text (this swatch is light)
};

static const FilamentColor FILAMENT_COLORS[] = {
    {"Jade White",       "#FFFFFF", 0xFFFFFF, true},
    {"Beige",            "#F7E6DE", 0xF7E6DE, true},
    {"Light Gray",       "#D1D3D5", 0xD1D3D5, true},
    {"Silver",           "#A6A9AA", 0xA6A9AA, true},
    {"Gray",             "#8E9089", 0x8E9089, true},
    {"Magenta",          "#EC008C", 0xEC008C, true},
    {"Pink",             "#F55A74", 0xF55A74, true},
    {"Hot Pink",         "#F5547C", 0xF5547C, true},
    {"Orange",           "#FF6A13", 0xFF6A13, true},
    {"Pumpkin Orange",   "#FF9016", 0xFF9016, true},
    {"Gold",             "#E4BD68", 0xE4BD68, true},
    {"Sunflower Yellow", "#FEC600", 0xFEC600, true},
    {"Yellow",           "#F4EE2A", 0xF4EE2A, true},
    {"Bright Green",     "#BECF00", 0xBECF00, true},
    {"Bambu Green",      "#00AE42", 0x00AE42, true},
    {"Mistletoe Green",  "#3F8E43", 0x3F8E43, true},
    {"Bronze",           "#847D48", 0x847D48, true},
    {"Cocoa Brown",      "#6F5034", 0x6F5034, false},
    {"Brown",            "#9D432C", 0x9D432C, false},
    {"Maroon Red",       "#9D2235", 0x9D2235, false},
    {"Red",              "#C12E1F", 0xC12E1F, false},
    {"Turquoise",        "#00B1B7", 0x00B1B7, true},
    {"Cyan",             "#0086D6", 0x0086D6, true},
    {"Blue",             "#0A2989", 0x0A2989, false},
    {"Cobalt Blue",      "#0056B8", 0x0056B8, false},
    {"Purple",           "#5E43B7", 0x5E43B7, false},
    {"Indigo Purple",    "#482960", 0x482960, false},
    {"Blue Grey",        "#5B6579", 0x5B6579, false},
    {"Dark Gray",        "#545454", 0x545454, false},
    {"Black",            "#000000", 0x000000, false},
};
static const int FILAMENT_COLOR_COUNT = sizeof(FILAMENT_COLORS) / sizeof(FILAMENT_COLORS[0]);
