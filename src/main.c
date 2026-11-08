/* Gyre & Gimble for Android and iOS (and desktop, for testing): the SDL
 * platform layer. It shows the X16's 320x240 screen, scaled to whole pixels,
 * with on-screen buttons beside it (landscape) or below it (portrait) in
 * place of the X16's Return, U, R, M and Q keys. Touches on the screen go
 * to the gesture recognizer (touch.c); game controllers are read as the
 * X16 reads SNES pads. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "game.h"
#include "gg_data.h"
#include "music.h"
#include "touch.h"
#include "vera.h"

#if defined(__ANDROID__) || defined(__IPHONEOS__)
#define MOBILE 1
#else
#define MOBILE 0
#endif

#define TICK_HZ 60
#define QUIT_CONFIRM_MS 3000
#define STICK_DEADZONE 16000

typedef struct { float x, y, w, h; } Rect;

enum { BTN_UNDO, BTN_RESET, BTN_NEXT, BTN_MUSIC, BTN_QUIT, NBUTTONS };

typedef struct {
    Rect r;
    bool enabled;
    char label[16];
} Button;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screen_tex, *font_tex;
static SDL_Texture *sharp_tex;  /* the screen at a whole multiple, for fractional scales */
static int sharp_k;
static SDL_AudioDeviceID audio_dev;
static uint32_t frame[SCREEN_W * SCREEN_H];

static int out_w, out_h;        /* renderer output, in pixels */
static Rect game_rect;          /* where the X16 screen goes */
static float game_scale;
static Button buttons[NBUTTONS];

static bool running = true, paused;
static Uint32 quit_armed_until;
static char save_path[1024];

/* Each finger (or the mouse, as finger -1) is pressing a button, turning
 * the gesture recognizer, or ignored */
#define MAX_POINTERS 10
typedef struct { SDL_FingerID id; int button; bool gesture; bool used; } Pointer;
static Pointer pointers[MAX_POINTERS];
static bool gesture_busy;

static SDL_GameController *pads[8];

/* --- audio ------------------------------------------------------------- */

static void lock_audio(void) { if (audio_dev) SDL_LockAudioDevice(audio_dev); }
static void unlock_audio(void) { if (audio_dev) SDL_UnlockAudioDevice(audio_dev); }

static void audio_callback(void *user, Uint8 *stream, int len)
{
    (void)user;
    audio_render((int16_t *)stream, len / 4);
}

static void open_audio(void)
{
    SDL_AudioSpec want = {0}, have;
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!audio_dev) {
        SDL_Log("No audio: %s", SDL_GetError());
        return;
    }
    audio_init(have.freq, music_tick);
    audio_set_lock(lock_audio, unlock_audio);
    SDL_PauseAudioDevice(audio_dev, 0);
}

/* --- saved progress ------------------------------------------------------ */

static void save_progress(void)
{
    uint8_t buf[256];
    int n = game_save(buf, sizeof buf);
    if (!save_path[0] || !n)
        return;
    SDL_RWops *f = SDL_RWFromFile(save_path, "wb");
    if (f) {
        SDL_RWwrite(f, buf, 1, n);
        SDL_RWclose(f);
    }
}

static void load_progress(void)
{
    char *dir = SDL_GetPrefPath("SlithyMatt", "GyreAndGimble");
    if (!dir)
        return;
    snprintf(save_path, sizeof save_path, "%sprogress.dat", dir);
    SDL_free(dir);
    SDL_RWops *f = SDL_RWFromFile(save_path, "rb");
    if (f) {
        uint8_t buf[256];
        size_t n = SDL_RWread(f, buf, 1, sizeof buf);
        SDL_RWclose(f);
        game_load(buf, (int)n);
    }
}

/* --- layout -------------------------------------------------------------- */

