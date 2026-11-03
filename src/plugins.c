#include "plugins.h"

#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "font.h"
#include "gfx.h"
#include "ui.h"

#define DIR "ms0:/SEPLUGINS/"
#define INI DIR "boro.ini"
// ARK reads its plugin list from SEPLUGINS and from its own folder.
static const char *const lists[] = { DIR "PLUGINS.TXT", "ms0:/PSP/SAVEDATA/ARK_01234/PLUGINS.TXT" };
#define LISTS 2
#define STATUS_STATE DIR "boro_status.txt"

enum { BRIGHT, STATUS, HOLD, SNAP, PLUGINS };
static const char *const files[PLUGINS] = { "boro_bright.prx", "boro_status.prx", "boro_hold.prx", "boro_snap.prx" };

// A plugin's own row (key NULL) turns it on or off; the rows under it are its options.
typedef struct { int plugin; const char *section, *key, *label, *help; } Row;
static const Row rows[] = {
    { BRIGHT, NULL, NULL, "Better Bright", "10 brightness steps on SCREEN" },
    { BRIGHT, "bright", "full_on_battery", "Full brightness on battery", "Steps 9 and 10 without the charger. Drains it faster." },
    { BRIGHT, "bright", "bar", "Brightness bar", "Shows for a second when brightness changes" },
    { BRIGHT, "bright", "extra_dim", "Extra-dim steps", "Steps 1 and 2. Turn off if they flicker." },
    { STATUS, NULL, NULL, "Boro Status", "Battery % in the XMB, SCREEN + L and SCREEN + Triangle" },
    { STATUS, "status", "xmb", "Battery % in the XMB", "Next to the clock. SCREEN + L there changes it too." },
    { HOLD, NULL, NULL, "Hold+", "HOLD locks the PSP, with a peek and sleep guard" },
    { HOLD, "hold", "screen_off", "Turn the screen off", "2 seconds after HOLD goes on" },
    { SNAP, NULL, NULL, "Snap", "SCREEN + R saves a screenshot" },
    { SNAP, "snap", "flash", "Edge flash", "White flash around the screen when a shot is taken" },
    { SNAP, "snap", "icon", "Camera icon", "Bottom right, with a check once it's saved" },
};
#define ROW_COUNT ((int)(sizeof(rows) / sizeof(rows[0])))

static int installed[PLUGINS], enabled[PLUGINS], value[ROW_COUNT];
static float knob[ROW_COUNT];
static int sel, top, changed;

// ---- Files -------------------------------------------------------------------

static char *read_all(const char *path, int *len)
{
    *len = 0;
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int size = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    char *buf = malloc(size + 1);
    if (buf) {
        *len = sceIoRead(fd, buf, size);
        if (*len < 0) *len = 0;
        buf[*len] = 0;
    }
    sceIoClose(fd);
    return buf;
}

static void write_all(const char *path, const char *buf, int len)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, len);
    sceIoClose(fd);
}

static int exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int truthy(const char *v) { return !strncmp(v, "on", 2) || *v == '1' || !strncmp(v, "yes", 3); }

// Finds key=value in [section] and returns where the value starts, or NULL.
static char *ini_find(char *buf, const char *section, const char *key)
{
    int in = 0, klen = strlen(key);
    for (char *line = buf; line && *line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '[') {
            char *end = strchr(line, ']');
            in = end && (int)(end - line - 1) == (int)strlen(section) && !strncmp(line + 1, section, end - line - 1);
        } else if (in && !strncmp(line, key, klen) && line[klen] == '=') {
            return line + klen + 1;
        }
    }
    return NULL;
}

static const char *find_ci(const char *hay, const char *needle)
{
    int n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return hay;
    return NULL;
}

// Plugin list lines look like "game, ms0:/SEPLUGINS/boro_snap.prx, on".
static int list_enabled(const char *list, const char *file)
{
    int on = 0;
    for (const char *p = list; p && (p = find_ci(p, file)); p += strlen(file)) {
        const char *eol = strchr(p, '\n');
        const char *comma = strchr(p, ',');
        if (comma && (!eol || comma < eol)) {
            while (*++comma == ' ');
            if (truthy(comma)) on = 1;
        }
    }
    return on;
}

