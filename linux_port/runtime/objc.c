/* Minimal Objective-C runtime + the slice of Foundation/UIKit/QuartzCore the game uses.
 *
 * Every object lives in guest memory (guest code reads ivars directly).  Classes defined
 * by the game are used straight from the binary's __objc_data; system classes are fake
 * class_t structures allocated here and implemented by host_msg().
 */
#include "rt.h"
#include <strings.h>
#include <ctype.h>
#include <SDL2/SDL.h>

/* ------------------------------------------------------------------ host classes */

enum {
    C_NSObject, C_NSString, C_NSNumber, C_NSDictionary, C_NSArray, C_NSData, C_NSBundle,
    C_NSAutoreleasePool, C_NSRunLoop, C_NSURL, C_NSLocale, C_UIView, C_UIViewController,
    C_UIDevice, C_UIScreen, C_UIImage, C_UIAccelerometer, C_UIAlertView, C_UIApplication,
    C_UIColor, C_CADisplayLink, C_CAEAGLLayer, C_EAGLContext, C_UIWindow, C_UITouch,
    C_NSSet, C_UIEvent, C_UIAcceleration, C_MFMailComposeViewController, C_COUNT
};

typedef struct {
    const char *name;
    int super;
    uint32_t cls, meta;
} HClass;

static HClass hclasses[C_COUNT] = {
    [C_NSObject] = {"NSObject", -1},
    [C_NSString] = {"NSString", C_NSObject},
    [C_NSNumber] = {"NSNumber", C_NSObject},
    [C_NSDictionary] = {"NSDictionary", C_NSObject},
    [C_NSArray] = {"NSArray", C_NSObject},
    [C_NSData] = {"NSData", C_NSObject},
    [C_NSBundle] = {"NSBundle", C_NSObject},
    [C_NSAutoreleasePool] = {"NSAutoreleasePool", C_NSObject},
    [C_NSRunLoop] = {"NSRunLoop", C_NSObject},
    [C_NSURL] = {"NSURL", C_NSObject},
    [C_NSLocale] = {"NSLocale", C_NSObject},
    [C_UIView] = {"UIView", C_NSObject},
    [C_UIViewController] = {"UIViewController", C_NSObject},
    [C_UIDevice] = {"UIDevice", C_NSObject},
    [C_UIScreen] = {"UIScreen", C_NSObject},
    [C_UIImage] = {"UIImage", C_NSObject},
    [C_UIAccelerometer] = {"UIAccelerometer", C_NSObject},
    [C_UIAlertView] = {"UIAlertView", C_UIView},
    [C_UIApplication] = {"UIApplication", C_NSObject},
    [C_UIColor] = {"UIColor", C_NSObject},
    [C_CADisplayLink] = {"CADisplayLink", C_NSObject},
    [C_CAEAGLLayer] = {"CAEAGLLayer", C_NSObject},
    [C_EAGLContext] = {"EAGLContext", C_NSObject},
    [C_UIWindow] = {"UIWindow", C_UIView},
    [C_UITouch] = {"UITouch", C_NSObject},
    [C_NSSet] = {"NSSet", C_NSObject},
    [C_UIEvent] = {"UIEvent", C_NSObject},
    [C_UIAcceleration] = {"UIAcceleration", C_NSObject},
    [C_MFMailComposeViewController] = {"MFMailComposeViewController", C_UIViewController},
};

static uint32_t hclass_lo, hclass_hi;   /* guest range holding fake class structs */

static int hclass_of_addr(uint32_t c, bool *is_meta) {
    if (c < hclass_lo || c >= hclass_hi) return -1;
    for (int i = 0; i < C_COUNT; i++) {
        if (hclasses[i].cls == c) { *is_meta = false; return i; }
        if (hclasses[i].meta == c) { *is_meta = true; return i; }
    }
    return -1;
}

static uint32_t new_obj(int cid, uint32_t size) {
    uint32_t o = gcalloc(size < 8 ? 8 : size);
    ST32(o, hclasses[cid].cls);
    return o;
}

/* ------------------------------------------------------------------ selectors */

#define SELTAB 1024
static struct { const char *name; uint32_t g; } seltab[SELTAB];