static void stack_buttons(Rect col, const int *ids, int n)
{
    float gap = col.h * 0.04f;
    float bw = col.w * 0.86f;
    float bh = (col.h - gap * (n - 1)) / n;
    float cap = (out_w < out_h ? out_w : out_h) * 0.16f;
    if (bh > cap)
        bh = cap;
    float top = col.y + (col.h - (bh * n + gap * (n - 1))) / 2;
    for (int i = 0; i < n; i++) {
        Rect *r = &buttons[ids[i]].r;
        r->w = bw;
        r->h = bh;
        r->x = col.x + (col.w - bw) / 2;
        r->y = top + i * (bh + gap);
    }
}

/* The scale at which the 320x240 screen just fits w x h */
static float fit_scale(float w, float h)
{
    return w / SCREEN_W < h / SCREEN_H ? w / SCREEN_W : h / SCREEN_H;
}

static void layout(void)
{
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    static const int left[] = {BTN_UNDO, BTN_RESET};
    static const int right[] = {BTN_NEXT, BTN_MUSIC, BTN_QUIT};
    float W = (float)out_w, H = (float)out_h;
    if (W >= H) {
        /* landscape: the screen fills the height, with buttons in columns
         * either side, clear of any notch */
        float inset = W * 0.03f, side = H * 0.18f;
        game_scale = fit_scale(W - 2 * inset - 2 * side, H);
        game_rect.w = SCREEN_W * game_scale;
        game_rect.h = SCREEN_H * game_scale;
        game_rect.x = (W - game_rect.w) / 2;
        game_rect.y = (H - game_rect.h) / 2;
        float col_w = game_rect.x - inset;
        stack_buttons((Rect){inset, H * 0.1f, col_w, H * 0.8f}, left, 2);
        stack_buttons((Rect){game_rect.x + game_rect.w, H * 0.1f, col_w, H * 0.8f}, right, 3);
    } else {
        /* portrait: the screen fills the width, with two columns of buttons
         * below */
        float top = H * 0.05f, bottom = H * 0.04f, below = W * 0.5f;
        game_scale = fit_scale(W, H - top - bottom - below);
        game_rect.w = SCREEN_W * game_scale;
        game_rect.h = SCREEN_H * game_scale;
        game_rect.x = (W - game_rect.w) / 2;
        float gap = H * 0.02f;
        float h = W * 0.75f;    /* the buttons' area below the screen */
        float spare = H - top - bottom - game_rect.h - gap;
        if (h > spare)
            h = spare;
        /* center the screen and buttons together */
        game_rect.y = top + (spare - h) / 2;
        float y = game_rect.y + game_rect.h + gap;
        stack_buttons((Rect){0, y, W / 2, h}, left, 2);
        stack_buttons((Rect){W / 2, y, W / 2, h}, right, 3);
    }
}

static void update_buttons(void)
{
    Uint32 now = SDL_GetTicks();
    bool armed = quit_armed_until && !SDL_TICKS_PASSED(now, quit_armed_until);
    if (!armed)
        quit_armed_until = 0;
    buttons[BTN_UNDO].enabled = game_can_undo();
    buttons[BTN_RESET].enabled = !game_finished();
    buttons[BTN_NEXT].enabled = game_can_advance();
    buttons[BTN_MUSIC].enabled = true;
    buttons[BTN_QUIT].enabled = true;
    strcpy(buttons[BTN_UNDO].label, "Undo");
    strcpy(buttons[BTN_RESET].label, "Reset");
    strcpy(buttons[BTN_NEXT].label, "Next");
    strcpy(buttons[BTN_MUSIC].label, game_music_off() ? "Music off" : "Music on");
    strcpy(buttons[BTN_QUIT].label, armed ? "Sure?" : "Quit");
}