static void load(void)
{
    int len;
    char *list[LISTS], *ini = read_all(INI, &len);
    for (int l = 0; l < LISTS; l++) list[l] = read_all(lists[l], &len);
    for (int i = 0; i < PLUGINS; i++) {
        char path[64];
        snprintf(path, sizeof(path), DIR "%s", files[i]);
        installed[i] = exists(path);
        enabled[i] = 0;
        for (int l = 0; l < LISTS; l++) enabled[i] |= list[l] && list_enabled(list[l], files[i]);
    }
    for (int i = 0; i < ROW_COUNT; i++) {
        const Row *r = &rows[i];
        if (!r->key) value[i] = enabled[r->plugin];
        else {
            char *v = ini ? ini_find(ini, r->section, r->key) : NULL;
            value[i] = v ? truthy(v) : 1; // the plugins' own defaults
            if (!strcmp(r->key, "full_on_battery") && !v) value[i] = 0;
        }
        knob[i] = value[i];
    }
    for (int l = 0; l < LISTS; l++) free(list[l]);
    free(ini);
}

static void save_option(const Row *r, int on)
{
    int len;
    char *ini = read_all(INI, &len);
    const char *word = on ? "on" : "off";
    char *out = malloc(len + 128);
    if (!out) { free(ini); return; }
    char *v = ini ? ini_find(ini, r->section, r->key) : NULL;
    int n;
    if (v) {
        char *eol = v + strcspn(v, "\r\n");
        n = sprintf(out, "%.*s%s%s", (int)(v - ini), ini, word, eol);
    } else {
        // No such line yet: add the section and key at the end.
        n = sprintf(out, "%s\n[%s]\n%s=%s\n", ini ? ini : "", r->section, r->key, word);
    }
    write_all(INI, out, n);
    free(out);
    free(ini);
    // Status keeps its own copy of the XMB toggle once SCREEN + L has been used.
    if (!strcmp(r->key, "xmb")) {
        char *state = read_all(STATUS_STATE, &len);
        if (state && len >= 2) {
            state[1] = on ? '1' : '0';
            write_all(STATUS_STATE, state, len);
        }
        free(state);
    }
}

// Rewrites the on/off field of the plugin's lines in one list. Returns
// whether the list had any.
static int rewrite_list(const char *path, int plugin, int on)
{
    int len, n = 0, found = 0;
    char *list = read_all(path, &len);
    if (!list) return 0;
    char *out = malloc(len + 16 * 4 + 1);
    if (!out) { free(list); return 0; }
    for (char *line = list; *line;) {
        char *eol = strchr(line, '\n');
        int ll = eol ? eol - line + 1 : (int)strlen(line);
        const char *hit = find_ci(line, files[plugin]);
        if (hit && hit < line + ll) {
            // Keep everything up to the last comma, and the line's own ending.
            char *last = line + ll;
            while (last > line && (last[-1] == '\n' || last[-1] == '\r')) last--;
            char *comma = last;
            while (comma > hit && *comma != ',') comma--;
            n += sprintf(out + n, "%.*s, %s%.*s", (int)(comma - line), line, on ? "on" : "off", (int)(line + ll - last), last);
            found = 1;
        } else {
            memcpy(out + n, line, ll);
            n += ll;
        }
        line += ll;
    }
    if (found) write_all(path, out, n);
    free(out);
    free(list);
    return found;
}

// Updates the plugin's lines wherever they are; adds them to SEPLUGINS only
// when no list has them yet.
static void save_enabled(int plugin, int on)
{
    int found = 0;
    for (int l = 0; l < LISTS; l++) found |= rewrite_list(lists[l], plugin, on);
    if (found || !on) return;
    int len;
    char *list = read_all(lists[0], &len);
    char *out = malloc(len + 160);
    if (!out) { free(list); return; }
    int n = list ? sprintf(out, "%s", list) : 0;
    if (n && out[n - 1] != '\n') out[n++] = '\n';
    n += sprintf(out + n, "vsh, " DIR "%s, on\ngame, " DIR "%s, on\n", files[plugin], files[plugin]);
    write_all(lists[0], out, n);
    free(out);
    free(list);
}

// ---- Screen -------------------------------------------------------------------

#define BG HEX(0x0E0F13, 255)
#define ROW_SEL HEX(0x1B1E27, 255)
#define TEXT HEX(0xF2F2F5, 255)
#define MUTED HEX(0xA9ACB8, 255)
#define DIM HEX(0x5C5F6B, 255)
#define ACCENT HEX(0x6C8CFF, 255)
#define TRACK HEX(0x3A3D48, 255)
#define ROW_Y 36
#define ROW_H 28
#define ROWS_SHOWN 6

static Font *S(int weight, int px) { return ui_font(FAMILY_SORA, weight, px); }

// Options of a plugin that's off or missing are hidden from the cursor.
static int usable(int i)
{
    const Row *r = &rows[i];
    if (!installed[r->plugin]) return !r->key;
    return !r->key || enabled[r->plugin];
}