uint32_t sel_guest(const char *name) {
    unsigned h = 5381;
    for (const char *p = name; *p; p++) h = h * 33 + (unsigned char)*p;
    for (unsigned i = 0;; i++) {
        unsigned k = (h + i) % SELTAB;
        if (!seltab[k].name) { seltab[k].name = strdup(name); seltab[k].g = gstrdup(name); return seltab[k].g; }
        if (!strcmp(seltab[k].name, name)) return seltab[k].g;
    }
}

/* ------------------------------------------------------------------ strings */

uint32_t ns_string(const char *s) {
    uint32_t o = new_obj(C_NSString, 16);
    ST32(o + 4, 0x7c8);
    ST32(o + 8, gstrdup(s));
    ST32(o + 12, (uint32_t)strlen(s));
    return o;
}

const char *ns_cstr(uint32_t str) {
    if (!str) return "(null)";
    return GSTR(LD32(str + 8));
}

/* ------------------------------------------------------------------ guest class lookup */

static uint32_t objc_data_lo, objc_data_hi;

static uint32_t guest_method(uint32_t cls, const char *sel) {
    uint32_t ro = LD32(cls + 16) & ~3u;
    if (!ro) return 0;
    uint32_t ml = LD32(ro + 20);
    if (!ml) return 0;
    uint32_t es = LD32(ml) & ~3u, n = LD32(ml + 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = ml + 8 + i * es;
        if (!strcmp(GSTR(LD32(e)), sel)) return LD32(e + 8);
    }
    return 0;
}

uint32_t objc_class_named(const char *name) {
    for (uint32_t c = objc_data_lo; c < objc_data_hi; c += 20) {
        uint32_t ro = LD32(c + 16) & ~3u;
        if (ro && !(LD32(ro) & 1) && !strcmp(GSTR(LD32(ro + 16)), name)) return c;
    }
    for (int i = 0; i < C_COUNT; i++)
        if (!strcmp(hclasses[i].name, name)) return hclasses[i].cls;
    return 0;
}

static const char *class_name(uint32_t c) {
    bool m;
    int h = hclass_of_addr(c, &m);
    if (h >= 0) return hclasses[h].name;
    uint32_t ro = LD32(c + 16) & ~3u;
    return ro ? GSTR(LD32(ro + 16)) : "?";
}

/* resolution result: either a guest imp or a host class id */
typedef struct { uint32_t imp; int hcid; bool meta; } Resolved;

#define MCACHE 4096
static struct { uint32_t cls; const char *sel; Resolved r; } mcache[MCACHE];

static Resolved resolve(uint32_t cls, const char *sel) {
    unsigned k = ((cls >> 2) * 2654435761u ^ (uint32_t)(uintptr_t)sel * 40503u) % MCACHE;
    if (mcache[k].cls == cls && mcache[k].sel == sel) return mcache[k].r;
    Resolved r = {0, -1, false};
    for (uint32_t c = cls; c; c = LD32(c + 4)) {
        bool meta;
        int h = hclass_of_addr(c, &meta);
        if (h >= 0) { r.hcid = h; r.meta = meta; break; }
        uint32_t imp = guest_method(c, sel);
        if (imp) { r.imp = imp; break; }
    }
    mcache[k].cls = cls;
    mcache[k].sel = sel;
    mcache[k].r = r;
    return r;
}

/* ------------------------------------------------------------------ host state */

typedef struct {
    uint32_t obj, layer, vc_view, superview, delegate;
    float scale;
    bool view_loaded;
} ObjState;

static ObjState states[64];
static int nstates;

static ObjState *state_of(uint32_t o) {
    for (int i = 0; i < nstates; i++)
        if (states[i].obj == o) return &states[i];
    if (nstates == 64) { LOG("too many stateful objects\n"); abort(); }
    states[nstates] = (ObjState){.obj = o, .scale = 1.0f};
    return &states[nstates++];
}

static uint32_t s_device, s_screen, s_app, s_bundle, s_runloop, s_accel, s_window, s_locale;
static uint32_t cur_context;
uint32_t main_view, main_vc, app_delegate;
static uint32_t pending_modal_presenter, pending_modal_vc;

/* display links */
DLink dlinks[8];
int ndlinks;

/* accelerometer */
uint32_t accel_delegate;
double accel_interval = 1.0 / 60;

/* ------------------------------------------------------------------ messaging */

static int stret_shift;      /* 1 when dispatching objc_msgSend_stret */
#define A(i) ARG(2 + stret_shift + (i))
#define AF(i) ARGF(2 + stret_shift + (i))