static bool in_rect(Rect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int button_at(float x, float y)
{
    for (int i = 0; i < NBUTTONS; i++)
        if (in_rect(buttons[i].r, x, y))
            return i;
    return -1;
}

/* --- actions ------------------------------------------------------------- */

static void request_quit(void)
{
    if (quit_armed_until) {
        running = false;
    } else {
        quit_armed_until = SDL_GetTicks() + QUIT_CONFIRM_MS;
        if (!quit_armed_until)
            quit_armed_until = 1;
    }
}

static void key(int k)
{
    if (game_key(k))
        running = false;
}

static void press_button(int b)
{
    if (b < 0 || !buttons[b].enabled)
        return;
    switch (b) {
    case BTN_UNDO: key(KEY_DEL); break;
    case BTN_RESET: key('r'); break;
    case BTN_NEXT: key(KEY_RETURN); break;
    case BTN_MUSIC: key('m'); break;
    case BTN_QUIT: request_quit(); break;
    }
    if (b != BTN_QUIT)
        quit_armed_until = 0;
}

/* --- pointers -------------------------------------------------------------- */

static Pointer *find_pointer(SDL_FingerID id, bool create)
{
    for (int i = 0; i < MAX_POINTERS; i++)
        if (pointers[i].used && pointers[i].id == id)
            return &pointers[i];
    if (!create)
        return NULL;
    for (int i = 0; i < MAX_POINTERS; i++)
        if (!pointers[i].used) {
            pointers[i] = (Pointer){id, -1, false, true};
            return &pointers[i];
        }
    return NULL;
}

static void to_game(float x, float y, float *gx, float *gy)
{
    *gx = (x - game_rect.x) / game_scale;
    *gy = (y - game_rect.y) / game_scale;
}

static void pointer_down(SDL_FingerID id, float x, float y)
{
    Pointer *p = find_pointer(id, true);
    if (!p)
        return;
    p->button = button_at(x, y);
    if (p->button < 0 && !gesture_busy) {
        float gx, gy;
        to_game(x, y, &gx, &gy);
        p->gesture = gesture_busy = true;
        touch_down(gx, gy);
    }
}

static void pointer_move(SDL_FingerID id, float x, float y)
{
    Pointer *p = find_pointer(id, false);
    if (p && p->gesture) {
        float gx, gy;
        to_game(x, y, &gx, &gy);
        touch_move(gx, gy);
    }
}

static void pointer_up(SDL_FingerID id, float x, float y)
{
    Pointer *p = find_pointer(id, false);
    if (!p)
        return;
    if (p->gesture) {
        float gx, gy;
        to_game(x, y, &gx, &gy);
        touch_up(gx, gy);
        gesture_busy = false;
    } else if (p->button >= 0 && button_at(x, y) == p->button) {
        press_button(p->button);
    }
    p->used = false;
}

static void cancel_pointers(void)
{
    memset(pointers, 0, sizeof pointers);
    gesture_busy = false;
    touch_cancel();
}

/* --- controllers and keyboard ---------------------------------------------- */

static void open_pad(int index)
{
    for (int i = 0; i < 8; i++)
        if (!pads[i]) {
            pads[i] = SDL_GameControllerOpen(index);
            return;
        }
}

static void close_pad(SDL_JoystickID id)
{
    for (int i = 0; i < 8; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
        }
}

/* Every pad's buttons, merged, as SNES buttons by position (the south face
 * button is the SNES B, east is A, west Y, north X), like x16emu. */
static void read_pads(uint8_t *joy0, uint8_t *joy1)
{
    static const struct { SDL_GameControllerButton b; uint8_t byte, bit; } map[] = {
        {SDL_CONTROLLER_BUTTON_A, 0, JOY_B},
        {SDL_CONTROLLER_BUTTON_X, 0, JOY_Y},
        {SDL_CONTROLLER_BUTTON_BACK, 0, JOY_SELECT},
        {SDL_CONTROLLER_BUTTON_START, 0, JOY_START},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, 0, JOY_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0, JOY_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0, JOY_LEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0, JOY_RIGHT},
        {SDL_CONTROLLER_BUTTON_B, 1, JOY_A},
        {SDL_CONTROLLER_BUTTON_Y, 1, JOY_X},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1, JOY_L},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1, JOY_R},
    };
    uint8_t j[2] = {0, 0};
    for (int i = 0; i < 8; i++) {
        SDL_GameController *c = pads[i];
        if (!c)
            continue;
        for (size_t m = 0; m < sizeof map / sizeof map[0]; m++)
            if (SDL_GameControllerGetButton(c, map[m].b))
                j[map[m].byte] |= map[m].bit;
        /* the left stick works as a D-pad too */
        int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax < -STICK_DEADZONE) j[0] |= JOY_LEFT;
        if (ax > STICK_DEADZONE) j[0] |= JOY_RIGHT;
        if (ay < -STICK_DEADZONE) j[0] |= JOY_UP;
        if (ay > STICK_DEADZONE) j[0] |= JOY_DOWN;
    }
    *joy0 = j[0];
    *joy1 = j[1];
}

