#define _POSIX_C_SOURCE 200809L
/* Heavily inspired by areofyl/gentoo.c: https://github.com/areofyl/gentoo.c */
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MASK_SIZE 96
static unsigned char arch_mask[MASK_SIZE][MASK_SIZE];

/* Official Arch emblem path: https://archlinux.org/art/ (Arch trademark). */
static const char emblem[] =
    "m 105.8125,16.625 c -7.39687,18.135158 -11.858304,29.997682 -20.09375,47.59375 "
    "5.04936,5.35232 11.247211,11.585364 21.3125,18.625 "
    "C 96.210077,78.390904 88.828713,73.920352 83.3125,69.28125 "
    "72.7727,91.274163 56.259864,122.60209 22.75,182.8125 "
    "49.087628,167.60733 69.504089,158.23318 88.53125,154.65625 "
    "87.714216,151.1422 87.2497,147.34107 87.28125,143.375 "
    "l 0.03125,-0.84375 c 0.417917,-16.87382 9.195665,-29.84979 19.59375,-28.96875 "
    "10.39809,0.88104 18.48041,15.28242 18.0625,32.15625 "
    "-0.0786,3.17512 -0.43674,6.22955 -1.0625,9.0625 "
    "18.82058,3.68164 39.01873,13.03179 65,28.03125 "
    "-5.123,-9.4318 -9.69572,-17.93388 -14.0625,-26.03125 "
    "-6.87839,-5.33121 -14.05289,-12.2698 -28.6875,-19.78125 "
    "10.05899,2.61375 17.2611,5.62932 22.875,9 "
    "C 124.63297,63.338161 121.03766,52.354109 105.8125,16.625 z";

static float path_number(const char **cursor) {
    while (**cursor == ',' || isspace((unsigned char)**cursor)) ++*cursor;
    char *end;
    float value = strtof(*cursor, &end);
    if (end == *cursor) { fputs("arch: invalid emblem geometry\n", stderr); exit(1); }
    *cursor = end;
    return value;
}

static void build_mask(void) {
    struct { float x, y; } vertices[1024];
    size_t count = 0;
    float x = 0, y = 0;
    char command = 'm';
    const char *cursor = emblem;
    while (*cursor) {
        while (isspace((unsigned char)*cursor) || *cursor == ',') ++cursor;
        if (!*cursor) break;
        if (isalpha((unsigned char)*cursor)) command = *cursor++;
        if (command == 'z') break;
        bool relative = islower((unsigned char)command) != 0;
        float v[6];
        int numbers = tolower((unsigned char)command) == 'c' ? 6 : 2;
        for (int i = 0; i < numbers; ++i)
            v[i] = path_number(&cursor) + (relative ? (i % 2 ? y : x) : 0);
        if (count + 24 >= sizeof(vertices)/sizeof(vertices[0])) exit(1);
        if (numbers == 6) {
            for (int step = 1; step <= 24; ++step) {
                float t = (float)step/24, u = 1-t;
                vertices[count].x = u*u*u*x + 3*u*u*t*v[0] + 3*u*t*t*v[2] + t*t*t*v[4];
                vertices[count++].y = u*u*u*y + 3*u*u*t*v[1] + 3*u*t*t*v[3] + t*t*t*v[5];
            }
            x = v[4]; y = v[5];
        } else {
            x = v[0]; y = v[1];
            vertices[count].x = x; vertices[count++].y = y;
        }
    }
    unsigned char inside[MASK_SIZE][MASK_SIZE] = {{0}};
    for (int row = 0; row < MASK_SIZE; ++row) {
        for (int col = 0; col < MASK_SIZE; ++col) {
            float px = 22 + ((float)col+.5f)*168/MASK_SIZE;
            float py = 16 + ((float)row+.5f)*168/MASK_SIZE;
            bool filled = false;
            for (size_t i = 0, j = count-1; i < count; j = i++) {
                float ax = vertices[i].x, ay = vertices[i].y;
                float bx = vertices[j].x, by = vertices[j].y;
                if ((ay > py) != (by > py) && px < (bx-ax)*(py-ay)/(by-ay)+ax)
                    filled = !filled;
            }
            inside[row][col] = (unsigned char)filled;
        }
    }
    for (int row = 0; row < MASK_SIZE; ++row) {
        for (int col = 0; col < MASK_SIZE; ++col) {
            if (!inside[row][col]) continue;
            float distance = 8;
            for (int dy = -8; dy <= 8; ++dy) {
                for (int dx = -8; dx <= 8; ++dx) {
                    int nx = col+dx, ny = row+dy;
                    if (nx < 0 || ny < 0 || nx >= MASK_SIZE || ny >= MASK_SIZE || !inside[ny][nx])
                        distance = fminf(distance, sqrtf((float)(dx*dx+dy*dy)));
                }
            }
            arch_mask[row][col] = (unsigned char)lroundf(distance*16);
        }
    }
}