static void host_msg(int cid, bool is_class, uint32_t self, const char *sel);

static void dispatch(uint32_t self, uint32_t cls, const char *sel) {
    Resolved r = resolve(cls, sel);
    if (r.imp) { call_indirect(r.imp); return; }
    if (r.hcid >= 0) { host_msg(r.hcid, r.meta, self, sel); return; }
    LOG("objc: [%s %s] not found\n", class_name(cls), sel);
    R[0] = 0;
}

void H_objc_msgSend(void) {
    uint32_t self = R[0];
    if (!self) { R[0] = R[1] = 0; return; }
    stret_shift = 0;
    dispatch(self, LD32(self), GSTR(R[1]));
}

void H_objc_msgSend_stret(void) {
    uint32_t self = R[1];
    if (!self) return;
    int saved = stret_shift;
    Resolved r = resolve(LD32(self), GSTR(R[2]));
    if (r.imp) { call_indirect(r.imp); return; }
    stret_shift = 1;
    if (r.hcid >= 0) host_msg(r.hcid, r.meta, self, GSTR(R[2]));
    else LOG("objc: stret [%s %s] not found\n", class_name(LD32(self)), GSTR(R[2]));
    stret_shift = saved;
}

void H_objc_msgSendSuper2(void) {
    uint32_t sup = R[0];
    uint32_t self = LD32(sup), cls = LD32(LD32(sup + 4) + 4);
    R[0] = self;
    stret_shift = 0;
    dispatch(self, cls, GSTR(R[1]));
}

void H_objc_setProperty(void) {
    /* objc_setProperty(self, _cmd, offset, newValue, atomic, copy) */
    ST32(R[0] + R[2], R[3]);
}

static uint32_t send_regs(uint32_t self, const char *sel, int nargs, const uint32_t *args, uint32_t *hi) {
    uint32_t a[10];
    a[0] = self;
    a[1] = sel_guest(sel);
    for (int i = 0; i < nargs && i < 8; i++) a[2 + i] = args[i];
    uint32_t saved[16];
    memcpy(saved, R, sizeof saved);
    int n = nargs + 2;
    uint32_t sp = R[13] - 64;
    sp &= ~7u;
    for (int i = 4; i < n; i++) ST32(sp + 4 * (i - 4), a[i]);
    for (int i = 0; i < 4 && i < n; i++) R[i] = a[i];
    R[13] = sp;
    int ss = stret_shift;
    if (self) { stret_shift = 0; dispatch(self, LD32(self), GSTR(a[1])); }
    else R[0] = R[1] = 0;
    stret_shift = ss;
    uint32_t r0 = R[0];
    if (hi) *hi = R[1];
    for (int i = 4; i < 16; i++) R[i] = saved[i];
    return r0;
}

uint32_t objc_send(uint32_t self, const char *sel, int nargs, const uint32_t *args, uint32_t *hi) {
    return send_regs(self, sel, nargs, args, hi);
}

bool objc_responds(uint32_t self, const char *sel) {
    if (!self) return false;
    Resolved r = resolve(LD32(self), sel);
    return r.imp != 0;
}

static uint32_t guest_alloc(uint32_t cls) {
    uint32_t ro = LD32(cls + 16) & ~3u;
    uint32_t size = LD32(ro + 8);
    uint32_t o = gcalloc(size < 64 ? 64 : size);
    ST32(o, cls);
    return o;
}

static int compare_versions(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
        if (!isdigit((unsigned char)*a) && *a) a++;
        if (!isdigit((unsigned char)*b) && *b) b++;
    }
    return 0;
}

static uint32_t make_array(int n, const uint32_t *items, int cid) {
    uint32_t o = new_obj(cid, 12);
    uint32_t it = gcalloc(4 * (n ? n : 1));
    for (int i = 0; i < n; i++) ST32(it + 4 * i, items[i]);
    ST32(o + 4, n);
    ST32(o + 8, it);
    return o;
}

static void load_main_view(uint32_t vc) {
    ObjState *st = state_of(vc);
    if (st->view_loaded) return;
    st->view_loaded = true;
    const char *cn = class_name(LD32(vc));
    uint32_t v;
    if (!strcmp(cn, "_d_pixel_racingViewController")) {
        uint32_t cls = objc_class_named("EAGLView");
        v = guest_alloc(cls);
        uint32_t zero = 0;
        v = objc_send(v, "initWithCoder:", 1, &zero, NULL);
        main_view = v;
    } else {
        v = new_obj(C_UIView, 32);
    }
    st->vc_view = v;
    objc_send(vc, "viewDidLoad", 0, NULL, NULL);
}