static void key_down(const SDL_KeyboardEvent *e)
{
    if (e->repeat && e->keysym.sym != SDLK_UP && e->keysym.sym != SDLK_DOWN &&
        e->keysym.sym != SDLK_LEFT && e->keysym.sym != SDLK_RIGHT)
        return;
    SDL_Keycode k = e->keysym.sym;
    switch (k) {
    case SDLK_UP: key(KEY_UP); break;
    case SDLK_DOWN: key(KEY_DOWN); break;
    case SDLK_LEFT: key(KEY_LEFT); break;
    case SDLK_RIGHT: key(KEY_RIGHT); break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: key(KEY_RETURN); break;
    case SDLK_DELETE:
    case SDLK_BACKSPACE: key(KEY_DEL); break;
    case SDLK_AC_BACK: request_quit(); break;   /* Android's back button */
    default:
        if (k >= SDLK_a && k <= SDLK_z)
            key((int)k);
    }
}

/* --- drawing -------------------------------------------------------------------- */

static void make_textures(void)
{
    if (screen_tex)
        SDL_DestroyTexture(screen_tex);
    if (font_tex)
        SDL_DestroyTexture(font_tex);
    if (sharp_tex)
        SDL_DestroyTexture(sharp_tex);
    sharp_tex = NULL;
    screen_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                   SCREEN_W, SCREEN_H);
    SDL_SetTextureScaleMode(screen_tex, SDL_ScaleModeNearest);

    uint32_t px[128 * 8 * 8];
    for (int c = 0; c < 128; c++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                px[y * 1024 + c * 8 + x] = vera.charset[c * 8 + y] & (0x80 >> x) ? 0xffffffffu : 0;
    font_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1024, 8);
    SDL_UpdateTexture(font_tex, NULL, px, 1024 * 4);
    SDL_SetTextureBlendMode(font_tex, SDL_BLENDMODE_BLEND);
}

static void set_color(uint32_t rgb)
{
    SDL_SetRenderDrawColor(renderer, rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff, 0xff);
}

static void fill(Rect r)
{
    SDL_FRect f = {r.x, r.y, r.w, r.h};
    SDL_RenderFillRectF(renderer, &f);
}

static uint8_t screen_code(char c)
{
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 1);
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 0x41);
    return (uint8_t)c & 0x3f;
}

static void draw_label(const char *s, Rect r, uint32_t rgb)
{
    int n = (int)strlen(s);
    float scale = (float)(int)(r.h * 0.42f / 8);
    float fit = (float)(int)(r.w * 0.85f / (n * 8));
    if (fit < scale)
        scale = fit;
    if (scale < 1)
        scale = 1;
    float x = r.x + (r.w - n * 8 * scale) / 2, y = r.y + (r.h - 8 * scale) / 2;
    SDL_SetTextureColorMod(font_tex, rgb >> 16 & 0xff, rgb >> 8 & 0xff, rgb & 0xff);
    for (int i = 0; i < n; i++) {
        SDL_Rect src = {screen_code(s[i]) * 8, 0, 8, 8};
        SDL_FRect dst = {x + i * 8 * scale, y, 8 * scale, 8 * scale};
        SDL_RenderCopyF(renderer, font_tex, &src, &dst);
    }
}

/* Garden palette slots (tools/shading.py's GARDEN_LAYOUT): the buttons are
 * paving in a kerb, like the HUD panels, in the level's theme */
