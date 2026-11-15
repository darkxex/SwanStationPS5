/*
 * SwanStationPS5 - a PS1 jewel case in 3D.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four meshes of assets/psx_case_NTSC.obj, drawn back to front with their own UV: ntsc_case_back and ntsc_case_inner
 * (the tray, whose ribbed edge is the spine), ntsc_case_promo (the printed sheet: the game's cover goes here) and
 * ntsc_case_front (the clear lid, whose printed area is transparent, so the cover shows through it from behind).
 * The renderer only takes 2D triangles, so the model is turned and projected here; faces looking away are skipped and
 * the rest are sorted by depth inside each mesh. The textures are made by tools/make_case_textures.py.
 *
 * Launching a game plays case3d_launch's animation on the chosen case: its lid swings open on the hinge, the CD (the
 * model's ntsc_disc meshes, the game's cover under its printed ring) spins on its tray.
 */
#include "case3d.h"

#include "../SwanStationPS5.h"
#include "../platform/platform.h"

#include "stb_image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum CasePart
{
    PART_BACK,
    PART_INNER,
    PART_PROMO,
    PART_FRONT,
    CASE_PARTS,
    PART_DISC_TOP = CASE_PARTS, /* the CD: its printed face */
    PART_DISC_BOTTOM,           /* and its silver back */
    PART_COUNT
};

static const char *const part_names[PART_COUNT] = {"ntsc_case_back", "ntsc_case_inner", "ntsc_case_promo",
                                                   "ntsc_case_front", "ntsc_disc_front", "ntsc_disc_back"};
/* the part's picture (the promo's is the cover); its colour when the file is missing */
#define HINGE_FILE "psx_case_hinge.png"
static const char *const part_files[PART_COUNT] = {"psx_case_back.jpg", "psx_case_inner.jpg", NULL,
                                                   "psx_case_front.png", "psx_disc.png", NULL};
static const uint32_t part_colors[PART_COUNT] = {0xff23232bu, 0xff0d0d11u, 0xff2a2a36u, 0xffa8b0b8u, 0xffd5d9e0u,
                                                 0xffc9ccd2u};

typedef struct
{
    int n; /* 3 or 4 */
    int v[4], t[4];
    bool inside; /* the inside of the closed case (walls looking inwards, the tray's floor): only seen with the lid open */
} Face;

typedef struct
{
    Face *faces;
    int count, cap;
} Mesh;

static float (*verts)[3];
static uint8_t *vpart; /* the part each vertex belongs to */
static float (*uvs)[2];
static int vert_count, vert_cap, uv_count, uv_cap;
static Mesh meshes[PART_COUNT];
static float case_h = 1.0f, case_w = 1.0f, case_d = 0.0f, case_floor;
static int load_state; /* 0 not tried, 1 loaded, -1 failed */

#define LIGHT_SCREEN_X 560.0f /* the light point, on the screen (px) */
#define LIGHT_HEIGHT 26.0f    /* above the case's middle (model units) */
#define LIGHT_DEPTH 70.0f     /* in front of the case's lid */
#define VIEW_DISTANCE_BASE 95.0f /* camera distance, in model units (the case is ~63 wide): near, for a strong perspective */
static float view_distance = VIEW_DISTANCE_BASE;
#define VIEW_DISTANCE view_distance

/* Where the cover goes: the lid's transparent printed area (the banner included: the downloaded cover has its own) (model units, from the template's UV). */
#define ART_X0 -25.8f
#define ART_X1 30.6f
#define ART_Y0 -27.15f
#define ART_Y1 27.05f
#define FRONT_Z 1.886f /* the lid's front plane */
#define SPINE_EDGE_X -26.63f /* where the lid's front plane begins: left of it is the ribbed strip */

static PlatTexture *part_tex[PART_COUNT];
static int tex_state[PART_COUNT]; /* 0 not tried, 1 made, -1 missing */

static PlatTexture *part_texture(int part)
{
    if (!part_files[part] || tex_state[part])
        return part_tex[part];
    tex_state[part] = -1;
    char path[512];
    plat_asset_path(path, sizeof(path), part_files[part]);
    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
    if (!rgba)
    {
        SwanStationPS5_log("case3d: can't read %s", path);
        return NULL;
    }
    part_tex[part] = plat_texture_create_mips(rgba, w, h);
    stbi_image_free(rgba);
    tex_state[part] = part_tex[part] ? 1 : -1;
    SwanStationPS5_log("case3d: %s %dx%d %s", part_files[part], w, h, part_tex[part] ? "ready" : "texture failed");
    return part_tex[part];
}

/* the lid's front over the ribbed strip at the case's left, scratched clear plastic (assets/psx_case_hinge.png) */
static PlatTexture *hinge_tex;
static int hinge_state;

static PlatTexture *hinge_texture(void)
{
    if (hinge_state)
        return hinge_tex;
    hinge_state = -1;
    char path[512];
    plat_asset_path(path, sizeof(path), HINGE_FILE);
    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
    if (!rgba)
        return NULL;
    hinge_tex = plat_texture_create_mips(rgba, w, h);
    stbi_image_free(rgba);
    hinge_state = hinge_tex ? 1 : -1;
    return hinge_tex;
}

/* the lid's paper seen from behind, with the lid open: black with the app's name (assets/psx_case_lidback.png) */
static PlatTexture *lidback_tex;
static int lidback_state;

static PlatTexture *lidback_texture(void)
{
    if (lidback_state)
        return lidback_tex;
    lidback_state = -1;
    char path[512];
    plat_asset_path(path, sizeof(path), "psx_case_lidback.png");
    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
    if (!rgba)
        return NULL;
    lidback_tex = plat_texture_create_mips(rgba, w, h);
    stbi_image_free(rgba);
    lidback_state = lidback_tex ? 1 : -1;
    return lidback_tex;
}

static bool add_face(Mesh *m, const Face *f)
{
    if (m->count == m->cap)
    {
        int cap = m->cap ? m->cap * 2 : 128;
        Face *grown = realloc(m->faces, (size_t)cap * sizeof(Face));
        if (!grown)
            return false;
        m->faces = grown;
        m->cap = cap;
    }
    m->faces[m->count++] = *f;
    return true;
}