#define SEL(s) (!strcmp(sel, s))
#define RET(v) do { R[0] = (uint32_t)(v); return; } while (0)

static void host_msg(int cid, bool is_class, uint32_t self, const char *sel) {
    int c = cid;
    /* walk the host class hierarchy */
    for (; c >= 0; c = hclasses[c].super) {
        if (is_class) {
            switch (c) {
            case C_NSObject:
                if (SEL("alloc") || SEL("allocWithZone:")) {
                    if (self >= hclass_lo && self < hclass_hi) {
                        bool m;
                        int h = hclass_of_addr(self, &m);
                        RET(new_obj(h, 64));
                    }
                    RET(guest_alloc(self));
                }
                if (SEL("class")) RET(self);
                if (SEL("new")) { uint32_t o = objc_send(self, "alloc", 0, NULL, NULL); RET(objc_send(o, "init", 0, NULL, NULL)); }
                if (SEL("respondsToSelector:") || SEL("instancesRespondToSelector:")) RET(1);
                break;
            case C_NSString:
                if (SEL("stringWithUTF8String:") || SEL("stringWithCString:")) RET(A(0) ? ns_string(GSTR(A(0))) : 0);
                if (SEL("stringWithFormat:")) {
                    char buf[4096];
                    gva va = {3};
                    guest_format(buf, sizeof buf, ns_cstr(A(0)), &va);
                    RET(ns_string(buf));
                }
                break;
            case C_NSNumber:
                if (SEL("numberWithBool:") || SEL("numberWithInt:")) { uint32_t o = new_obj(C_NSNumber, 8); ST32(o + 4, A(0)); RET(o); }
                break;
            case C_NSDictionary:
                if (SEL("dictionaryWithObjectsAndKeys:") || SEL("dictionary")) RET(new_obj(C_NSDictionary, 8));
                break;
            case C_NSArray:
                if (SEL("arrayWithObject:")) { uint32_t x = A(0); RET(make_array(1, &x, C_NSArray)); }
                break;
            case C_NSBundle:
                if (SEL("mainBundle")) RET(s_bundle);
                break;
            case C_NSRunLoop:
                if (SEL("currentRunLoop") || SEL("mainRunLoop")) RET(s_runloop);
                break;
            case C_NSURL:
                if (SEL("URLWithString:")) RET(A(0));
                break;
            case C_NSLocale:
                if (SEL("currentLocale")) RET(s_locale);
                break;
            case C_UIDevice:
                if (SEL("currentDevice")) RET(s_device);
                break;
            case C_UIScreen:
                if (SEL("mainScreen")) RET(s_screen);
                break;
            case C_UIApplication:
                if (SEL("sharedApplication")) RET(s_app);
                break;
            case C_UIAccelerometer:
                if (SEL("sharedAccelerometer")) RET(s_accel);
                break;
            case C_UIColor:
                RET(new_obj(C_UIColor, 8));
            case C_CADisplayLink:
                if (SEL("displayLinkWithTarget:selector:")) {
                    uint32_t o = new_obj(C_CADisplayLink, 16);
                    if (ndlinks < 8) dlinks[ndlinks++] = (DLink){o, A(0), A(1), 1, false};
                    RET(o);
                }
                break;
            case C_EAGLContext:
                if (SEL("setCurrentContext:")) { cur_context = A(0); RET(1); }
                if (SEL("currentContext")) RET(cur_context);
                break;
            case C_MFMailComposeViewController:
                if (SEL("canSendMail")) RET(0);
                break;
            }
            continue;
        }
        /* instance methods */
        switch (c) {
        case C_NSObject:
            if (SEL("init") || SEL("retain") || SEL("autorelease") || SEL("self") || SEL("initWithCoder:")) RET(self);
            if (SEL("release") || SEL("dealloc") || SEL("drain")) RET(0);
            if (SEL("class")) RET(LD32(self));
            if (SEL("respondsToSelector:")) {
                const char *s = GSTR(A(0));
                if (resolve(LD32(self), s).imp) RET(1);
                RET(!strcmp(s, "userInterfaceIdiom") || !strcmp(s, "contentScaleFactor") ||
                    !strcmp(s, "setContentScaleFactor:") || !strcmp(s, "scale"));
            }
            if (SEL("isEqual:")) RET(self == A(0));
            break;
        case C_NSString:
            if (SEL("UTF8String") || SEL("cString")) RET(LD32(self + 8));
            if (SEL("length")) RET(LD32(self + 12));
            if (SEL("isEqualToString:")) RET(A(0) && !strcmp(ns_cstr(self), ns_cstr(A(0))));
            if (SEL("compare:options:") || SEL("compare:")) {
                int r = compare_versions(ns_cstr(self), ns_cstr(A(0)));
                RET((uint32_t)r);
            }
            if (SEL("stringByAddingPercentEscapesUsingEncoding:")) RET(self);
            if (SEL("intValue")) RET(atoi(ns_cstr(self)));
            break;
        case C_NSNumber:
            if (SEL("boolValue") || SEL("intValue")) RET(LD32(self + 4));
            break;
        case C_NSArray:
        case C_NSSet:
            if (SEL("count")) RET(LD32(self + 4));
            if (SEL("objectAtIndex:")) RET(A(0) < LD32(self + 4) ? LD32(LD32(self + 8) + 4 * A(0)) : 0);
            if (SEL("allObjects")) RET(make_array(LD32(self + 4), GPTR(LD32(self + 8)), C_NSArray));
            if (SEL("anyObject")) RET(LD32(self + 4) ? LD32(LD32(self + 8)) : 0);
            break;
        case C_NSData:
            if (SEL("initWithContentsOfFile:")) {
                char buf[1200];
                const char *p = map_path(ns_cstr(A(0)), buf, sizeof buf);
                FILE *f = fopen(p, "rb");
                if (!f) { LOG("NSData: cannot open %s\n", p); RET(0); }
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                fseek(f, 0, SEEK_SET);
                uint32_t b = gmalloc(n);
                if (fread(GPTR(b), 1, n, f) != (size_t)n) n = 0;
                fclose(f);
                ST32(self + 4, b);
                ST32(self + 8, n);
                RET(self);
            }
            if (SEL("bytes")) RET(LD32(self + 4));
            if (SEL("length")) RET(LD32(self + 8));
            if (SEL("release")) { gfree(LD32(self + 4)); RET(0); }
            break;
        case C_UIImage:
            if (SEL("initWithData:")) {
                uint32_t d = A(0);
                int w, h;
                uint8_t *px = d ? png_decode(GPTR(LD32(d + 4)), LD32(d + 8), &w, &h) : NULL;
                if (!px) { LOG("UIImage: decode failed\n"); RET(0); }
                uint32_t img = gmalloc(12);
                uint32_t gp = gmalloc(w * h * 4);
                memcpy(GPTR(gp), px, (size_t)w * h * 4);
                free(px);
                ST32(img, w);
                ST32(img + 4, h);
                ST32(img + 8, gp);
                ST32(self + 4, img);
                RET(self);
            }
            if (SEL("CGImage")) RET(LD32(self + 4));
            if (SEL("release")) {
                uint32_t img = LD32(self + 4);
                if (img) { gfree(LD32(img + 8)); gfree(img); ST32(self + 4, 0); }
                RET(0);
            }
            break;
        case C_NSBundle:
            if (SEL("pathForResource:ofType:")) {
                char buf[1200];
                const char *name = ns_cstr(A(0));
                const char *type = A(1) ? ns_cstr(A(1)) : NULL;
                if (type && *type) snprintf(buf, sizeof buf, "%s/%s.%s", game_dir, name, type);
                else snprintf(buf, sizeof buf, "%s/%s", game_dir, name);
                RET(ns_string(buf));
            }
            if (SEL("objectForInfoDictionaryKey:")) {
                const char *k = ns_cstr(A(0));
                if (!strcmp(k, "CFBundleDisplayName")) RET(ns_string("3D Pixel Racing"));
                if (!strcmp(k, "CFBundleIdentifier")) RET(ns_string("com.vidiasrl.-d-pixel-racing"));
                if (!strcmp(k, "CFBundleVersion")) RET(ns_string("1.1"));
                RET(0);
            }
            break;
        case C_NSRunLoop:
            break;
        case C_NSLocale:
            if (SEL("localeIdentifier")) RET(ns_string("en_US"));
            break;
        case C_UIDevice:
            if (SEL("userInterfaceIdiom")) RET(device_idiom);
            if (SEL("systemVersion")) RET(ns_string("4.3.3"));
            if (SEL("systemName")) RET(ns_string("iPhone OS"));
            if (SEL("orientation")) RET(platform_device_orientation());        /* UIDeviceOrientationLandscapeLeft */
            if (SEL("model")) RET(ns_string(device_idiom ? "iPad" : "iPhone"));
            break;
        case C_UIScreen:
            if (SEL("scale")) { RET_F((float)screen_scale); return; }
            if (SEL("bounds") || SEL("applicationFrame")) {
                if (stret_shift) {   /* the screen itself is always reported in portrait */
                    uint32_t p = R[0];
                    ST32(p, F2U(0)); ST32(p + 4, F2U(0));
                    ST32(p + 8, F2U((float)screen_h_pt)); ST32(p + 12, F2U((float)screen_w_pt));
                }
                return;
            }
            break;
        case C_UIApplication:
            if (SEL("openURL:")) { LOG("openURL: %s\n", ns_cstr(A(0))); RET(0); }
            if (SEL("canOpenURL:")) RET(0);
            if (SEL("setIdleTimerDisabled:") || SEL("setStatusBarHidden:")) RET(0);
            break;
        case C_UIAccelerometer:
            if (SEL("setUpdateInterval:")) { accel_interval = U2D(A(0), A(1)); RET(0); }
            if (SEL("setDelegate:")) { accel_delegate = A(0); RET(0); }
            break;
        case C_UIAcceleration:
            if (SEL("x")) { RET_D(U2D(LD32(self + 8), LD32(self + 12))); return; }
            if (SEL("y")) { RET_D(U2D(LD32(self + 16), LD32(self + 20))); return; }
            if (SEL("z")) { RET_D(U2D(LD32(self + 24), LD32(self + 28))); return; }
            break;
        case C_UIAlertView:
            if (SEL("initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:")) {
                LOG("[alert] %s: %s\n", ns_cstr(A(0)), A(1) ? ns_cstr(A(1)) : "");
                RET(self);
            }
            if (SEL("show")) RET(0);
            break;
        case C_CADisplayLink:
            for (int i = 0; i < ndlinks; i++) {
                if (dlinks[i].obj != self) continue;
                if (SEL("setFrameInterval:")) { dlinks[i].interval = A(0) ? A(0) : 1; RET(0); }
                if (SEL("addToRunLoop:forMode:")) { dlinks[i].active = true; RET(0); }
                if (SEL("invalidate")) { dlinks[i].active = false; RET(0); }
            }
            if (SEL("timestamp")) { RET_D(SDL_GetTicks() / 1000.0); return; }
            break;
        case C_CAEAGLLayer:
            if (SEL("setOpaque:") || SEL("setDrawableProperties:")) RET(0);
            break;
        case C_EAGLContext:
            if (SEL("initWithAPI:")) { ST32(self + 4, A(0)); RET(self); }
            if (SEL("renderbufferStorage:fromDrawable:")) {
                ObjState *st = state_of(main_view);
                gl_renderbuffer_from_drawable((int)(screen_w_pt * st->scale), (int)(screen_h_pt * st->scale));
                RET(1);
            }
            if (SEL("presentRenderbuffer:")) {
                extern void gl_present(void);
                gl_present();
                RET(1);
            }
            break;
        case C_UITouch:
            if (SEL("phase")) RET(LD32(self + 4));
            if (SEL("locationInView:") || SEL("previousLocationInView:")) {
                if (stret_shift) {
                    uint32_t off = SEL("locationInView:") ? 8 : 16;
                    ST32(R[0], LD32(self + off));
                    ST32(R[0] + 4, LD32(self + off + 4));
                }
                return;
            }
            if (SEL("tapCount")) RET(1);
            break;
        case C_UIEvent:
            if (SEL("allTouches")) RET(LD32(self + 4));
            break;
        case C_UIWindow:
            if (SEL("addSubview:")) { state_of(A(0))->superview = self; RET(0); }
            if (SEL("makeKeyAndVisible")) RET(0);
            break;
        case C_UIView:
            if (SEL("initWithCoder:") || SEL("initWithFrame:") || SEL("init")) {
                ObjState *st = state_of(self);
                st->scale = 1.0f;
                /* layerClass is a class method implemented by EAGLView */
                st->layer = new_obj(C_CAEAGLLayer, 16);
                ST32(st->layer + 4, self);
                RET(self);
            }
            if (SEL("layer")) RET(state_of(self)->layer);
            if (SEL("setContentScaleFactor:")) { state_of(self)->scale = AF(0); RET(0); }
            if (SEL("contentScaleFactor")) { RET_F(state_of(self)->scale); return; }
            if (SEL("setMultipleTouchEnabled:") || SEL("setBackgroundColor:") || SEL("setNeedsLayout") ||
                SEL("setUserInteractionEnabled:") || SEL("setOpaque:"))
                RET(0);
            if (SEL("addSubview:")) { state_of(A(0))->superview = self; RET(0); }
            if (SEL("bounds") || SEL("frame")) {
                if (stret_shift) {
                    uint32_t p = R[0];
                    ST32(p, F2U(0)); ST32(p + 4, F2U(0));
                    ST32(p + 8, F2U((float)screen_w_pt)); ST32(p + 12, F2U((float)screen_h_pt));
                }
                return;
            }
            if (SEL("layoutSubviews")) RET(0);
            break;
        case C_UIViewController:
            if (SEL("initWithNibName:bundle:") || SEL("initWithCoder:")) RET(self);
            if (SEL("view")) { load_main_view(self); RET(state_of(self)->vc_view); }
            if (SEL("viewDidLoad") || SEL("viewWillAppear:") || SEL("viewWillDisappear:") ||
                SEL("viewDidUnload") || SEL("didReceiveMemoryWarning") || SEL("viewDidAppear:"))
                RET(0);
            if (SEL("setModalTransitionStyle:")) RET(0);
            if (SEL("setDelegate:")) { state_of(self)->delegate = A(0); RET(0); }
            if (SEL("delegate")) RET(state_of(self)->delegate);
            if (SEL("presentModalViewController:animated:")) {
                /* The only modal is the "do you like this app" nag: dismiss it on the next frame. */
                pending_modal_presenter = self;
                pending_modal_vc = A(0);
                RET(0);
            }
            if (SEL("dismissModalViewControllerAnimated:")) RET(0);
            if (SEL("shouldAutorotateToInterfaceOrientation:")) RET(0);
            break;
        }
    }
    LOG("objc: unhandled host selector -[%s %s]%s\n", hclasses[cid].name, sel, is_class ? " (class)" : "");
    R[0] = 0;
}

