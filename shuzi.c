/*
 * shuzi - a small, fast image viewer for Linux
 *
 * Features:
 *   - Opens PNG, JPEG, GIF, WEBP, BMP, TIFF, etc. (anything SDL2_image supports)
 *   - Pan (drag with mouse, or arrow keys), zoom (wheel or +/-), fit-to-window
 *   - Browse a whole folder: next/previous image
 *   - Fullscreen toggle, checkerboard background for transparent images
 *
 * Usage:
 *   ./shuzi photo.png                 # open one image (folder is scanned for next/prev)
 *   ./shuzi a.jpg b.png c.gif         # open several
 *   ./shuzi ~/Pictures/               # open every image in a folder
 */

#define _POSIX_C_SOURCE 200809L

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

typedef struct {
    char **paths;
    size_t count;
    size_t cap;
} ImageList;

static void list_init(ImageList *l) {
    l->paths = NULL;
    l->count = 0;
    l->cap = 0;
}

static void list_push(ImageList *l, const char *path) {
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        char **np = realloc(l->paths, ncap * sizeof(*np));
        if (!np) { fprintf(stderr, "out of memory\n"); exit(1); }
        l->paths = np;
        l->cap = ncap;
    }
    l->paths[l->count++] = strdup(path);
}

static void list_free(ImageList *l) {
    for (size_t i = 0; i < l->count; i++) free(l->paths[i]);
    free(l->paths);
    list_init(l);
}

/* recognised by extension (SDL2_image can do more, but this keeps folder
 * scans sane and avoids trying to decode random files) */
static int has_image_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *exts[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp",
        ".tif", ".tiff", ".tga", ".pnm", ".ppm", ".pgm",
        ".xpm", ".xcf", ".ico", ".cur", ".qoi", ".jxl", ".avif", NULL
    };
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* scan a directory, append sorted image paths */
static void scan_dir(ImageList *l, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return; }

    ImageList tmp; list_init(&tmp);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!has_image_ext(e->d_name)) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        list_push(&tmp, full);
    }
    closedir(d);

    qsort(tmp.paths, tmp.count, sizeof(*tmp.paths), cmp_str);
    for (size_t i = 0; i < tmp.count; i++) list_push(l, tmp.paths[i]);
    list_free(&tmp);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* directory part of a path, into buf */
static void dirname_of(const char *path, char *buf, size_t n) {
    const char *slash = strrchr(path, '/');
    if (!slash) { snprintf(buf, n, "."); return; }
    size_t len = (size_t)(slash - path);
    if (len == 0) len = 1; /* root */
    if (len >= n) len = n - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;      
    SDL_Texture  *checker;  
    int img_w, img_h;       

    double scale;           
    double pan_x, pan_y;    
    int fit;                
    int fullscreen;
} Viewer;

static SDL_Texture *make_checker(SDL_Renderer *ren) {
    const int T = 16;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, T * 2, T * 2, 32,
                                                    SDL_PIXELFORMAT_RGBA8888);
    if (!s) return NULL;
    Uint32 a = SDL_MapRGB(s->format, 0x99, 0x99, 0x99);
    Uint32 b = SDL_MapRGB(s->format, 0x66, 0x66, 0x66);
    SDL_Rect r;
    r.w = T; r.h = T;
    r.x = 0;   r.y = 0;   SDL_FillRect(s, &r, a);
    r.x = T;   r.y = 0;   SDL_FillRect(s, &r, b);
    r.x = 0;   r.y = T;   SDL_FillRect(s, &r, b);
    r.x = T;   r.y = T;   SDL_FillRect(s, &r, a);
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    return t;
}

static void window_size(Viewer *v, int *w, int *h) {
    SDL_GetRendererOutputSize(v->ren, w, h);
}

static void do_fit(Viewer *v) {
    int ww, wh;
    window_size(v, &ww, &wh);
    if (v->img_w <= 0 || v->img_h <= 0) return;
    double sx = (double)ww / v->img_w;
    double sy = (double)wh / v->img_h;
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;             
    v->scale = s;
    v->pan_x = (ww - v->img_w * s) / 2.0;
    v->pan_y = (wh - v->img_h * s) / 2.0;
}

static void center_at_scale(Viewer *v, double s) {
    int ww, wh;
    window_size(v, &ww, &wh);
    v->scale = s;
    v->pan_x = (ww - v->img_w * s) / 2.0;
    v->pan_y = (wh - v->img_h * s) / 2.0;
}

static void zoom_at(Viewer *v, double factor, int cx, int cy) {
    double ns = v->scale * factor;
    if (ns < 0.02) ns = 0.02;
    if (ns > 64.0) ns = 64.0;
    v->pan_x = cx - (cx - v->pan_x) * (ns / v->scale);
    v->pan_y = cy - (cy - v->pan_y) * (ns / v->scale);
    v->scale = ns;
    v->fit = 0;
}