static void parse(char *text)
{
    int part = -1;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n"))
    {
        if (line[0] == 'v' && line[1] == ' ')
        {
            float x, y, z;
            if (sscanf(line + 2, "%f %f %f", &x, &y, &z) != 3)
                continue;
            if (vert_count == vert_cap)
            {
                int cap = vert_cap ? vert_cap * 2 : 256;
                float(*grown)[3] = realloc(verts, (size_t)cap * sizeof(*verts));
                if (!grown)
                    return;
                verts = grown;
                uint8_t *parts = realloc(vpart, (size_t)cap);
                if (!parts)
                    return;
                vpart = parts;
                vert_cap = cap;
            }
            verts[vert_count][0] = x;
            verts[vert_count][1] = y;
            verts[vert_count][2] = z;
            vpart[vert_count] = 0xff;
            ++vert_count;
        }
        else if (line[0] == 'v' && line[1] == 't')
        {
            float u, v;
            if (sscanf(line + 2, "%f %f", &u, &v) != 2)
                continue;
            if (uv_count == uv_cap)
            {
                int cap = uv_cap ? uv_cap * 2 : 256;
                float(*grown)[2] = realloc(uvs, (size_t)cap * sizeof(*uvs));
                if (!grown)
                    return;
                uvs = grown;
                uv_cap = cap;
            }
            uvs[uv_count][0] = u;
            uvs[uv_count][1] = v;
            ++uv_count;
        }
        else if (line[0] == 'o' && line[1] == ' ')
        {
            char name[64] = "";
            sscanf(line + 2, "%63s", name);
            part = -1;
            for (int i = 0; i < PART_COUNT; ++i)
                if (strcmp(name, part_names[i]) == 0)
                    part = i;
        }
        else if (line[0] == 'f' && line[1] == ' ' && part >= 0)
        {
            Face f = {0, {0}, {0}, false};
            for (char *p = line + 2; *p && f.n < 4;)
            {
                while (*p == ' ')
                    ++p;
                if (!*p)
                    break;
                int vi = atoi(p), ti = 0;
                char *slash = strchr(p, '/');
                char *space = strchr(p, ' ');
                if (slash && (!space || slash < space))
                    ti = atoi(slash + 1);
                if (vi < 1 || vi > vert_count)
                    break;
                f.v[f.n] = vi - 1;
                f.t[f.n] = ti >= 1 && ti <= uv_count ? ti - 1 : -1;
                ++f.n;
                while (*p && *p != ' ')
                    ++p;
            }
            if (f.n >= 3)
            {
                if (part == PART_PROMO)
                {
                    /* the printed sheet is modelled facing away: turn it to the front */
                    for (int a = 0, b = f.n - 1; a < b; ++a, --b)
                    {
                        int t = f.v[a];
                        f.v[a] = f.v[b];
                        f.v[b] = t;
                    }
                }
                add_face(&meshes[part], &f);
                for (int k = 0; k < f.n; ++k)
                    vpart[f.v[k]] = (uint8_t)part;
            }
        }
    }
}

/* The model has the inside of the case too (the faces of the walls that look inwards, the tray's floor and hub). The
 * case is closed: none of that may ever show through, so those faces are flagged and only the outer shell is drawn (the
 * faces looking away from the middle of the case, and from the tray only its front: the spine and the rim around the
 * cover). With the lid open the flagged ones are drawn too. */