void uikit_service_modals(void) {
    if (!pending_modal_presenter) return;
    uint32_t p = pending_modal_presenter, vc = pending_modal_vc;
    pending_modal_presenter = 0;
    if (objc_responds(p, "DoYouLikeViewControllerDidFinish:"))
        objc_send(p, "DoYouLikeViewControllerDidFinish:", 1, &vc, NULL);
}

/* ------------------------------------------------------------------ binds */

static uint32_t eagl_str(const char *s) { return ns_string(s); }

void objc_init(void) {
    /* locate __objc_data so guest classes can be found by name */
    objc_data_lo = 0x4f2d8;
    objc_data_hi = 0x4f2d8 + 0xc8;
    /* fake class structures for host classes */
    hclass_lo = gcalloc(C_COUNT * 40 + 16);
    hclass_hi = hclass_lo + C_COUNT * 40;
    for (int i = 0; i < C_COUNT; i++) {
        hclasses[i].cls = hclass_lo + i * 40;
        hclasses[i].meta = hclass_lo + i * 40 + 20;
    }
    for (int i = 0; i < C_COUNT; i++) {
        uint32_t c = hclasses[i].cls, m = hclasses[i].meta;
        int s = hclasses[i].super;
        ST32(c, m);
        ST32(c + 4, s >= 0 ? hclasses[s].cls : 0);
        ST32(m, hclasses[C_NSObject].meta);
        ST32(m + 4, s >= 0 ? hclasses[s].meta : hclasses[C_NSObject].cls);
    }
    for (unsigned i = 0; i < guest_bind_count; i++) {
        const struct guest_bind *b = &guest_binds[i];
        const char *s = b->sym;
        uint32_t v = 0;
        if (b->stub) v = b->stub;
        else if (!strncmp(s, "_OBJC_CLASS_$_", 14) || !strncmp(s, "_OBJC_METACLASS_$_", 18)) {
            bool meta = s[6] == 'M';
            const char *n = s + (meta ? 18 : 14);
            for (int k = 0; k < C_COUNT; k++)
                if (!strcmp(hclasses[k].name, n)) v = meta ? hclasses[k].meta : hclasses[k].cls;
            if (!v) LOG("unknown class %s\n", n);
            if (!strcmp(n, "MFMailComposeViewController")) v = 0;    /* weak import: not available */
        } else if (!strcmp(s, "___CFConstantStringClassReference")) v = hclasses[C_NSString].cls;
        else if (!strcmp(s, "_kEAGLColorFormatRGBA8")) v = eagl_str("EAGLColorFormatRGBA8");
        else if (!strcmp(s, "_kEAGLDrawablePropertyColorFormat")) v = eagl_str("EAGLDrawablePropertyColorFormat");
        else if (!strcmp(s, "_kEAGLDrawablePropertyRetainedBacking")) v = eagl_str("EAGLDrawablePropertyRetainedBacking");
        else if (!strcmp(s, "_NSDefaultRunLoopMode")) v = ns_string("kCFRunLoopDefaultMode");
        else if (!strcmp(s, "dyld_stub_binder") || !strcmp(s, "___gxx_personality_sj0") ||
                 !strcmp(s, "__objc_empty_cache") || !strcmp(s, "__objc_empty_vtable") ||
                 !strcmp(s, "_kCFAllocatorDefault")) v = 0;
        else LOG("unresolved bind %s\n", s);
        ST32(b->addr, v);
    }
    s_device = new_obj(C_UIDevice, 8);
    s_screen = new_obj(C_UIScreen, 8);
    s_app = new_obj(C_UIApplication, 8);
    s_bundle = new_obj(C_NSBundle, 8);
    s_runloop = new_obj(C_NSRunLoop, 8);
    s_accel = new_obj(C_UIAccelerometer, 8);
    s_window = new_obj(C_UIWindow, 32);
    s_locale = new_obj(C_NSLocale, 8);
}