#define VERSION "1.0.0"
#define MAX_POINTS (MASK_SIZE * MASK_SIZE * 14)
#define PI 3.14159265358979323846

typedef struct { float x, y, z; } Vec;
typedef struct { Vec position, normal; } Point;
typedef struct { float depth; unsigned char shade; char glyph; } Cell;
typedef struct {
    int cols, rows, height;
    Cell *cells;
    char *output;
    size_t capacity;
} Canvas;
typedef struct {
    int fps, palette;
    long frames;
    double speed;
    bool snapshot, hud, color;
} Options;

static Point points[MAX_POINTS];
static size_t point_count;
static struct termios saved_termios;
static bool terminal_changed, screen_entered;
static volatile sig_atomic_t stop_signal, resized = 1, suspend_requested;
static Canvas canvas;
static const char *palette_names[] = {"arch", "ice", "rose", "mono"};
static const float palette_rgb[][3] = {
    {23, 147, 209}, {118, 215, 235}, {196, 167, 231}, {220, 225, 235}
};

static float clampf(float value, float low, float high) {
    return fminf(high, fmaxf(low, value));
}
static Vec normalize(Vec v) {
    float length = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (length < 1e-6f) return (Vec){0, 0, 1};
    return (Vec){v.x/length, v.y/length, v.z/length};
}
static float dot(Vec a, Vec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static Vec rotate(Vec p, float ca, float sa, float cb, float sb) {
    float y = p.y*ca - p.z*sa, z = p.y*sa + p.z*ca;
    return (Vec){p.x*cb + z*sb, y, -p.x*sb + z*cb};
}
static float distance_at(int x, int y) {
    if (x < 0 || y < 0 || x >= MASK_SIZE || y >= MASK_SIZE) return 0;
    return (float)arch_mask[y][x] / 16.0f;
}
static float face_height(int x, int y) {
    float t = clampf(distance_at(x, y) / 4.0f, 0, 1);
    return .035f + .105f * t*t*(3 - 2*t);
}
static void add_point(Vec position, Vec normal) {
    if (point_count >= MAX_POINTS) {
        fputs("arch: geometry capacity exceeded\n", stderr);
        exit(EXIT_FAILURE);
    }
    points[point_count++] = (Point){position, normalize(normal)};
}
static void build_points(void) {
    build_mask();
    const float step = 2.6f / MASK_SIZE;
    for (int y = 0; y < MASK_SIZE; ++y) {
        for (int x = 0; x < MASK_SIZE; ++x) {
            float d = distance_at(x, y);
            if (d == 0) continue;
            float px = ((float)x + .5f - MASK_SIZE*.5f)*step;
            float py = (MASK_SIZE*.5f - (float)y - .5f)*step;
            float z = face_height(x, y);
            float dx = (face_height(x+1,y) - face_height(x-1,y)) / (2*step);
            float dy = (face_height(x,y-1) - face_height(x,y+1)) / (2*step);
            add_point((Vec){px, py, -z}, (Vec){-dx, -dy, -1});
            add_point((Vec){px, py, z}, (Vec){-dx, -dy, 1});
            /* Only the silhouette needs side samples; no hidden solid voxels. */
            if (d <= 1.5f) {
                Vec outward = normalize((Vec){distance_at(x-1,y)-distance_at(x+1,y),
                                              distance_at(x,y+1)-distance_at(x,y-1), 0});
                for (int k = 1; k < 10; ++k) {
                    float t = (float)k/10;
                    add_point((Vec){px, py, z*(2*t-1)}, outward);
                }
            }
        }
    }
}

static void restore_terminal(void) {
    if (screen_entered) {
        fputs("\033[0m\033[?25h\033[?1049l", stdout);
        fflush(stdout);
        screen_entered = false;
    }
    if (terminal_changed) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        terminal_changed = false;
    }
}
static void cleanup(void) {
    restore_terminal();
    free(canvas.cells);
    free(canvas.output);
}
static void signal_handler(int sig) {
    /* Signal handlers only set flags: no stdio, allocation, or termios calls. */
    if (sig == SIGWINCH) resized = 1;
    else if (sig == SIGTSTP) suspend_requested = 1;
    else stop_signal = sig;
}
static bool enter_terminal(void) {
    if (isatty(STDIN_FILENO)) {
        struct termios raw;
        if (tcgetattr(STDIN_FILENO, &saved_termios) == -1) return false;
        raw = saved_termios;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) return false;
        terminal_changed = true;
    }
    screen_entered = true;
    fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout);
    return fflush(stdout) == 0;
}
static double now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("arch: clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}
static void sleep_until(double deadline) {
    struct timespec ts = {.tv_sec = (time_t)deadline,
                          .tv_nsec = (long)((deadline-floor(deadline))*1e9)};
    /* Interrupts wake the main loop promptly for resize, suspend, and exit. */
    (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}
static bool resize_canvas(bool hud, bool snapshot) {
    struct winsize ws = {0};
    int cols = 80, rows = 24;
    if (!snapshot && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col) cols = ws.ws_col;
        if (ws.ws_row) rows = ws.ws_row;
    }
    /* Bound memory even if a terminal reports a pathological size. */
    if (cols > 400) cols = 400;
    if (rows > 160) rows = 160;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    size_t count = (size_t)cols*(size_t)rows;
    size_t capacity = count*40 + (size_t)rows*16 + 1024;
    Cell *cells = calloc(count, sizeof(*cells));
    char *output = malloc(capacity);
    if (!cells || !output) {
        free(cells); free(output);
        return false;
    }
    free(canvas.cells); free(canvas.output);
    int height = rows - (hud && rows >= 12 ? 2 : 0);
    canvas = (Canvas){.cols=cols, .rows=rows, .height=height,
                      .cells=cells, .output=output, .capacity=capacity};
    if (!snapshot) fputs("\033[2J", stdout);
    return true;
}
static void render(double phase, double yaw_offset, double pitch_offset) {
    size_t count = (size_t)canvas.cols*(size_t)canvas.rows;
    for (size_t i = 0; i < count; ++i) canvas.cells[i] = (Cell){0, 0, ' '};
    float pitch = (float)(-.06 + .16*sin(phase*.55) + pitch_offset);
    /* Dwell near the recognizable front/back; pass the thin edge quickly. */
    float yaw = (float)(phase - .30*sin(2*phase) + yaw_offset);
    float ca = cosf(pitch), sa = sinf(pitch), cb = cosf(yaw), sb = sinf(yaw);
    const float camera = 6, radius = 1.86f;
    float vertical = (float)canvas.height * .44f;
    const float aspect = 2.0f;
    float horizontal = (float)canvas.cols * .44f / aspect;
    float scale = fminf(vertical, horizontal)*sqrtf(camera*camera-radius*radius)/1.45f;
    const Vec light = {-.3578f, .7155f, -.6000f};
    const Vec fill = {.8081f, .3030f, .5051f};
    const Vec half_light = {-.2000f, .4000f, -.8944f};
    const char ramp[] = ".:-=+*#%@";
    const int ramp_last = (int)sizeof(ramp)-2;
    for (size_t i = 0; i < point_count; ++i) {
        Vec p = rotate(points[i].position, ca, sa, cb, sb);
        Vec n = rotate(points[i].normal, ca, sa, cb, sb);
        float depth = 1 / (p.z + camera);
        int x = (int)lroundf((float)canvas.cols*.5f + scale*aspect*p.x*depth);
        int y = (int)lroundf((float)canvas.height*.5f - scale*p.y*depth);
        if (x < 0 || y < 0 || x >= canvas.cols || y >= canvas.height) continue;
        Cell *cell = &canvas.cells[(size_t)y*(size_t)canvas.cols+(size_t)x];
        if (depth <= cell->depth) continue;
        float diffuse = fmaxf(0, dot(n, light));
        float rim = fmaxf(0, dot(n, fill));
        float spec = fmaxf(0, dot(n, half_light));
        spec *= spec; spec *= spec; spec *= spec; spec *= spec;
        float brightness = clampf(.18f + .58f*diffuse + .14f*rim + .38f*spec, 0, 1);
        *cell = (Cell){depth, (unsigned char)lroundf(brightness*23),
                       ramp[(int)lroundf(brightness*(float)ramp_last)]};
    }
}
static bool present(const Options *options, bool paused, bool snapshot) {
    char colors[24][32];
    for (int i = 0; i < 24; ++i) {
        float light = (float)i/23;
        float mix = clampf((light-.75f)*1.4f, 0, .35f);
        int rgb[3];
        for (int c = 0; c < 3; ++c) {
            float base = palette_rgb[options->palette][c]*(.30f + .70f*light);
            rgb[c] = (int)clampf(base*(1-mix) + 225*mix, 0, 255);
        }
        (void)snprintf(colors[i], sizeof(colors[i]), "\033[38;2;%d;%d;%dm", rgb[0], rgb[1], rgb[2]);
    }
    char *out = canvas.output;
    if (!snapshot) { memcpy(out, "\033[H", 3); out += 3; }
    for (int y = 0; y < canvas.rows; ++y) {
        int previous = -1;
        for (int x = 0; x < canvas.cols; ++x) {
            Cell cell = canvas.cells[(size_t)y*(size_t)canvas.cols+(size_t)x];
            if (options->color && cell.glyph != ' ' && cell.shade != previous) {
                size_t length = strlen(colors[cell.shade]);
                memcpy(out, colors[cell.shade], length); out += length;
                previous = cell.shade;
            }
            *out++ = cell.glyph;
        }
        if (options->color) { memcpy(out, "\033[0m", 4); out += 4; }
        if (y+1 < canvas.rows) {
            if (!snapshot) *out++ = '\r';
            *out++ = '\n';
        }
    }
    if (!snapshot && options->hud && canvas.rows >= 12) {
        char status[256];
        if (canvas.cols >= 110)
            (void)snprintf(status, sizeof(status), "ARCH / 3D   %s   %.2fx   %s   Space pause | +/- speed | C palette | HJKL orbit | R reset | Q quit",
                           paused ? "PAUSED" : "LIVE", options->speed, palette_names[options->palette]);
        else if (canvas.cols >= 55)
            (void)snprintf(status, sizeof(status), "ARCH / 3D   %.2fx   %s   Space pause  +/- speed  C color  Q quit",
                           options->speed, paused ? "PAUSED" : palette_names[options->palette]);
        else
            (void)snprintf(status, sizeof(status), "ARCH / 3D   %s   Q quit", paused ? "PAUSED" : "LIVE");
        size_t length = strlen(status);
        if (length > (size_t)canvas.cols) length = (size_t)canvas.cols;
        int padding = (canvas.cols-(int)length)/2;
        int written = snprintf(out, canvas.capacity-(size_t)(out-canvas.output),
                               "\033[%d;%dH", canvas.rows, padding+1);
        if (written < 0) return false;
        out += written;
        if (options->color) {
            const char *muted = "\033[38;2;144;140;170m";
            size_t length_muted = strlen(muted);
            memcpy(out, muted, length_muted); out += length_muted;
        }
        memcpy(out, status, length); out += length;
        if (options->color) { memcpy(out, "\033[0m", 4); out += 4; }
    }
    if (snapshot) *out++ = '\n';
    size_t size = (size_t)(out-canvas.output);
    return fwrite(canvas.output, 1, size, stdout) == size && fflush(stdout) == 0;
}
static void usage(FILE *stream) {
    fputs("arch - a beveled, rotating Arch Linux emblem for your terminal\n\n"
          "Usage: arch [options]\n"
          "  --fps N            Frame rate, 1..120 (default 60)\n"
          "  --speed N          Rotation speed, 0.05..4 (default 1)\n"
          "  --palette NAME     arch, ice, rose, or mono\n"
          "  --no-color         Disable colors\n"
          "  --no-hud           Hide the control strip\n"
          "  --frames N         Stop after N frames\n"
          "  --snapshot         One plain 80x24 frame; also used when piped\n"
          "  --help             Show this help\n"
          "  --version          Show version\n\n"
          "Keys: Space pause, +/- speed, C palette, HJKL orbit, R reset, Q/Esc quit.\n"
          "Ctrl+C exits cleanly; Ctrl+Z suspends and restores the terminal.\n", stream);
}
static double parse_number(const char *argument, double low, double high, const char *flag) {
    errno = 0;
    char *end;
    double value = strtod(argument, &end);
    if (errno || *argument == '\0' || *end != '\0' || !isfinite(value) || value < low || value > high) {
        fprintf(stderr, "arch: %s expects a number between %g and %g\n", flag, low, high);
        exit(EXIT_FAILURE);
    }
    return value;
}
static Options parse_options(int argc, char **argv) {
    Options options = {.fps=60, .palette=0, .frames=0, .speed=1, .snapshot=false, .hud=true,
                       .color=isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL};
    for (int i = 1; i < argc; ++i) {
        const char *flag = argv[i];
        if (!strcmp(flag, "--help") || !strcmp(flag, "-h")) { usage(stdout); exit(0); }
        else if (!strcmp(flag, "--version")) { puts("arch " VERSION); exit(0); }
        else if (!strcmp(flag, "--no-color")) options.color = false;
        else if (!strcmp(flag, "--no-hud")) options.hud = false;
        else if (!strcmp(flag, "--snapshot")) options.snapshot = true;
        else if (!strcmp(flag, "--fps") || !strcmp(flag, "--speed") || !strcmp(flag, "--frames") || !strcmp(flag, "--palette")) {
            if (++i >= argc) { fprintf(stderr, "arch: missing value for %s\n", flag); exit(1); }
            if (!strcmp(flag, "--speed")) options.speed = parse_number(argv[i], .05, 4, flag);
            else if (!strcmp(flag, "--palette")) {
                int palette = 0;
                while (palette < 4 && strcmp(argv[i], palette_names[palette])) ++palette;
                if (palette == 4) { fputs("arch: palette must be arch, ice, rose, or mono\n", stderr); exit(1); }
                options.palette = palette;
            } else {
                double limit = !strcmp(flag, "--fps") ? 120 : INT_MAX;
                double value = parse_number(argv[i], 1, limit, flag);
                if (floor(value) != value) { fprintf(stderr, "arch: %s requires a whole number\n", flag); exit(1); }
                if (!strcmp(flag, "--fps")) options.fps = (int)value;
                else options.frames = (long)value;
            }
        } else { fprintf(stderr, "arch: unknown option '%s' (try --help)\n", flag); exit(1); }
    }
    if (!isatty(STDOUT_FILENO) || (getenv("TERM") && !strcmp(getenv("TERM"), "dumb"))) options.snapshot = true;
    if (options.snapshot) { options.color = false; options.hud = false; }
    return options;
}
int main(int argc, char **argv) {
    Options options = parse_options(argc, argv);
    if (atexit(cleanup) != 0) return EXIT_FAILURE;
    struct sigaction action = {.sa_handler=signal_handler};
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGWINCH, SIGTSTP};
    for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); ++i)
        if (sigaction(signals[i], &action, NULL) == -1) { perror("arch: sigaction"); return 1; }
    (void)signal(SIGPIPE, SIG_IGN);
    build_points();
    if (!options.snapshot && !enter_terminal()) { perror("arch: terminal setup"); return 1; }
    double last = now_seconds(), deadline = last, phase = 0, yaw = 0, pitch = 0;
    bool paused = false;
    long frame = 0;
    while (!stop_signal) {
        if (suspend_requested) {
            suspend_requested = 0;
            restore_terminal();
            raise(SIGSTOP);
            if (!enter_terminal()) { perror("arch: terminal resume"); return 1; }
            resized = 1;
            last = deadline = now_seconds();
        }
        if (resized) {
            resized = 0;
            if (!resize_canvas(options.hud, options.snapshot)) { fputs("arch: out of memory\n", stderr); return 1; }
        }
        if (!options.snapshot && terminal_changed) {
            struct pollfd input = {.fd=STDIN_FILENO, .events=POLLIN};
            if (poll(&input, 1, 0) > 0 && (input.revents & POLLIN)) {
                char keys[64];
                ssize_t length = read(STDIN_FILENO, keys, sizeof(keys));
                for (ssize_t i = 0; i < length; ++i) {
                    switch (keys[i]) {
                    case 'q': case 'Q': case 27: goto done;
                    case ' ': paused = !paused; break;
                    case '+': case '=': options.speed = fmin(4, options.speed*1.2); break;
                    case '-': case '_': options.speed = fmax(.05, options.speed/1.2); break;
                    case 'c': case 'C': options.palette = (options.palette+1)%4; break;
                    case 'h': yaw -= .18; break;
                    case 'l': yaw += .18; break;
                    case 'j': pitch -= .12; break;
                    case 'k': pitch += .12; break;
                    case 'r': case 'R': phase = yaw = pitch = 0; options.speed = 1; paused = false; break;
                    default: break;
                    }
                }
            }
        }
        double now = now_seconds();
        if (!paused && frame != 0) phase = fmod(phase + fmin(now-last, .1)*options.speed*.75, 2*PI*20);
        last = now;
        render(phase, yaw, pitch);
        if (!present(&options, paused, options.snapshot)) return 1;
        ++frame;
        if (options.snapshot || (options.frames > 0 && frame >= options.frames)) break;
        deadline += 1.0/options.fps;
        if (deadline < now) deadline = now;
        sleep_until(deadline);
    }
done:
    return stop_signal ? 128+stop_signal : 0;
}
