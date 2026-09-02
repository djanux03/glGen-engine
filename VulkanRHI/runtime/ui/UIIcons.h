#pragma once

// Font Awesome 7 Free Solid glyphs used by the editor UI.
//
// Every codepoint below was verified against the cmap of
// assets/fonts/Font Awesome 7 Free-Solid-900.otf before being written here,
// so none of them render as a tofu box. If you add one, check it against
// that font first -- FA moves codepoints between major versions.
//
// UIFonts.h merges that OTF over the text font across kIconMin..kIconMax.

namespace UIIcons {
inline constexpr unsigned int kIconMin = 0xF002;
inline constexpr unsigned int kIconMax = 0xF72E;
} // namespace UIIcons

#define ICON_PLAY           "\xef\x81\x8b" // U+F04B
#define ICON_PAUSE          "\xef\x81\x8c" // U+F04C
#define ICON_STOP           "\xef\x81\x8d" // U+F04D
#define ICON_STEP           "\xef\x81\x91" // U+F051
#define ICON_MOVE           "\xef\x82\xb2" // U+F0B2
#define ICON_ROTATE         "\xef\x8b\xb1" // U+F2F1
#define ICON_SCALE          "\xef\x81\xa5" // U+F065
#define ICON_COMPRESS       "\xef\x81\xa6" // U+F066
#define ICON_CUBE           "\xef\x86\xb2" // U+F1B2
#define ICON_CUBES          "\xef\x86\xb3" // U+F1B3
#define ICON_LIGHT          "\xef\x83\xab" // U+F0EB
#define ICON_SUN            "\xef\x86\x85" // U+F185
#define ICON_CLOUD          "\xef\x83\x82" // U+F0C2
#define ICON_MOUNTAIN       "\xef\x9b\xbc" // U+F6FC
#define ICON_DROPLET        "\xef\x81\x83" // U+F043
#define ICON_FIRE           "\xef\x81\xad" // U+F06D
#define ICON_EYE            "\xef\x81\xae" // U+F06E
#define ICON_GEAR           "\xef\x80\x93" // U+F013
#define ICON_SEARCH         "\xef\x80\x82" // U+F002
#define ICON_FOLDER         "\xef\x81\xbb" // U+F07B
#define ICON_FOLDER_OPEN    "\xef\x81\xbc" // U+F07C
#define ICON_TRASH          "\xef\x87\xb8" // U+F1F8
#define ICON_COPY           "\xef\x83\x85" // U+F0C5
#define ICON_CLONE          "\xef\x89\x8d" // U+F24D
#define ICON_PLUS           "\xef\x81\xa7" // U+F067
#define ICON_CLOSE          "\xef\x80\x8d" // U+F00D
#define ICON_TERMINAL       "\xef\x84\xa0" // U+F120
#define ICON_CHART          "\xef\x82\x80" // U+F080
#define ICON_GAUGE          "\xef\x98\xa5" // U+F625
#define ICON_BUG            "\xef\x86\x88" // U+F188
#define ICON_LAYERS         "\xef\x97\xbd" // U+F5FD
#define ICON_INFO           "\xef\x81\x9a" // U+F05A
#define ICON_WARNING        "\xef\x81\xb1" // U+F071
#define ICON_FILE           "\xef\x85\x9b" // U+F15B
#define ICON_SAVE           "\xef\x83\x87" // U+F0C7
#define ICON_BRUSH          "\xef\x87\xbc" // U+F1FC
#define ICON_MAGIC          "\xef\x83\x90" // U+F0D0
#define ICON_GLOBE          "\xef\x82\xac" // U+F0AC
#define ICON_CAMERA         "\xef\x80\xb0" // U+F030
#define ICON_PERSON         "\xef\x86\x83" // U+F183
#define ICON_SEEDLING       "\xef\x93\x98" // U+F4D8
#define ICON_TREE           "\xef\x86\xbb" // U+F1BB
#define ICON_SLIDERS        "\xef\x87\x9e" // U+F1DE
#define ICON_LIST           "\xef\x80\xba" // U+F03A
#define ICON_BARS           "\xef\x83\x89" // U+F0C9
#define ICON_CHECK          "\xef\x80\x8c" // U+F00C
#define ICON_CHEVRON_RIGHT  "\xef\x81\x94" // U+F054
#define ICON_CHEVRON_DOWN   "\xef\x81\xb8" // U+F078
#define ICON_CIRCLE         "\xef\x84\x91" // U+F111
#define ICON_LOCK           "\xef\x80\xa3" // U+F023
#define ICON_LINK           "\xef\x83\x81" // U+F0C1
#define ICON_CROSSHAIRS     "\xef\x81\x9b" // U+F05B
#define ICON_MAP            "\xef\x89\xb9" // U+F279
#define ICON_REFRESH        "\xef\x80\xa1" // U+F021
#define ICON_FILM           "\xef\x80\x88" // U+F008
#define ICON_PALETTE        "\xef\x94\xbf" // U+F53F
#define ICON_WIND           "\xef\x9c\xae" // U+F72E
#define ICON_POLYGON        "\xef\x97\xae" // U+F5EE
#define ICON_GRID           "\xef\x80\x8a" // U+F00A
#define ICON_SQUARE         "\xef\x83\x88" // U+F0C8