/* objects used to deliver input events */
uint32_t make_touch(int phase, float x, float y, float px, float py) {
    uint32_t t = new_obj(C_UITouch, 24);
    ST32(t + 4, phase);
    ST32(t + 8, F2U(x)); ST32(t + 12, F2U(y));
    ST32(t + 16, F2U(px)); ST32(t + 20, F2U(py));
    return t;
}

uint32_t make_set(int n, const uint32_t *items) { return make_array(n, items, C_NSSet); }

uint32_t make_event(uint32_t all) {
    uint32_t e = new_obj(C_UIEvent, 8);
    ST32(e + 4, all);
    return e;
}

uint32_t make_acceleration(double x, double y, double z) {
    uint32_t a = new_obj(C_UIAcceleration, 32);
    memcpy(GPTR(a + 8), &x, 8);
    memcpy(GPTR(a + 16), &y, 8);
    memcpy(GPTR(a + 24), &z, 8);
    return a;
}

uint32_t accel_object(void) { return s_accel; }

/* ------------------------------------------------------------------ app launch */

void uikit_launch(void) {
    uint32_t dcls = objc_class_named("_d_pixel_racingAppDelegate");
    uint32_t vcls = objc_class_named("_d_pixel_racingViewController");
    if (!dcls || !vcls) { LOG("app classes not found\n"); exit(1); }
    app_delegate = objc_send(guest_alloc(dcls), "init", 0, NULL, NULL);
    uint32_t zero = 0;
    main_vc = objc_send(guest_alloc(vcls), "initWithCoder:", 1, &zero, NULL);
    objc_send(app_delegate, "setWindow:", 1, &s_window, NULL);
    objc_send(app_delegate, "setViewController:", 1, &main_vc, NULL);
    objc_send(main_vc, "awakeFromNib", 0, NULL, NULL);
    uint32_t args[2] = {s_app, 0};
    objc_send(app_delegate, "application:didFinishLaunchingWithOptions:", 2, args, NULL);
    uint32_t yes = 1;
    objc_send(main_vc, "viewWillAppear:", 1, &yes, NULL);
    if (main_view) objc_send(main_view, "layoutSubviews", 0, NULL, NULL);
    objc_send(app_delegate, "applicationDidBecomeActive:", 1, &s_app, NULL);
}