static void update_title(Viewer *v, const ImageList *l, size_t idx) {
    char title[1024];
    snprintf(title, sizeof(title), "%s  [%zu/%zu]  %dx%d  %.0f%%",
             basename_of(l->paths[idx]), idx + 1, l->count,
             v->img_w, v->img_h, v->scale * 100.0);
    SDL_SetWindowTitle(v->win, title);
}

static int load_image(Viewer *v, const ImageList *l, size_t idx) {
    SDL_Texture *t = IMG_LoadTexture(v->ren, l->paths[idx]);
    if (!t) {
        fprintf(stderr, "cannot load %s: %s\n", l->paths[idx], IMG_GetError());
        return -1;
    }
    if (v->tex) SDL_DestroyTexture(v->tex);
    v->tex = t;
    SDL_QueryTexture(t, NULL, NULL, &v->img_w, &v->img_h);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    v->fit = 1;
    do_fit(v);
    update_title(v, l, idx);
    return 0;
}

static void set_idle_title(Viewer *v) {
    SDL_SetWindowTitle(v->win,
        "shuzi \xE2\x80\x94 drop an image here, or press O to open  (Q to quit)");
}

static void render_dropzone(Viewer *v) {
    int ww, wh;
    window_size(v, &ww, &wh);

    int bw = ww * 6 / 10, bh = wh * 5 / 10;
    if (bw > 520) bw = 520;
    if (bh > 360) bh = 360;
    int bx = (ww - bw) / 2, by = (wh - bh) / 2;

    SDL_SetRenderDrawColor(v->ren, 0x40, 0x40, 0x40, 0xff);
    const int dash = 14, gap = 10, thick = 3;
    for (int x = bx; x < bx + bw; x += dash + gap) {
        SDL_Rect a = { x, by, dash, thick };
        SDL_Rect b = { x, by + bh - thick, dash, thick };
        SDL_RenderFillRect(v->ren, &a);
        SDL_RenderFillRect(v->ren, &b);
    }
    for (int y = by; y < by + bh; y += dash + gap) {
        SDL_Rect a = { bx, y, thick, dash };
        SDL_Rect b = { bx + bw - thick, y, thick, dash };
        SDL_RenderFillRect(v->ren, &a);
        SDL_RenderFillRect(v->ren, &b);
    }

    int cx = ww / 2, cy = wh / 2, s = 48, t = 8;
    SDL_SetRenderDrawColor(v->ren, 0x5a, 0x5a, 0x5a, 0xff);
    SDL_Rect hbar = { cx - s / 2, cy - t / 2, s, t };
    SDL_Rect vbar = { cx - t / 2, cy - s / 2, t, s };
    SDL_RenderFillRect(v->ren, &hbar);
    SDL_RenderFillRect(v->ren, &vbar);
}

static void push_selection(ImageList *out, char *line, int *got) {
    char *start = line;
    for (char *p = line; ; p++) {
        if (*p == '|' || *p == '\0') {
            char end = *p;
            *p = '\0';
            if (*start) {
                if (is_dir(start)) scan_dir(out, start);
                else               list_push(out, start);
                *got = 1;
            }
            start = p + 1;
            if (end == '\0') break;
        }
    }
}

static int run_dialog(ImageList *out) {
    static const char *cmds[] = {
        "zenity --file-selection --multiple --title='Open image(s)' "
            "--file-filter='Images | *.png *.jpg *.jpeg *.gif *.bmp *.webp "
            "*.tif *.tiff *.tga *.ppm *.pgm *.xpm *.ico *.qoi *.jxl *.avif' "
            "--file-filter='All files | *' 2>/dev/null",
        "kdialog --getopenfilename . "
            "'Images (*.png *.jpg *.jpeg *.gif *.bmp *.webp *.tif *.tiff)' "
            "2>/dev/null",
        "Xdialog --stdout --title 'Open image' --fselect \"$HOME/\" 0 0 2>/dev/null",
        NULL
    };
    for (int i = 0; cmds[i]; i++) {
        FILE *fp = popen(cmds[i], "r");
        if (!fp) continue;
        char line[8192];
        int got = 0;
        while (fgets(line, sizeof line, fp)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
                line[--n] = '\0';
            push_selection(out, line, &got);
        }
        pclose(fp);
        if (got) return 1;
    }
    return 0;
}

static long load_from(Viewer *v, ImageList *l, size_t start) {
    if (l->count == 0) return -1;
    size_t t = start % l->count, tries = 0;
    while (load_image(v, l, t) != 0) {
        t = (t + 1) % l->count;
        if (++tries >= l->count) return -1;
    }
    return (long)t;
}