#define GARDEN(slot) vera_rgb(ROW_GARDEN * 16 + (slot))
#define HEDGE_DARK GARDEN(16)
#define PAVING GARDEN(48 + 2)
#define PAVING_LIT GARDEN(48 + 6)
#define KERB GARDEN(56 + 5)
#define KERB_DARK GARDEN(56 + 1)

static void draw_buttons(void)
{
    for (int i = 0; i < NBUTTONS; i++) {
        Button *b = &buttons[i];
        bool held = false;
        for (int p = 0; p < MAX_POINTERS; p++)
            held |= pointers[p].used && pointers[p].button == i;
        float k = b->r.h * 0.07f;
        set_color(b->enabled ? KERB : KERB_DARK);
        fill(b->r);
        set_color(held && b->enabled ? PAVING_LIT : PAVING);
        fill((Rect){b->r.x + k, b->r.y + k, b->r.w - 2 * k, b->r.h - 2 * k});
        draw_label(b->label, b->r, vera_rgb(b->enabled ? COLOR_WHITE : COLOR_GRAY));
    }
}

/* Draw the X16 screen into game_rect. At a whole-number scale its pixels
 * are copied as they are; otherwise it's first scaled up by the next whole
 * number, then smoothly down to size, so every pixel comes out the same
 * size with soft edges a fraction of a pixel wide. */
static void draw_screen(void)
{
    SDL_FRect dst = {game_rect.x, game_rect.y, game_rect.w, game_rect.h};
    int k = (int)SDL_ceilf(game_scale - 0.001f);
    if (k > 8)
        k = 8;
    if (game_scale == (float)(int)game_scale || k < 2) {
        SDL_RenderCopyF(renderer, screen_tex, NULL, &dst);
        return;
    }
    if (!sharp_tex || sharp_k != k) {
        if (sharp_tex)
            SDL_DestroyTexture(sharp_tex);
        sharp_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                      SCREEN_W * k, SCREEN_H * k);
        sharp_k = k;
        if (!sharp_tex) {
            SDL_RenderCopyF(renderer, screen_tex, NULL, &dst);
            return;
        }
        SDL_SetTextureScaleMode(sharp_tex, SDL_ScaleModeLinear);
    }
    SDL_SetRenderTarget(renderer, sharp_tex);
    SDL_RenderCopy(renderer, screen_tex, NULL, NULL);
    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopyF(renderer, sharp_tex, NULL, &dst);
}

static void draw_scene(void)
{
    set_color(HEDGE_DARK);
    SDL_RenderClear(renderer);
    draw_screen();
    draw_buttons();
}

static void draw(void)
{
    vera_render(frame);
    SDL_UpdateTexture(screen_tex, NULL, frame, SCREEN_W * 4);
    update_buttons();
    draw_scene();
    SDL_RenderPresent(renderer);
}