void plugins_enter(void)
{
    load();
    sel = top = changed = 0;
}

int plugins_on_count(void)
{
    load();
    int n = 0;
    for (int i = 0; i < PLUGINS; i++) n += installed[i] && enabled[i];
    return n;
}

int plugins_hold_on(void)
{
    load();
    return installed[HOLD] && enabled[HOLD];
}

static void step(int dir)
{
    for (int i = sel + dir; i >= 0 && i < ROW_COUNT; i += dir)
        if (usable(i)) { sel = i; return; }
}

int plugins_update(const Input *in, float dt)
{
    u32 n = in->nav, p = in->pressed;
    if (n & PSP_CTRL_UP) step(-1);
    if (n & PSP_CTRL_DOWN) step(1);
    if (sel < top) top = sel;
    if (sel >= top + ROWS_SHOWN) top = sel - ROWS_SHOWN + 1;
    const Row *r = &rows[sel];
    if (installed[r->plugin] && (p & PSP_CTRL_CROSS || n & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT))) {
        value[sel] = !value[sel];
        if (r->key) save_option(r, value[sel]);
        else {
            enabled[r->plugin] = value[sel];
            save_enabled(r->plugin, value[sel]);
        }
        changed = 1;
    }
    for (int i = 0; i < ROW_COUNT; i++) knob[i] = ui_approach(knob[i], value[i], dt, 20);
    return (p & PSP_CTRL_CIRCLE) != 0;
}

void plugins_draw(void)
{
    gfx_begin(BG);
    Font *title = S(700, 15), *label = S(700, 12), *opt = S(600, 11), *help = S(400, 9), *small = S(400, 10);
    // Plug: two prongs over a rounded body.
    gfx_rect(18, 10, 2, 6, ACCENT);
    gfx_rect(26, 10, 2, 6, ACCENT);
    ui_rrect(15, 15, 16, 10, 3, ACCENT);
    gfx_rect(22, 25, 2, 6, ACCENT);
    font_draw(title, 38, 26, "Plugins", TEXT, 0);
    ui_battery(small, SCREEN_W - 16, 20, MUTED);

    for (int r = 0, i = top; r < ROWS_SHOWN && i < ROW_COUNT; i++) {
        const Row *row = &rows[i];
        int plugin_off = !installed[row->plugin] || !enabled[row->plugin];
        if (row->key && !installed[row->plugin]) continue; // nothing to set
        float y = ROW_Y + r * (ROW_H + 2), x = row->key ? 26 : 12, w = SCREEN_W - 12 - x;
        r++;
        if (i == sel) {
            ui_rrect(x, y, w, ROW_H, 8, ROW_SEL);
            ui_ring(x, y, w, ROW_H, 8, ACCENT);
        }
        u32 tc = row->key && plugin_off ? DIM : TEXT, hc = row->key && plugin_off ? DIM : MUTED;
        font_draw(row->key ? opt : label, x + 10, y + 13, row->label, tc, 0);
        font_draw(help, x + 10, y + 24, row->help, hc, 0);
        float right = x + w - 10;
        if (!installed[row->plugin]) {
            const char *s = "Not installed";
            font_draw(S(700, 11), right - font_width(S(700, 11), s, 0), y + 18, s, DIM, 0);
        } else {
            float k = knob[i];
            u32 on = row->key && plugin_off ? TRACK : ACCENT;
            ui_rrect(right - 30, y + 6, 30, 16, 8, color_lerp(TRACK, on, k));
            ui_rrect(right - 28 + 14 * k, y + 8, 12, 12, 6, row->key && plugin_off ? DIM : 0xFFFFFFFF);
        }
    }
    if (top + ROWS_SHOWN < ROW_COUNT) font_draw_center(help, SCREEN_W / 2, ROW_Y + ROWS_SHOWN * (ROW_H + 2) + 6, "\xE2\x80\xA6", MUTED);

    Font *fh = S(400, 9);
    float hy = SCREEN_H - 8;
    font_draw(fh, 16, hy, changed ? "Restart the PSP to apply changes" : "Changes apply after a restart", changed ? ACCENT : MUTED, 0);
    float hx = SCREEN_W - 16 - ui_hint_width(fh, ICON_CIRCLE, "Back");
    ui_hint(fh, hx, hy, ICON_CIRCLE, "Back", MUTED);
    hx -= 12 + ui_hint_width(fh, ICON_CROSS, "Toggle");
    ui_hint(fh, hx, hy, ICON_CROSS, "Toggle", MUTED);
    ui_volume(S(700, 10), HEX(0x1A1C24, 240), HEX(0x2E3240, 255), ACCENT, TEXT);
    gfx_end();
}