static void prune_interior(void)
{
    for (int part = 0; part < CASE_PARTS; ++part)
    {
        Mesh *m = &meshes[part];
        for (int i = 0; i < m->count; ++i)
        {
            Face *f = &m->faces[i];
            float c[3] = {0, 0, 0}, min_z = 1e9f;
            for (int k = 0; k < f->n; ++k)
                for (int a = 0; a < 3; ++a)
                    c[a] += verts[f->v[k]][a] / f->n;
            for (int k = 0; k < f->n; ++k)
                min_z = fminf(min_z, verts[f->v[k]][2]);
            const float *p0 = verts[f->v[0]], *p1 = verts[f->v[1]], *p2 = verts[f->v[2]];
            float u[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, v[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
            float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            const float outward = n[0] * c[0] + n[1] * c[1] + n[2] * c[2];
            bool keep = outward >= 0.0f;
            if (part == PART_INNER)
                keep = min_z > 1.3f && outward >= 0.0f;
            f->inside = !keep;
        }
    }
}

bool case3d_available(void)
{
    if (load_state)
        return load_state > 0;
    load_state = -1;
    char path[512];
    plat_asset_path(path, sizeof(path), "psx_case_NTSC.obj");
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (text && fread(text, 1, (size_t)size, fp) == (size_t)size)
    {
        text[size] = 0;
        parse(text);
    }
    free(text);
    fclose(fp);

    for (int i = 0; i < PART_COUNT; ++i)
        if (meshes[i].count == 0)
            return false;
    prune_interior();
    /* measures of the case as a whole */
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < CASE_PARTS; ++i)
        for (int f = 0; f < meshes[i].count; ++f)
            for (int k = 0; k < meshes[i].faces[f].n; ++k)
            {
                const float *p = verts[meshes[i].faces[f].v[k]];
                for (int a = 0; a < 3; ++a)
                {
                    lo[a] = fminf(lo[a], p[a]);
                    hi[a] = fmaxf(hi[a], p[a]);
                }
            }
    case_h = hi[1] - lo[1];
    case_w = hi[0] - lo[0];
    case_d = hi[2] - lo[2];
    case_floor = lo[1];
    load_state = case_h > 0.0f && case_w > 0.0f ? 1 : -1;
    return load_state > 0;
}

float case3d_aspect(void)
{
    return case3d_available() ? case_w / case_h : 1.0f;
}

float case3d_depth(void)
{
    return case3d_available() ? case_d / case_h : 0.0f;
}

float case3d_max_pitch(void)
{
    /* enough to see the top (or the bottom) edge */
    return case3d_available() ? 0.62f : 0.0f;
}

float case3d_max_yaw(void)
{
    /* the back of the case shows from a quarter turn on (90 degrees, at any tilt): its sides are meant to be seen */
    return case3d_available() ? 1.40f : 0.0f;
}

/* the turn of the case being drawn: yaw about the vertical axis, then pitch about the horizontal one */
static float turn_c, turn_s, tilt_c, tilt_s;

static void turn_point(float x, float y, float z, float *ox, float *oy, float *oz)
{
    const float x1 = x * turn_c + z * turn_s, z1 = -x * turn_s + z * turn_c;
    *ox = x1;
    *oy = y * tilt_c + z1 * tilt_s;
    *oz = -y * tilt_s + z1 * tilt_c;
}

/* The launch animation (case3d_launch), taken by the next case3d_draw only. */
static float launch_open, launch_spin, disc_lift, disc_scale = 1.0f;
static bool launch_on, launch_tray;
static float lid_c = 1.0f, lid_s, disc_c = 1.0f, disc_s;
static float draw_cx, draw_scale = 1.0f; /* the case being drawn: its centre on the screen and px per model unit */
static bool lighting_on = true;                  /* case3d_lighting: the cover's light */
static float floor_shadow_strength = 1.0f;       /* case3d_floor_shadow: how dark the patch under the case is */
static bool matte_next, matte;           /* the cover sheet is lit like paper (case3d_matte): taken by the next draw only */

#define LID_OPEN 2.2689f    /* the lid's full swing, radians: 130 degrees */
#define HINGE_Z 1.64f       /* the hinge's axis: the lid's middle, at the case's left edge */
#define DISC_GROW 0.05f      /* the CD grows by this much (0.1 = 10%) as it comes towards the screen while it spins */
#define DISC_CX 2.156f      /* the CD's centre in the model, and its radii */
#define DISC_CY (-0.054f)
#define DISC_R 27.3f
#define DISC_HUB 8.2f
#define DISC_MID_Z 0.285f   /* the CD's middle, on the tray */
#define DISC_TOP_Z 0.513f

/* a point of the lid, swung open on the hinge */
static void lid_point(float x, float y, float z, float *ox, float *oy, float *oz)
{
    const float hx = -case_w * 0.5f, dx = x - hx, dz = z - HINGE_Z;
    *ox = hx + dx * lid_c - dz * lid_s;
    *oy = y;
    *oz = HINGE_Z + dx * lid_s + dz * lid_c;
}

/* a direction of the lid (turned by the swing, not moved) */
static void lid_vector(float x, float y, float z, float *ox, float *oy, float *oz)
{
    *ox = x * lid_c - z * lid_s;
    *oy = y;
    *oz = x * lid_s + z * lid_c;
}

/* a point of the CD: spun about its axis, on its tray */
static void disc_point(float x, float y, float z, float *ox, float *oy, float *oz)
{
    const float dx = (x - DISC_CX) * disc_scale, dy = (y - DISC_CY) * disc_scale;
    *ox = DISC_CX + dx * disc_c - dy * disc_s;
    *oy = DISC_CY + dx * disc_s + dy * disc_c;
    *oz = z + disc_lift;
}

typedef struct
{
    float x, y, z; /* turned */
    float sx, sy;  /* on screen */
    float h;       /* height above the table (the same for a point and its mirror image) */
} Pt;

/* The case stands on a table: its image in the table is the case turned over the table's plane (model units). */
typedef struct
{
    float reach, near_a; /* how high the image still shows (it fades out upwards), its opacity at the table */
} Mirror;

static uint32_t shade_color(float k, uint32_t rgb)
{
    float r = k * ((rgb >> 16) & 0xff), g = k * ((rgb >> 8) & 0xff), b = k * (rgb & 0xff);
    r = r > 255.0f ? 255.0f : r;
    g = g > 255.0f ? 255.0f : g;
    b = b > 255.0f ? 255.0f : b;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* the image fades out with the height above the table */
static void fade_vertex(PlatVertex *q, const Mirror *mirror, float height)
{
    float fade = 1.0f - height / mirror->reach;
    fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
    float a = ((q->argb >> 24) & 0xff) * mirror->near_a * fade;
    q->argb = (q->argb & 0x00ffffffu) | (uint32_t)a << 24;
}

/* Draws one mesh's faces looking at the camera, far to near. `cover` is the promo's picture. With `mirror`, `pts` are
 * the points of the case's image in the table. */
static void draw_part(int part, const Pt *pts, PlatTexture *cover, uint32_t tint, const Mirror *mirror)
{
    static const float light[3] = {-0.36f, 0.50f, 0.79f};
    const float flip = mirror ? -1.0f : 1.0f; /* turning over the table reverses the winding */
    PlatTexture *tex = part == PART_PROMO ? cover : part_texture(part);
    const Mesh *m = &meshes[part];
    const bool show_inside = launch_on && launch_open > 0.01f; /* the lid is open: the inside of the case shows */
    enum { MAX_FACES = 128 };
    int order[MAX_FACES];
    float depth[MAX_FACES], shade[MAX_FACES];
    bool paper_back[MAX_FACES]; /* the cover paper seen from behind: plain, with the lid open */
    int n = 0;
    for (int i = 0; i < m->count && n < MAX_FACES; ++i)
    {
        const Face *f = &m->faces[i];
        if (f->inside && !show_inside)
            continue;
        const Pt *a = &pts[f->v[0]], *b = &pts[f->v[1]], *c = &pts[f->v[2]];
        float ux = b->x - a->x, uy = b->y - a->y, uz = b->z - a->z;
        float vx = c->x - a->x, vy = c->y - a->y, vz = c->z - a->z;
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        float len = sqrtf(nx * nx + ny * ny + nz * nz);
        if (len < 1e-6f)
            continue;
        len *= flip;
        nx /= len;
        ny /= len;
        nz /= len;
        /* looking away from the camera (at (0, 0, VIEW_DISTANCE)) */
        bool away = nx * (0.0f - a->x) + ny * (0.0f - a->y) + nz * (VIEW_DISTANCE - a->z) <= 0.0f;
        if (away && !(part == PART_PROMO && show_inside))
            continue;
        float d = 0.0f;
        for (int k = 0; k < f->n; ++k)
            d += pts[f->v[k]].z;
        float lit = nx * light[0] + ny * light[1] + nz * light[2];
        paper_back[n] = away;
        order[n] = i;
        depth[n] = d / f->n;
        shade[n] = lit > 0.0f ? lit : 0.0f;
        ++n;
    }
    /* far (small z) first */
    for (int i = 1; i < n; ++i)
    {
        int o = order[i];
        float d = depth[i], s = shade[i];
        bool pb = paper_back[i];
        int j = i - 1;
        for (; j >= 0 && depth[j] > d; --j)
        {
            order[j + 1] = order[j];
            depth[j + 1] = depth[j];
            shade[j + 1] = shade[j];
            paper_back[j + 1] = paper_back[j];
        }
        order[j + 1] = o;
        depth[j + 1] = d;
        shade[j + 1] = s;
        paper_back[j + 1] = pb;
    }

    static PlatVertex out[MAX_FACES * 6], flat_out[MAX_FACES * 6];
    int count = 0, flat_count = 0;
    /* the cover keeps its proportions: it fills the lid's window and what overflows is cropped (centred) */
    float cover_u = 1.0f, cover_v = 1.0f;
    if (part == PART_PROMO && cover)
    {
        int tw = 1, th = 1;
        plat_texture_size(cover, &tw, &th);
        const float win_w = ART_X1 - ART_X0, win_h = ART_Y1 - ART_Y0;
        const float fit = fmaxf(win_w / (float)tw, win_h / (float)th);
        cover_u = win_w / ((float)tw * fit);
        cover_v = win_h / ((float)th * fit);
    }
    const uint32_t base = tex ? (tint & 0x00ffffffu) : shade_color(1.0f, part_colors[part]);
    const float tr = ((tint >> 16) & 0xff) / 255.0f, tg = ((tint >> 8) & 0xff) / 255.0f, tb = (tint & 0xff) / 255.0f;
    const uint32_t alpha = tint & 0xff000000u;
    for (int i = 0; i < n; ++i)
    {
        const Face *f = &m->faces[order[i]];
        /* the pictures carry their own light: a little shading keeps the case's volume */
        const float k = lighting_on || part != PART_PROMO ? 0.62f + 0.38f * shade[i] : 1.0f;
        uint32_t rgb = shade_color(k, tex ? 0xffffffu : base);
        rgb = (uint32_t)(((rgb >> 16) & 0xff) * tr) << 16 | (uint32_t)(((rgb >> 8) & 0xff) * tg) << 8 |
              (uint32_t)((rgb & 0xff) * tb);
        PlatVertex q[4];
        const bool plain = paper_back[i];
        /* the paper's back is lit like paper: plain diffuse light from the light point of the plastic's glare, so it
         * darkens as it turns away from the light and brightens towards it (matte: no shine) */
        float side[3] = {0.0f, 0.0f, 0.0f};
        /* lit like matte paper: the cover sheet with the picture of a game without a cover, and all that the open lid shows
         * (the lid's back, the tray's inside) */
        const bool paper_lit = plain || (matte && part == PART_PROMO) || (show_inside && f->inside) ||
                              part == PART_DISC_TOP || part == PART_DISC_BOTTOM; /* the CD takes the light too */
        if (paper_lit)
        {
            const Pt *pa = &pts[f->v[0]], *pb = &pts[f->v[1]], *pc = &pts[f->v[2]];
            const float ux = pb->x - pa->x, uy = pb->y - pa->y, uz = pb->z - pa->z;
            const float vx = pc->x - pa->x, vy = pc->y - pa->y, vz = pc->z - pa->z;
            float gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
            const float gl = sqrtf(gx * gx + gy * gy + gz * gz);
            if (gl > 1e-6f)
            {
                /* the side that shows: its winding's normal (they look at the camera), the opposite one for the cover's
                 * back (plain) */
                const float flip_side = plain ? -1.0f : 1.0f;
                side[0] = flip_side * gx / gl;
                side[1] = flip_side * gy / gl;
                side[2] = flip_side * gz / gl;
            }
        }
        for (int v = 0; v < f->n; ++v)
        {
            const float *p = verts[f->v[v]];
            q[v].x = pts[f->v[v]].sx;
            q[v].y = pts[f->v[v]].sy;
            if (plain)
            {
                /* seen from behind: across the window, mirrored, so the text reads */
                float u = 1.0f - (p[0] - ART_X0) / (ART_X1 - ART_X0), w = 1.0f - (p[1] - ART_Y0) / (ART_Y1 - ART_Y0);
                q[v].u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
                q[v].v = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
            }
            else if (part == PART_PROMO)
            {
                /* the cover across the lid's window (the plane is a little larger: its border hides under the frame) */
                float u = 0.5f + ((p[0] - ART_X0) / (ART_X1 - ART_X0) - 0.5f) * cover_u;
                float w = 0.5f + (0.5f - (p[1] - ART_Y0) / (ART_Y1 - ART_Y0)) * cover_v;
                q[v].u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
                q[v].v = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
            }
            else if (f->t[v] >= 0)
            {
                q[v].u = uvs[f->t[v]][0];
                q[v].v = 1.0f - uvs[f->t[v]][1]; /* OBJ's v runs upwards */
            }
            else
                q[v].u = q[v].v = 0.0f;
            q[v].argb = alpha | rgb;
            if (paper_lit)
            {
                const Pt *pp = &pts[f->v[v]];
                float dx = (LIGHT_SCREEN_X - draw_cx) / draw_scale - pp->x, dy = LIGHT_HEIGHT - pp->y, dz = LIGHT_DEPTH - pp->z;
                const float dl = sqrtf(dx * dx + dy * dy + dz * dz);
                const float lambert = fmaxf((side[0] * dx + side[1] * dy + side[2] * dz) / dl, 0.0f);
                const float kk = lighting_on || part != PART_PROMO ? fminf(0.42f + 0.62f * lambert, 1.0f) : 1.0f;
                q[v].argb = alpha | (uint32_t)(255.0f * kk * tr) << 16 | (uint32_t)(255.0f * kk * tg) << 8 | (uint32_t)(255.0f * kk * tb);
            }
            if (mirror)
                fade_vertex(&q[v], mirror, pts[f->v[v]].h);
        }
        PlatVertex *dst = plain ? flat_out : out;
        int *dn = plain ? &flat_count : &count;
        dst[(*dn)++] = q[0];
        dst[(*dn)++] = q[1];
        dst[(*dn)++] = q[2];
        if (f->n == 4)
        {
            dst[(*dn)++] = q[0];
            dst[(*dn)++] = q[2];
            dst[(*dn)++] = q[3];
        }
    }
    if (count)
        plat_draw_mesh(tex, out, count, NULL, 0);
    if (flat_count)
        plat_draw_mesh(lidback_texture(), flat_out, flat_count, NULL, 0);
}

/* The case's footprint on the table and a soft shade around it, so it sits there instead of floating. */
static void draw_contact_shadow(float c, float s, float cx, float cy, float scale, float tint_alpha)
{
    const float hw = case_w * 0.5f, hd = case_d * 0.5f, grow = 5.0f;
    const float ring[2][4][2] = {{{-hw, -hd}, {hw, -hd}, {hw, hd}, {-hw, hd}},
                                 {{-hw - grow, -hd - grow}, {hw + grow, -hd - grow}, {hw + grow, hd + grow * 1.6f},
                                  {-hw - grow, hd + grow * 1.6f}}};
    PlatVertex v[2][4];
    for (int r = 0; r < 2; ++r)
        for (int i = 0; i < 4; ++i)
        {
            float x = ring[r][i][0] * c + ring[r][i][1] * s, z = -ring[r][i][0] * s + ring[r][i][1] * c;
            float persp = VIEW_DISTANCE / (VIEW_DISTANCE - z);
            v[r][i].x = cx + x * scale * persp;
            v[r][i].y = cy - case_floor * scale * persp;
            v[r][i].u = v[r][i].v = 0.0f;
            v[r][i].argb = (uint32_t)((r == 0 ? 0x80 : 0x00) * tint_alpha) << 24;
        }
    PlatVertex out[8 * 3 + 6];
    int n = 0;
    for (int i = 0; i < 4; ++i)
    {
        int j = (i + 1) & 3;
        out[n++] = v[0][i];
        out[n++] = v[0][j];
        out[n++] = v[1][j];
        out[n++] = v[0][i];
        out[n++] = v[1][j];
        out[n++] = v[1][i];
    }
    out[n++] = v[0][0];
    out[n++] = v[0][1];
    out[n++] = v[0][2];
    out[n++] = v[0][0];
    out[n++] = v[0][2];
    out[n++] = v[0][3];
    plat_draw_mesh(NULL, out, n, NULL, 0);
}

/* A sheet of clear plastic in front of a flat part of the case, centred on `centre`, with half-extents `eu` and `ev`
 * (model units) and `normal` out of it: a faint sheen all over and the shine of a light point. The plastic is a mirror,
 * so the glare sits where the light, the plane's normal and the eye line up; it slides across as the case turns or
 * moves over the shelf (the light stays in place on the screen). `sigma_u`, `sigma_v` are the glare's width along each
 * extent: a long streak when one is wide and the other narrow. */
typedef struct
{
    float centre[3], eu[3], ev[3], normal[3];
    float sigma_u, sigma_v, peak, sheen;
    bool on_lid; /* it moves with the lid when it swings open */
} Sheet;

static void draw_sheet(const Sheet *sh, float cx, float cy, float scale, float alpha)
{
    float c3[3], n3[3], u3[3], v3[3];
    float c0[3] = {sh->centre[0], sh->centre[1], sh->centre[2]}, n0[3] = {sh->normal[0], sh->normal[1], sh->normal[2]};
    float u0[3] = {sh->eu[0], sh->eu[1], sh->eu[2]}, v0[3] = {sh->ev[0], sh->ev[1], sh->ev[2]};
    if (sh->on_lid)
    {
        lid_point(sh->centre[0], sh->centre[1], sh->centre[2], &c0[0], &c0[1], &c0[2]);
        lid_vector(sh->normal[0], sh->normal[1], sh->normal[2], &n0[0], &n0[1], &n0[2]);
        lid_vector(sh->eu[0], sh->eu[1], sh->eu[2], &u0[0], &u0[1], &u0[2]);
        lid_vector(sh->ev[0], sh->ev[1], sh->ev[2], &v0[0], &v0[1], &v0[2]);
    }
    turn_point(c0[0], c0[1], c0[2], &c3[0], &c3[1], &c3[2]);
    turn_point(n0[0], n0[1], n0[2], &n3[0], &n3[1], &n3[2]);
    turn_point(u0[0], u0[1], u0[2], &u3[0], &u3[1], &u3[2]);
    turn_point(v0[0], v0[1], v0[2], &v3[0], &v3[1], &v3[2]);
    /* looking away from the camera */
    if (n3[0] * -c3[0] + n3[1] * -c3[1] + n3[2] * (VIEW_DISTANCE - c3[2]) <= 0.0f)
        return;
    const float ul = sqrtf(u3[0] * u3[0] + u3[1] * u3[1] + u3[2] * u3[2]);
    const float vl_ = sqrtf(v3[0] * v3[0] + v3[1] * v3[1] + v3[2] * v3[2]);
    const float uh[3] = {u3[0] / ul, u3[1] / ul, u3[2] / ul}, vh[3] = {v3[0] / vl_, v3[1] / vl_, v3[2] / vl_};
    /* the light, in the case's view space: fixed on the screen, a little above the case's middle, in front of it */
    const float lx = (LIGHT_SCREEN_X - cx) / scale, ly = LIGHT_HEIGHT, lz = LIGHT_DEPTH;
    enum { COLS = 16, ROWS = 12 };
    static PlatVertex grid[(COLS + 1) * (ROWS + 1)];
    for (int j = 0; j <= ROWS; ++j)
        for (int i = 0; i <= COLS; ++i)
        {
            const float a = 2.0f * i / COLS - 1.0f, b = 1.0f - 2.0f * j / ROWS;
            const float px = c3[0] + u3[0] * a + v3[0] * b, py = c3[1] + u3[1] * a + v3[1] * b,
                        pz = c3[2] + u3[2] * a + v3[2] * b;
            float vx = -px, vy = -py, vz = VIEW_DISTANCE - pz; /* to the eye */
            float dx = lx - px, dy = ly - py, dz = lz - pz;    /* to the light */
            float vl = sqrtf(vx * vx + vy * vy + vz * vz), dl = sqrtf(dx * dx + dy * dy + dz * dz);
            float hx = vx / vl + dx / dl, hy = vy / vl + dy / dl, hz = vz / vl + dz / dl;
            float hl = sqrtf(hx * hx + hy * hy + hz * hz);
            hx /= hl;
            hy /= hl;
            hz /= hl;
            /* how far the half-way vector strays from the normal, along each of the sheet's directions */
            const float ex = hx - n3[0], ey = hy - n3[1], ez = hz - n3[2];
            const float du = (ex * uh[0] + ey * uh[1] + ez * uh[2]) / sh->sigma_u;
            const float dv = (ex * vh[0] + ey * vh[1] + ez * vh[2]) / sh->sigma_v;
            const float glare = expf(-(du * du + dv * dv));
            const float soft = 0.03f * expf(-(ex * ex + ey * ey + ez * ez) * 4.0f);
            float al = (glare * sh->peak + soft + sh->sheen) * alpha;
            PlatVertex *q = &grid[j * (COLS + 1) + i];
            float persp = VIEW_DISTANCE / (VIEW_DISTANCE - pz);
            q->x = cx + px * scale * persp;
            q->y = cy - py * scale * persp;
            q->u = q->v = 0.0f;
            q->argb = (uint32_t)(al > 1.0f ? 255.0f : al * 255.0f) << 24 | 0xffffffu;
        }
    static PlatVertex tris[COLS * ROWS * 6];
    int n = 0;
    for (int j = 0; j < ROWS; ++j)
        for (int i = 0; i < COLS; ++i)
        {
            const PlatVertex *a = &grid[j * (COLS + 1) + i], *b = a + 1, *d = a + (COLS + 1), *e = d + 1;
            if (!((a->argb | b->argb | d->argb | e->argb) >> 24))
                continue;
            tris[n++] = *a;
            tris[n++] = *b;
            tris[n++] = *e;
            tris[n++] = *a;
            tris[n++] = *e;
            tris[n++] = *d;
        }
    if (n)
        plat_draw_mesh(NULL, tris, n, NULL, 0);
}

/* The shine over the lid's clear plastic: the long horizontal streak. The sides are left as the example textures have
 * them. */
static void draw_plastic(float cx, float cy, float scale, float alpha, bool strip_pass, bool shine_pass)
{
    /* the lid's front runs over the ribbed strip too, flush with it (the model's front plane starts right of it) */
    PlatTexture *strip = hinge_texture();
    float nx, ny, nz;
    turn_point(0.0f, 0.0f, 1.0f, &nx, &ny, &nz);
    if (strip_pass && strip && nz > 0.0f)
    {
        const float x0 = -case_w * 0.5f, x1 = SPINE_EDGE_X, z = FRONT_Z + 0.01f, hh = case_h * 0.5f;
        const float corner[4][2] = {{x0, hh}, {x1, hh}, {x1, -hh}, {x0, -hh}};
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        PlatVertex q[4];
        for (int i = 0; i < 4; ++i)
        {
            float px, py, pz;
            turn_point(corner[i][0], corner[i][1], z, &px, &py, &pz);
            const float persp = VIEW_DISTANCE / (VIEW_DISTANCE - pz);
            q[i] = (PlatVertex){cx + px * scale * persp, cy - py * scale * persp, uv[i][0], uv[i][1],
                                (uint32_t)(255.0f * alpha) << 24 | 0xffffffu};
        }
        const PlatVertex tri[6] = {q[0], q[1], q[2], q[0], q[2], q[3]};
        plat_draw_mesh(strip, tri, 6, NULL, 0);
    }

    /* the shine of the light point over the lid */
    const Sheet lid = {{0, 0, FRONT_Z + 0.02f}, {case_w * 0.5f, 0, 0}, {0, case_h * 0.5f, 0}, {0, 0, 1},
                       0.90f, 0.24f, 0.08f, 0.0f, true};
    if (shine_pass && lighting_on)
        draw_sheet(&lid, cx, cy, scale, alpha);
}

/* The game's cover under the CD's printed ring: a flat ring on the disc's face, turning with it, showing the middle
 * square of the cover. The CD's own picture (psx_disc.png) is drawn over it with that ring cut out. */
static void draw_disc_cover(PlatTexture *cover, uint32_t tint, float cx, float cy, float scale, const Mirror *mirror)
{
    enum { SEG = 72 };
    int tw = 1, th = 1;
    if (cover)
        plat_texture_size(cover, &tw, &th);
    const float ux = tw > th ? (float)th / tw : 1.0f, uy = th > tw ? (float)tw / th : 1.0f;
    const uint32_t argb = cover ? (tint | 0xff000000u) : (0xff000000u | (part_colors[PART_PROMO] & 0xffffffu));
    const float tr = ((argb >> 16) & 0xff) / 255.0f, tg = ((argb >> 8) & 0xff) / 255.0f, tb = (argb & 0xff) / 255.0f;
    /* the CD's face is lit by the same light as the rest of the inside of the case (it is flat: its normal is the tray's) */
    float nx, ny, nz;
    turn_point(0.0f, 0.0f, 1.0f, &nx, &ny, &nz);
    const float lx = (LIGHT_SCREEN_X - draw_cx) / draw_scale, ly = LIGHT_HEIGHT, lz = LIGHT_DEPTH;
    const float radii[2] = {DISC_HUB, DISC_R - 0.3f};
    static PlatVertex ring[2][SEG + 1], tris[SEG * 6];
    for (int i = 0; i <= SEG; ++i)
    {
        const float a = 6.2831853f * (float)i / SEG;
        for (int r = 0; r < 2; ++r)
        {
            const float x = DISC_CX + cosf(a) * radii[r], y = DISC_CY + sinf(a) * radii[r];
            float mx, my, mz, px, py, pz;
            disc_point(x, y, DISC_TOP_Z - 0.006f, &mx, &my, &mz);
            turn_point(mx, my, mz, &px, &py, &pz);
            const float persp = VIEW_DISTANCE / (VIEW_DISTANCE - pz);
            const float dx = lx - px, dy = ly - py, dz = lz - pz, dl = sqrtf(dx * dx + dy * dy + dz * dz);
            const float lambert = fmaxf((nx * dx + ny * dy + nz * dz) / dl, 0.0f);
            const float kk = fminf(0.42f + 0.62f * lambert, 1.0f);
            uint32_t alpha_mask = argb & 0xff000000u;
            float mirrored_y = py;
            if (mirror)
            {
                /* its image in the table: the same height below it, fading out with the height */
                const float height = py - case_floor;
                mirrored_y = case_floor - height;
                float fade = 1.0f - height / mirror->reach;
                fade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
                alpha_mask = (uint32_t)(((argb >> 24) & 0xff) * mirror->near_a * fade) << 24;
            }
            const uint32_t lit = alpha_mask | (uint32_t)(255.0f * kk * tr) << 16 |
                                 (uint32_t)(255.0f * kk * tg) << 8 | (uint32_t)(255.0f * kk * tb);
            ring[r][i] = (PlatVertex){cx + px * scale * persp, cy - mirrored_y * scale * persp,
                                      0.5f + (x - DISC_CX) / DISC_R * 0.5f * ux, 0.5f - (y - DISC_CY) / DISC_R * 0.5f * uy,
                                      lit};
        }
    }
    int n = 0;
    for (int i = 0; i < SEG; ++i)
    {
        tris[n++] = ring[0][i];
        tris[n++] = ring[1][i];
        tris[n++] = ring[1][i + 1];
        tris[n++] = ring[0][i];
        tris[n++] = ring[1][i + 1];
        tris[n++] = ring[0][i + 1];
    }
    plat_draw_mesh(cover, tris, n, NULL, 0);
}

/* The CD's metallic shine over its (paper-like) light: a specular highlight where the light bounces to the camera, a little
 * stronger along the light's own axis (the tracks are brushed metal). Blended white (there is no additive blending); the
 * highlight stays where the light puts it, the disc spinning does not move it. */
#define GLOSS_ALPHA 0.30f /* the highlight at its brightest: the share of white over the disc */

static void draw_disc_gloss(uint32_t tint, float cx, float cy, float scale)
{
    enum { SEG = 72, RINGS = 5 };
    const uint32_t tint_a = tint >> 24;
    if (tint_a == 0)
        return;
    float nx, ny, nz;
    turn_point(0.0f, 0.0f, 1.0f, &nx, &ny, &nz);
    const float lx = (LIGHT_SCREEN_X - draw_cx) / draw_scale, ly = LIGHT_HEIGHT, lz = LIGHT_DEPTH;
    float ccx, ccy, ccz;
    turn_point(DISC_CX, DISC_CY, DISC_TOP_Z + disc_lift, &ccx, &ccy, &ccz);
    const float light_angle = atan2f(ly - ccy, lx - ccx);
    static PlatVertex ring[RINGS][SEG + 1], tris[SEG * (RINGS - 1) * 6];
    for (int i = 0; i <= SEG; ++i)
    {
        const float a = 6.2831853f * (float)i / SEG;
        /* brushed: brighter along the light's axis than across it */
        const float along = 0.7f + 0.3f * cosf(2.0f * (a - light_angle));
        for (int r = 0; r < RINGS; ++r)
        {
            const float rad = DISC_HUB + (DISC_R - 0.3f - DISC_HUB) * (float)r / (RINGS - 1);
            const float x = DISC_CX + cosf(a) * rad * disc_scale, y = DISC_CY + sinf(a) * rad * disc_scale;
            float px, py, pz;
            turn_point(x, y, DISC_TOP_Z - 0.012f + disc_lift, &px, &py, &pz);
            const float persp = VIEW_DISTANCE / (VIEW_DISTANCE - pz);
            /* half-way between the light and the camera */
            float hx = (lx - px) / 1.0f, hy = ly - py, hz = lz - pz;
            const float hl = sqrtf(hx * hx + hy * hy + hz * hz);
            hx /= hl, hy /= hl, hz /= hl;
            float vx = -px, vy = -py, vz = VIEW_DISTANCE - pz;
            const float vl = sqrtf(vx * vx + vy * vy + vz * vz);
            hx += vx / vl, hy += vy / vl, hz += vz / vl;
            const float h2 = sqrtf(hx * hx + hy * hy + hz * hz);
            const float nh = h2 > 1e-6f ? fmaxf((nx * hx + ny * hy + nz * hz) / h2, 0.0f) : 0.0f;
            float k = powf(nh, 14.0f) * along * GLOSS_ALPHA;
            k = k > GLOSS_ALPHA ? GLOSS_ALPHA : k;
            const uint32_t ca = (uint32_t)(255.0f * k * tint_a / 255.0f);
            ring[r][i] = (PlatVertex){cx + px * scale * persp, cy - py * scale * persp, 0.0f, 0.0f, ca << 24 | 0xffffffu};
        }
    }
    int n = 0;
    for (int i = 0; i < SEG; ++i)
        for (int r = 0; r < RINGS - 1; ++r)
        {
            tris[n++] = ring[r][i];
            tris[n++] = ring[r + 1][i];
            tris[n++] = ring[r + 1][i + 1];
            tris[n++] = ring[r][i];
            tris[n++] = ring[r + 1][i + 1];
            tris[n++] = ring[r][i + 1];
        }
    plat_draw_mesh(NULL, tris, n, NULL, 0);
}

#if 0 /* the disc shader (rainbow spokes), taken out */
/* hue 0..1 (full saturation, a little washed out towards white) to RGB bytes */
static uint32_t rainbow(float hue)
{
    hue -= floorf(hue);
    const float h6 = hue * 6.0f, f = h6 - floorf(h6);
    float r = 0, g = 0, b = 0;
    switch ((int)h6 % 6)
    {
    case 0: r = 1; g = f; break;
    case 1: r = 1 - f; g = 1; break;
    case 2: g = 1; b = f; break;
    case 3: g = 1 - f; b = 1; break;
    case 4: r = f; b = 1; break;
    default: r = 1; b = 1 - f; break;
    }
    const float w = 0.2f; /* washed out */
    return (uint32_t)(255.0f * (r + (1.0f - r) * w)) << 16 | (uint32_t)(255.0f * (g + (1.0f - g) * w)) << 8 |
           (uint32_t)(255.0f * (b + (1.0f - b) * w));
}

/* The disc's shine: a CD's tracks split the light into colours, seen as bright rainbow spokes that run out from the hub
 * towards (and away from) the light. They stay where the light puts them (the disc spinning does not move them) and
 * follow the case as it turns. Drawn as blended colour over the face (there is no additive blending). */
#define SHEEN_ALPHA 0.06f /* the spokes at their brightest: the share of their colour over the disc (a tenth of the first try) */

static void draw_disc_sheen(uint32_t tint, float cx, float cy, float scale)
{
    enum { SEG = 180, RINGS = 3 };
    const uint32_t tint_a = tint >> 24;
    if (tint_a == 0)
        return;
    float ccx, ccy, ccz, nx, ny, nz;
    turn_point(DISC_CX, DISC_CY, DISC_TOP_Z + disc_lift, &ccx, &ccy, &ccz);
    turn_point(0.0f, 0.0f, 1.0f, &nx, &ny, &nz);
    const float lx = (LIGHT_SCREEN_X - draw_cx) / draw_scale - ccx, ly = LIGHT_HEIGHT - ccy, lz = LIGHT_DEPTH - ccz;
    const float ll = sqrtf(lx * lx + ly * ly + lz * lz);
    /* the more the disc faces the light, the brighter the shine */
    const float facing = fminf(0.35f + 0.9f * fmaxf((nx * lx + ny * ly + nz * lz) / ll, 0.0f), 1.0f);
    const float light_angle = atan2f(ly, lx);
    const float radii[RINGS] = {DISC_HUB + 0.5f, 0.5f * (DISC_HUB + DISC_R), DISC_R - 0.3f};
    const float radial[RINGS] = {0.25f, 1.0f, 0.7f}; /* fades near the hub, strong in the middle */
    static PlatVertex ring[RINGS][SEG + 1], tris[SEG * (RINGS - 1) * 6];
    for (int i = 0; i <= SEG; ++i)
    {
        const float a = 6.2831853f * (float)i / SEG;
        /* the angle from the light's spoke (it runs both ways), and from a weaker second pair */
        float d1 = fmodf(a - light_angle + 9.42477796f, 3.14159265f) - 1.57079633f; /* -pi/2..pi/2 about the spoke */
        float d2 = fmodf(a - light_angle - 1.0f + 9.42477796f, 3.14159265f) - 1.57079633f;
        const float w1 = 0.22f, w2 = 0.16f;
        const float s1 = expf(-(d1 * d1) / (w1 * w1)), s2 = 0.45f * expf(-(d2 * d2) / (w2 * w2));
        const float spoke = s1 > s2 ? s1 : s2;
        const float hue_t = s1 >= s2 ? d1 / w1 : d2 / w2; /* the colour runs across the spoke */
        const uint32_t rgb = rainbow(0.9f + 0.28f * hue_t);
        for (int r = 0; r < RINGS; ++r)
        {
            const float x = DISC_CX + cosf(a) * radii[r] * disc_scale, y = DISC_CY + sinf(a) * radii[r] * disc_scale;
            float px, py, pz;
            turn_point(x, y, DISC_TOP_Z - 0.012f + disc_lift, &px, &py, &pz);
            const float persp = VIEW_DISTANCE / (VIEW_DISTANCE - pz);
            float k = SHEEN_ALPHA * spoke * radial[r] * facing;
            k = k > SHEEN_ALPHA ? SHEEN_ALPHA : k;
            const uint32_t ca = (uint32_t)(255.0f * k * tint_a / 255.0f);
            ring[r][i] = (PlatVertex){cx + px * scale * persp, cy - py * scale * persp, 0.0f, 0.0f, ca << 24 | rgb};
        }
    }
    int n = 0;
    for (int i = 0; i < SEG; ++i)
        for (int r = 0; r < RINGS - 1; ++r)
        {
            tris[n++] = ring[r][i];
            tris[n++] = ring[r + 1][i];
            tris[n++] = ring[r + 1][i + 1];
            tris[n++] = ring[r][i];
            tris[n++] = ring[r + 1][i + 1];
            tris[n++] = ring[r][i + 1];
        }
    plat_draw_mesh(NULL, tris, n, NULL, 0);
}
#endif

static void draw_disc(const Pt *pts, PlatTexture *cover, uint32_t tint, float cx, float cy, float scale)
{
    draw_part(PART_DISC_BOTTOM, pts, cover, tint, NULL);
    draw_disc_cover(cover, tint, cx, cy, scale, NULL);
    draw_disc_gloss(tint, cx, cy, scale);
    draw_part(PART_DISC_TOP, pts, cover, tint, NULL);
}

void case3d_lighting(bool on)
{
    lighting_on = on;
}

void case3d_floor_shadow(float strength)
{
    floor_shadow_strength = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
}

void case3d_matte(bool on)
{
    matte_next = on;
}

void case3d_launch(float open, float spin, bool in_tray)
{
    launch_on = true;
    launch_tray = in_tray;
    launch_open = open < 0.0f ? 0.0f : open > 1.0f ? 1.0f : open;
    launch_spin = spin;
}

void case3d_draw(PlatTexture *cover, float cx, float cy, float h, float yaw, float pitch, uint32_t tint,
                 bool reflection)
{
    if (!case3d_available() || ((tint >> 24) & 0xff) == 0)
        return;
    /* whatever turns the case (the stick, a knock, the shelf), its back never shows: it shows from 90 degrees on, so it
     * stops at case3d_max_yaw (80), at any tilt */
    yaw = fmaxf(fminf(yaw, case3d_max_yaw()), -case3d_max_yaw());
    pitch = fmaxf(fminf(pitch, case3d_max_pitch()), -case3d_max_pitch());
    const float scale = h / case_h, c = cosf(yaw), s = sinf(yaw);
    /* the case must not come nearer as it opens (its lid swings towards the viewer): its perspective flattens out as it
     * opens */
    view_distance = VIEW_DISTANCE_BASE * (1.0f + 9.0f * launch_open);
    lid_c = cosf(launch_open * LID_OPEN);
    lid_s = sinf(launch_open * LID_OPEN);
    const float disc_t = launch_on && !launch_tray ? 0.4f * launch_open + 0.6f * fminf(launch_spin / 18.0f, 1.0f) : 0.0f;
    disc_lift = disc_t * 36.0f;
    disc_scale = 1.0f + DISC_GROW * disc_t;
    disc_c = cosf(launch_spin);
    disc_s = -sinf(launch_spin);
    /* past 90 degrees the lid lies out to the left of the hinge: the pair (case and lid) is kept centred where the case was
    cx += 0.5f * case_w * scale * fmaxf(-lid_c, 0.0f); */
    draw_cx = cx;
    draw_scale = scale;
    matte = matte_next;
    matte_next = false;
    turn_c = c;
    turn_s = s;
    tilt_c = cosf(pitch);
    tilt_s = sinf(pitch);
    static Pt *pts, *mirrored;
    static int pts_cap;
    if (pts_cap < vert_count)
    {
        Pt *grown = realloc(pts, (size_t)vert_count * sizeof(Pt));
        if (!grown)
            return;
        pts = grown;
        grown = realloc(mirrored, (size_t)vert_count * sizeof(Pt));
        if (!grown)
            return;
        mirrored = grown;
        pts_cap = vert_count;
    }
    for (int i = 0; i < vert_count; ++i)
    {
        const float *p = verts[i];
        Pt *o = &pts[i];
        float mx = p[0], my = p[1], mz = p[2];
        if (launch_on)
        {
            if (vpart[i] == PART_PROMO || vpart[i] == PART_FRONT)
                lid_point(p[0], p[1], p[2], &mx, &my, &mz);
            else if (vpart[i] == PART_DISC_TOP || vpart[i] == PART_DISC_BOTTOM)
                disc_point(p[0], p[1], p[2], &mx, &my, &mz);
        }
        turn_point(mx, my, mz, &o->x, &o->y, &o->z);
        o->h = o->y - case_floor;
        float persp = VIEW_DISTANCE / (VIEW_DISTANCE - o->z);
        o->sx = cx + o->x * scale * persp;
        o->sy = cy - o->y * scale * persp;
        /* the same point turned over the table's plane */
        Pt *m = &mirrored[i];
        *m = *o;
        m->y = case_floor - o->h;
        m->sy = cy - m->y * scale * persp;
    }
    const float alpha = ((tint >> 24) & 0xff) / 255.0f;
    float ax, ay, az, bx, by, bz;
    turn_point(1.0f, 0.0f, 0.0f, &ax, &ay, &az);
    turn_point(0.0f, 0.0f, 0.0f, &bx, &by, &bz);
    const bool disc_last = launch_on && !launch_tray && launch_open > 0.55f && az >= bz - 0.001f;
    if (reflection)
    {
        /* the image in the table: the case turned over its plane, fading out with height; the CD, once the lid is
         * open, is in it too (in the same order as over the table) */
        const Mirror mirror = {case_h * 0.55f, 0.34f};
        const bool disc_out = launch_on && !launch_tray && launch_open > 0.01f;
        draw_part(PART_BACK, mirrored, cover, tint, &mirror);
        draw_part(PART_INNER, mirrored, cover, tint, &mirror);
        if (disc_out && !disc_last)
        {
            draw_part(PART_DISC_BOTTOM, mirrored, cover, tint, &mirror);
            draw_disc_cover(cover, tint, cx, cy, scale, &mirror);
            draw_part(PART_DISC_TOP, mirrored, cover, tint, &mirror);
        }
        draw_part(PART_PROMO, mirrored, cover, tint, &mirror);
        draw_part(PART_FRONT, mirrored, cover, tint, &mirror);
        if (disc_out && disc_last)
        {
            draw_part(PART_DISC_BOTTOM, mirrored, cover, tint, &mirror);
            draw_disc_cover(cover, tint, cx, cy, scale, &mirror);
            draw_part(PART_DISC_TOP, mirrored, cover, tint, &mirror);
        }
        if (floor_shadow_strength > 0.0f)
            draw_contact_shadow(c, s, cx, cy, scale, alpha * floor_shadow_strength);
    }
    draw_part(PART_BACK, pts, cover, tint, NULL);
    draw_part(PART_INNER, pts, cover, tint, NULL);
    /* the CD sits on the tray, behind the lid; once it lifts it is over the ribbed strip at the case's left */
    const bool strip_first = launch_on && !launch_tray && launch_open > 0.08f;
    if (strip_first)
        draw_plastic(cx, cy, scale, alpha, true, false);
    if (launch_on && !disc_last)
        draw_disc(pts, cover, tint, cx, cy, scale);
    draw_part(PART_PROMO, pts, cover, tint, NULL);
    draw_part(PART_FRONT, pts, cover, tint, NULL);
    if (disc_last)
        draw_disc(pts, cover, tint, cx, cy, scale);
    draw_plastic(cx, cy, scale, alpha, !strip_first, true);
    launch_on = false;
    launch_open = launch_spin = 0.0f;
    view_distance = VIEW_DISTANCE_BASE;
}