static void save_screenshot(const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, out_w, out_h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return;
    draw_scene();
    SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

/* --- events --------------------------------------------------------------------- */

/* App lifecycle events need handling at once on iOS, so they're watched */
static int SDLCALL lifecycle(void *user, SDL_Event *e)
{
    (void)user;
    switch (e->type) {
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_TERMINATING:
        paused = true;
        if (audio_dev)
            SDL_PauseAudioDevice(audio_dev, 1);
        save_progress();
        break;
    case SDL_APP_DIDENTERFOREGROUND:
        paused = false;
        if (audio_dev)
            SDL_PauseAudioDevice(audio_dev, 0);
        break;
    }
    return 1;
}

static void handle_event(const SDL_Event *e)
{
    float sx = (float)out_w, sy = (float)out_h;
    switch (e->type) {
    case SDL_QUIT:
        running = false;
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            layout();
        break;
    case SDL_APP_WILLENTERBACKGROUND:
        cancel_pointers();
        break;
    case SDL_RENDER_DEVICE_RESET:
        make_textures();
        break;
    case SDL_FINGERDOWN:
        pointer_down(e->tfinger.fingerId, e->tfinger.x * sx, e->tfinger.y * sy);
        break;
    case SDL_FINGERMOTION:
        pointer_move(e->tfinger.fingerId, e->tfinger.x * sx, e->tfinger.y * sy);
        break;
    case SDL_FINGERUP:
        pointer_up(e->tfinger.fingerId, e->tfinger.x * sx, e->tfinger.y * sy);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION: {
        /* the mouse, on desktop (touches arrive as finger events) */
        if (e->button.which == SDL_TOUCH_MOUSEID)
            break;
        int ww, wh;
        SDL_GetWindowSize(window, &ww, &wh);
        float k = (float)out_w / ww;
        if (e->type == SDL_MOUSEMOTION) {
            if (e->motion.state & SDL_BUTTON_LMASK)
                pointer_move(-1, e->motion.x * k, e->motion.y * k);
        } else if (e->button.button == SDL_BUTTON_LEFT) {
            if (e->type == SDL_MOUSEBUTTONDOWN)
                pointer_down(-1, e->button.x * k, e->button.y * k);
            else
                pointer_up(-1, e->button.x * k, e->button.y * k);
        }
        break;
    }
    case SDL_KEYDOWN:
        key_down(&e->key);
        break;
    case SDL_CONTROLLERDEVICEADDED:
        open_pad(e->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        close_pad(e->cdevice.which);
        break;
    }
}

int main(int argc, char *argv[])
{
    /* Desktop testing: --touch shows the touch help text, --size WxH sets
     * the window size, --shot FILE.bmp saves the window after --wait MS
     * (default 1500) and quits, without touching saved progress, and
     * --keys STRING types X16 keys at startup (e.g. "nnn" skips to level 4) */
    HelpStyle help = MOBILE ? HELP_TOUCH : HELP_KEYBOARD;
    int win_w = 1280, win_h = 720;
    const char *shot = NULL, *keys = "";
    Uint32 wait = 1500;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--touch"))
            help = HELP_TOUCH;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc)
            wait = (Uint32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
            keys = argv[++i];
    }

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");    /* by position, like a SNES pad */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, "Gyre & Gimble");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (MOBILE)
        flags |= SDL_WINDOW_FULLSCREEN;
    window = SDL_CreateWindow("Gyre & Gimble", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              win_w, win_h, flags);
    if (!window) {
        SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_AddEventWatch(lifecycle, NULL);

    vera_init();
    music_init(true);           /* every X16 has its YM2151 */
    open_audio();
    game_init(SDL_GetTicks() ^ (Uint32)SDL_GetPerformanceCounter(), help);
    if (!shot)
        load_progress();
    for (const char *k = keys; *k; k++)
        key(*k);
    make_textures();    /* after game_init, which puts the star glyphs in the charset */
    layout();

    Uint64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
    double pending = 0;
    while (running) {
        SDL_Event e;
        if (paused) {
            if (SDL_WaitEvent(&e))
                handle_event(&e);
            last = SDL_GetPerformanceCounter();
            continue;
        }
        while (SDL_PollEvent(&e))
            handle_event(&e);

        Uint64 now = SDL_GetPerformanceCounter();
        pending += (double)(now - last) * TICK_HZ / freq;
        last = now;
        if (pending > 4)
            pending = 4;        /* after a stall, don't race to catch up */
        while (pending >= 1 && running) {
            uint8_t j0, j1;
            read_pads(&j0, &j1);
            if (game_tick(j0, j1))
                running = false;
            pending -= 1;
        }
        if (game_progress_changed() && !shot)
            save_progress();
        draw();
        if (shot && SDL_GetTicks() > wait) {
            save_screenshot(shot);
            running = false;
        }
    }
    if (!shot)
        save_progress();

    if (audio_dev) {
        lock_audio();
        music_want(NO_TUNE);
        music_silence();
        unlock_audio();
        SDL_CloseAudioDevice(audio_dev);
    }
    SDL_Quit();
    return 0;
}