static long adopt_list(Viewer *v, ImageList *dst, ImageList *src) {
    if (src->count == 0) return -1;
    list_free(dst);
    *dst = *src;
    list_init(src);
    return load_from(v, dst, 0);
}

static void render(Viewer *v) {
    int ww, wh;
    window_size(v, &ww, &wh);

    SDL_SetRenderDrawColor(v->ren, 0x1e, 0x1e, 0x1e, 0xff);
    SDL_RenderClear(v->ren);

    if (!v->tex) { render_dropzone(v); SDL_RenderPresent(v->ren); return; }

    SDL_Rect dst;
    dst.x = (int)(v->pan_x + 0.5);
    dst.y = (int)(v->pan_y + 0.5);
    dst.w = (int)(v->img_w * v->scale + 0.5);
    dst.h = (int)(v->img_h * v->scale + 0.5);

    if (v->checker) {
        SDL_Rect clip = dst;
        if (clip.x < 0) { clip.w += clip.x; clip.x = 0; }
        if (clip.y < 0) { clip.h += clip.y; clip.y = 0; }
        if (clip.x + clip.w > ww) clip.w = ww - clip.x;
        if (clip.y + clip.h > wh) clip.h = wh - clip.y;
        if (clip.w > 0 && clip.h > 0) {
            SDL_RenderSetClipRect(v->ren, &clip);
            int tw, th;
            SDL_QueryTexture(v->checker, NULL, NULL, &tw, &th);
            for (int y = clip.y; y < clip.y + clip.h; y += th)
                for (int x = clip.x; x < clip.x + clip.w; x += tw) {
                    SDL_Rect tile = { x, y, tw, th };
                    SDL_RenderCopy(v->ren, v->checker, NULL, &tile);
                }
            SDL_RenderSetClipRect(v->ren, NULL);
        }
    }

    SDL_ScaleMode mode = (v->scale >= 2.0) ? SDL_ScaleModeNearest
                                           : SDL_ScaleModeLinear;
    SDL_SetTextureScaleMode(v->tex, mode);
    SDL_RenderCopy(v->ren, v->tex, NULL, &dst);

    SDL_RenderPresent(v->ren);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s [image|directory ...]\n"
        "  with no arguments, opens an empty window:\n"
        "  drag an image onto it, or press O to pick files.\n\n"
        "keys:\n"
        "  q / Esc        quit            o            open file chooser\n"
        "  n / Right / Spc next image      p / Left / Bksp  previous image\n"
        "  Home / End     first / last image\n"
        "  + / = / wheel  zoom in          - / wheel        zoom out\n"
        "  0              100%% (actual size)\n"
        "  f / Enter      fit to window\n"
        "  arrows         pan (also drag with the mouse)\n"
        "  F11            toggle fullscreen\n",
        prog);
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    ImageList list; list_init(&list);
    size_t start_idx = 0;

    if (argc < 2) {
    } else if (argc == 2 && is_dir(argv[1])) {
        scan_dir(&list, argv[1]);
    } else if (argc == 2) {
        char dir[4096];
        dirname_of(argv[1], dir, sizeof(dir));
        scan_dir(&list, dir);
        const char *want = basename_of(argv[1]);
        int found = 0;
        for (size_t i = 0; i < list.count; i++) {
            if (strcmp(basename_of(list.paths[i]), want) == 0) {
                start_idx = i; found = 1; break;
            }
        }
        if (!found) { list_free(&list); list_init(&list); list_push(&list, argv[1]); }
    } else {
        for (int i = 1; i < argc; i++) {
            if (is_dir(argv[i])) scan_dir(&list, argv[i]);
            else                 list_push(&list, argv[i]);
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_TIF | IMG_INIT_WEBP;
    IMG_Init(img_flags);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    Viewer v;
    memset(&v, 0, sizeof(v));
    v.scale = 1.0;

    v.win = SDL_CreateWindow("shuzi",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             1000, 700,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!v.win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return 1; }

    v.ren = SDL_CreateRenderer(v.win, -1,
                               SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!v.ren) v.ren = SDL_CreateRenderer(v.win, -1, 0);
    if (!v.ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return 1; }

    SDL_SetRenderDrawBlendMode(v.ren, SDL_BLENDMODE_BLEND);
    v.checker = make_checker(v.ren);

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    size_t idx = start_idx;
    if (list.count > 0) {
        long got = load_from(&v, &list, start_idx);
        if (got < 0) {
            fprintf(stderr, "no loadable images; opening empty window\n");
            set_idle_title(&v);
        } else {
            idx = (size_t)got;
        }
    } else {
        set_idle_title(&v);
    }

    int running = 1;
    int dragging = 0;
    int collecting = 0; 
    ImageList incoming; list_init(&incoming);
    while (running) {
        SDL_Event ev;
        if (!SDL_WaitEvent(&ev)) break;

        do {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    if (v.fit) do_fit(&v);
                }
                break;

            case SDL_KEYDOWN: {
                int ww, wh;
                window_size(&v, &ww, &wh);
                int step = 60;
                size_t cnt = list.count;
                size_t newidx = idx;
                switch (ev.key.keysym.sym) {
                case SDLK_q:
                case SDLK_ESCAPE:
                    running = 0; break;

                case SDLK_o: {
                    ImageList sel; list_init(&sel);
                    if (run_dialog(&sel)) {
                        long got = adopt_list(&v, &list, &sel);
                        if (got >= 0) idx = (size_t)got;
                    } else {
                        list_free(&sel);
                    }
                    break;
                }

                case SDLK_n:
                case SDLK_SPACE:
                case SDLK_PAGEDOWN:
                    if (cnt) newidx = (idx + 1) % cnt; break;
                case SDLK_p:
                case SDLK_BACKSPACE:
                case SDLK_PAGEUP:
                    if (cnt) newidx = (idx + cnt - 1) % cnt; break;
                case SDLK_HOME:
                    if (cnt) newidx = 0; break;
                case SDLK_END:
                    if (cnt) newidx = cnt - 1; break;

                case SDLK_RIGHT:
                    if (v.fit && cnt) newidx = (idx + 1) % cnt;
                    else v.pan_x -= step;
                    break;
                case SDLK_LEFT:
                    if (v.fit && cnt) newidx = (idx + cnt - 1) % cnt;
                    else v.pan_x += step;
                    break;
                case SDLK_UP:    v.pan_y += step; v.fit = 0; break;
                case SDLK_DOWN:  v.pan_y -= step; v.fit = 0; break;

                case SDLK_EQUALS:
                case SDLK_PLUS:
                case SDLK_KP_PLUS:
                    if (v.tex) zoom_at(&v, 1.25, ww / 2, wh / 2); break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    if (v.tex) zoom_at(&v, 1.0 / 1.25, ww / 2, wh / 2); break;

                case SDLK_0:
                case SDLK_KP_0:
                    if (v.tex) { center_at_scale(&v, 1.0); v.fit = 0; } break;

                case SDLK_f:
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    if (v.tex) { v.fit = 1; do_fit(&v); } break;

                case SDLK_F11:
                    v.fullscreen = !v.fullscreen;
                    SDL_SetWindowFullscreen(v.win,
                        v.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    if (v.fit) do_fit(&v);
                    break;
                }

                if (cnt && newidx != idx) {
                    size_t tried = newidx;
                    int forward = (newidx == (idx + 1) % cnt) ||
                                  (newidx > idx) || newidx == 0;
                    while (load_image(&v, &list, tried) != 0) {
                        tried = forward ? (tried + 1) % cnt
                                        : (tried + cnt - 1) % cnt;
                        if (tried == idx) break;
                    }
                    idx = tried;
                } else if (v.tex && cnt) {
                    update_title(&v, &list, idx);
                }
                break;
            }

            case SDL_MOUSEWHEEL: {
                if (v.tex) {
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    double f = ev.wheel.y > 0 ? 1.15 : (1.0 / 1.15);
                    /* respect natural/inverted wheel */
                    if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) f = 1.0 / f;
                    zoom_at(&v, f, mx, my);
                    if (list.count) update_title(&v, &list, idx);
                }
                break;
            }

            case SDL_DROPBEGIN:
                list_free(&incoming);
                list_init(&incoming);
                collecting = 1;
                break;

            case SDL_DROPFILE: {
                char *f = ev.drop.file;
                if (f) {
                    if (!collecting) {
                        list_free(&incoming);
                        list_init(&incoming);
                    }
                    if (is_dir(f)) scan_dir(&incoming, f);
                    else           list_push(&incoming, f);
                    SDL_free(f);
                    if (!collecting) {
                        long got = adopt_list(&v, &list, &incoming);
                        if (got >= 0) idx = (size_t)got;
                    }
                }
                break;
            }

            case SDL_DROPCOMPLETE: {
                collecting = 0;
                long got = adopt_list(&v, &list, &incoming);
                if (got >= 0) idx = (size_t)got;
                break;
            }

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = 1;
                    v.fit = 0;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) dragging = 0;
                break;
            case SDL_MOUSEMOTION:
                if (dragging) {
                    v.pan_x += ev.motion.xrel;
                    v.pan_y += ev.motion.yrel;
                }
                break;
            }
        } while (SDL_PollEvent(&ev));

        render(&v);
    }

    if (v.tex)     SDL_DestroyTexture(v.tex);
    if (v.checker) SDL_DestroyTexture(v.checker);
    SDL_DestroyRenderer(v.ren);
    SDL_DestroyWindow(v.win);
    IMG_Quit();
    SDL_Quit();
    list_free(&list);
    list_free(&incoming);
    return 0;
}
